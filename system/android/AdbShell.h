#pragma once

#include "core/Result.h"

#include <QString>

#include <optional>

namespace nexa::adbshell {

// Pure builders for the command lines Nexa sends to `adb shell`. adb joins its arguments into one
// string that the phone's `sh` parses again, so anything that came from a person or a model is
// single-quoted here — a message containing `; reboot` is text, not a second command.

// 'it'\''s' — safe for any byte string.
QString quote(const QString &text);

// `input text` types ASCII only, and reads a space as the end of its argument, so spaces become
// %s. `skipped` lists what had to be left out (anything outside printable ASCII).
struct TextCommand {
    QString command;
    QString skipped;
};
TextCommand inputText(const QString &text);

// A key by its everyday name ("home", "back", "volume up", "enter") or as KEYCODE_X / a number.
// Empty when the name is not known.
std::optional<QString> keyCode(const QString &name);
// play, pause, toggle, next, previous, stop → the media key.
std::optional<QString> mediaKey(const QString &action);

// "+92 300-1234567" → "+923001234567". Empty when there are not at least three digits, so
// "mom" is not mistaken for a number.
QString phoneNumber(const QString &text);

QString callCommand(const QString &number);
// Opens the messaging app on a new message with the text filled in. It does not send it.
QString composeSmsCommand(const QString &number, const QString &body);

} // namespace nexa::adbshell
