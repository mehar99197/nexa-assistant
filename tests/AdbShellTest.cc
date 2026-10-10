// system/android/AdbShell: the command lines sent to the phone's shell. Text from a person or a
// model goes into them, so quoting is what stands between a message and a second command.

#include "system/android/AdbShell.h"

#include "QtStringMakers.h"

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace Qt::StringLiterals;

TEST_CASE("Quoting keeps any text a single word for the phone's shell") {
    CHECK(adbshell::quote(u"hello"_s) == u"'hello'"_s);
    CHECK(adbshell::quote(u"it's"_s) == u"'it'\\''s'"_s);
    // Shell syntax stays text.
    CHECK(adbshell::quote(u"hi; reboot"_s) == u"'hi; reboot'"_s);
    CHECK(adbshell::quote(u"$(rm -rf /sdcard)"_s) == u"'$(rm -rf /sdcard)'"_s);
    CHECK(adbshell::quote(QString()) == u"''"_s);
}

TEST_CASE("input text: spaces become %s, and what adb cannot type is reported") {
    const adbshell::TextCommand plain = adbshell::inputText(u"hello world"_s);
    CHECK(plain.command == u"input text 'hello%sworld'"_s);
    CHECK(plain.skipped.isEmpty());

    const adbshell::TextCommand mixed = adbshell::inputText(u"Café ☕ ok"_s);
    CHECK(mixed.command == u"input text 'Caf%s%sok'"_s);
    CHECK(mixed.skipped == u"é☕"_s);

    const adbshell::TextCommand quote = adbshell::inputText(u"don't"_s);
    CHECK(quote.command == u"input text 'don'\\''t'"_s);

    CHECK(adbshell::inputText(u"日本"_s).command.isEmpty());
}

TEST_CASE("Keys by everyday name or by KEYCODE") {
    CHECK(adbshell::keyCode(u"home"_s) == u"KEYCODE_HOME"_s);
    CHECK(adbshell::keyCode(u"Back"_s) == u"KEYCODE_BACK"_s);
    CHECK(adbshell::keyCode(u"recent apps"_s) == u"KEYCODE_APP_SWITCH"_s);
    CHECK(adbshell::keyCode(u"volume_up"_s) == u"KEYCODE_VOLUME_UP"_s);
    CHECK(adbshell::keyCode(u"volume-down button"_s) == u"KEYCODE_VOLUME_DOWN"_s);
    CHECK(adbshell::keyCode(u"enter key"_s) == u"KEYCODE_ENTER"_s);
    CHECK(adbshell::keyCode(u"lock"_s) == u"KEYCODE_SLEEP"_s);
    CHECK(adbshell::keyCode(u"KEYCODE_CALL"_s) == u"KEYCODE_CALL"_s);
    CHECK(adbshell::keyCode(u"26"_s) == u"26"_s);
    CHECK_FALSE(adbshell::keyCode(u"self destruct"_s));
    // A KEYCODE name is passed through only when it really is one: no shell syntax rides along.
    CHECK_FALSE(adbshell::keyCode(u"KEYCODE_HOME; reboot"_s));

    CHECK(adbshell::mediaKey(u"toggle"_s) == u"KEYCODE_MEDIA_PLAY_PAUSE"_s);
    CHECK(adbshell::mediaKey(u"Next"_s) == u"KEYCODE_MEDIA_NEXT"_s);
    CHECK_FALSE(adbshell::mediaKey(u"rewind"_s));
}

TEST_CASE("Phone numbers: separators go, names are not numbers") {
    CHECK(adbshell::phoneNumber(u"+92 300-1234567"_s) == u"+923001234567"_s);
    CHECK(adbshell::phoneNumber(u"(0300) 123.4567"_s) == u"03001234567"_s);
    CHECK(adbshell::phoneNumber(u"911"_s) == u"911"_s);
    CHECK(adbshell::phoneNumber(u"mom"_s).isEmpty());
    CHECK(adbshell::phoneNumber(u"room 12"_s).isEmpty());
    CHECK(adbshell::phoneNumber(u"12"_s).isEmpty());
    CHECK(adbshell::phoneNumber(u"1+2+3"_s).isEmpty());             // a + belongs only at the start
}

TEST_CASE("Calls and messages are intents with the number and text quoted") {
    CHECK(adbshell::callCommand(u"+923001234567"_s) == u"am start -a android.intent.action.CALL -d 'tel:+923001234567'"_s);
    CHECK(adbshell::composeSmsCommand(u"0300"_s, u"I'm late; sorry"_s)
          == u"am start -a android.intent.action.SENDTO -d 'sms:0300' --es sms_body 'I'\\''m late; sorry' "
             "--ez exit_on_sent true"_s);
}
