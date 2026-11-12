#include "system/a11y/AtspiBus.h"

#include "core/Logging.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusVariant>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

constexpr int kCallTimeoutMs = 1000;
constexpr QLatin1StringView kService{"org.a11y.Bus"};
constexpr QLatin1StringView kPath{"/org/a11y/bus"};
constexpr QLatin1StringView kStatus{"org.a11y.Status"};
constexpr QLatin1StringView kProperties{"org.freedesktop.DBus.Properties"};

} // namespace

AtspiBus::AtspiBus(QObject *parent) : QObject(parent) {}

void AtspiBus::ensureConnected() const {
    if (tried_) return;
    tried_ = true;
    const QDBusConnection session = QDBusConnection::sessionBus();
    if (!session.isConnected()) {
        reason_ = u"There is no session bus, so no accessibility bus either."_s;
        return;
    }
    const QDBusMessage ask = QDBusMessage::createMethodCall(QString(kService), QString(kPath), QString(kService), u"GetAddress"_s);
    const QDBusMessage reply = session.call(ask, QDBus::Block, kCallTimeoutMs);
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) {
        reason_ = u"The desktop has no accessibility bus (org.a11y.Bus did not answer: %1)."_s.arg(
            reply.errorMessage().isEmpty() ? u"no reply"_s : reply.errorMessage());
        qCInfo(lcApp).noquote() << "a11y:" << reason_;
        return;
    }
    const QString address = reply.arguments().first().toString();
    const QDBusConnection bus = QDBusConnection::connectToBus(address, QString(kConnectionName));
    if (!bus.isConnected()) {
        reason_ = u"The accessibility bus could not be reached: %1"_s.arg(bus.lastError().message());
        qCInfo(lcApp).noquote() << "a11y:" << reason_;
        return;
    }
    available_ = true;
    qCInfo(lcApp).noquote() << "a11y: connected to the accessibility bus";
}

bool AtspiBus::isAvailable() const {
    ensureConnected();
    return available_;
}

QString AtspiBus::unavailableReason() const {
    ensureConnected();
    return available_ ? QString() : reason_;
}

Result<bool> AtspiBus::appsExposeContents() const {
    const QDBusConnection session = QDBusConnection::sessionBus();
    QDBusMessage ask = QDBusMessage::createMethodCall(QString(kService), QString(kPath), QString(kProperties), u"Get"_s);
    ask.setArguments({QString(kStatus), u"IsEnabled"_s});
    const QDBusMessage reply = session.call(ask, QDBus::Block, kCallTimeoutMs);
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) {
        return Error{ErrorCode::Io, u"The desktop's accessibility switch could not be read: %1"_s.arg(reply.errorMessage())};
    }
    return reply.arguments().first().value<QDBusVariant>().variant().toBool();
}

Result<void> AtspiBus::setAppsExposeContents(bool on) {
    const QDBusConnection session = QDBusConnection::sessionBus();
    QDBusMessage set = QDBusMessage::createMethodCall(QString(kService), QString(kPath), QString(kProperties), u"Set"_s);
    set.setArguments({QString(kStatus), u"IsEnabled"_s, QVariant::fromValue(QDBusVariant(on))});
    const QDBusMessage reply = session.call(set, QDBus::Block, kCallTimeoutMs);
    if (reply.type() == QDBusMessage::ErrorMessage) {
        return Error{ErrorCode::Io, u"The desktop's accessibility switch could not be set: %1"_s.arg(reply.errorMessage())};
    }
    qCInfo(lcApp) << "a11y: apps expose their contents:" << on;
    return {};
}

} // namespace nexa
