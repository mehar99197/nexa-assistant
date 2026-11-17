#pragma once

#include "core/account/Entitlements.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QObject>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;
class QSettings;
class QTimer;

namespace nexa {

// Live monthly quota & usage reported by GET /api/assistant/v1/usage or X-Nexa-Remaining.
struct NexaAiUsage {
    bool known{false};
    bool enabled{false};
    QString plan;
    QString month;
    int used{0};
    int limit{0};
    int remaining{0};
    QDateTime resetsAt;

    bool operator==(const NexaAiUsage &) const = default;
};

// Account & license controller for Nexa Assistant (port of NDM's M29 security model).
// Pure core networking (no QtWidgets): sign-in uses OAuth device flow against /api/device/*,
// validation uses /api/license/* with product="assistant", entitlements derive solely from the
// Ed25519-signed token behind Guard + KeyIntegrity + 7-day offline grace.
class AccountManager : public QObject {
    Q_OBJECT
public:
    static constexpr int kOfflineGraceDays = 7;

    explicit AccountManager(QString settingsPath = {}, QObject *parent = nullptr);
    ~AccountManager() override;

    void start();
    void activate(const QString &licenseKey);
    void deactivate();

    // OAuth device sign-in flow (/api/device/*).
    [[nodiscard]] bool isSignedIn() const { return !accountToken_.isEmpty(); }
    [[nodiscard]] bool signInInProgress() const { return !deviceCode_.isEmpty(); }
    [[nodiscard]] QString accountEmail() const { return accountEmail_; }
    [[nodiscard]] QString signInCode() const { return userCode_; }
    [[nodiscard]] QString signInUrl() const { return verificationUrl_; }
    void beginSignIn();
    void cancelSignIn();
    void signOut();

    [[nodiscard]] QString plan() const { return plan_; }
    [[nodiscard]] QString status() const { return status_; }
    [[nodiscard]] bool isPaid() const { return isPaidPlan(plan_); }
    [[nodiscard]] bool isTrial() const { return trial_; }
    [[nodiscard]] QDateTime expiresAt() const { return expires_; }
    [[nodiscard]] QString licenseToken() const { return licenseToken_; }
    [[nodiscard]] int seats() const { return features_.seats; }
    [[nodiscard]] int activeSeats() const { return activeSeats_; }
    [[nodiscard]] bool hasLicenseKey() const { return !licenseKey_.isEmpty(); }
    [[nodiscard]] bool tamperObserved() const { return tamperObserved_; }

    // Cached entitlements (refreshed every 60 s from the signed token).
    [[nodiscard]] const Entitlements &features() const { return features_; }
    // Expensive gate: re-verifies Ed25519 signature + KeyIntegrity + Guard canary now.
    [[nodiscard]] Entitlements verifiedFeatures() const;

    // Best-effort synchronous seat release on app quit (bounded 3 s timeout).
    void releaseSeat();

    // Nexa AI relay endpoint & monthly usage tracking.
    [[nodiscard]] QString nexaAiBaseUrl() const;
    [[nodiscard]] const NexaAiUsage &aiUsage() const { return usage_; }
    void fetchUsage();
    void noteUsageRemaining(int remaining);

    // Hardware fingerprint v3 (pure / deterministic) + short machine label.
    [[nodiscard]] static bool isVirtualMac(const QString &mac);
    [[nodiscard]] static QString deviceFingerprint(const QString &settingsPath = {});
    [[nodiscard]] static QString deviceName();
    // Base API root (https://nexadownloadmanager.com/api, or $NEXA_LICENSE_API_URL origin in NEXA_DEV_BUILD).
    [[nodiscard]] static QUrl apiRoot();

signals:
    void entitlementChanged(const QString &plan);
    void statusChanged(const QString &status);
    void activationFinished(bool valid, const QString &message);
    void seatLimitReached(int seats);
    void seatRevoked();
    void featuresChanged(const Entitlements &features);
    void signInCodeReady(const QString &userCode, const QString &verificationUrl);
    void signInFinished(bool ok, const QString &message);
    void accountChanged(const QString &email);
    void usageChanged(const NexaAiUsage &usage);

private:
    std::unique_ptr<QSettings> openSettings() const;
    QString fingerprint() const;
    void validate(const QString &licenseKey, bool userInitiated);
    void scheduleValidateRetry(const QString &licenseKey, int retryAfterSecs);
    void cancelValidateRetry();
    void setPlan(const QString &plan, const QString &status);
    void clearCache();
    void pollSignIn();
    void endSignIn(bool ok, const QString &message);
    void forgetAccount();
    void cacheEntitlement(const QString &token, bool trial);
    void applyCachedEntitlement(const QString &offlineReason, bool userInitiated);
    void applyFeatures(const QJsonObject &features, const QString &plan);
    void setFeaturesForPlan(const QString &plan);
    void sendHeartbeat();
    void adoptRefreshedToken(const QString &token);
    bool deriveFromToken(QString *plan, Entitlements *features) const;

    static bool isPaidPlan(const QString &plan);
    bool liveTokenGrantsPaid() const;
    bool cachedTokenGrantsPaidInGrace() const;
    bool hasSignedPaidGrant() const;
    void recheckEntitlements();

    QString settingsPath_;
    QNetworkAccessManager *network_{nullptr};
    QNetworkReply *reply_{nullptr};
    QString plan_{QStringLiteral("free")};
    QString status_{QStringLiteral("Free plan")};
    QString licenseKey_;
    QString licenseToken_;
    QString accountToken_;
    QString accountEmail_;
    QString accountId_;
    QString deviceCode_;
    QString userCode_;
    QString verificationUrl_;
    QDateTime signInExpires_;
    QTimer *signInPoll_{nullptr};
    QNetworkReply *signInReply_{nullptr};
    QNetworkReply *usageReply_{nullptr};
    bool trial_{false};
    QDateTime expires_;
    QTimer *revalidate_{nullptr};
    QTimer *validateRetry_{nullptr};
    int validateRetryAttempt_{0};
    QString retryKey_;
    QTimer *recheck_{nullptr};
    QTimer *heartbeat_{nullptr};
    QNetworkReply *heartbeatReply_{nullptr};
    QElapsedTimer lastBeat_;
    Entitlements features_;
    int activeSeats_{0};
    NexaAiUsage usage_;
    mutable bool tamperObserved_{false};
};

} // namespace nexa
