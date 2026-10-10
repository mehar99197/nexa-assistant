#include "app/TrayIcon.h"

#include "app/AppController.h"
#include "app/MainWindow.h"
#include "voice/pipeline/VoicePipeline.h"

#include <QAction>
#include <QApplication>
#include <QIcon>
#include <QMenu>
#include <QSystemTrayIcon>

using namespace Qt::StringLiterals;

namespace nexa {

TrayIcon::TrayIcon(MainWindow *window, AppController *controller, QObject *parent)
    : QObject(parent), window_(window) {
    if (!isAvailable()) return;

    auto *menu = new QMenu(window);
    showAction_ = menu->addAction(tr("Show Nexa"), this, &TrayIcon::toggleWindow);
    if (VoicePipeline *voice = controller->voice()) {
        menu->addAction(QIcon::fromTheme(u"audio-input-microphone"_s), tr("Talk"), voice, &VoicePipeline::toggleListening);
    }
    menu->addAction(QIcon::fromTheme(u"process-stop"_s), tr("Stop everything"), controller, &AppController::panic);
    menu->addSeparator();
    menu->addAction(QIcon::fromTheme(u"preferences-system"_s), tr("Settings…"), window, [window] {
        window->show();
        window->raise();
        window->activateWindow();
        window->openSettings();
    });
    menu->addAction(QIcon::fromTheme(u"application-exit"_s), tr("Quit Nexa"), qApp, &QApplication::quit);
    // The menu says "Hide" while the window is up, and "Show" while it is not.
    connect(menu, &QMenu::aboutToShow, this, [this] {
        showAction_->setText(window_->isVisible() ? tr("Hide Nexa") : tr("Show Nexa"));
    });

    icon_ = new QSystemTrayIcon(QApplication::windowIcon(), this);
    icon_->setToolTip(tr("Nexa"));
    icon_->setContextMenu(menu);
    connect(icon_, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick) toggleWindow();
    });
    icon_->show();
}

TrayIcon::~TrayIcon() = default;

bool TrayIcon::isAvailable() {
    return QSystemTrayIcon::isSystemTrayAvailable();
}

bool TrayIcon::isVisible() const {
    return icon_ && icon_->isVisible();
}

void TrayIcon::toggleWindow() {
    if (window_->isVisible() && window_->isActiveWindow()) {
        window_->hide();
        return;
    }
    window_->show();
    window_->raise();
    window_->activateWindow();
}

void TrayIcon::noteHidden() {
    if (!icon_ || told_) return;
    told_ = true;
    icon_->showMessage(tr("Nexa is still running"),
                       tr("Its hotkeys and the web remote keep working. Quit from this icon's menu."),
                       QSystemTrayIcon::Information, 6'000);
}

} // namespace nexa
