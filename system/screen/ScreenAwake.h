#pragma once

#include <QObject>
#include <QString>

namespace nexa {

// Keeps the screen on (Facade over the desktop's inhibitor): while held, the session does not go idle — no blanking,
// no screen lock, no suspend — so the owner can watch a hardcore task with the keyboard and mouse locked. The
// org.freedesktop.portal.Inhibit portal first; GNOME's own session manager when the portal refuses. The desktop drops
// the inhibitor by itself if Nexa goes away.
class ScreenAwake : public QObject {
    Q_OBJECT
public:
    explicit ScreenAwake(QObject *parent = nullptr);
    ~ScreenAwake() override;

    void hold(const QString &reason);
    void release();
#ifdef Q_OS_WIN
    bool isHeld() const { return winHeld_; }
#else
    bool isHeld() const { return !portalHandle_.isEmpty() || gnomeCookie_ != 0; }
#endif

private:
#ifndef Q_OS_WIN
    void holdThroughGnome(const QString &reason);
#endif
    void drop();

    bool wanted_{false};
#ifdef Q_OS_WIN
    bool winHeld_{false};
#else
    bool asking_{false};
    QString portalHandle_;      // the portal's request object: closing it ends the inhibition
    uint gnomeCookie_{0};
#endif
};

} // namespace nexa
