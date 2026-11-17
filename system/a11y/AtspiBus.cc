#include "system/a11y/AtspiBus.h"

#include "core/Logging.h"

#ifdef Q_OS_UNIX
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusVariant>
#elif defined(Q_OS_WIN)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#endif

using namespace Qt::StringLiterals;

namespace nexa {

#ifdef Q_OS_UNIX
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
#elif defined(Q_OS_WIN)
namespace {

constexpr CLSID kClsidCUIAutomation = {
    0xff48dba4, 0x60ef, 0x4201, {0xaa, 0x87, 0x54, 0x10, 0x3e, 0xef, 0x59, 0x4e}};
constexpr IID kIidIUIAutomation = {
    0x30cbe57d, 0xd9d0, 0x452a, {0xab, 0x13, 0x7a, 0xc5, 0xac, 0x48, 0x25, 0xee}};

} // namespace

AtspiBus::AtspiBus(QObject *parent) : QObject(parent) {}

void AtspiBus::ensureConnected() const {
    if (tried_) return;
    tried_ = true;
    const HRESULT hrCo = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool needUninit = (hrCo == S_OK || hrCo == S_FALSE);
    if (FAILED(hrCo) && hrCo != RPC_E_CHANGED_MODE) {
        reason_ = u"Windows COM could not be initialised for UI Automation (0x%1)."_s
                      .arg(static_cast<quint32>(hrCo), 8, 16, u'0');
        return;
    }
    IUnknown *uia = nullptr;
    const HRESULT hr = ::CoCreateInstance(kClsidCUIAutomation, nullptr, CLSCTX_INPROC_SERVER, kIidIUIAutomation,
                                          reinterpret_cast<void **>(&uia));
    if (SUCCEEDED(hr) && uia) {
        uia->Release();
        available_ = true;
        qCInfo(lcApp).noquote() << "a11y: Windows UI Automation ready";
    } else {
        reason_ = u"Windows UI Automation is not available (0x%1)."_s
                      .arg(static_cast<quint32>(hr), 8, 16, u'0');
        qCInfo(lcApp).noquote() << "a11y:" << reason_;
    }
    if (needUninit) ::CoUninitialize();
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
    if (!isAvailable()) return Error{ErrorCode::NotSupported, unavailableReason()};
    return true;
}

Result<void> AtspiBus::setAppsExposeContents(bool on) {
    if (!isAvailable()) return Error{ErrorCode::NotSupported, unavailableReason()};
    if (!on) return Error{ErrorCode::NotSupported, u"Windows UI Automation is always enabled by the system."_s};
    return {};
}
#else
AtspiBus::AtspiBus(QObject *parent) : QObject(parent) {}

void AtspiBus::ensureConnected() const {
    if (tried_) return;
    tried_ = true;
    reason_ = u"AT-SPI2 accessibility bus is not available on this platform."_s;
}

bool AtspiBus::isAvailable() const {
    ensureConnected();
    return false;
}

QString AtspiBus::unavailableReason() const {
    ensureConnected();
    return reason_;
}

Result<bool> AtspiBus::appsExposeContents() const {
    return Error{ErrorCode::NotSupported, unavailableReason()};
}

Result<void> AtspiBus::setAppsExposeContents(bool) {
    return Error{ErrorCode::NotSupported, unavailableReason()};
}
#endif

} // namespace nexa
