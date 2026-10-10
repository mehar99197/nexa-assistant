#include "app/GlobalShortcuts.h"

#include "core/Logging.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QRandomGenerator>

#include <utility>

using namespace Qt::StringLiterals;

namespace nexa {

// One entry of the portal's a(sa{sv}) shortcut list.
struct PortalShortcut {
    QString id;
    QVariantMap options;
};

} // namespace nexa

Q_DECLARE_METATYPE(nexa::PortalShortcut)
Q_DECLARE_METATYPE(QList<nexa::PortalShortcut>)

namespace nexa {

QDBusArgument &operator<<(QDBusArgument &argument, const PortalShortcut &shortcut) {
    argument.beginStructure();
    argument << shortcut.id << shortcut.options;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument, PortalShortcut &shortcut) {
    argument.beginStructure();
    argument >> shortcut.id >> shortcut.options;
    argument.endStructure();
    return argument;
}

namespace {

const QString kPortal = u"org.freedesktop.portal.Desktop"_s;
const QString kPortalPath = u"/org/freedesktop/portal/desktop"_s;
const QString kInterface = u"org.freedesktop.portal.GlobalShortcuts"_s;
const QString kRegistry = u"org.freedesktop.host.portal.Registry"_s;
const QString kRequestInterface = u"org.freedesktop.portal.Request"_s;
const QString kSessionInterface = u"org.freedesktop.portal.Session"_s;
// The portal ties a D-Bus connection to one app id, on the first call from that connection. Qt
// talks to the portal while the application starts, so the shortcuts get a connection of their own.
const QString kConnectionName = u"nexa-global-shortcuts"_s;
// GNOME's dialog waits for the user.
constexpr int kCallTimeoutMs = 120'000;

QString randomToken() {
    return u"nexa%1"_s.arg(QRandomGenerator::global()->generate(), 0, 16);
}

} // namespace

GlobalShortcuts::GlobalShortcuts(QString appId, QObject *parent)
    : QObject(parent),
      appId_(std::move(appId)),
      bus_(QDBusConnection::connectToBus(QDBusConnection::SessionBus, kConnectionName)) {
    qDBusRegisterMetaType<PortalShortcut>();
    qDBusRegisterMetaType<QList<PortalShortcut>>();
}

GlobalShortcuts::~GlobalShortcuts() {
    // Closing the session releases the keys; the compositor remembers the user's choice.
    if (!sessionPath_.isEmpty()) {
        bus_.call(QDBusMessage::createMethodCall(kPortal, sessionPath_, kSessionInterface, u"Close"_s),
                  QDBus::NoBlock);
    }
    QDBusConnection::disconnectFromBus(kConnectionName);
}

void GlobalShortcuts::bind(QList<Shortcut> shortcuts) {
    if (!bus_.isConnected()) {
        giveUp(tr("there is no D-Bus session bus"));
        return;
    }
    wanted_ = std::move(shortcuts);
    if (wanted_.isEmpty()) return;
    registerApp();
}

// An app outside a sandbox has no app id of its own, and GNOME discards hotkey requests without
// one. org.freedesktop.host.portal.Registry is how a host app says which .desktop file it is; it
// has to be the first portal call on this connection.
void GlobalShortcuts::registerApp() {
    QDBusMessage call = QDBusMessage::createMethodCall(kPortal, kPortalPath, kRegistry, u"Register"_s);
    call << appId_ << QVariantMap{};
    auto *watcher = new QDBusPendingCallWatcher(bus_.asyncCall(call), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *reply) {
        reply->deleteLater();
        // A portal without the Registry may still accept the session; try either way.
        if (reply->isError()) qCDebug(lcApp) << "portal Registry:" << reply->error().message();
        createSession();
    });
}

void GlobalShortcuts::createSession() {
    createPath_ = subscribe(SLOT(onSessionCreated(uint, QVariantMap)));
    QDBusMessage call = QDBusMessage::createMethodCall(kPortal, kPortalPath, kInterface, u"CreateSession"_s);
    call << QVariantMap{{u"handle_token"_s, createPath_.section(u'/', -1)},
                        {u"session_handle_token"_s, randomToken()}};
    auto *watcher = new QDBusPendingCallWatcher(bus_.asyncCall(call, kCallTimeoutMs), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *reply) {
        reply->deleteLater();
        const QDBusPendingReply<QDBusObjectPath> handle = *reply;
        if (handle.isError()) {
            giveUp(handle.error().message());
            return;
        }
        resubscribe(createPath_, handle.value().path(), SLOT(onSessionCreated(uint, QVariantMap)));
    });
}

void GlobalShortcuts::onSessionCreated(uint response, const QVariantMap &results) {
    unsubscribe(createPath_, SLOT(onSessionCreated(uint, QVariantMap)));
    if (response != 0) {
        giveUp(tr("the session was refused"));
        return;
    }
    sessionPath_ = results.value(u"session_handle"_s).toString();
    if (sessionPath_.isEmpty()) {
        giveUp(tr("the portal returned no session"));
        return;
    }
    bus_.connect(kPortal, sessionPath_, kSessionInterface, u"Closed"_s, this, SLOT(onSessionClosed(QVariantMap)));
    bus_.connect(kPortal, kPortalPath, kInterface, u"Activated"_s, this,
                 SLOT(onActivated(QDBusObjectPath, QString, qulonglong, QVariantMap)));
    bus_.connect(kPortal, kPortalPath, kInterface, u"Deactivated"_s, this,
                 SLOT(onDeactivated(QDBusObjectPath, QString, qulonglong, QVariantMap)));
    bus_.connect(kPortal, kPortalPath, kInterface, u"ShortcutsChanged"_s, this,
                 SLOT(onShortcutsChanged(QDBusMessage)));
    requestShortcuts();
}

void GlobalShortcuts::requestShortcuts() {
    QList<PortalShortcut> shortcuts;
    for (const Shortcut &shortcut : wanted_) {
        QVariantMap options{{u"description"_s, shortcut.description}};
        if (!shortcut.preferredTrigger.isEmpty()) {
            options.insert(u"preferred_trigger"_s, shortcut.preferredTrigger);
        }
        shortcuts.append(PortalShortcut{shortcut.id, options});
    }

    bindPath_ = subscribe(SLOT(onShortcutsBound(uint, QVariantMap)));
    QDBusMessage call = QDBusMessage::createMethodCall(kPortal, kPortalPath, kInterface, u"BindShortcuts"_s);
    call << QVariant::fromValue(QDBusObjectPath(sessionPath_)) << QVariant::fromValue(shortcuts) << QString()
         << QVariantMap{{u"handle_token"_s, bindPath_.section(u'/', -1)}};
    auto *watcher = new QDBusPendingCallWatcher(bus_.asyncCall(call, kCallTimeoutMs), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *reply) {
        reply->deleteLater();
        const QDBusPendingReply<QDBusObjectPath> handle = *reply;
        if (handle.isError()) {
            giveUp(handle.error().message());
            return;
        }
        resubscribe(bindPath_, handle.value().path(), SLOT(onShortcutsBound(uint, QVariantMap)));
    });
}

void GlobalShortcuts::onShortcutsBound(uint response, const QVariantMap &results) {
    unsubscribe(bindPath_, SLOT(onShortcutsBound(uint, QVariantMap)));
    if (response != 0) {
        giveUp(response == 1 ? tr("you declined the hotkeys") : tr("the hotkeys were refused"));
        return;
    }
    // GNOME lists only the shortcuts that carry a key, so an empty list is not a failure: Nexa is
    // registered and the user assigns the keys in Settings → Keyboard, which arrives as
    // ShortcutsChanged. Until then the in-window shortcuts are the only ones.
    registered_ = true;
    readTriggers(results.value(u"shortcuts"_s));
    qCInfo(lcApp) << (triggers_.isEmpty() ? "global hotkeys: registered, no keys assigned yet"
                                          : "global hotkeys:")
                  << triggers_;
    emit registered();
}

// The user assigned or cleared a key in Settings → Keyboard.
void GlobalShortcuts::onShortcutsChanged(const QDBusMessage &message) {
    const QList<QVariant> arguments = message.arguments();
    if (arguments.size() < 2 || arguments.at(0).value<QDBusObjectPath>().path() != sessionPath_) return;
    readTriggers(arguments.at(1));
    if (!registered_) return;       // still inside BindShortcuts; it reports the result itself
    qCInfo(lcApp) << "global hotkeys changed:" << triggers_;
    emit registered();
}

void GlobalShortcuts::readTriggers(const QVariant &shortcuts) {
    if (!shortcuts.canConvert<QDBusArgument>()) return;
    QList<PortalShortcut> listed;
    shortcuts.value<QDBusArgument>() >> listed;
    triggers_.clear();
    for (const PortalShortcut &shortcut : listed) {
        // No trigger_description means the shortcut exists but carries no key yet.
        triggers_.insert(shortcut.id, shortcut.options.value(u"trigger_description"_s).toString());
    }
}

void GlobalShortcuts::onActivated(const QDBusObjectPath &session, const QString &id, qulonglong, const QVariantMap &) {
    if (session.path() == sessionPath_) emit pressed(id);
}

void GlobalShortcuts::onDeactivated(const QDBusObjectPath &session, const QString &id, qulonglong,
                                    const QVariantMap &) {
    if (session.path() == sessionPath_) emit released(id);
}

void GlobalShortcuts::onSessionClosed(const QVariantMap &) {
    sessionPath_.clear();
    registered_ = false;
    triggers_.clear();
    emit unavailable(tr("the hotkey session was closed"));
}

QString GlobalShortcuts::subscribe(const char *slot) {
    const QString sender = bus_.baseService().mid(1).replace(u'.', u'_');
    const QString path = u"/org/freedesktop/portal/desktop/request/%1/%2"_s.arg(sender, randomToken());
    // Subscribe before calling, or a fast Response would be missed.
    bus_.connect(kPortal, path, kRequestInterface, u"Response"_s, this, slot);
    return path;
}

void GlobalShortcuts::resubscribe(QString &path, const QString &actual, const char *slot) {
    if (path == actual) return;     // a portal that picks its own Request path
    unsubscribe(path, slot);
    path = actual;
    bus_.connect(kPortal, path, kRequestInterface, u"Response"_s, this, slot);
}

void GlobalShortcuts::unsubscribe(const QString &path, const char *slot) {
    if (path.isEmpty()) return;
    bus_.disconnect(kPortal, path, kRequestInterface, u"Response"_s, this, slot);
}

void GlobalShortcuts::giveUp(const QString &reason) {
    qCInfo(lcApp) << "no global hotkeys:" << reason;
    registered_ = false;
    triggers_.clear();
    emit unavailable(reason);
}

} // namespace nexa
