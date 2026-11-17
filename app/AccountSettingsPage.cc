#include "app/AccountSettingsPage.h"

#include "core/account/AccountManager.h"
#include "core/account/UpdateChecker.h"
#include "remote/LocalApiServer.h"

#include <QCheckBox>
#include <QClipboard>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QFormLayout>
#include <QGroupBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QUrl>
#include <QVBoxLayout>

using namespace Qt::StringLiterals;

namespace nexa {

AccountSettingsPage::AccountSettingsPage(const LocalApiConfig &localApi, AccountManager *account,
                                         UpdateChecker *updater, QWidget *parent)
    : QWidget(parent), account_(account), updater_(updater) {
    auto *layout = new QVBoxLayout(this);

    // ---- Account & Licence
    auto *accountBox = new QGroupBox(tr("Nexa Account & Licence"), this);
    auto *accountForm = new QFormLayout(accountBox);

    planLabel_ = new QLabel(accountBox);
    planLabel_->setWordWrap(true);
    accountEmailLabel_ = new QLabel(accountBox);
    accountEmailLabel_->setWordWrap(true);

    signInCodeLabel_ = new QLabel(accountBox);
    signInCodeLabel_->setWordWrap(true);
    signInCodeLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    signInCodeLabel_->hide();

    signInButton_ = new QPushButton(tr("Sign in with account…"), accountBox);
    signInButton_->setObjectName(u"accountSignIn"_s);
    cancelSignInButton_ = new QPushButton(tr("Cancel"), accountBox);
    cancelSignInButton_->hide();
    openSignInUrlButton_ = new QPushButton(tr("Open browser"), accountBox);
    openSignInUrlButton_->hide();
    signOutButton_ = new QPushButton(tr("Sign out"), accountBox);
    signOutButton_->hide();

    auto *signInRow = new QHBoxLayout;
    signInRow->addWidget(signInButton_);
    signInRow->addWidget(openSignInUrlButton_);
    signInRow->addWidget(cancelSignInButton_);
    signInRow->addWidget(signOutButton_);
    signInRow->addStretch();

    licenseKeyEdit_ = new QLineEdit(accountBox);
    licenseKeyEdit_->setObjectName(u"licenseKeyEdit"_s);
    licenseKeyEdit_->setPlaceholderText(u"NDM-XXXX-XXXX-XXXX"_s);
    activateKeyButton_ = new QPushButton(tr("Activate key"), accountBox);
    activateKeyButton_->setObjectName(u"activateKeyButton"_s);
    deactivateKeyButton_ = new QPushButton(tr("Remove"), accountBox);
    deactivateKeyButton_->setObjectName(u"deactivateKeyButton"_s);

    auto *keyRow = new QHBoxLayout;
    keyRow->addWidget(licenseKeyEdit_, 1);
    keyRow->addWidget(activateKeyButton_);
    keyRow->addWidget(deactivateKeyButton_);

    activationStatusLabel_ = new QLabel(accountBox);
    activationStatusLabel_->setWordWrap(true);

    accountForm->addRow(tr("Plan:"), planLabel_);
    accountForm->addRow(tr("Account:"), accountEmailLabel_);
    accountForm->addRow(QString(), signInRow);
    accountForm->addRow(QString(), signInCodeLabel_);
    accountForm->addRow(tr("Licence key:"), keyRow);
    accountForm->addRow(QString(), activationStatusLabel_);
    layout->addWidget(accountBox);

    // ---- Nexa AI Relay Usage
    auto *usageBox = new QGroupBox(tr("Nexa AI Managed Relay"), this);
    auto *usageLayout = new QVBoxLayout(usageBox);
    usageLabel_ = new QLabel(usageBox);
    usageLabel_->setWordWrap(true);
    refreshUsageButton_ = new QPushButton(tr("Refresh quota"), usageBox);
    refreshUsageButton_->setObjectName(u"refreshUsageButton"_s);
    auto *usageRow = new QHBoxLayout;
    usageRow->addWidget(usageLabel_, 1);
    usageRow->addWidget(refreshUsageButton_);
    auto *usageNote = new QLabel(
        tr("Pro and Team plans include 1,000 Nexa AI requests/month per person using your signed licence token "
           "(stored via OS credential storage and never written to config.json). Own Voice cloning and Away from Home "
           "remote tunnels also unlock on paid plans."),
        usageBox);
    usageNote->setWordWrap(true);
    usageNote->setObjectName(u"sectionNote"_s);
    usageLayout->addLayout(usageRow);
    usageLayout->addWidget(usageNote);
    layout->addWidget(usageBox);

    // ---- Signed Updates
    auto *updateBox = new QGroupBox(tr("Application Updates"), this);
    auto *updateLayout = new QVBoxLayout(updateBox);
    updateStatusLabel_ = new QLabel(updateBox);
    updateStatusLabel_->setWordWrap(true);
    updateNotesLabel_ = new QLabel(updateBox);
    updateNotesLabel_->setWordWrap(true);
    updateNotesLabel_->hide();
    updateProgress_ = new QProgressBar(updateBox);
    updateProgress_->setRange(0, 100);
    updateProgress_->hide();
    checkUpdateButton_ = new QPushButton(tr("Check for updates"), updateBox);
    checkUpdateButton_->setObjectName(u"checkUpdatesButton"_s);
    downloadUpdateButton_ = new QPushButton(tr("Download & Install"), updateBox);
    downloadUpdateButton_->setObjectName(u"downloadUpdateButton"_s);
    downloadUpdateButton_->hide();
    auto *updateButtons = new QHBoxLayout;
    updateButtons->addWidget(checkUpdateButton_);
    updateButtons->addWidget(downloadUpdateButton_);
    updateButtons->addStretch();
    updateLayout->addWidget(updateStatusLabel_);
    updateLayout->addWidget(updateNotesLabel_);
    updateLayout->addWidget(updateProgress_);
    updateLayout->addLayout(updateButtons);
    layout->addWidget(updateBox);

    // ---- M15 Part B: Loopback OpenAI-Compatible API
    auto *apiBox = new QGroupBox(tr("Local OpenAI-Compatible API (Loopback Only)"), this);
    auto *apiForm = new QFormLayout(apiBox);
    localApiEnabled_ = new QCheckBox(tr("Expose local OpenAI API on 127.0.0.1 (off by default)"), apiBox);
    localApiEnabled_->setObjectName(u"localApiEnabled"_s);
    localApiEnabled_->setChecked(localApi.enabled);

    localApiPort_ = new QSpinBox(apiBox);
    localApiPort_->setObjectName(u"localApiPort"_s);
    localApiPort_->setRange(1024, 65535);
    localApiPort_->setValue(localApi.port > 0 ? localApi.port : LocalApiConfig::kDefaultPort);

    localApiToken_ = new QLineEdit(apiBox);
    localApiToken_->setObjectName(u"localApiToken"_s);
    localApiToken_->setEchoMode(QLineEdit::Password);
    localApiToken_->setText(localApi.bearerToken);
    localApiToken_->setPlaceholderText(tr("Generated bearer token"));

    showApiToken_ = new QCheckBox(tr("Show"), apiBox);
    connect(showApiToken_, &QCheckBox::toggled, this, [this](bool visible) {
        localApiToken_->setEchoMode(visible ? QLineEdit::Normal : QLineEdit::Password);
    });
    regenApiTokenButton_ = new QPushButton(tr("Regenerate"), apiBox);
    regenApiTokenButton_->setObjectName(u"regenApiToken"_s);
    copyApiTokenButton_ = new QPushButton(tr("Copy"), apiBox);
    copyApiTokenButton_->setObjectName(u"copyApiToken"_s);

    auto *tokenRow = new QHBoxLayout;
    tokenRow->addWidget(localApiToken_, 1);
    tokenRow->addWidget(showApiToken_);
    tokenRow->addWidget(regenApiTokenButton_);
    tokenRow->addWidget(copyApiTokenButton_);

    auto *apiNote = new QLabel(
        tr("Binds only to 127.0.0.1. Exposes GET /v1/models and POST /v1/chat/completions over your active provider "
           "chain and session pool, with PC-tool execution completely disabled."),
        apiBox);
    apiNote->setWordWrap(true);
    apiNote->setObjectName(u"sectionNote"_s);

    apiForm->addRow(localApiEnabled_);
    apiForm->addRow(tr("Port:"), localApiPort_);
    apiForm->addRow(tr("Bearer token:"), tokenRow);
    apiForm->addRow(apiNote);
    layout->addWidget(apiBox);
    layout->addStretch();

    connect(localApiEnabled_, &QCheckBox::toggled, this, [this](bool on) {
        if (on && localApiToken_->text().trimmed().isEmpty()) regenerateToken();
    });
    connect(regenApiTokenButton_, &QPushButton::clicked, this, &AccountSettingsPage::regenerateToken);
    connect(copyApiTokenButton_, &QPushButton::clicked, this, &AccountSettingsPage::copyToken);

    connect(signInButton_, &QPushButton::clicked, this, &AccountSettingsPage::beginSignIn);
    connect(cancelSignInButton_, &QPushButton::clicked, this, [this] {
        if (account_) account_->cancelSignIn();
    });
    connect(openSignInUrlButton_, &QPushButton::clicked, this, [this] {
        if (account_ && !account_->signInUrl().isEmpty()) {
            QDesktopServices::openUrl(QUrl::fromUserInput(account_->signInUrl()));
        }
    });
    connect(signOutButton_, &QPushButton::clicked, this, [this] {
        if (account_) account_->signOut();
    });
    connect(activateKeyButton_, &QPushButton::clicked, this, &AccountSettingsPage::activateKey);
    connect(deactivateKeyButton_, &QPushButton::clicked, this, [this] {
        if (account_) account_->deactivate();
    });
    connect(refreshUsageButton_, &QPushButton::clicked, this, [this] {
        if (account_) account_->fetchUsage();
    });

    connect(checkUpdateButton_, &QPushButton::clicked, this, &AccountSettingsPage::checkUpdates);
    connect(downloadUpdateButton_, &QPushButton::clicked, this, &AccountSettingsPage::downloadUpdate);

    if (account_) {
        connect(account_, &AccountManager::entitlementChanged, this, &AccountSettingsPage::refreshAccount);
        connect(account_, &AccountManager::statusChanged, this, &AccountSettingsPage::refreshAccount);
        connect(account_, &AccountManager::accountChanged, this, &AccountSettingsPage::refreshAccount);
        connect(account_, &AccountManager::featuresChanged, this, &AccountSettingsPage::refreshAccount);
        connect(account_, &AccountManager::usageChanged, this, &AccountSettingsPage::refreshUsage);
        connect(account_, &AccountManager::signInCodeReady, this,
                [this](const QString &code, const QString &url) {
                    signInCodeLabel_->setText(
                        tr("Verification code: <b>%1</b><br>Open %2 and enter this code to finish signing in.")
                            .arg(code.toHtmlEscaped(), url.toHtmlEscaped()));
                    signInCodeLabel_->show();
                    refreshAccount();
                });
        connect(account_, &AccountManager::signInFinished, this, [this](bool ok, const QString &msg) {
            if (!ok) signInCodeLabel_->hide();
            activationStatusLabel_->setText(msg);
            activationStatusLabel_->setStyleSheet(ok ? u"color: #2bb3a0;"_s : u"color: #e5534b;"_s);
            refreshAccount();
        });
        connect(account_, &AccountManager::activationFinished, this, [this](bool ok, const QString &msg) {
            activationStatusLabel_->setText(msg);
            activationStatusLabel_->setStyleSheet(ok ? u"color: #2bb3a0;"_s : u"color: #e5534b;"_s);
            refreshAccount();
        });
    } else {
        for (QWidget *w : std::initializer_list<QWidget *>{signInButton_, activateKeyButton_, deactivateKeyButton_,
                                                             licenseKeyEdit_, refreshUsageButton_}) {
            w->setEnabled(false);
        }
    }

    if (updater_) {
        connect(updater_, &UpdateChecker::updateAvailable, this,
                [this](const QString &version, const QString &url, const QString &notes, const QString &sha) {
                    pendingVersion_ = version;
                    pendingUrl_ = url;
                    pendingSha256_ = sha;
                    downloadedPath_.clear();
                    updateStatusLabel_->setText(tr("Version %1 is available (verified signature).").arg(version));
                    if (!notes.trimmed().isEmpty()) {
                        updateNotesLabel_->setText(notes.trimmed());
                        updateNotesLabel_->show();
                    }
                    checkUpdateButton_->setEnabled(true);
                    downloadUpdateButton_->setText(tr("Download & Install"));
                    downloadUpdateButton_->show();
                    downloadUpdateButton_->setEnabled(true);
                });
        connect(updater_, &UpdateChecker::upToDate, this, [this] {
            checkUpdateButton_->setEnabled(true);
            updateStatusLabel_->setText(
                tr("You are on the latest version (%1).").arg(QCoreApplication::applicationVersion()));
            downloadUpdateButton_->hide();
            updateNotesLabel_->hide();
        });
        connect(updater_, &UpdateChecker::checkFailed, this, [this](const QString &reason) {
            checkUpdateButton_->setEnabled(true);
            updateStatusLabel_->setText(tr("Update check failed: %1").arg(reason));
        });
        connect(updater_, &UpdateChecker::downloadProgress, this, [this](qint64 received, qint64 total) {
            updateProgress_->show();
            if (total > 0) {
                updateProgress_->setValue(int(std::clamp<qint64>(received * 100 / total, 0, 100)));
            }
        });
        connect(updater_, &UpdateChecker::downloadReady, this, [this](const QString &filePath) {
            downloadedPath_ = filePath;
            updateProgress_->setValue(100);
            updateStatusLabel_->setText(tr("Downloaded and verified SHA-256: %1").arg(filePath));
            downloadUpdateButton_->setText(tr("Open / Install"));
            downloadUpdateButton_->setEnabled(true);
            downloadUpdateButton_->show();
        });
        connect(updater_, &UpdateChecker::downloadFailed, this, [this](const QString &reason) {
            updateProgress_->hide();
            downloadUpdateButton_->setEnabled(true);
            updateStatusLabel_->setText(tr("Download failed: %1").arg(reason));
        });
    } else {
        checkUpdateButton_->setEnabled(false);
    }

    refreshAccount();
    refreshUsage();
    refreshUpdateUi();
}

void AccountSettingsPage::store(LocalApiConfig &localApi) const {
    localApi.enabled = localApiEnabled_->isChecked();
    localApi.port = quint16(localApiPort_->value());
    localApi.bearerToken = localApiToken_->text().trimmed();
    if (localApi.enabled && localApi.bearerToken.isEmpty()) {
        localApi.bearerToken = LocalApiServer::generateBearerToken();
    }
}

void AccountSettingsPage::refreshAccount() {
    if (!account_) {
        planLabel_->setText(tr("Unavailable"));
        accountEmailLabel_->setText(tr("Not connected"));
        return;
    }
    planLabel_->setText(account_->status());
    const bool signedIn = account_->isSignedIn();
    const bool inProgress = account_->signInInProgress();
    accountEmailLabel_->setText(signedIn ? account_->accountEmail() : tr("Not signed in"));
    signInButton_->setVisible(!signedIn && !inProgress);
    signInButton_->setEnabled(!inProgress);
    cancelSignInButton_->setVisible(inProgress);
    openSignInUrlButton_->setVisible(inProgress && !account_->signInUrl().isEmpty());
    signOutButton_->setVisible(signedIn);
    deactivateKeyButton_->setEnabled(account_->hasLicenseKey());

    if (inProgress && !account_->signInCode().isEmpty()) {
        signInCodeLabel_->setText(
            tr("Verification code: <b>%1</b><br>Open %2 and enter this code.")
                .arg(account_->signInCode().toHtmlEscaped(), account_->signInUrl().toHtmlEscaped()));
        signInCodeLabel_->show();
    } else if (!inProgress) {
        signInCodeLabel_->hide();
    }
    refreshUsage();
}

void AccountSettingsPage::refreshUsage() {
    if (!account_) {
        usageLabel_->setText(tr("Unavailable"));
        return;
    }
    const Entitlements ent = account_->verifiedFeatures();
    if (!ent.nexaAi) {
        usageLabel_->setText(tr("Free plan — upgrade to Pro or Team for 1,000 Nexa AI requests/month."));
        refreshUsageButton_->setEnabled(false);
        return;
    }
    refreshUsageButton_->setEnabled(true);
    const NexaAiUsage &u = account_->aiUsage();
    if (!u.known) {
        usageLabel_->setText(tr("Included: %1 Nexa AI requests/month.").arg(ent.nexaAiMonthlyRequests));
        return;
    }
    const QString reset = u.resetsAt.isValid()
        ? u.resetsAt.toLocalTime().toString(u"d MMM yyyy"_s)
        : tr("next month");
    usageLabel_->setText(
        tr("%1 used · %2 / %3 remaining this month (resets %4).")
            .arg(u.used).arg(u.remaining).arg(u.limit).arg(reset));
}

void AccountSettingsPage::refreshUpdateUi() {
    if (!updater_) {
        updateStatusLabel_->setText(tr("Updater unavailable."));
        return;
    }
    const QString ver = QCoreApplication::applicationVersion().isEmpty()
        ? QString::fromLatin1(NEXA_VERSION)
        : QCoreApplication::applicationVersion();
    updateStatusLabel_->setText(tr("Current version: %1 (%2)").arg(ver, UpdateChecker::platformKey()));
}

void AccountSettingsPage::beginSignIn() {
    if (!account_) return;
    activationStatusLabel_->clear();
    account_->beginSignIn();
    refreshAccount();
}

void AccountSettingsPage::activateKey() {
    if (!account_) return;
    account_->activate(licenseKeyEdit_->text());
}

void AccountSettingsPage::checkUpdates() {
    if (!updater_) return;
    checkUpdateButton_->setEnabled(false);
    updateStatusLabel_->setText(tr("Checking for updates…"));
    const QString ver = QCoreApplication::applicationVersion().isEmpty()
        ? QString::fromLatin1(NEXA_VERSION)
        : QCoreApplication::applicationVersion();
    updater_->check(ver);
}

void AccountSettingsPage::downloadUpdate() {
    if (!updater_) return;
    if (!downloadedPath_.isEmpty()) {
        if (!UpdateChecker::launchInstaller(downloadedPath_)) {
            QDesktopServices::openUrl(QUrl::fromLocalFile(downloadedPath_));
        }
        return;
    }
    if (pendingUrl_.isEmpty()) return;
    downloadUpdateButton_->setEnabled(false);
    updateProgress_->setValue(0);
    updateProgress_->show();
    updater_->downloadUpdate(pendingUrl_, pendingSha256_, pendingVersion_);
}

void AccountSettingsPage::regenerateToken() {
    localApiToken_->setText(LocalApiServer::generateBearerToken());
}

void AccountSettingsPage::copyToken() {
    if (QClipboard *cb = QGuiApplication::clipboard()) {
        cb->setText(localApiToken_->text());
    }
}

} // namespace nexa
