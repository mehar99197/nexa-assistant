#pragma once

#include <QString>

#include <vector>

namespace nexa::inputdevices {

// Which input devices hardcore mode switches off (pure). Read from what the kernel says a device can do — its
// capability bitmaps in /sys/class/input/inputN/capabilities — never from anything it sends.
enum class Kind { Keyboard, Pointer, Other };

// What one device says about itself: its name, its bitmaps as sysfs writes them, and whether it lives under
// /sys/devices/virtual (uinput devices — Nexa's own keyboard and pointer — and the kernel's platform hotkeys).
struct Caps {
    QString name;
    QString ev;
    QString key;
    QString rel;
    QString abs;
    bool isVirtual{false};
};

// A sysfs bitmap: hexadecimal words of `wordBits` bits, the highest first ("1 0" is bit 64). Empty when it is not
// one — a bitmap that cannot be read locks nothing.
std::vector<bool> parseBitmap(const QString &sysfsWords, int wordBits = 64);

// Keyboard: it has the keys to type (A, Z, space, Enter). Pointer: it moves and clicks — a mouse (relative X and Y
// and the left button), a touchpad, touchscreen or tablet (absolute X and Y and a touch, a finger or the left
// button). Everything else — the power button, the lid, jacks, brightness and media keys — and every virtual device
// is Other, and stays working.
Kind classify(const Caps &caps);

} // namespace nexa::inputdevices
