#pragma once

#include "core/Result.h"
#include "proto/KeyMap.h"

#include <QList>

namespace nexa {

// The helper's virtual input devices, created through /dev/uinput (root-only, which is why this
// lives in the helper rather than in Nexa itself).
//
// Three separate devices, because libinput decides what a device *is* from the events it claims:
// a keyboard that only has keys, a mouse that only has relative motion and buttons, and an
// absolute pointer that reports a position. Mixing them on one device confuses that guess.
//
// Write-only by design. There is no method here that reads a key, a button or the pointer
// position: Nexa can act on the desktop, never observe its input.
class UinputDevice {
public:
    UinputDevice() = default;
    ~UinputDevice();

    UinputDevice(const UinputDevice &) = delete;
    UinputDevice &operator=(const UinputDevice &) = delete;

    // Creates the devices if they are not there yet. Called on the first injected event, so a
    // helper that only ever runs commands never opens /dev/uinput at all.
    Result<void> open();
    bool isOpen() const { return keyboard_ >= 0; }
    void close();

    // Each of these presses and releases as one action, ending with a SYN report.
    Result<void> type(const QList<KeyStroke> &strokes);
    Result<void> pressChord(const QList<int> &codes);        // in order, released in reverse
    Result<void> moveBy(int dx, int dy);
    Result<void> moveTo(int x, int y, int screenWidth, int screenHeight);
    Result<void> button(int code, bool down);
    Result<void> scroll(int dx, int dy);

    // Screen pixels → the absolute device's own 0…65535 axis (pure, unit-tested). Keeping the
    // device's range fixed means it does not care what resolution the caller measured on.
    static int toAbsolute(int position, int screenSize);

private:
    Result<void> createKeyboard();
    Result<void> createMouse();
    Result<void> createAbsolutePointer();
    Result<void> writeEvent(int fd, int type, int code, int value);
    Result<void> sync(int fd);
    Result<void> tapKey(int code);

    int keyboard_{-1};
    int mouse_{-1};
    int absolute_{-1};
    // A button has to come from the same device that owns the pointer position, or the compositor
    // has no position to associate it with.
    int lastPointer_{-1};
};

} // namespace nexa
