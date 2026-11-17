#pragma once

#include "core/config/Config.h"

#include <QWidget>

class QCheckBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QSpinBox;

namespace nexa {

class AccountManager;
class UpdateChecker;

// Settings → Account & API:
// 1. OAuth device sign-in or manual NDM- license key activation (backed by AccountManager).
// 2. Live Nexa AI relay monthly quota / reset date + entitlement summary.
// 3. Signed release updates (Ed25519 manifest + SHA-256 download via UpdateChecker).
// 4. M15 Part B: Loopback-only (127.0.0.1) OpenAI-compatible HTTP server toggle, port, bearer token.
class AccountSettingsPage : public QWidget {
    Q_OBJECT
public:
    AccountSettingsPage(const LocalApiConfig &localApi, AccountManager *account, UpdateChecker *updater,
                        QWidget *parent = nullptr);

    void store(LocalApiConfig &localApi) const;

private:
    void refreshAccount();
    void refreshUsage();
    void refreshUpdateUi();
    void beginSignIn();
    void activateKey();
    void checkUpdates();
    void downloadUpdate();
    void regenerateToken();
    void copyToken();

    AccountManager *account_{nullptr};
    UpdateChecker *updater_{nullptr};

    QLabel *planLabel_{nullptr};
    QLabel *accountEmailLabel_{nullptr};
    QLabel *signInCodeLabel_{nullptr};
    QPushButton *signInButton_{nullptr};
    QPushButton *cancelSignInButton_{nullptr};
    QPushButton *openSignInUrlButton_{nullptr};
    QPushButton *signOutButton_{nullptr};

    QLineEdit *licenseKeyEdit_{nullptr};
    QPushButton *activateKeyButton_{nullptr};
    QPushButton *deactivateKeyButton_{nullptr};
    QLabel *activationStatusLabel_{nullptr};

    QLabel *usageLabel_{nullptr};
    QPushButton *refreshUsageButton_{nullptr};

    QLabel *updateStatusLabel_{nullptr};
    QLabel *updateNotesLabel_{nullptr};
    QProgressBar *updateProgress_{nullptr};
    QPushButton *checkUpdateButton_{nullptr};
    QPushButton *downloadUpdateButton_{nullptr};
    QString pendingVersion_;
    QString pendingUrl_;
    QString pendingSha256_;
    QString downloadedPath_;

    QCheckBox *localApiEnabled_{nullptr};
    QSpinBox *localApiPort_{nullptr};
    QLineEdit *localApiToken_{nullptr};
    QCheckBox *showApiToken_{nullptr};
    QPushButton *regenApiTokenButton_{nullptr};
    QPushButton *copyApiTokenButton_{nullptr};
};

} // namespace nexa
