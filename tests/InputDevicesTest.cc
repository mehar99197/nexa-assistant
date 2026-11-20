// helper/InputDevices: which input devices hardcore mode switches off. The bitmaps are this laptop's own
// (/sys/class/input/input*/capabilities, 2026-11-10), so the rule is checked against what the kernel really reports.

#include "helper/InputDevices.h"

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace nexa::inputdevices;
using namespace Qt::StringLiterals;

namespace {

Caps device(const QString &name, const QString &ev, const QString &key, const QString &rel = u"0"_s,
            const QString &abs = u"0"_s, bool isVirtual = false) {
    return Caps{name, ev, key, rel, abs, isVirtual};
}

} // namespace

TEST_CASE("A capability bitmap is read the way the kernel writes it: the highest word first") {
    const std::vector<bool> bits = parseBitmap(u"1 0"_s);
    REQUIRE(bits.size() == 128);
    CHECK(bits[64]);
    CHECK_FALSE(bits[0]);
    CHECK(parseBitmap(u"b"_s)[0]);
    CHECK(parseBitmap(u"b"_s)[1]);
    CHECK_FALSE(parseBitmap(u"b"_s)[2]);
    CHECK(parseBitmap(u"b"_s)[3]);
    CHECK(parseBitmap(u""_s).empty());
    CHECK(parseBitmap(u"zz 1"_s).empty());      // not hexadecimal: nothing, never a guess
}

TEST_CASE("The laptop's own keyboard and touchpad are what hardcore mode locks") {
    CHECK(classify(device(u"AT Translated Set 2 keyboard"_s, u"120013"_s,
                          u"180000 20000 4000000000000020 0 0 10500f02140007 ff803078f900d401 feffffdfffcfffff "
                          "fffffffffffffffe"_s))
          == Kind::Keyboard);
    CHECK(classify(device(u"SynPS/2 Synaptics TouchPad"_s, u"b"_s, u"e520 10000 0 0 0 0"_s, u"0"_s,
                          u"660800011000003"_s))
          == Kind::Pointer);
}

TEST_CASE("The power button, the lid, the jacks and the brightness keys stay working") {
    CHECK(classify(device(u"Power Button"_s, u"3"_s, u"8000 10000000000000 0"_s)) == Kind::Other);
    CHECK(classify(device(u"Lid Switch"_s, u"21"_s, u"0"_s)) == Kind::Other);
    CHECK(classify(device(u"PC Speaker"_s, u"40001"_s, u"0"_s)) == Kind::Other);
    CHECK(classify(device(u"Video Bus"_s, u"3"_s, u"3e000b00000000 0 0 0"_s)) == Kind::Other);
    CHECK(classify(device(u"HDA Intel PCH Mic"_s, u"21"_s, u"0"_s)) == Kind::Other);
}

TEST_CASE("A virtual device is never locked: Nexa's own keyboard and pointer go on typing and clicking") {
    // HP WMI hotkeys is a platform device the kernel makes, under /devices/virtual — like the helper's uinput ones.
    CHECK(classify(device(u"HP WMI hotkeys"_s, u"33"_s, u"180000 20000 0 4000000000 0 101000700000000 2302400 0 0"_s,
                          u"0"_s, u"0"_s, true))
          == Kind::Other);
    CHECK(classify(device(u"Nexa virtual keyboard"_s, u"120013"_s,
                          u"180000 20000 4000000000000020 0 0 10500f02140007 ff803078f900d401 feffffdfffcfffff "
                          "fffffffffffffffe"_s,
                          u"0"_s, u"0"_s, true))
          == Kind::Other);
}

TEST_CASE("A USB mouse is a pointer; a keyboard's media-key interface is not a keyboard") {
    // EV_SYN, EV_KEY, EV_REL, EV_MSC; REL_X, REL_Y, REL_WHEEL, REL_HWHEEL…; BTN_LEFT … BTN_TASK.
    CHECK(classify(device(u"Logitech USB Optical Mouse"_s, u"17"_s, u"ff0000 0 0 0 0"_s, u"1943"_s)) == Kind::Pointer);
    // Volume, mute, play: it can neither type nor click.
    CHECK(classify(device(u"Logitech USB Keyboard Consumer Control"_s, u"13"_s,
                          u"3f000303ff 0 0 483ffff17aff32d bfd4444600000000 1 130c130b17c000 267bfad9415fed "
                          "9e168000004400 10000002"_s))
          == Kind::Other);
    // Without BTN_LEFT, relative axes are a knob or a wheel, not a pointer.
    CHECK(classify(device(u"Volume knob"_s, u"7"_s, u"0"_s, u"3"_s)) == Kind::Other);
}
