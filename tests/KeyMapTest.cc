// proto/KeyMap: characters and key names → Linux key codes (US layout).

#include "proto/KeyMap.h"

#include "QtStringMakers.h"

#include <linux/input-event-codes.h>

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace nexa::keymap;
using namespace Qt::StringLiterals;

namespace {

QList<int> chord(QStringView text) {
    const Result<QList<int>> keys = parseChord(text);
    REQUIRE(keys.isOk());
    return keys.value();
}

} // namespace

TEST_CASE("forCharacter maps printable ASCII, with Shift where the layout needs it") {
    CHECK(forCharacter(u'a') == KeyStroke{KEY_A, false});
    CHECK(forCharacter(u'A') == KeyStroke{KEY_A, true});
    CHECK(forCharacter(u'z') == KeyStroke{KEY_Z, false});
    CHECK(forCharacter(u'0') == KeyStroke{KEY_0, false});
    CHECK(forCharacter(u'9') == KeyStroke{KEY_9, false});
    CHECK(forCharacter(u' ') == KeyStroke{KEY_SPACE, false});
    CHECK(forCharacter(u'\n') == KeyStroke{KEY_ENTER, false});
    CHECK(forCharacter(u'\t') == KeyStroke{KEY_TAB, false});

    // Every key of the US layout that carries two characters.
    CHECK(forCharacter(u'-') == KeyStroke{KEY_MINUS, false});
    CHECK(forCharacter(u'_') == KeyStroke{KEY_MINUS, true});
    CHECK(forCharacter(u'=') == KeyStroke{KEY_EQUAL, false});
    CHECK(forCharacter(u'+') == KeyStroke{KEY_EQUAL, true});
    CHECK(forCharacter(u'[') == KeyStroke{KEY_LEFTBRACE, false});
    CHECK(forCharacter(u'{') == KeyStroke{KEY_LEFTBRACE, true});
    CHECK(forCharacter(u';') == KeyStroke{KEY_SEMICOLON, false});
    CHECK(forCharacter(u':') == KeyStroke{KEY_SEMICOLON, true});
    CHECK(forCharacter(u'\'') == KeyStroke{KEY_APOSTROPHE, false});
    CHECK(forCharacter(u'"') == KeyStroke{KEY_APOSTROPHE, true});
    CHECK(forCharacter(u'`') == KeyStroke{KEY_GRAVE, false});
    CHECK(forCharacter(u'~') == KeyStroke{KEY_GRAVE, true});
    CHECK(forCharacter(u'\\') == KeyStroke{KEY_BACKSLASH, false});
    CHECK(forCharacter(u'|') == KeyStroke{KEY_BACKSLASH, true});
    CHECK(forCharacter(u'/') == KeyStroke{KEY_SLASH, false});
    CHECK(forCharacter(u'?') == KeyStroke{KEY_SLASH, true});
    CHECK(forCharacter(u'1') == KeyStroke{KEY_1, false});
    CHECK(forCharacter(u'!') == KeyStroke{KEY_1, true});
    CHECK(forCharacter(u'$') == KeyStroke{KEY_4, true});
    CHECK(forCharacter(u'*') == KeyStroke{KEY_8, true});
    CHECK(forCharacter(u'<') == KeyStroke{KEY_COMMA, true});
    CHECK(forCharacter(u'>') == KeyStroke{KEY_DOT, true});
}

TEST_CASE("forCharacter has no code for anything outside printable ASCII") {
    CHECK_FALSE(forCharacter(u'é').has_value());
    CHECK_FALSE(forCharacter(u'€').has_value());
    CHECK_FALSE(forCharacter(QChar(0x0627)).has_value());     // Arabic alef
    // An emoji is two QChars, a surrogate pair; neither half is a key.
    CHECK_FALSE(forCharacter(QChar(0xD83D)).has_value());
    CHECK_FALSE(forCharacter(QChar(0xDE00)).has_value());
    CHECK_FALSE(forCharacter(QChar(0x07)).has_value());       // bell
}

TEST_CASE("every printable ASCII character maps to a distinct key and shift state") {
    QList<KeyStroke> seen;
    for (char16_t code = 0x21; code < 0x7F; ++code) {
        const std::optional<KeyStroke> stroke = forCharacter(QChar(code));
        REQUIRE(stroke.has_value());
        CHECK_FALSE(seen.contains(*stroke));
        seen.append(*stroke);
    }
    CHECK(seen.size() == 0x7F - 0x21);     // every printable character from ! to ~
}

TEST_CASE("forName accepts the spellings people and models use") {
    CHECK(forName(u"enter") == KEY_ENTER);
    CHECK(forName(u"Return") == KEY_ENTER);
    CHECK(forName(u"ESC") == KEY_ESC);
    CHECK(forName(u"page down") == KEY_PAGEDOWN);
    CHECK(forName(u"page_down") == KEY_PAGEDOWN);
    CHECK(forName(u"PageDown") == KEY_PAGEDOWN);
    CHECK(forName(u"pgdn") == KEY_PAGEDOWN);
    CHECK(forName(u"f5") == KEY_F5);
    CHECK(forName(u"F12") == KEY_F12);
    CHECK(forName(u"f13") == KEY_F13);
    CHECK(forName(u"f24") == KEY_F24);
    CHECK(forName(u"super") == KEY_LEFTMETA);
    CHECK(forName(u"windows") == KEY_LEFTMETA);
    CHECK(forName(u"t") == KEY_T);          // a single character is a key too
    CHECK(forName(u"?") == KEY_SLASH);
    CHECK_FALSE(forName(u"f25").has_value());
    CHECK_FALSE(forName(u"any key").has_value());
    CHECK_FALSE(forName(u"").has_value());
    CHECK(knownNames().contains(u"pagedown"_s));
}

TEST_CASE("parseChord puts modifiers first so they wrap the other keys") {
    CHECK(chord(u"ctrl+shift+t") == QList{KEY_LEFTCTRL, KEY_LEFTSHIFT, KEY_T});
    CHECK(chord(u"shift+ctrl+t") == QList{KEY_LEFTCTRL, KEY_LEFTSHIFT, KEY_T});   // same order out
    CHECK(chord(u"t+ctrl") == QList{KEY_LEFTCTRL, KEY_T});
    CHECK(chord(u"alt+F4") == QList{KEY_LEFTALT, KEY_F4});
    CHECK(chord(u"ctrl+A") == QList{KEY_LEFTCTRL, KEY_A});       // a capital is not Shift
    CHECK(chord(u"f11") == QList{KEY_F11});                      // not Num Lock
    CHECK(chord(u"f12") == QList{KEY_F12});                      // not Scroll Lock
    CHECK(chord(u"super") == QList{KEY_LEFTMETA});
    CHECK(chord(u"ctrl+alt+delete") == QList{KEY_LEFTCTRL, KEY_LEFTALT, KEY_DELETE});
    CHECK(chord(u" CTRL + A ") == QList{KEY_LEFTCTRL, KEY_A});
    CHECK(chord(u"enter") == QList{KEY_ENTER});
}

TEST_CASE("parseChord adds Shift when the character itself needs it") {
    CHECK(chord(u"ctrl+?") == QList{KEY_LEFTCTRL, KEY_LEFTSHIFT, KEY_SLASH});
    CHECK(chord(u"ctrl+shift+?") == QList{KEY_LEFTCTRL, KEY_LEFTSHIFT, KEY_SLASH});   // not twice
    CHECK(chord(u"ctrl+_") == QList{KEY_LEFTCTRL, KEY_LEFTSHIFT, KEY_MINUS});
}

TEST_CASE("parseChord handles the punctuation that is also its own separator") {
    CHECK(chord(u"ctrl+-") == QList{KEY_LEFTCTRL, KEY_MINUS});
    CHECK(chord(u"ctrl++") == QList{KEY_LEFTCTRL, KEY_LEFTSHIFT, KEY_EQUAL});
    CHECK(chord(u"ctrl+=") == QList{KEY_LEFTCTRL, KEY_EQUAL});
    CHECK(chord(u"+") == QList{KEY_LEFTSHIFT, KEY_EQUAL});
}

TEST_CASE("parseChord refuses what it cannot press") {
    const Result<QList<int>> unknown = parseChord(u"ctrl+banana");
    REQUIRE_FALSE(unknown.isOk());
    CHECK(unknown.error().code == ErrorCode::InvalidArgument);
    CHECK(unknown.error().message.contains(u"banana"_s));
    CHECK_FALSE(parseChord(u"").isOk());
    CHECK_FALSE(parseChord(u"   ").isOk());
    CHECK_FALSE(parseChord(u"ctrl+alt+shift+super+a+b").isOk());     // six keys at once
    CHECK(parseChord(u"ctrl+alt+shift+super+a").isOk());             // five is allowed
    CHECK_FALSE(parseChord(u"é").isOk());
}

TEST_CASE("forText plans the keystrokes and reports what it cannot type") {
    const TypePlan hello = forText(u"Hi!");
    CHECK(hello.isComplete());
    CHECK(hello.strokes == QList<KeyStroke>{{KEY_H, true}, {KEY_I, false}, {KEY_1, true}});

    const TypePlan shell = forText(u"ls -la ~/dev | grep \"a_b\"\n");
    CHECK(shell.isComplete());
    CHECK(shell.strokes.size() == 26);
    CHECK(shell.strokes.last() == KeyStroke{KEY_ENTER, false});

    const TypePlan accented = forText(u"café — 5€");
    CHECK_FALSE(accented.isComplete());
    CHECK(accented.skipped == u"é—€"_s);        // each reported once, in order
    CHECK(accented.strokes.size() == 6);        // c a f, two spaces and 5; the rest left out

    const TypePlan emoji = forText(u"ok 👍");
    CHECK_FALSE(emoji.isComplete());
    CHECK(emoji.strokes.size() == 3);           // "ok "
    CHECK(emoji.skipped == u"👍"_s);             // the surrogate pair, reported as the character

    CHECK(forText(u"").isComplete());
    CHECK(forText(u"").strokes.isEmpty());
}
