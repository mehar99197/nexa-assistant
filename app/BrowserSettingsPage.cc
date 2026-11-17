#include "app/BrowserSettingsPage.h"

#include "system/browser/BrowserService.h"
#include "system/browser/PlaywrightInstaller.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QProcess>
#include <QPushButton>
#include <QStandardPaths>
#include <QTimer>
#include <QVBoxLayout>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

void selectData(QComboBox *combo, const QString &value) {
    const int index = combo->findData(value);
    combo->setCurrentIndex(index >= 0 ? index : 0);
}

QLabel *wrapped(const QString &text, QWidget *parent) {
    auto *label = new QLabel(text, parent);
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return label;
}

} // namespace

BrowserSettingsPage::BrowserSettingsPage(const BrowserConfig &config, BrowserService *browser, QWidget *parent)
    : QWidget(parent), browser_(browser), wantedProfile_(config.profile) {
    enabled_ = new QCheckBox(tr("Let Nexa work in a web browser (Playwright)"), this);
    enabled_->setChecked(config.enabled && config.mode != u"off"_s);

    mode_ = new QComboBox(this);
    mode_->addItem(tr("Your browser when the extension is there, otherwise Nexa's own profile"), u"auto"_s);
    mode_->addItem(tr("Only your browser, through the extension"), u"extension"_s);
    mode_->addItem(tr("Always a separate window with Nexa's own profile"), u"nexa-profile"_s);
    selectData(mode_, config.mode == u"off"_s ? u"auto"_s : config.mode);

    browserCombo_ = new QComboBox(this);
    profileCombo_ = new QComboBox(this);
    extensionState_ = wrapped({}, this);
    extensionButton_ = new QPushButton(this);
    connect(extensionButton_, &QPushButton::clicked, this, &BrowserSettingsPage::installExtension);

    token_ = new QLineEdit(config.extensionToken, this);
    token_->setEchoMode(QLineEdit::Password);
    token_->setPlaceholderText(tr("Optional: the token the extension shows — then it connects without asking"));

    playwrightState_ = wrapped({}, this);
    installButton_ = new QPushButton(this);
    connect(installButton_, &QPushButton::clicked, this, [this] {
        if (!browser_) return;
        browser_->installer()->install(installs_.isEmpty());
        showPlaywright();
    });
    serviceState_ = wrapped({}, this);

    detect();
    selectData(browserCombo_, config.browser);
    fillProfiles();
    connect(browserCombo_, &QComboBox::currentIndexChanged, this, [this] {
        fillProfiles();
        showExtension();
    });
    connect(profileCombo_, &QComboBox::currentIndexChanged, this, [this] {
        wantedProfile_ = profileCombo_->currentData().toString();
        showExtension();
    });
    showExtension();
    showPlaywright();
    if (browser_) {
        connect(browser_, &BrowserService::stateChanged, this, &BrowserSettingsPage::showPlaywright);
        connect(browser_->installer(), &PlaywrightInstaller::progress, this, &BrowserSettingsPage::showPlaywright);
        connect(browser_->installer(), &PlaywrightInstaller::failed, this, &BrowserSettingsPage::showPlaywright);
    }

    // While the page is in view, the chosen profile is looked at again: the extension added from the
    // store shows by itself. Only that profile — reading every profile's preferences each time would
    // make the page stutter.
    poll_ = new QTimer(this);
    poll_->setInterval(2000);
    connect(poll_, &QTimer::timeout, this, [this] {
        const BrowserInstall *install = chosenBrowser();
        if (!install || !install->supportsExtension) return;
        const QString directory = chosenProfile();
        for (BrowserInstall &candidate : installs_) {
            if (candidate.id != install->id) continue;
            for (BrowserProfile &profile : candidate.profiles) {
                if (profile.directory != directory) continue;
                const bool there = ChromeProfiles::extensionInstalled(QDir(candidate.userDataDir).filePath(directory));
                if (there == profile.extension) return;
                profile.extension = there;
                showExtension();
            }
        }
    });

    auto *extensionRow = new QHBoxLayout;
    extensionRow->addWidget(extensionState_, 1);
    extensionRow->addWidget(extensionButton_);
    auto *playwrightRow = new QHBoxLayout;
    playwrightRow->addWidget(playwrightState_, 1);
    playwrightRow->addWidget(installButton_);

    auto *form = new QFormLayout;
    form->addRow(tr("Browser:"), browserCombo_);
    form->addRow(tr("Profile:"), profileCombo_);
    form->addRow(tr("Extension:"), extensionRow);
    form->addRow(tr("Token:"), token_);
    form->addRow(tr("Use:"), mode_);
    form->addRow(tr("Playwright:"), playwrightRow);
    form->addRow(tr("Now:"), serviceState_);

    QString help = tr("Nexa uses Playwright first for anything on the web: it reads pages as text, so it works even with "
                      "models that cannot see the screen. Screen sharing is only for what it cannot reach. To work in "
                      "your own browser, signed in as you, add the Playwright extension to the profile above "
                      "(Install in …), then optionally paste the token the extension shows so it connects without "
                      "asking — Playwright passes that token to the browser on its command line. Until then Nexa "
                      "works in a separate window with its own profile. In confirm-risky mode Nexa asks before it "
                      "clicks or types on a page. Changes take effect when you press OK.");
    if (ChromeProfiles::firefoxInstalled()) {
        help += u"\n\n"_s + tr("Firefox cannot be driven this way (there is no Playwright extension for it); Chrome, "
                               "Edge or Nexa's own profile can.");
    }

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(enabled_);
    layout->addLayout(form);
    layout->addStretch(1);
    layout->addWidget(wrapped(help, this));

    const auto enable = [this](bool on) {
        for (QWidget *widget : {static_cast<QWidget *>(mode_), static_cast<QWidget *>(browserCombo_),
                                static_cast<QWidget *>(profileCombo_), static_cast<QWidget *>(extensionButton_),
                                static_cast<QWidget *>(token_)}) {
            widget->setEnabled(on);
        }
    };
    enable(enabled_->isChecked());
    connect(enabled_, &QCheckBox::toggled, this, enable);
}

void BrowserSettingsPage::store(BrowserConfig &config) const {
    config.enabled = enabled_->isChecked();
    config.mode = enabled_->isChecked() ? mode_->currentData().toString() : u"off"_s;
    if (!installs_.isEmpty()) config.browser = browserCombo_->currentData().toString();
    config.profile = profileCombo_->currentData().toString();
    config.extensionToken = token_->text().trimmed();
}

void BrowserSettingsPage::showEvent(QShowEvent *event) {
    QWidget::showEvent(event);
    poll_->start();
}

void BrowserSettingsPage::hideEvent(QHideEvent *event) {
    QWidget::hideEvent(event);
    poll_->stop();
}

void BrowserSettingsPage::detect() {
    installs_ = ChromeProfiles::detect();
    const QSignalBlocker blocker(browserCombo_);
    browserCombo_->clear();
    for (const BrowserInstall &install : installs_) browserCombo_->addItem(install.displayName, install.id);
    if (installs_.isEmpty()) browserCombo_->addItem(tr("None found — Playwright's Chromium is used"), QString());
    browserCombo_->setEnabled(installs_.size() > 1);
}

const BrowserInstall *BrowserSettingsPage::chosenBrowser() const {
    const QString id = browserCombo_->currentData().toString();
    for (const BrowserInstall &install : installs_) {
        if (install.id == id) return &install;
    }
    return nullptr;
}

QString BrowserSettingsPage::chosenProfile() const {
    const QString picked = profileCombo_->currentData().toString();
    if (!picked.isEmpty()) return picked;
    const BrowserInstall *install = chosenBrowser();
    if (!install) return {};
    return install->lastUsed.isEmpty() ? u"Default"_s : install->lastUsed;
}

void BrowserSettingsPage::fillProfiles() {
    const QSignalBlocker blocker(profileCombo_);
    profileCombo_->clear();
    const BrowserInstall *install = chosenBrowser();
    QString lastName;
    if (install && install->profile(install->lastUsed)) lastName = install->profile(install->lastUsed)->name;
    profileCombo_->addItem(lastName.isEmpty() ? tr("The one used last") : tr("The one used last (%1)").arg(lastName), QString());
    if (install) {
        for (const BrowserProfile &profile : install->profiles) {
            profileCombo_->addItem(profile.name == profile.directory ? profile.name
                                                                     : u"%1 (%2)"_s.arg(profile.name, profile.directory),
                                   profile.directory);
        }
    }
    selectData(profileCombo_, wantedProfile_);
    profileCombo_->setEnabled(install && !install->profiles.isEmpty());
}

void BrowserSettingsPage::showExtension() {
    const BrowserInstall *install = chosenBrowser();
    if (!install) {
        extensionState_->setText(tr("No Chrome or Edge on this PC: Nexa works in Playwright's own Chromium."));
        extensionButton_->hide();
        return;
    }
    // The profile's name may have changed under the poll: refresh the entries' labels quietly.
    if (profileCombo_->count() != install->profiles.size() + 1) fillProfiles();
    if (!install->supportsExtension) {
        extensionState_->setText(tr("%1 cannot take the Playwright extension: Nexa works there with its own profile.")
                                     .arg(install->displayName));
        extensionButton_->hide();
        return;
    }
    const QString directory = chosenProfile();
    const BrowserProfile *profile = install->profile(directory);
    const QString name = profile ? profile->name : directory;
    extensionButton_->show();
    if (profile && profile->extension) {
        extensionState_->setText(tr("✓ Installed in %1 — Nexa works in your own %2 there.").arg(name, install->displayName));
        extensionButton_->setText(tr("Open the store page"));
    } else {
        extensionState_->setText(tr("Not installed in %1 yet.").arg(name));
        extensionButton_->setText(tr("Install in %1…").arg(install->displayName));
    }
}

void BrowserSettingsPage::installExtension() {
    const BrowserInstall *install = chosenBrowser();
    if (!install) return;
    // The store page in the chosen profile; the extension is noticed by the poll once added.
    QProcess::startDetached(install->executable, {u"--profile-directory="_s + chosenProfile(), ChromeProfiles::webStoreUrl()});
    extensionState_->setText(tr("The store page is open in %1: press \"Add to Chrome\" there. This page notices it by itself.")
                                 .arg(install->displayName));
}

void BrowserSettingsPage::showPlaywright() {
    if (!browser_) {
        playwrightState_->setText(tr("Not available in this copy."));
        installButton_->hide();
        serviceState_->clear();
        return;
    }
    PlaywrightInstaller *installer = browser_->installer();
    if (installer->isRunning()) {
        playwrightState_->setText(tr("Installing…"));
        installButton_->setEnabled(false);
        installButton_->setText(tr("Install"));
    } else if (installer->isInstalled()) {
        playwrightState_->setText(tr("%1 %2, in %3").arg(QString(PlaywrightInstaller::kPackage),
                                                         installer->installedVersion(), installer->prefix()));
        installButton_->setEnabled(true);
        installButton_->setText(tr("Reinstall"));
    } else {
        playwrightState_->setText(tr("Not installed (Nexa installs it by itself with Node.js and npm)."));
        installButton_->setEnabled(true);
        installButton_->setText(tr("Install"));
    }
    serviceState_->setText(browser_->detail());
}

} // namespace nexa
