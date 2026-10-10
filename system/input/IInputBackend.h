#pragma once

#include "core/Result.h"

#include <QList>
#include <QObject>
#include <QString>

#include <functional>

namespace nexa {

// How Nexa moves the keyboard and pointer (Strategy). Write-only: there is no call here that reads
// a key, a button or where the pointer is.
//
// Asynchronous, because the backend that actually works on Wayland goes through the root helper and
// the first call of a session waits for polkit's password prompt. `done` carries a short note on
// success — for typing, the characters that had to be left out.
class IInputBackend : public QObject {
    Q_OBJECT
public:
    using Done = std::function<void(Result<QString>)>;

    using QObject::QObject;

    virtual QString name() const = 0;
    // Which windows this backend can reach, for the status bar and for tool results.
    virtual QString reach() const = 0;

    virtual void typeText(const QString &text, Done done) = 0;
    virtual void pressKeys(const QList<int> &keyCodes, Done done) = 0;
    virtual void mouseMove(int dx, int dy, Done done) = 0;
    // Absolute position in the physical pixels of a screen that size — the space a portal
    // screenshot uses, so coordinates read off an image can be used directly.
    virtual void mouseMoveAbs(int x, int y, int screenWidth, int screenHeight, Done done) = 0;
    // button: 1 left, 2 right, 3 middle. count 2 is a double click.
    virtual void mouseClick(int button, int count, Done done) = 0;
    virtual void mouseScroll(int dx, int dy, Done done) = 0;
};

} // namespace nexa
