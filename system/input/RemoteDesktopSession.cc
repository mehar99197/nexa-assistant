#include "system/input/RemoteDesktopSession.h"

#include "core/Logging.h"
#include "core/config/Portable.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QRectF>
#include <QSaveFile>
#include <QStringList>

#ifdef Q_OS_UNIX
#include <QDBusArgument>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusVariant>
#endif

#include <algorithm>
#include <limits>
#include <utility>

using namespace Qt::StringLiterals;

namespace nexa {

#ifdef Q_OS_UNIX
namespace {

const QString kPortal = u"org.freedesktop.portal.Desktop"_s;
const QString kPortalPath = u"/org/freedesktop/portal/desktop"_s;
const QString kRemoteDesktop = u"org.freedesktop.portal.RemoteDesktop"_s;
const QString kScreenCast = u"org.freedesktop.portal.ScreenCast"_s;
const QString kRequestInterface = u"org.freedesktop.portal.Request"_s;
const QString kSessionInterface = u"org.freedesktop.portal.Session"_s;
const QString kRegistry = u"org.freedesktop.host.portal.Registry"_s;
// The portal ties a D-Bus connection to one app id, on the first call from that connection, and
// Qt has already used the shared one by the time this runs.
const QString kConnectionName = u"nexa-remote-desktop"_s;

constexpr uint kDeviceKeyboard = 1;
constexpr uint kDevicePointer = 2;
constexpr uint kSourceMonitor = 1;
constexpr uint kCursorHidden = 1;           // nobody watches the stream, so it need not draw one
constexpr uint kPersistUntilRevoked = 2;

// GNOME's dialog waits for the user.
constexpr int kStartTimeoutMs = 120'000;
constexpr int kNotifyTimeoutMs = 5'000;
// Long enough to cover the gaps in one agent turn, short enough that the "screen is shared"
// indicator does not linger once Nexa is done.
constexpr int kIdleCloseMs = 20'000;

QString randomToken() {
    return u"nexa%1"_s.arg(QRandomGenerator::global()->generate(), 0, 16);
}

// A pair of integers, (ii), as the portal reports positions and sizes. The argument is const on
// purpose: QDBusArgument's reading API is the const one.
std::optional<QPoint> readPair(const QVariant &value) {
    if (!value.canConvert<QDBusArgument>()) return std::nullopt;
    const QDBusArgument argument = value.value<QDBusArgument>();
    if (argument.currentType() != QDBusArgument::StructureType) return std::nullopt;
    int first = 0;
    int second = 0;
    argument.beginStructure();
    argument >> first >> second;
    argument.endStructure();
    return QPoint(first, second);
}

// Start's "streams": a(ua{sv}) — node id, then position and size among the properties.
QList<RemoteDesktopSession::Stream> readStreams(const QVariant &value) {
    QList<RemoteDesktopSession::Stream> streams;
    if (!value.canConvert<QDBusArgument>()) return streams;
    const QDBusArgument argument = value.value<QDBusArgument>();
    argument.beginArray();
    while (!argument.atEnd()) {
        uint node = 0;
        QVariantMap properties;
        argument.beginStructure();
        argument >> node >> properties;
        argument.endStructure();
        const std::optional<QPoint> size = readPair(properties.value(u"size"_s));
        if (!size || size->x() <= 0 || size->y() <= 0) continue;   // nothing to point into
        const QPoint position = readPair(properties.value(u"position"_s)).value_or(QPoint());
        streams.append({node, QRect(position, QSize(size->x(), size->y()))});
    }
    argument.endArray();
    return streams;
}

} // namespace

RemoteDesktopSession::RemoteDesktopSession(QString appId, QString tokenPath, QObject *parent)
    : QObject(parent),
      appId_(std::move(appId)),
      tokenPath_(std::move(tokenPath)),
      units_(unitsFor(qEnvironmentVariable("XDG_CURRENT_DESKTOP"))),
      bus_(kConnectionName) {      // connected on first use, not at start-up
    startTimeout_.setSingleShot(true);
    connect(&startTimeout_, &QTimer::timeout, this, [this] {
        fail(Error{ErrorCode::Timeout, tr("The desktop did not answer the request to control the pointer.")});
    });
    idle_.setSingleShot(true);
    idle_.setInterval(kIdleCloseMs);
    connect(&idle_, &QTimer::timeout, this, &RemoteDesktopSession::close);
    loadToken();
}

RemoteDesktopSession::~RemoteDesktopSession() {
    close();
    if (bus_.isConnected()) QDBusConnection::disconnectFromBus(kConnectionName);
}

#endif

QString RemoteDesktopSession::defaultTokenPath() {
    return portable::dataDir() + u"/remote-desktop-token"_s;
}

RemoteDesktopSession::StreamUnits RemoteDesktopSession::unitsFor(const QString &currentDesktop) {
    // A colon-separated list, e.g. "ubuntu:GNOME" or "GNOME-Classic:GNOME".
    const QStringList desktops = currentDesktop.split(u':', Qt::SkipEmptyParts);
    const bool gnome = std::any_of(desktops.cbegin(), desktops.cend(), [](const QString &desktop) {
        return desktop.compare(u"GNOME"_s, Qt::CaseInsensitive) == 0
               || desktop.startsWith(u"GNOME-"_s, Qt::CaseInsensitive);
    });
    return gnome ? StreamUnits::Pixels : StreamUnits::Logical;
}

std::optional<RemoteDesktopSession::StreamPoint> RemoteDesktopSession::locate(QPoint physical,
                                                                              QSize physicalDesktop,
                                                                              const QList<Stream> &streams,
                                                                              StreamUnits units) {
    if (streams.isEmpty() || physicalDesktop.width() <= 0 || physicalDesktop.height() <= 0) return std::nullopt;
    QRect bounds;
    for (const Stream &stream : streams) bounds = bounds.united(stream.logical);
    if (bounds.isEmpty()) return std::nullopt;

    // The screenshot shows the same desktop the streams cover, only in physical pixels; a
    // fractional scale of 1.25 turns 1920 of them into 1536 logical ones. The centre of the pixel,
    // so the last one still lands inside the screen.
    const double scaleX = double(bounds.width()) / physicalDesktop.width();
    const double scaleY = double(bounds.height()) / physicalDesktop.height();
    const QPointF logical(bounds.x() + (physical.x() + 0.5) * scaleX, bounds.y() + (physical.y() + 0.5) * scaleY);

    // The stream that contains the point — or, in a gap between monitors, the nearest one.
    const Stream *best = nullptr;
    double bestDistance = std::numeric_limits<double>::max();
    for (const Stream &stream : streams) {
        const QRectF area(stream.logical);
        const double dx = std::max({area.left() - logical.x(), 0.0, logical.x() - area.right()});
        const double dy = std::max({area.top() - logical.y(), 0.0, logical.y() - area.bottom()});
        const double distance = dx * dx + dy * dy;
        if (distance < bestDistance) {
            bestDistance = distance;
            best = &stream;
        }
    }
    // Within the stream, in the units the desktop reads: logical as reported, or those times the
    // monitor's scale — which, for a screenshot of the same desktop, is the physical/logical ratio.
    const double perLogicalX = units == StreamUnits::Pixels ? 1.0 / scaleX : 1.0;
    const double perLogicalY = units == StreamUnits::Pixels ? 1.0 / scaleY : 1.0;
    const QRectF area(best->logical);
    const double width = area.width() * perLogicalX;
    const double height = area.height() * perLogicalY;
    return StreamPoint{best->nodeId,
                       QPointF(std::clamp((logical.x() - area.left()) * perLogicalX, 0.0, width - 1.0),
                               std::clamp((logical.y() - area.top()) * perLogicalY, 0.0, height - 1.0))};
}

#ifdef Q_OS_UNIX
bool RemoteDesktopSession::isAvailable() {
    // Asked once: the answer does not change while the session lasts, and the check is a blocking
    // round trip to the portal.
    static const bool available = [] {
        const QDBusConnection bus = QDBusConnection::sessionBus();
        if (!bus.isConnected()) return false;
        QDBusMessage get = QDBusMessage::createMethodCall(kPortal, kPortalPath,
                                                          u"org.freedesktop.DBus.Properties"_s, u"Get"_s);
        get << kRemoteDesktop << u"AvailableDeviceTypes"_s;
        const QDBusMessage reply = bus.call(get, QDBus::Block, 1500);
        if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) return false;
        const uint types = reply.arguments().constFirst().value<QDBusVariant>().variant().toUInt();
        return (types & kDevicePointer) != 0;
    }();
    return available;
}

void RemoteDesktopSession::pointerTo(QPoint physical, QSize physicalDesktop, Done done) {
    whenReady([this, physical, physicalDesktop, done] {
        const std::optional<StreamPoint> target = locate(physical, physicalDesktop, streams_, units_);
        if (!target) {
            done(Error{ErrorCode::NotSupported, tr("The shared screen has no size, so the pointer cannot be placed.")});
            return;
        }
        notify(u"NotifyPointerMotionAbsolute"_s, {target->nodeId, target->position.x(), target->position.y()}, done);
    }, done);
}

void RemoteDesktopSession::pointerBy(double dx, double dy, Done done) {
    whenReady([this, dx, dy, done] { notify(u"NotifyPointerMotion"_s, {dx, dy}, done); }, done);
}

void RemoteDesktopSession::pointerButton(int button, bool down, Done done) {
    whenReady([this, button, down, done] {
        notify(u"NotifyPointerButton"_s, {button, uint(down ? 1 : 0)}, done);
    }, done);
}

void RemoteDesktopSession::pointerScroll(int verticalSteps, int horizontalSteps, Done done) {
    whenReady([this, verticalSteps, horizontalSteps, done] {
        if (verticalSteps == 0) {
            notify(u"NotifyPointerAxisDiscrete"_s, {uint(1), horizontalSteps}, done);
            return;
        }
        if (horizontalSteps == 0) {
            notify(u"NotifyPointerAxisDiscrete"_s, {uint(0), verticalSteps}, done);
            return;
        }
        // Both axes: the horizontal one once the vertical one has been taken.
        notify(u"NotifyPointerAxisDiscrete"_s, {uint(0), verticalSteps}, [this, horizontalSteps, done](Result<void> vertical) {
            if (!vertical) {
                done(vertical);
                return;
            }
            notify(u"NotifyPointerAxisDiscrete"_s, {uint(1), horizontalSteps}, done);
        });
    }, done);
}

void RemoteDesktopSession::keyboardKey(int key, bool down, Done done) {
    whenReady([this, key, down, done] {
        notify(u"NotifyKeyboardKeycode"_s, {key, uint(down ? 1 : 0)}, done);
    }, done);
}

void RemoteDesktopSession::whenReady(std::function<void()> action, Done done) {
    if (state_ == State::Ready) {
        touch();
        action();
        return;
    }
    waiting_.emplace_back(std::move(action), std::move(done));
    if (state_ != State::Idle) return;      // already starting; this waits with the rest

    if (!bus_.isConnected()) {
        bus_ = QDBusConnection::connectToBus(QDBusConnection::SessionBus, kConnectionName);
        registered_ = false;
    }
    if (!bus_.isConnected()) {
        fail(Error{ErrorCode::NotSupported, tr("There is no D-Bus session bus to reach the desktop through.")});
        return;
    }
    startTimeout_.start(kStartTimeoutMs);
    if (registered_) {
        createSession();
    } else {
        registerApp();
    }
}

// A program outside a sandbox has no app id of its own, and the portal keys its permissions —
// including the "remember this" that spares the user a second prompt — by app id. Registering says
// which .desktop file Nexa is; it has to be the first portal call on this connection.
void RemoteDesktopSession::registerApp() {
    state_ = State::Creating;
    QDBusMessage call = QDBusMessage::createMethodCall(kPortal, kPortalPath, kRegistry, u"Register"_s);
    call << appId_ << QVariantMap{};
    auto *watcher = new QDBusPendingCallWatcher(bus_.asyncCall(call), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *reply) {
        reply->deleteLater();
        // A portal without the Registry may still accept the session; try either way.
        if (reply->isError()) qCDebug(lcApp) << "portal Registry:" << reply->error().message();
        registered_ = true;
        if (state_ == State::Creating) createSession();
    });
}

void RemoteDesktopSession::createSession() {
    request(kRemoteDesktop, u"CreateSession"_s, {}, {{u"session_handle_token"_s, randomToken()}}, State::Creating);
}

void RemoteDesktopSession::selectDevices() {
    QVariantMap options{{u"types"_s, kDeviceKeyboard | kDevicePointer},
                        {u"persist_mode"_s, kPersistUntilRevoked}};
    if (!restoreToken_.isEmpty()) options.insert(u"restore_token"_s, restoreToken_);
    request(kRemoteDesktop, u"SelectDevices"_s, {QVariant::fromValue(QDBusObjectPath(sessionPath_))}, options,
            State::SelectingDevices);
}

void RemoteDesktopSession::selectSources() {
    request(kScreenCast, u"SelectSources"_s, {QVariant::fromValue(QDBusObjectPath(sessionPath_))},
            {{u"types"_s, kSourceMonitor}, {u"multiple"_s, true}, {u"cursor_mode"_s, kCursorHidden}},
            State::SelectingSources);
}

void RemoteDesktopSession::start() {
    request(kRemoteDesktop, u"Start"_s, {QVariant::fromValue(QDBusObjectPath(sessionPath_)), QString()}, {},
            State::Starting);
}

void RemoteDesktopSession::request(const QString &interface, const QString &method, QList<QVariant> arguments,
                                   QVariantMap options, State next) {
    state_ = next;
    const QString token = randomToken();
    const QString sender = bus_.baseService().mid(1).replace(u'.', u'_');
    // Subscribe before calling, or a fast Response would be missed.
    subscribe(u"/org/freedesktop/portal/desktop/request/%1/%2"_s.arg(sender, token));
    options.insert(u"handle_token"_s, token);
    arguments.append(options);

    QDBusMessage call = QDBusMessage::createMethodCall(kPortal, kPortalPath, interface, method);
    call.setArguments(arguments);
    const QString expected = requestPath_;
    auto *watcher = new QDBusPendingCallWatcher(bus_.asyncCall(call, kStartTimeoutMs), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, method, expected](QDBusPendingCallWatcher *reply) {
        reply->deleteLater();
        if (requestPath_ != expected) return;       // superseded by a failure or a restart
        const QDBusPendingReply<QDBusObjectPath> handle = *reply;
        if (handle.isError()) {
            fail(Error{ErrorCode::NotSupported, tr("The desktop refused %1: %2").arg(method, handle.error().message())});
            return;
        }
        // Older portals pick their own Request path.
        if (handle.value().path() != requestPath_) subscribe(handle.value().path());
    });
}

void RemoteDesktopSession::onResponse(uint response, const QVariantMap &results) {
    unsubscribe();
    if (response != 0) {
        fail(response == 1 ? Error{ErrorCode::PermissionDenied, tr("You declined to let Nexa control the pointer.")}
                           : Error{ErrorCode::Unknown, tr("The desktop could not start remote control.")});
        return;
    }
    switch (state_) {
    case State::Creating: {
        const QVariant handle = results.value(u"session_handle"_s);
        sessionPath_ = handle.metaType() == QMetaType::fromType<QDBusObjectPath>()
                           ? handle.value<QDBusObjectPath>().path()
                           : handle.toString();
        if (sessionPath_.isEmpty()) {
            fail(Error{ErrorCode::Unknown, tr("The desktop returned no remote-control session.")});
            return;
        }
        bus_.connect(kPortal, sessionPath_, kSessionInterface, u"Closed"_s, this, SLOT(onClosed(QVariantMap)));
        selectDevices();
        return;
    }
    case State::SelectingDevices:
        selectSources();
        return;
    case State::SelectingSources:
        start();
        return;
    case State::Starting: {
        startTimeout_.stop();
        // Restore tokens are single-use: each start hands out the one for next time.
        if (const QString token = results.value(u"restore_token"_s).toString(); !token.isEmpty()) saveToken(token);
        if ((results.value(u"devices"_s).toUInt() & kDevicePointer) == 0) {
            fail(Error{ErrorCode::PermissionDenied, tr("The desktop did not allow Nexa to use the pointer.")});
            return;
        }
        streams_ = readStreams(results.value(u"streams"_s));
        if (streams_.isEmpty()) {
            fail(Error{ErrorCode::NotSupported, tr("The desktop shared no screen, so there is nothing to point at.")});
            return;
        }
        state_ = State::Ready;
        touch();
        qCInfo(lcApp) << "remote desktop session ready on" << streams_.size() << "screen(s)";
        for (auto &[action, done] : std::exchange(waiting_, {})) action();
        return;
    }
    case State::Idle:
    case State::Ready:
        return;
    }
}

void RemoteDesktopSession::onClosed(const QVariantMap &) {
    // The user stopped it from GNOME's indicator, or the desktop ended it. Nothing to close.
    qCInfo(lcApp) << "the desktop ended the remote desktop session";
    bus_.disconnect(kPortal, sessionPath_, kSessionInterface, u"Closed"_s, this, SLOT(onClosed(QVariantMap)));
    sessionPath_.clear();
    close();
}

void RemoteDesktopSession::close() {
    idle_.stop();
    if (!sessionPath_.isEmpty() && bus_.isConnected()) {
        bus_.disconnect(kPortal, sessionPath_, kSessionInterface, u"Closed"_s, this, SLOT(onClosed(QVariantMap)));
        bus_.call(QDBusMessage::createMethodCall(kPortal, sessionPath_, kSessionInterface, u"Close"_s), QDBus::NoBlock);
        qCInfo(lcApp) << "remote desktop session closed";
    }
    sessionPath_.clear();
    streams_.clear();
    state_ = State::Idle;
}

void RemoteDesktopSession::notify(const QString &method, QList<QVariant> arguments, Done done) {
    arguments.prepend(QVariantMap{});
    arguments.prepend(QVariant::fromValue(QDBusObjectPath(sessionPath_)));
    QDBusMessage call = QDBusMessage::createMethodCall(kPortal, kPortalPath, kRemoteDesktop, method);
    call.setArguments(arguments);
    auto *watcher = new QDBusPendingCallWatcher(bus_.asyncCall(call, kNotifyTimeoutMs), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [method, done](QDBusPendingCallWatcher *reply) {
        reply->deleteLater();
        if (reply->isError()) {
            done(Error{ErrorCode::Io, tr("The desktop refused %1: %2").arg(method, reply->error().message())});
            return;
        }
        done(Result<void>{});
    });
}

void RemoteDesktopSession::fail(const Error &error) {
    startTimeout_.stop();
    unsubscribe();
    qCInfo(lcApp).noquote() << "remote desktop:" << error.message;
    close();        // a half-made session is closed, not left with the portal
    for (auto &[action, done] : std::exchange(waiting_, {})) done(error);
}

void RemoteDesktopSession::subscribe(const QString &path) {
    unsubscribe();
    requestPath_ = path;
    bus_.connect(kPortal, path, kRequestInterface, u"Response"_s, this, SLOT(onResponse(uint, QVariantMap)));
}

void RemoteDesktopSession::unsubscribe() {
    if (requestPath_.isEmpty()) return;
    bus_.disconnect(kPortal, requestPath_, kRequestInterface, u"Response"_s, this, SLOT(onResponse(uint, QVariantMap)));
    requestPath_.clear();
}

void RemoteDesktopSession::touch() {
    idle_.start();
}

void RemoteDesktopSession::loadToken() {
    QFile file(tokenPath_);
    if (file.open(QIODevice::ReadOnly)) restoreToken_ = QString::fromUtf8(file.readAll()).trimmed();
}

// The token lets Nexa start a session without asking, so it is kept like a credential: readable by
// the user alone.
void RemoteDesktopSession::saveToken(const QString &token) {
    restoreToken_ = token;
    if (tokenPath_.isEmpty()) return;
    QDir().mkpath(QFileInfo(tokenPath_).absolutePath());
    QSaveFile file(tokenPath_);
    if (!file.open(QIODevice::WriteOnly)) {
        qCWarning(lcApp).noquote() << "cannot keep the remote desktop token:" << file.errorString();
        return;
    }
    file.write(token.toUtf8());
    if (file.commit()) QFile::setPermissions(tokenPath_, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
}
#else
RemoteDesktopSession::RemoteDesktopSession(QString appId, QString tokenPath, QObject *parent)
    : QObject(parent),
      appId_(std::move(appId)),
      tokenPath_(std::move(tokenPath)),
      units_(StreamUnits::Logical) {}

RemoteDesktopSession::~RemoteDesktopSession() = default;

bool RemoteDesktopSession::isAvailable() { return false; }

void RemoteDesktopSession::pointerTo(QPoint, QSize, Done done) {
    done(Error{ErrorCode::NotSupported, tr("Desktop remote-control portal is only used on Linux.")});
}
void RemoteDesktopSession::pointerBy(double, double, Done done) {
    done(Error{ErrorCode::NotSupported, tr("Desktop remote-control portal is only used on Linux.")});
}
void RemoteDesktopSession::pointerButton(int, bool, Done done) {
    done(Error{ErrorCode::NotSupported, tr("Desktop remote-control portal is only used on Linux.")});
}
void RemoteDesktopSession::pointerScroll(int, int, Done done) {
    done(Error{ErrorCode::NotSupported, tr("Desktop remote-control portal is only used on Linux.")});
}
void RemoteDesktopSession::keyboardKey(int, bool, Done done) {
    done(Error{ErrorCode::NotSupported, tr("Desktop remote-control portal is only used on Linux.")});
}
void RemoteDesktopSession::close() {}
void RemoteDesktopSession::onResponse(uint, const QVariantMap &) {}
void RemoteDesktopSession::onClosed(const QVariantMap &) {}
#endif

} // namespace nexa
