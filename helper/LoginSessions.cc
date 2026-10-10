#include "helper/LoginSessions.h"

#include "core/Logging.h"

#include <QDBusArgument>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusReply>
#include <QVariantMap>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

constexpr auto kService = "org.freedesktop.login1"_L1;
constexpr auto kManagerPath = "/org/freedesktop/login1"_L1;
constexpr auto kManagerInterface = "org.freedesktop.login1.Manager"_L1;
constexpr auto kUserInterface = "org.freedesktop.login1.User"_L1;
constexpr auto kSessionInterface = "org.freedesktop.login1.Session"_L1;
constexpr auto kPropertiesInterface = "org.freedesktop.DBus.Properties"_L1;

// logind is a local daemon that never calls back into the helper, so waiting for its answer cannot
// deadlock; and the helper has no user interface to keep responsive. A short timeout all the same,
// so a wedged logind turns into a refusal rather than a hang.
constexpr int kTimeoutMs = 3000;

QVariantMap allProperties(const QDBusConnection &bus, const QString &path, QLatin1StringView interface) {
    QDBusMessage call =
        QDBusMessage::createMethodCall(kService, path, kPropertiesInterface, u"GetAll"_s);
    call << QString(interface);
    const QDBusMessage reply = bus.call(call, QDBus::Block, kTimeoutMs);
    if (reply.type() != QDBusMessage::ReplyMessage) {
        qCWarning(lcApp).noquote() << "logind did not describe" << path << ":" << reply.errorMessage();
        return {};
    }
    return qdbus_cast<QVariantMap>(reply.arguments().value(0).value<QDBusArgument>());
}

// logind reports a session reference as (session id, object path).
//
// The argument has to be const: QDBusArgument's reading and writing APIs are told apart by
// constness alone, so a non-const one here calls the *writing* beginStructure() — which warns
// "write from a read-only object", extracts nothing, and looks exactly like a user with no
// graphical session.
std::pair<QString, QString> sessionReference(const QVariant &value) {
    const QDBusArgument argument = value.value<QDBusArgument>();
    if (argument.currentType() != QDBusArgument::StructureType) return {};
    QString id;
    QDBusObjectPath path;
    argument.beginStructure();
    argument >> id >> path;
    argument.endStructure();
    return {id, path.path()};
}

} // namespace

LoginSessions::LoginSessions(const QDBusConnection &bus) : bus_(bus) {}

LoginSession LoginSessions::displaySessionOf(uint uid) const {
    QDBusMessage call =
        QDBusMessage::createMethodCall(kService, kManagerPath, kManagerInterface, u"GetUser"_s);
    call << uid;
    const QDBusMessage userReply = bus_.call(call, QDBus::Block, kTimeoutMs);
    if (userReply.type() != QDBusMessage::ReplyMessage) {
        qCWarning(lcApp).noquote() << "logind knows no user" << uid << ":" << userReply.errorMessage();
        return {};
    }
    const QString userPath = userReply.arguments().value(0).value<QDBusObjectPath>().path();
    if (userPath.isEmpty()) return {};

    const QVariantMap user = allProperties(bus_, userPath, kUserInterface);
    const auto [displayId, displayPath] = sessionReference(user.value(u"Display"_s));
    if (displayId.isEmpty() || displayPath.isEmpty()) {
        qCWarning(lcApp) << "user" << uid << "has no graphical session";
        return {};
    }

    const QVariantMap session = allProperties(bus_, displayPath, kSessionInterface);
    if (session.isEmpty()) return {};

    LoginSession found;
    found.uid = uid;
    found.id = session.value(u"Id"_s).toString();
    found.startedMonotonicUsec = session.value(u"TimestampMonotonic"_s).toULongLong();
    found.active = session.value(u"Active"_s).toBool();
    found.remote = session.value(u"Remote"_s).toBool();
    if (found.id.isEmpty()) found.id = displayId;      // older logind may not publish Id
    return found;
}

} // namespace nexa
