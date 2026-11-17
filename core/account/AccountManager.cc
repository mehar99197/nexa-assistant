#include "core/account/AccountManager.h"

#include "core/Logging.h"
#include "core/account/CredentialStore.h"
#include "core/account/Guard.h"
#include "core/account/KeyIntegrity.h"
#include "core/account/LicenseToken.h"
#include "core/config/Portable.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QEventLoop>
#include <QHostInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QNetworkAccessManager>
#include <QNetworkInterface>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSettings>
#include <QSysInfo>
#include <QTimeZone>
#include <QTimer>
#include <QUrl>
#include <QUuid>

#include <algorithm>
#include <array>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

constexpr auto kProduct = "assistant";
constexpr auto kDeviceSeed = "account/deviceSeed";
constexpr auto kCachedToken = "account/cachedToken";
constexpr auto kCachedTrial = "account/cachedTrial";
constexpr auto kAccountEmail = "account/email";
constexpr auto kAccountId = "account/id";

constexpr int kSignInPollMs = 5'000;
constexpr int kRevalidateMs = 6 * 60 * 60 * 1000;
constexpr int kHeartbeatMs = 5 * 60 * 1000;
constexpr int kClockSkewToleranceSecs = 30 * 60;
constexpr int kEntitlementRecheckMs = 60 * 1000;

constexpr auto kProductionLicenseApi = "https://nexadownloadmanager.com/api/license/validate";

QString planLabel(const QString &plan) {
    return plan.toUpper();
}

QUrl licenseEndpoint(const QString &action) {
#ifdef NEXA_DEV_BUILD
    const QUrl base(qEnvironmentVariable("NEXA_LICENSE_API_URL", QString::fromLatin1(kProductionLicenseApi)));
#else
    const QUrl base{QLatin1String(kProductionLicenseApi)};
#endif
    if (action == u"validate"_s) return base;
    QUrl url(base);
    QString path = base.path();
    const qsizetype slash = path.lastIndexOf(u'/');
    if (slash >= 0) path.truncate(slash + 1);
    url.setPath(path + action);
    return url;
}

bool endpointAllowed(const QUrl &endpoint) {
#ifdef NEXA_DEV_BUILD
    const bool insecureDevelopment = qEnvironmentVariableIntValue("NEXA_ALLOW_INSECURE_LICENSE_API") == 1
        && (endpoint.host() == u"localhost"_s || endpoint.host() == u"127.0.0.1"_s);
#else
    constexpr bool insecureDevelopment = false;
#endif
    return endpoint.isValid() && (endpoint.scheme() == u"https"_s || insecureDevelopment);
}

int validateRetryDelayMs(int attempt) {
    static constexpr std::array<int, 6> kStepsMinutes{1, 2, 5, 10, 20, 30};
    int unitMs = 60 * 1000;
#ifdef NEXA_DEV_BUILD
    bool ok = false;
    const int unit = qEnvironmentVariableIntValue("NEXA_LICENSE_RETRY_UNIT_MS", &ok);
    if (ok && unit > 0) unitMs = unit;
#endif
    return kStepsMinutes[std::clamp(attempt, 0, int(kStepsMinutes.size()) - 1)] * unitMs;
}

QUrl deviceEndpoint(const QString &action) {
    const QUrl base = licenseEndpoint(u"validate"_s);
    QUrl url(base);
    QString path = base.path();
    qsizetype slash = path.lastIndexOf(u'/');
    if (slash >= 0) path.truncate(slash);
    slash = path.lastIndexOf(u'/');
    if (slash >= 0) path.truncate(slash + 1);
    url.setPath(path + u"device/"_s + action);
    return url;
}

QNetworkRequest jsonRequest(const QUrl &endpoint, int timeoutMs = 15'000, const QString &bearer = {}) {
    QNetworkRequest request(endpoint);
    request.setHeader(QNetworkRequest::ContentTypeHeader, u"application/json"_s);
    const QString version = QCoreApplication::applicationVersion().isEmpty()
        ? u"1.0.0"_s : QCoreApplication::applicationVersion();
    request.setHeader(QNetworkRequest::UserAgentHeader, u"NexaAssistant/%1"_s.arg(version));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(timeoutMs);
    if (!bearer.isEmpty()) request.setRawHeader("Authorization", "Bearer " + bearer.toUtf8());
    return request;
}

QJsonObject envelopeData(const QByteArray &response) {
    const QJsonObject envelope = QJsonDocument::fromJson(response).object();
    if (!envelope.value(u"ok"_s).toBool()) return {};
    return envelope.value(u"data"_s).toObject();
}

QString envelopeError(const QByteArray &response) {
    return QJsonDocument::fromJson(response).object().value(u"error"_s).toObject().value(u"message"_s).toString();
}

int parseRetryAfterSecs(const QNetworkReply *reply) {
    const QByteArray raw = reply->rawHeader("Retry-After").trimmed();
    if (raw.isEmpty()) return 0;
    bool ok = false;
    const int secs = raw.toInt(&ok);
    if (ok && secs > 0) return secs;
    const QDateTime when = QDateTime::fromString(QString::fromLatin1(raw), Qt::RFC2822Date);
    if (when.isValid()) return int(std::max<qint64>(1, QDateTime::currentDateTimeUtc().secsTo(when)));
    return 0;
}

QByteArray primaryMacAddress() {
    QString best;
    for (const QNetworkInterface &iface : QNetworkInterface::allInterfaces()) {
        const QNetworkInterface::InterfaceFlags flags = iface.flags();
        if (flags & QNetworkInterface::IsLoopBack) continue;
        if (!(flags & QNetworkInterface::IsUp)) continue;
        const QNetworkInterface::InterfaceType type = iface.type();
        if (type != QNetworkInterface::Ethernet && type != QNetworkInterface::Wifi) continue;
        const QString hw = iface.hardwareAddress().toLower();
        if (hw.isEmpty() || hw == u"00:00:00:00:00:00"_s) continue;
        if (AccountManager::isVirtualMac(hw)) continue;
        if (best.isEmpty() || hw < best) best = hw;
    }
    return best.toLatin1();
}

QByteArray stableMachineUniqueId() {
    const QByteArray id = QSysInfo::machineUniqueId().trimmed();
    if (id.isEmpty()) return {};
    if (id == QByteArray(32, '0')) return {};
    const QByteArray lower = id.toLower();
    if (lower == "uninitialized" || lower == "unknown") return {};
    return id;
}

} // namespace

bool AccountManager::isVirtualMac(const QString &mac) {
    const QString hw = mac.trimmed().toLower();
    static constexpr std::array<const char *, 10> kVirtualOuis = {
        "00:15:5d", // Hyper-V / WSL
        "00:0c:29", // VMware
        "00:05:69", // VMware
        "00:1c:14", // VMware
        "00:50:56", // VMware
        "08:00:27", // VirtualBox
        "52:54:00", // QEMU / KVM / libvirt
        "00:16:3e", // Xen
        "00:1c:42", // Parallels
        "00:03:ff", // Microsoft loopback
    };
    for (const char *oui : kVirtualOuis) {
        if (hw.startsWith(QLatin1String(oui))) return true;
    }
    bool ok = false;
    const unsigned int firstOctet = hw.left(2).toUInt(&ok, 16);
    return ok && ((firstOctet & 0x02u) != 0);
}

QString AccountManager::deviceFingerprint(const QString &settingsPath) {
    QByteArray identity = stableMachineUniqueId();
    if (identity.isEmpty()) identity = primaryMacAddress();
    if (identity.isEmpty()) {
        const QString path = settingsPath.isEmpty() ? (portable::configDir() + u"/account.ini"_s) : settingsPath;
        auto settings = std::make_unique<QSettings>(path, QSettings::IniFormat);
        identity = settings->value(QLatin1String(kDeviceSeed)).toByteArray();
        if (identity.isEmpty()) {
            identity = QUuid::createUuid().toByteArray(QUuid::WithoutBraces);
            settings->setValue(QLatin1String(kDeviceSeed), identity);
        }
    }
    return QString::fromLatin1(
        QCryptographicHash::hash("NexaAssistant/license/v3/" + identity, QCryptographicHash::Sha256).toHex());
}

QString AccountManager::deviceName() {
    const QString host = QHostInfo::localHostName().trimmed();
    return host.isEmpty() ? QSysInfo::prettyProductName() : host;
}

QUrl AccountManager::apiRoot() {
    const QUrl validate = licenseEndpoint(u"validate"_s);
    QUrl root(validate);
    QString path = validate.path();
    const qsizetype licenseIdx = path.indexOf(u"/license"_s);
    if (licenseIdx >= 0) path.truncate(licenseIdx);
    root.setPath(path);
    return root;
}

AccountManager::AccountManager(QString settingsPath, QObject *parent)
    : QObject(parent), settingsPath_(std::move(settingsPath)), network_(new QNetworkAccessManager(this)) {
    revalidate_ = new QTimer(this);
    revalidate_->setInterval(kRevalidateMs);
    connect(revalidate_, &QTimer::timeout, this, [this] {
        if ((!licenseKey_.isEmpty() || !accountToken_.isEmpty()) && !reply_) validate(licenseKey_, false);
    });
    revalidate_->start();

    validateRetry_ = new QTimer(this);
    validateRetry_->setSingleShot(true);
    connect(validateRetry_, &QTimer::timeout, this, [this] {
        if (reply_) return;
        if (!retryKey_.isEmpty() || !accountToken_.isEmpty()) validate(retryKey_, false);
    });

    heartbeat_ = new QTimer(this);
    heartbeat_->setInterval(kHeartbeatMs);
    connect(heartbeat_, &QTimer::timeout, this, &AccountManager::sendHeartbeat);

    signInPoll_ = new QTimer(this);
    signInPoll_->setInterval(kSignInPollMs);
    connect(signInPoll_, &QTimer::timeout, this, &AccountManager::pollSignIn);

    recheck_ = new QTimer(this);
    recheck_->setInterval(kEntitlementRecheckMs);
    connect(recheck_, &QTimer::timeout, this, &AccountManager::recheckEntitlements);
    recheck_->start();
}

AccountManager::~AccountManager() = default;

std::unique_ptr<QSettings> AccountManager::openSettings() const {
    const QString path = settingsPath_.isEmpty() ? (portable::configDir() + u"/account.ini"_s) : settingsPath_;
    return std::make_unique<QSettings>(path, QSettings::IniFormat);
}

QString AccountManager::fingerprint() const {
    return deviceFingerprint(settingsPath_);
}

QString AccountManager::nexaAiBaseUrl() const {
    QUrl root = apiRoot();
    QString path = root.path();
    while (path.endsWith(u'/')) path.chop(1);
    root.setPath(path + u"/assistant/v1"_s);
    return root.toString();
}

void AccountManager::start() {
    auto settings = openSettings();
    accountToken_ = credentialstore::readAccountToken().trimmed();
    if (!accountToken_.isEmpty()) {
        accountEmail_ = settings->value(QLatin1String(kAccountEmail)).toString().trimmed();
        accountId_ = settings->value(QLatin1String(kAccountId)).toString().trimmed();
        emit accountChanged(accountEmail_);
    } else {
        licenseKey_ = credentialstore::readLicenseKey().trimmed();
    }

    if (accountToken_.isEmpty() && licenseKey_.isEmpty()) {
        setFeaturesForPlan(u"free"_s);
        setPlan(u"free"_s, tr("Free plan"));
        return;
    }

    if (!deriveFromToken(&plan_, &features_)) {
        plan_ = u"free"_s;
        features_ = Entitlements{};
    } else {
        trial_ = settings->value(QLatin1String(kCachedTrial), false).toBool();
        emit featuresChanged(features_);
        if (isPaidPlan(plan_)) {
            setPlan(plan_, u"%1 active (verifying…)"_s.arg(planLabel(plan_)));
        }
    }

    validate(accountToken_.isEmpty() ? licenseKey_ : QString(), false);
}

void AccountManager::activate(const QString &licenseKey) {
    const QString key = licenseKey.trimmed().toUpper();
    if (key.isEmpty()) {
        emit activationFinished(false, tr("Enter a license key."));
        return;
    }
    static const QRegularExpression keyRe(u"^NDM(-[A-Z0-9]{4}){3}$"_s);
    if (!keyRe.match(key).hasMatch()) {
        emit activationFinished(false, tr("License keys look like NDM-XXXX-XXXX-XXXX."));
        return;
    }
    if (!accountToken_.isEmpty()) signOut();
    validate(key, true);
}

void AccountManager::deactivate() {
    if (!accountToken_.isEmpty()) {
        credentialstore::removeLicenseKey();
        licenseKey_.clear();
        emit activationFinished(true, tr("License key removed"));
        return;
    }
    if (reply_) {
        reply_->disconnect(this);
        reply_->abort();
        reply_->deleteLater();
        reply_ = nullptr;
    }
    cancelValidateRetry();
    releaseSeat();
    credentialstore::removeLicenseKey();
    clearCache();
    licenseKey_.clear();
    licenseToken_.clear();
    trial_ = false;
    expires_ = {};
    activeSeats_ = 0;
    usage_ = {};
    emit usageChanged(usage_);
    if (heartbeat_) heartbeat_->stop();
    setFeaturesForPlan(u"free"_s);
    setPlan(u"free"_s, tr("Free plan"));
    emit activationFinished(true, tr("License removed"));
}

void AccountManager::validate(const QString &licenseKey, bool userInitiated) {
    const bool accountMode = licenseKey.isEmpty() && !accountToken_.isEmpty();
    if (!accountMode && licenseKey.isEmpty()) return;

    if (reply_) {
        reply_->disconnect(this);
        reply_->abort();
        reply_->deleteLater();
        reply_ = nullptr;
    }
    cancelValidateRetry();

    const QUrl endpoint = licenseEndpoint(u"validate"_s);
    if (!endpointAllowed(endpoint)) {
        const QString reason = tr("License endpoint must use HTTPS.");
        applyCachedEntitlement(reason, userInitiated);
        return;
    }

    QJsonObject payload{
        {u"product"_s, QLatin1String(kProduct)},
        {u"device_fingerprint"_s, fingerprint()},
        {u"device_name"_s, deviceName()},
    };
    const QString appVer = QCoreApplication::applicationVersion();
    if (!appVer.isEmpty()) payload.insert(u"app_version"_s, appVer);
    if (accountMode) {
        payload.insert(u"device_token"_s, accountToken_);
    } else {
        payload.insert(u"license_key"_s, licenseKey);
    }

    const QByteArray body = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    reply_ = network_->post(jsonRequest(endpoint), body);
    connect(reply_, &QNetworkReply::finished, this, [this, licenseKey, accountMode, userInitiated] {
        QNetworkReply *reply = reply_;
        reply_ = nullptr;
        if (!reply) return;

        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const int retryAfterSecs = parseRetryAfterSecs(reply);
        const bool transportOk = reply->error() == QNetworkReply::NoError && status == 200;
        const QByteArray response = reply->readAll();
        reply->deleteLater();

        if (!transportOk) {
            applyCachedEntitlement(tr("Cannot reach license server (%1)").arg(status > 0 ? QString::number(status) : tr("offline")),
                                   userInitiated);
            if (status == 0 || status == 408 || status == 429 || status >= 500) {
                scheduleValidateRetry(licenseKey, retryAfterSecs);
            }
            return;
        }

        validateRetryAttempt_ = 0;
        retryKey_.clear();
        if (response.size() > 64 * 1024) {
            const QString msg = tr("License server returned an invalid response.");
            setPlan(u"free"_s, msg);
            if (userInitiated) emit activationFinished(false, msg);
            return;
        }

        QJsonParseError parseError{};
        const QJsonDocument doc = QJsonDocument::fromJson(response, &parseError);
        const QJsonObject object = doc.object();
        if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
            const QString msg = tr("License server returned an invalid response.");
            setPlan(u"free"_s, msg);
            if (userInitiated) emit activationFinished(false, msg);
            return;
        }

        if (!object.value(u"valid"_s).toBool()) {
            const QString reason = object.value(u"reason"_s).toString(u"invalid"_s);
            if (reason == u"seat_limit"_s) {
                const int seats = object.value(u"seats"_s).toInt(1);
                if (!accountMode) licenseKey_ = licenseKey;
                licenseToken_.clear();
                heartbeat_->stop();
                setFeaturesForPlan(u"free"_s);
                const QString msg = tr("All %n seat(s) on this license are in use on other devices.", nullptr, seats);
                setPlan(u"free"_s, msg);
                emit seatLimitReached(seats);
                if (userInitiated) emit activationFinished(false, msg);
                return;
            }
            if (accountMode) {
                licenseToken_.clear();
                trial_ = false;
                expires_ = {};
                heartbeat_->stop();
                clearCache();
                setFeaturesForPlan(u"free"_s);
                QString msg;
                if (reason == u"signed_out"_s) {
                    forgetAccount();
                    msg = tr("Signed out — sign in again in Settings.");
                } else {
                    msg = accountEmail_.isEmpty()
                        ? tr("Free plan (%1)").arg(reason)
                        : tr("Signed in as %1 — Free plan (%2)").arg(accountEmail_, reason);
                }
                setPlan(u"free"_s, msg);
                if (userInitiated) emit activationFinished(false, msg);
                return;
            }
            credentialstore::removeLicenseKey();
            clearCache();
            licenseKey_.clear();
            licenseToken_.clear();
            trial_ = false;
            expires_ = {};
            heartbeat_->stop();
            setFeaturesForPlan(u"free"_s);
            const QString msg = tr("License rejected: %1").arg(reason);
            setPlan(u"free"_s, msg);
            if (userInitiated) emit activationFinished(false, msg);
            return;
        }

        const QString token = object.value(u"token"_s).toString();
        licensetoken::Claims claims;
        QString tokenError;
        if (!licensetoken::verify(token, &claims, &tokenError)) {
            const QString msg = tr("License response could not be verified.");
            setFeaturesForPlan(u"free"_s);
            setPlan(u"free"_s, msg);
            if (userInitiated) emit activationFinished(false, msg);
            return;
        }
        if (claims.device != fingerprint()) {
            const QString msg = tr("License token was issued to a different device.");
            setFeaturesForPlan(u"free"_s);
            setPlan(u"free"_s, msg);
            if (userInitiated) emit activationFinished(false, msg);
            return;
        }
        const QString subjectError = accountMode
            ? (claims.account.isEmpty()
                   ? tr("License token is not bound to an account.")
                   : ((!accountId_.isEmpty() && claims.account != accountId_)
                          ? tr("License token was issued to a different account.")
                          : QString()))
            : ((!claims.licenseKey.isEmpty() && claims.licenseKey != licenseKey)
                   ? tr("License token does not match this key.")
                   : QString());
        if (!subjectError.isEmpty()) {
            setFeaturesForPlan(u"free"_s);
            setPlan(u"free"_s, subjectError);
            if (userInitiated) emit activationFinished(false, subjectError);
            return;
        }
        if (claims.expiresAt.addSecs(kClockSkewToleranceSecs) <= QDateTime::currentDateTimeUtc()) {
            const QString msg = tr("License token has already expired.");
            setFeaturesForPlan(u"free"_s);
            setPlan(u"free"_s, msg);
            if (userInitiated) emit activationFinished(false, msg);
            return;
        }

        const QString plan = claims.plan;
        auto settings = openSettings();
        if (accountMode) {
            accountId_ = claims.account;
            settings->setValue(QLatin1String(kAccountId), accountId_);
            const QString email = object.value(u"account"_s).toObject().value(u"email"_s).toString();
            if (!email.isEmpty() && email != accountEmail_) {
                accountEmail_ = email;
                settings->setValue(QLatin1String(kAccountEmail), email);
                emit accountChanged(accountEmail_);
            }
        } else {
            credentialstore::writeLicenseKey(licenseKey);
            licenseKey_ = licenseKey;
        }

        licenseToken_ = token;
        trial_ = object.value(u"trial"_s).toBool(false);
        expires_ = QDateTime::fromString(object.value(u"expires"_s).toString(), Qt::ISODate);
        activeSeats_ = object.value(u"activeSeats"_s).toInt(activeSeats_);
        applyFeatures(claims.features, plan);
        cacheEntitlement(token, trial_);
        if (!heartbeat_->isActive()) heartbeat_->start();

        QString message;
        if (trial_ && expires_.isValid()) {
            const qint64 hoursLeft = QDateTime::currentDateTimeUtc().secsTo(expires_) / 3600;
            const qint64 daysLeft = std::max<qint64>(0, (hoursLeft + 23) / 24);
            message = tr("%1 trial — %2 day%3 left")
                          .arg(planLabel(plan))
                          .arg(daysLeft)
                          .arg(daysLeft == 1 ? QString() : u"s"_s);
        } else if (plan == u"free"_s) {
            message = accountMode && !accountEmail_.isEmpty()
                ? tr("Signed in as %1 · Free plan").arg(accountEmail_)
                : tr("Free plan");
        } else {
            message = accountMode && !accountEmail_.isEmpty()
                ? tr("Signed in as %1 · %2 active").arg(accountEmail_, planLabel(plan))
                : tr("%1 license active").arg(planLabel(plan));
        }
        setPlan(plan, message);
        if (userInitiated) emit activationFinished(true, message);
        fetchUsage();
    });
}

void AccountManager::scheduleValidateRetry(const QString &licenseKey, int retryAfterSecs) {
    retryKey_ = licenseKey;
    const int backoffMs = validateRetryDelayMs(validateRetryAttempt_);
    const int delayMs = std::max(backoffMs, retryAfterSecs * 1000);
    ++validateRetryAttempt_;
    validateRetry_->start(delayMs);
}

void AccountManager::cancelValidateRetry() {
    if (validateRetry_) validateRetry_->stop();
    validateRetryAttempt_ = 0;
    retryKey_.clear();
}

void AccountManager::setPlan(const QString &plan, const QString &status) {
    const bool changed = plan_ != plan;
    plan_ = plan;
    status_ = status;
    if (changed) emit entitlementChanged(plan_);
    emit statusChanged(status_);
}

void AccountManager::clearCache() {
    auto settings = openSettings();
    settings->remove(QLatin1String(kCachedToken));
    settings->remove(QLatin1String(kCachedTrial));
}

void AccountManager::cacheEntitlement(const QString &token, bool trial) {
    auto settings = openSettings();
    settings->setValue(QLatin1String(kCachedToken), token);
    settings->setValue(QLatin1String(kCachedTrial), trial);
}

void AccountManager::applyCachedEntitlement(const QString &offlineReason, bool userInitiated) {
    auto settings = openSettings();
    const QString stored = settings->value(QLatin1String(kCachedToken)).toString();
    licensetoken::Claims claims;
    const QDateTime now = QDateTime::currentDateTimeUtc();
    if (!stored.isEmpty() && licensetoken::verify(stored, &claims)
        && licensetoken::offlineGraceAllows(claims, fingerprint(), now, kOfflineGraceDays)) {
        trial_ = settings->value(QLatin1String(kCachedTrial), false).toBool();
        applyFeatures(claims.features, claims.plan);
        const QString msg = tr("%1 (offline grace — %2)").arg(planLabel(claims.plan), offlineReason);
        setPlan(claims.plan, msg);
        if (userInitiated) emit activationFinished(true, msg);
        return;
    }
    setFeaturesForPlan(u"free"_s);
    setPlan(u"free"_s, offlineReason);
    if (userInitiated) emit activationFinished(false, offlineReason);
}

void AccountManager::applyFeatures(const QJsonObject &features, const QString &plan) {
    features_ = Entitlements::fromClaims(plan, features);
    emit featuresChanged(features_);
}

void AccountManager::setFeaturesForPlan(const QString &plan) {
    features_ = Entitlements::forPlan(plan);
    emit featuresChanged(features_);
}

bool AccountManager::deriveFromToken(QString *plan, Entitlements *features) const {
    if (!licensetoken::publicKeyIntact()) return false;
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const QString fp = fingerprint();

    const auto accept = [&](const licensetoken::Claims &claims) {
        if (plan) *plan = claims.plan;
        if (features) *features = Entitlements::fromClaims(claims.plan, claims.features);
        return true;
    };

    licensetoken::Claims live;
    if (!licenseToken_.isEmpty() && licensetoken::verify(licenseToken_, &live)
        && live.device == fp && live.expiresAt.addSecs(kClockSkewToleranceSecs) > now) {
        return accept(live);
    }

    auto settings = openSettings();
    const QString stored = settings->value(QLatin1String(kCachedToken)).toString();
    licensetoken::Claims cached;
    if (!stored.isEmpty() && licensetoken::verify(stored, &cached)
        && licensetoken::offlineGraceAllows(cached, fp, now, kOfflineGraceDays)) {
        return accept(cached);
    }
    return false;
}

bool AccountManager::isPaidPlan(const QString &plan) {
    return plan == u"pro"_s || plan == u"team"_s;
}

bool AccountManager::liveTokenGrantsPaid() const {
    if (licenseToken_.isEmpty()) return false;
    licensetoken::Claims c;
    if (!licensetoken::verify(licenseToken_, &c)) return false;
    if (c.device != fingerprint()) return false;
    if (c.expiresAt.addSecs(kClockSkewToleranceSecs) <= QDateTime::currentDateTimeUtc()) return false;
    return isPaidPlan(c.plan);
}

bool AccountManager::cachedTokenGrantsPaidInGrace() const {
    auto settings = openSettings();
    const QString stored = settings->value(QLatin1String(kCachedToken)).toString();
    if (stored.isEmpty()) return false;
    licensetoken::Claims c;
    if (!licensetoken::verify(stored, &c)) return false;
    return licensetoken::offlineGraceAllows(c, fingerprint(), QDateTime::currentDateTimeUtc(), kOfflineGraceDays);
}

bool AccountManager::hasSignedPaidGrant() const {
    const QString fp = fingerprint();
    const auto grantsPaid = [&fp](const QString &token) {
        if (token.isEmpty()) return false;
        licensetoken::Claims c;
        if (!licensetoken::verify(token, &c)) return false;
        return c.device == fp && isPaidPlan(c.plan);
    };
    if (grantsPaid(licenseToken_)) return true;
    auto settings = openSettings();
    return grantsPaid(settings->value(QLatin1String(kCachedToken)).toString());
}

Entitlements AccountManager::verifiedFeatures() const {
    QString plan;
    Entitlements derived;
    const bool derivedOk = deriveFromToken(&plan, &derived);
    const bool derivedPaid = derivedOk && isPaidPlan(plan);

    const guard::Checks checks = {
        [] { return licensetoken::publicKeyIntact(); },
        [this] { return liveTokenGrantsPaid(); },
        [this] { return cachedTokenGrantsPaidInGrace(); },
        [this] { return isPaidPlan(plan_); },
        [this] { return hasSignedPaidGrant(); },
    };
    const guard::Result g = guard::evaluate(checks);
    if (g.tamper) tamperObserved_ = true;

    if (derivedPaid && g.paid && !tamperObserved_) return derived;
    if (derivedOk && !derivedPaid) return derived;
    return Entitlements{};
}

void AccountManager::recheckEntitlements() {
    QString plan = u"free"_s;
    Entitlements ignore;
    deriveFromToken(&plan, &ignore);
    const Entitlements derived = verifiedFeatures();
    if (tamperObserved_) plan = u"free"_s;
    if (derived == features_ && plan == plan_) return;
    features_ = derived;
    emit featuresChanged(features_);
    if (plan != plan_ && !isPaidPlan(plan)) setPlan(plan, tr("Free plan"));
}

void AccountManager::sendHeartbeat() {
    if ((licenseKey_.isEmpty() && accountToken_.isEmpty()) || heartbeatReply_) return;
    const QUrl endpoint = licenseEndpoint(u"heartbeat"_s);
    if (!endpointAllowed(endpoint)) return;

    QJsonObject payload{
        {u"product"_s, QLatin1String(kProduct)},
        {u"device_fingerprint"_s, fingerprint()},
        {u"device_name"_s, deviceName()},
    };
    if (accountToken_.isEmpty()) payload.insert(u"license_key"_s, licenseKey_);
    else payload.insert(u"device_token"_s, accountToken_);

    lastBeat_.restart();
    heartbeatReply_ = network_->post(jsonRequest(endpoint, 10'000), QJsonDocument(payload).toJson(QJsonDocument::Compact));
    connect(heartbeatReply_, &QNetworkReply::finished, this, [this] {
        QNetworkReply *reply = heartbeatReply_;
        heartbeatReply_ = nullptr;
        if (!reply) return;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = reply->readAll();
        const QNetworkReply::NetworkError err = reply->error();
        reply->deleteLater();
        if (err != QNetworkReply::NoError || status != 200) return;

        const QJsonObject obj = QJsonDocument::fromJson(body).object();
        if (!obj.value(u"valid"_s).toBool(true)) {
            const QString reason = obj.value(u"reason"_s).toString();
            heartbeat_->stop();
            licenseToken_.clear();
            clearCache();
            setFeaturesForPlan(u"free"_s);
            if (reason == u"signed_out"_s) {
                forgetAccount();
                setPlan(u"free"_s, tr("Signed out — sign in again in Settings."));
            } else {
                setPlan(u"free"_s, tr("Free plan"));
                emit seatRevoked();
            }
            return;
        }
        adoptRefreshedToken(obj.value(u"token"_s).toString());
    });
}

void AccountManager::adoptRefreshedToken(const QString &token) {
    if (token.isEmpty()) return;
    licensetoken::Claims claims;
    if (!licensetoken::verify(token, &claims)) return;
    if (claims.device != fingerprint()) return;
    if (!licenseKey_.isEmpty() && !claims.licenseKey.isEmpty() && claims.licenseKey != licenseKey_) return;
    if (!accountToken_.isEmpty()
        && (claims.account.isEmpty() || (!accountId_.isEmpty() && claims.account != accountId_))) {
        return;
    }

    licenseToken_ = token;
    cacheEntitlement(token, trial_);
    if (claims.plan != plan_) {
        applyFeatures(claims.features, claims.plan);
        QString msg = claims.plan == u"free"_s ? tr("Free plan")
                                               : tr("%1 active").arg(planLabel(claims.plan));
        if (!accountToken_.isEmpty() && !accountEmail_.isEmpty()) {
            msg = tr("Signed in as %1 · %2").arg(accountEmail_, msg);
        }
        setPlan(claims.plan, msg);
    } else {
        recheckEntitlements();
    }
}

void AccountManager::releaseSeat() {
    if (heartbeat_) heartbeat_->stop();
    if (licenseKey_.isEmpty() && accountToken_.isEmpty()) return;
    const QUrl endpoint = licenseEndpoint(u"release"_s);
    if (!endpointAllowed(endpoint)) return;

    QJsonObject payload{
        {u"product"_s, QLatin1String(kProduct)},
        {u"device_fingerprint"_s, fingerprint()},
    };
    if (accountToken_.isEmpty()) payload.insert(u"license_key"_s, licenseKey_);
    else payload.insert(u"device_token"_s, accountToken_);

    QNetworkReply *reply = network_->post(jsonRequest(endpoint, 3'000), QJsonDocument(payload).toJson(QJsonDocument::Compact));
    QEventLoop loop;
    QTimer guard;
    guard.setSingleShot(true);
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    connect(&guard, &QTimer::timeout, &loop, &QEventLoop::quit);
    guard.start(3'000);
    loop.exec(QEventLoop::ExcludeUserInputEvents);
    if (reply->isRunning()) reply->abort();
    reply->deleteLater();
}

void AccountManager::beginSignIn() {
    if (signInReply_ || signInPoll_->isActive()) return;
    const QUrl endpoint = deviceEndpoint(u"code"_s);
    if (!endpointAllowed(endpoint)) {
        emit signInFinished(false, tr("Signing in needs a secure HTTPS connection."));
        return;
    }

    QJsonObject request{
        {u"product"_s, QLatin1String(kProduct)},
        {u"device_fingerprint"_s, fingerprint()},
        {u"device_name"_s, deviceName()},
    };
    if (!QCoreApplication::applicationVersion().isEmpty()) {
        request.insert(u"app_version"_s, QCoreApplication::applicationVersion());
    }

    signInReply_ = network_->post(jsonRequest(endpoint), QJsonDocument(request).toJson(QJsonDocument::Compact));
    connect(signInReply_, &QNetworkReply::finished, this, [this] {
        QNetworkReply *reply = signInReply_;
        signInReply_ = nullptr;
        if (!reply) return;
        const bool ok = reply->error() == QNetworkReply::NoError;
        const QByteArray response = reply->readAll();
        reply->deleteLater();
        if (!ok || response.size() > 64 * 1024) {
            emit signInFinished(false, tr("Could not reach nexadownloadmanager.com. Check your connection."));
            return;
        }
        const QJsonObject data = envelopeData(response);
        const QString deviceCode = data.value(u"deviceCode"_s).toString();
        const QString userCode = data.value(u"userCode"_s).toString();
        const QString url = data.value(u"verificationUrlComplete"_s)
                                .toString(data.value(u"verificationUrl"_s).toString());
        if (deviceCode.isEmpty() || userCode.isEmpty() || url.isEmpty()) {
            const QString err = envelopeError(response);
            emit signInFinished(false, err.isEmpty() ? tr("Could not start sign-in.") : err);
            return;
        }
        deviceCode_ = deviceCode;
        userCode_ = userCode;
        verificationUrl_ = url;
        signInExpires_ = QDateTime::currentDateTimeUtc().addSecs(std::max(30, data.value(u"expiresIn"_s).toInt(600)));
        signInPoll_->setInterval(std::clamp(data.value(u"interval"_s).toInt(5), 1, 60) * 1000);
        signInPoll_->start();
        emit signInCodeReady(userCode_, url);
    });
}

void AccountManager::pollSignIn() {
    if (deviceCode_.isEmpty() || signInReply_) return;
    if (signInExpires_.isValid() && QDateTime::currentDateTimeUtc() >= signInExpires_) {
        endSignIn(false, tr("That code expired — start sign-in again."));
        return;
    }
    const QUrl endpoint = deviceEndpoint(u"token"_s);
    if (!endpointAllowed(endpoint)) {
        endSignIn(false, tr("Signing in needs a secure HTTPS connection."));
        return;
    }

    const QByteArray body = QJsonDocument(QJsonObject{
        {u"device_code"_s, deviceCode_},
        {u"device_fingerprint"_s, fingerprint()},
    }).toJson(QJsonDocument::Compact);

    signInReply_ = network_->post(jsonRequest(endpoint), body);
    connect(signInReply_, &QNetworkReply::finished, this, [this] {
        QNetworkReply *reply = signInReply_;
        signInReply_ = nullptr;
        if (!reply) return;
        const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QNetworkReply::NetworkError netErr = reply->error();
        const QByteArray response = reply->readAll();
        reply->deleteLater();
        if (netErr != QNetworkReply::NoError && httpStatus == 0) return;

        const QJsonObject data = envelopeData(response);
        const QString state = data.value(u"status"_s).toString();
        if (state == u"pending"_s || httpStatus == 429) return;
        if (state == u"slow_down"_s) {
            signInPoll_->setInterval(std::min(60'000, signInPoll_->interval() + 5'000));
            return;
        }
        if (state == u"denied"_s) {
            endSignIn(false, tr("Sign-in was denied in the browser."));
            return;
        }
        if (state == u"expired"_s) {
            endSignIn(false, tr("That code expired — start sign-in again."));
            return;
        }
        if (state != u"approved"_s) return;

        const QString token = data.value(u"deviceToken"_s).toString();
        if (token.isEmpty()) {
            endSignIn(false, tr("Server returned no device token."));
            return;
        }
        const QJsonObject account = data.value(u"account"_s).toObject();
        accountToken_ = token;
        accountEmail_ = account.value(u"email"_s).toString();
        const QJsonValue idVal = account.value(u"id"_s);
        accountId_ = idVal.isDouble() ? QString::number(qint64(idVal.toDouble())) : idVal.toString();

        const bool stored = credentialstore::writeAccountToken(accountToken_);
        auto settings = openSettings();
        settings->setValue(QLatin1String(kAccountEmail), accountEmail_);
        settings->setValue(QLatin1String(kAccountId), accountId_);
        credentialstore::removeLicenseKey();
        licenseKey_.clear();
        clearCache();
        licenseToken_.clear();
        emit accountChanged(accountEmail_);
        endSignIn(true, stored
            ? tr("Signed in as %1").arg(accountEmail_)
            : tr("Signed in as %1 (session only; credential store unavailable)").arg(accountEmail_));
        validate(QString(), false);
    });
}

void AccountManager::endSignIn(bool ok, const QString &message) {
    signInPoll_->stop();
    deviceCode_.clear();
    userCode_.clear();
    verificationUrl_.clear();
    signInExpires_ = {};
    emit signInFinished(ok, message);
}

void AccountManager::cancelSignIn() {
    if (signInReply_) {
        signInReply_->disconnect(this);
        signInReply_->abort();
        signInReply_->deleteLater();
        signInReply_ = nullptr;
    }
    if (deviceCode_.isEmpty() && !signInPoll_->isActive()) return;
    endSignIn(false, tr("Sign-in cancelled."));
}

void AccountManager::signOut() {
    if (accountToken_.isEmpty()) return;
    const QUrl endpoint = deviceEndpoint(u"signout"_s);
    if (endpointAllowed(endpoint)) {
        const QByteArray body = QJsonDocument(QJsonObject{
            {u"device_token"_s, accountToken_},
            {u"device_fingerprint"_s, fingerprint()},
        }).toJson(QJsonDocument::Compact);
        QNetworkReply *reply = network_->post(jsonRequest(endpoint, 5'000), body);
        QEventLoop loop;
        QTimer guard;
        guard.setSingleShot(true);
        connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        connect(&guard, &QTimer::timeout, &loop, &QEventLoop::quit);
        guard.start(5'000);
        loop.exec(QEventLoop::ExcludeUserInputEvents);
        if (reply->isRunning()) reply->abort();
        reply->deleteLater();
    }
    forgetAccount();
    setFeaturesForPlan(u"free"_s);
    setPlan(u"free"_s, tr("Free plan"));
}

void AccountManager::forgetAccount() {
    credentialstore::removeAccountToken();
    auto settings = openSettings();
    settings->remove(QLatin1String(kAccountEmail));
    settings->remove(QLatin1String(kAccountId));
    accountToken_.clear();
    accountId_.clear();
    accountEmail_.clear();
    licenseToken_.clear();
    trial_ = false;
    expires_ = {};
    activeSeats_ = 0;
    usage_ = {};
    emit usageChanged(usage_);
    if (heartbeat_) heartbeat_->stop();
    cancelValidateRetry();
    clearCache();
    emit accountChanged(QString());
}

void AccountManager::fetchUsage() {
    if (licenseToken_.isEmpty() || usageReply_) return;
    const QUrl url(nexaAiBaseUrl() + u"/usage"_s);
    if (!endpointAllowed(url)) return;

    usageReply_ = network_->get(jsonRequest(url, 10'000, licenseToken_));
    connect(usageReply_, &QNetworkReply::finished, this, [this] {
        QNetworkReply *reply = usageReply_;
        usageReply_ = nullptr;
        if (!reply) return;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = reply->readAll();
        const QNetworkReply::NetworkError err = reply->error();
        reply->deleteLater();
        if (err != QNetworkReply::NoError || status != 200) return;

        const QJsonObject obj = QJsonDocument::fromJson(body).object();
        usage_.known = true;
        usage_.enabled = obj.value(u"enabled"_s).toBool();
        usage_.plan = obj.value(u"plan"_s).toString(plan_);
        usage_.month = obj.value(u"month"_s).toString();
        usage_.used = std::max(0, obj.value(u"used"_s).toInt());
        usage_.limit = std::max(0, obj.value(u"limit"_s).toInt());
        usage_.remaining = std::max(0, obj.value(u"remaining"_s).toInt());
        usage_.resetsAt = QDateTime::fromString(obj.value(u"resetsAt"_s).toString(), Qt::ISODate);
        emit usageChanged(usage_);
    });
}

void AccountManager::noteUsageRemaining(int remaining) {
    if (remaining < 0) return;
    const int limit = usage_.limit > 0 ? usage_.limit : verifiedFeatures().nexaAiMonthlyRequests;
    usage_.known = true;
    usage_.enabled = limit > 0;
    usage_.plan = plan_;
    usage_.limit = limit;
    usage_.remaining = std::min(limit, remaining);
    usage_.used = std::max(0, limit - usage_.remaining);
    emit usageChanged(usage_);
}

} // namespace nexa
