#pragma once

#include "core/Result.h"

#include <QList>
#include <QString>
#include <QStringView>

#include <optional>

namespace nexa {

// One key to press, with Shift if the character needs it.
struct KeyStroke {
    int code{0};            // a KEY_* code from linux/input-event-codes.h
    bool shift{false};

    bool operator==(const KeyStroke &) const = default;
};

// What typing a piece of text comes down to.
struct TypePlan {
    QList<KeyStroke> strokes;
    QString skipped;        // characters that cannot be typed this way, in the order they appeared

    bool isComplete() const { return skipped.isEmpty(); }
};

// Characters and key names → Linux key codes (pure, unit-tested).
//
// uinput sends key *codes*, and the compositor turns them into characters using the user's own XKB
// layout. These tables are the US layout, which is what this machine uses; on another layout the
// same codes produce other characters, so `forCharacter` is documented as US-only rather than
// pretending to be general. Characters outside printable ASCII have no code at all — the caller
// falls back to the clipboard for those.
namespace keymap {

// 'A' → {KEY_A, shift}, '?' → {KEY_SLASH, shift}; nothing for non-ASCII.
std::optional<KeyStroke> forCharacter(QChar character);

// "enter", "f5", "page_down", "ctrl" → a key code; case and -/_/space are ignored.
std::optional<int> forName(QStringView name);

// A chord as a person writes it ("ctrl+shift+t", "super", "alt+F4") → the codes to press, with
// modifiers first so the helper can press in order and release in reverse. Needing Shift for the
// character (as "ctrl+?" does) adds it.
Result<QList<int>> parseChord(QStringView chord);

// Everything needed to type `text`, and whatever had to be left out.
TypePlan forText(QStringView text);

// Key names the parser understands, for tool descriptions and error messages.
QStringList knownNames();

// Every key code these tables can produce, sorted. The helper enables exactly these on its
// virtual keyboard, so the device claims no key Nexa cannot actually send.
QList<int> allCodes();

} // namespace keymap

} // namespace nexa
