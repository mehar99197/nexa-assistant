#pragma once

#include "core/config/Config.h"
#include "system/browser/ChromeProfiles.h"

#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTimer;

namespace nexa {

class BrowserService;

// Settings → Browser: whether Nexa works in a web browser, which browser and profile, the
// Playwright extension that lets it work in the user's own (installed from here in a click, then
// noticed), the extension's token, and Playwright itself (installed, or install it again).
class BrowserSettingsPage : public QWidget {
    Q_OBJECT
public:
    BrowserSettingsPage(const BrowserConfig &config, BrowserService *browser, QWidget *parent = nullptr);

    void store(BrowserConfig &config) const;

protected:
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    void detect();                      // the browsers and profiles again: the extension may be new
    void fillProfiles();
    void showExtension();
    void showPlaywright();
    void installExtension();
    const BrowserInstall *chosenBrowser() const;
    QString chosenProfile() const;      // the folder: the one picked, or the one used last

    BrowserService *browser_;
    QList<BrowserInstall> installs_;
    QCheckBox *enabled_;
    QComboBox *mode_;
    QComboBox *browserCombo_;
    QComboBox *profileCombo_;
    QLabel *extensionState_;
    QPushButton *extensionButton_;
    QLineEdit *token_;
    QLabel *playwrightState_;
    QPushButton *installButton_;
    QLabel *serviceState_;
    QTimer *poll_;
    QString wantedProfile_;
};

} // namespace nexa
