#include "PhoneSettings.h"

#include <QSysInfo>
#ifdef Q_OS_ANDROID
#include <QJniObject>
#endif

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

const QString kHost = QStringLiteral("nexa/host");
const QString kPort = QStringLiteral("nexa/port");
const QString kTls = QStringLiteral("nexa/tls");
const QString kToken = QStringLiteral("nexa/token");
const QString kFingerprint = QStringLiteral("nexa/fingerprint");
const QString kName = QStringLiteral("nexa/name");

QString modelName() {
#ifdef Q_OS_ANDROID
    const QString model = QJniObject::getStaticObjectField<jstring>("android/os/Build", "MODEL").toString();
    if (!model.isEmpty()) return model;
#endif
    return QSysInfo::prettyProductName();
}

} // namespace

PhoneSettings::PhoneSettings(const QString &file)
    : settings_(file.isEmpty() ? QSettings() : QSettings(file, QSettings::IniFormat)) {}

RemoteEndpoint PhoneSettings::endpoint() const {
    RemoteEndpoint endpoint;
    endpoint.host = settings_.value(kHost).toString();
    endpoint.port = settings_.value(kPort, RemoteEndpoint::kDefaultPort).toInt();
    endpoint.tls = settings_.value(kTls, true).toBool();
    return endpoint;
}

QString PhoneSettings::token() const {
    return settings_.value(kToken).toString();
}

QString PhoneSettings::fingerprint() const {
    return settings_.value(kFingerprint).toString();
}

QString PhoneSettings::deviceName() const {
    const QString chosen = settings_.value(kName).toString().trimmed();
    return chosen.isEmpty() ? u"Nexa app on %1"_s.arg(modelName()) : chosen;
}

void PhoneSettings::save(const RemoteEndpoint &endpoint, const QString &token, const QString &fingerprint) {
    settings_.setValue(kHost, endpoint.host);
    settings_.setValue(kPort, endpoint.port);
    settings_.setValue(kTls, endpoint.tls);
    settings_.setValue(kToken, token);
    settings_.setValue(kFingerprint, fingerprint);
    settings_.sync();
}

void PhoneSettings::setDeviceName(const QString &name) {
    settings_.setValue(kName, name.trimmed());
}

void PhoneSettings::forgetToken() {
    settings_.remove(kToken);
    settings_.sync();
}

} // namespace nexa
