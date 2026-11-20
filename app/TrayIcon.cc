#include "app/TrayIcon.h"

#include "app/AppController.h"
#include "app/MainWindow.h"
#include "ui/NexaCoreWidget.h"
#include "voice/pipeline/VoicePipeline.h"

#include <QAction>
#include <QApplication>
#include <QIcon>
#include <QMenu>
#include <QStyle>
#include <QSystemTrayIcon>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

QIcon trayMenuIcon(const QString &themeName, QStyle::StandardPixmap fallback) {
    if (const QIcon themed = QIcon::fromTheme(themeName); !themed.isNull()) return themed;
    if (QStyle *style = QApplication::style()) {
        if (const QIcon standard = style->standardIcon(fallback); !standard.isNull()) return standard;
    }
    return QIcon(u":/nexa/nexa.svg"_s);
}

} // namespace

TrayIcon::TrayIcon(MainWindow *window, AppController *controller, QObject *parent)
    : QObject(parent), window_(window) {
    if (!isAvailable()) return;

    auto *menu = new QMenu(window);
    showAction_ = menu->addAction(tr("Show Nexa"), this, &TrayIcon::toggleWindow);
    if (VoicePipeline *voice = controller->voice()) {
        menu->addAction(trayMenuIcon(u"audio-input-microphone"_s, QStyle::SP_MediaVolume), tr("Talk"), voice,
                        &VoicePipeline::toggleListening);
    }
    menu->addAction(trayMenuIcon(u"process-stop"_s, QStyle::SP_BrowserStop), tr("Stop everything"), controller,
                    &AppController::panic);
    menu->addSeparator();
    menu->addAction(trayMenuIcon(u"preferences-system"_s, QStyle::SP_FileDialogDetailedView), tr("Settings…"), window,
                    [window] {
        window->show();
        window->raise();
        window->activateWindow();
        window->openSettings();
    });
    menu->addAction(trayMenuIcon(u"application-exit"_s, QStyle::SP_DialogCloseButton), tr("Quit Nexa"), qApp,
                    &QApplication::quit);
    // The menu says "Hide" while the window is up, and "Show" while it is not.
    connect(menu, &QMenu::aboutToShow, this, [this] {
        showAction_->setText(window_->isVisible() ? tr("Hide Nexa") : tr("Show Nexa"));
    });

    icon_ = new QSystemTrayIcon(QApplication::windowIcon(), this);
    icon_->setContextMenu(menu);
    connect(icon_, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick) toggleWindow();
    });
    // The core, at rest in its current state: the third size of the one widget.
    const auto showMode = [this](CoreMode mode) {
        icon_->setIcon(QIcon(window_->presence()->snapshot(mode, 64)));
        icon_->setToolTip(tr("Nexa · %1").arg(mode == CoreMode::Idle ? tr("Ready") : coreModeName(mode)));
    };
    showMode(controller->coreMode());
    connect(controller, &AppController::coreModeChanged, this, showMode);
    connect(controller, &AppController::themeChanged, this, [controller, showMode] { showMode(controller->coreMode()); });
    // A task from the phone shows on the desktop, whatever window is in front.
    connect(controller, &AppController::phoneMessageReceived, this, [this](const QString &text) {
        icon_->showMessage(tr("Message received from your phone"), text, QSystemTrayIcon::Information, 6'000);
    });
    // Hardcore mode says so on the desktop, where it is seen whatever window is in front.
    if (HardcoreMode *hardcore = controller->hardcore()) {
        connect(hardcore, &HardcoreMode::stateChanged, this, [this, hardcore, controller](HardcoreMode::State state) {
            using State = HardcoreMode::State;
            if (state == State::Locked) {
                wasLocked_ = true;
                icon_->showMessage(tr("Keyboard and mouse locked"), hardcore->banner() + u' ' + controller->hardcoreWaysOut(),
                                   QSystemTrayIcon::Warning, 8'000);
            } else if (state == State::Failed) {
                icon_->showMessage(tr("Hardcore mode could not lock"), hardcore->reason(), QSystemTrayIcon::Warning, 8'000);
            } else if ((state == State::Armed || state == State::Off) && std::exchange(wasLocked_, false)) {
                icon_->showMessage(tr("Keyboard and mouse are back"), tr("No hardcore task is running any more."),
                                   QSystemTrayIcon::Information, 5'000);
            }
        });
        // An early end says why (it comes before the state goes back to off).
        connect(hardcore, &HardcoreMode::unlocked, this, [this](const QString &why) {
            if (!std::exchange(wasLocked_, false)) return;
            icon_->showMessage(tr("Keyboard and mouse are back"), why, QSystemTrayIcon::Information, 5'000);
        });
    }
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
