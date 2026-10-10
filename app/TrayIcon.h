#pragma once

#include <QObject>

class QAction;
class QSystemTrayIcon;

namespace nexa {

class AppController;
class MainWindow;

// Nexa's icon in the top bar, where the desktop shows one (GNOME does with its AppIndicator
// extension). Its click shows or hides the window, and its menu reaches what matters without the
// window: talk, stop everything, settings, quit. While it is there, closing the window only hides
// it, so the hotkeys, the wake word and the web remote keep working.
class TrayIcon : public QObject {
    Q_OBJECT
public:
    TrayIcon(MainWindow *window, AppController *controller, QObject *parent = nullptr);
    ~TrayIcon() override;

    static bool isAvailable();
    bool isVisible() const;
    // Once per run: the first time the window is closed to the tray, say where Nexa went.
    void noteHidden();

private:
    void toggleWindow();

    MainWindow *window_;
    QSystemTrayIcon *icon_{nullptr};
    QAction *showAction_{nullptr};
    bool told_{false};
};

} // namespace nexa
