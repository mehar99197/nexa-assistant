#include "system/screen/ScreenAwake.h"

#include "core/Logging.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QPointer>
#include <QVariantMap>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

// Inhibit flags, the same in the portal and in GNOME's session manager: 4 suspend, 8 idle (blanking and the lock).
constexpr uint kSuspendAndIdle = 4 | 8;

} // namespace

ScreenAwake::ScreenAwake(QObject *parent) : QObject(parent) {}

ScreenAwake::~ScreenAwake() { drop(); }

void ScreenAwake::hold(const QString &reason) {
    wanted_ = true;
    if (isHeld() || asking_) return;
    asking_ = true;
    QDBusMessage call = QDBusMessage::createMethodCall(u"org.freedesktop.portal.Desktop"_s,
                                                       u"/org/freedesktop/portal/desktop"_s,
                                                       u"org.freedesktop.portal.Inhibit"_s, u"Inhibit"_s);
    call << QString() << kSuspendAndIdle << QVariantMap{{u"reason"_s, reason}};
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(call), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, reason](QDBusPendingCallWatcher *finished) {
        finished->deleteLater();
        asking_ = false;
        const QDBusPendingReply<QDBusObjectPath> reply = *finished;
        if (reply.isError()) {
            qCDebug(lcApp).noquote() << "the portal will not keep the screen on:" << reply.error().message();
            if (wanted_) holdThroughGnome(reason);
            return;
        }
        portalHandle_ = reply.value().path();
        qCDebug(lcApp) << "screen kept on through the portal";
        if (!wanted_) drop();       // released while the portal was asked
    });
}

void ScreenAwake::holdThroughGnome(const QString &reason) {
    asking_ = true;
    QDBusMessage call = QDBusMessage::createMethodCall(u"org.gnome.SessionManager"_s, u"/org/gnome/SessionManager"_s,
                                                       u"org.gnome.SessionManager"_s, u"Inhibit"_s);
    call << u"org.nexa.Assistant"_s << uint(0) << reason << kSuspendAndIdle;
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(call), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *finished) {
        finished->deleteLater();
        asking_ = false;
        const QDBusPendingReply<uint> reply = *finished;
        if (reply.isError()) {
            qCWarning(lcApp).noquote() << "the screen may blank: nothing would keep it on:" << reply.error().message();
            return;
        }
        gnomeCookie_ = reply.value();
        qCDebug(lcApp) << "screen kept on through GNOME's session manager";
        if (!wanted_) drop();
    });
}

void ScreenAwake::release() {
    wanted_ = false;
    drop();
}

void ScreenAwake::drop() {
    if (!portalHandle_.isEmpty()) {
        QDBusConnection::sessionBus().asyncCall(QDBusMessage::createMethodCall(
            u"org.freedesktop.portal.Desktop"_s, portalHandle_, u"org.freedesktop.portal.Request"_s, u"Close"_s));
        portalHandle_.clear();
    }
    if (gnomeCookie_ != 0) {
        QDBusMessage call = QDBusMessage::createMethodCall(u"org.gnome.SessionManager"_s, u"/org/gnome/SessionManager"_s,
                                                           u"org.gnome.SessionManager"_s, u"Uninhibit"_s);
        call << gnomeCookie_;
        QDBusConnection::sessionBus().asyncCall(call);
        gnomeCookie_ = 0;
    }
}

} // namespace nexa
