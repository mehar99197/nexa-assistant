#include "proto/KeyMap.h"

#include <QHash>
#include <QStringList>

#include "proto/LinuxKeyCodes.h"

#include <algorithm>

using namespace Qt::StringLiterals;

namespace nexa::keymap {

namespace {

// A row of the US layout. The key codes of each row are consecutive from `firstCode`, so the
// position of a character in `plain` (or `shifted`) gives its code.
struct Row {
    QLatin1StringView plain;
    QLatin1StringView shifted;
    int firstCode;
};

constexpr Row kRows[] = {
    {"1234567890-="_L1, "!@#$%^&*()_+"_L1, KEY_1},          // 2…13
    {"qwertyuiop[]"_L1, "QWERTYUIOP{}"_L1, KEY_Q},          // 16…27
    {"asdfghjkl;'`"_L1, "ASDFGHJKL:\"~"_L1, KEY_A},         // 30…41
    {"\\"_L1, "|"_L1, KEY_BACKSLASH},                       // 43
    {"zxcvbnm,./"_L1, "ZXCVBNM<>?"_L1, KEY_Z},              // 44…53
};

// Named keys. The names are what a person (or a model) is likely to write; `normalize` strips
// spaces, dashes and underscores, so "page down", "page_down" and "pagedown" all arrive here the
// same. Modifiers are listed first in a chord, in this order.
const QHash<QString, int> &names() {
    static const QHash<QString, int> table = [] {
        QHash<QString, int> keys{
            {u"ctrl"_s, KEY_LEFTCTRL},      {u"control"_s, KEY_LEFTCTRL},
            {u"alt"_s, KEY_LEFTALT},        {u"altgr"_s, KEY_RIGHTALT},
            {u"shift"_s, KEY_LEFTSHIFT},
            {u"super"_s, KEY_LEFTMETA},     {u"meta"_s, KEY_LEFTMETA},
            {u"win"_s, KEY_LEFTMETA},       {u"windows"_s, KEY_LEFTMETA},
            {u"cmd"_s, KEY_LEFTMETA},       {u"command"_s, KEY_LEFTMETA},

            {u"enter"_s, KEY_ENTER},        {u"return"_s, KEY_ENTER},
            {u"tab"_s, KEY_TAB},            {u"space"_s, KEY_SPACE},
            {u"esc"_s, KEY_ESC},            {u"escape"_s, KEY_ESC},
            {u"backspace"_s, KEY_BACKSPACE},
            {u"delete"_s, KEY_DELETE},      {u"del"_s, KEY_DELETE},
            {u"insert"_s, KEY_INSERT},      {u"ins"_s, KEY_INSERT},
            {u"up"_s, KEY_UP},              {u"down"_s, KEY_DOWN},
            {u"left"_s, KEY_LEFT},          {u"right"_s, KEY_RIGHT},
            {u"home"_s, KEY_HOME},          {u"end"_s, KEY_END},
            {u"pageup"_s, KEY_PAGEUP},      {u"pgup"_s, KEY_PAGEUP},
            {u"pagedown"_s, KEY_PAGEDOWN},  {u"pgdn"_s, KEY_PAGEDOWN},
            {u"capslock"_s, KEY_CAPSLOCK},  {u"numlock"_s, KEY_NUMLOCK},
            {u"scrolllock"_s, KEY_SCROLLLOCK},
            {u"menu"_s, KEY_COMPOSE},       {u"compose"_s, KEY_COMPOSE},
            {u"printscreen"_s, KEY_SYSRQ},  {u"print"_s, KEY_SYSRQ},
            {u"pause"_s, KEY_PAUSE},

            // Spelled-out punctuation, for when a model writes the word instead of the character.
            {u"plus"_s, KEY_EQUAL},         {u"minus"_s, KEY_MINUS},
            {u"comma"_s, KEY_COMMA},        {u"period"_s, KEY_DOT},
            {u"dot"_s, KEY_DOT},            {u"slash"_s, KEY_SLASH},
            {u"backslash"_s, KEY_BACKSLASH},{u"semicolon"_s, KEY_SEMICOLON},
        };
        // Only F1…F10 are consecutive: F11 and F12 sit past the keypad codes, and F13 starts a
        // third run. Computing them from KEY_F1 would land on Num Lock and Scroll Lock.
        for (int n = 1; n <= 10; ++n) keys.insert(u"f%1"_s.arg(n), KEY_F1 + n - 1);
        keys.insert(u"f11"_s, KEY_F11);
        keys.insert(u"f12"_s, KEY_F12);
        for (int n = 13; n <= 24; ++n) keys.insert(u"f%1"_s.arg(n), KEY_F13 + n - 13);
        return keys;
    }();
    return table;
}

// Modifiers are pressed in this order whatever order the chord was written in, so "ctrl+shift+t"
// and "shift+ctrl+t" send exactly the same thing. -1 means the key is not a modifier.
int modifierRank(int code) {
    switch (code) {
    case KEY_LEFTCTRL:
        return 0;
    case KEY_RIGHTCTRL:
        return 1;
    case KEY_LEFTALT:
        return 2;
    case KEY_RIGHTALT:
        return 3;
    case KEY_LEFTSHIFT:
        return 4;
    case KEY_RIGHTSHIFT:
        return 5;
    case KEY_LEFTMETA:
        return 6;
    case KEY_RIGHTMETA:
        return 7;
    default:
        return -1;
    }
}

bool isModifier(int code) {
    return modifierRank(code) >= 0;
}

QString normalize(QStringView name) {
    QString cleaned;
    cleaned.reserve(name.size());
    for (const QChar character : name) {
        if (character == u'-' || character == u'_' || character.isSpace()) continue;
        cleaned.append(character.toLower());
    }
    return cleaned;
}

} // namespace

std::optional<KeyStroke> forCharacter(QChar character) {
    if (character == u' ') return KeyStroke{KEY_SPACE, false};
    if (character == u'\n' || character == u'\r') return KeyStroke{KEY_ENTER, false};
    if (character == u'\t') return KeyStroke{KEY_TAB, false};
    if (character.unicode() >= 128) return std::nullopt;
    for (const Row &row : kRows) {
        if (const qsizetype at = row.plain.indexOf(character); at >= 0) {
            return KeyStroke{row.firstCode + int(at), false};
        }
        if (const qsizetype at = row.shifted.indexOf(character); at >= 0) {
            return KeyStroke{row.firstCode + int(at), true};
        }
    }
    return std::nullopt;
}

std::optional<int> forName(QStringView name) {
    const QString cleaned = normalize(name);
    if (cleaned.isEmpty()) return std::nullopt;
    if (const auto found = names().constFind(cleaned); found != names().constEnd()) return found.value();
    // A single character is a key in its own right ("t" in ctrl+shift+t).
    if (cleaned.size() == 1) {
        if (const std::optional<KeyStroke> stroke = forCharacter(cleaned.front())) return stroke->code;
    }
    return std::nullopt;
}

Result<QList<int>> parseChord(QStringView chord) {
    const QString text = chord.trimmed().toString();
    if (text.isEmpty()) return Error{ErrorCode::InvalidArgument, u"No keys given."_s};

    QList<int> modifiers;
    QList<int> keys;
    // "+" separates the keys, but a lone "+" is the key itself ("ctrl++" zooms in).
    QStringList parts = text.split(u'+');
    for (qsizetype i = 0; i < parts.size(); ++i) {
        QString part = parts.at(i).trimmed();
        if (part.isEmpty()) {
            // An empty part came from a literal "+"; ignore the stray at the end of "ctrl++".
            if (i + 1 == parts.size() && !keys.isEmpty()) continue;
            part = u"+"_s;
        }
        // A single character is a key of its own. Capitals are only how people write a chord
        // ("Ctrl+A" is not Ctrl+Shift+A), so a letter never brings Shift with it — but punctuation
        // does, since Shift is how the layout reaches it at all.
        std::optional<KeyStroke> character = part.size() == 1 ? forCharacter(part.front()) : std::nullopt;
        if (character && part.front().isLetter()) character->shift = false;
        std::optional<int> code = forName(part);
        if (!code && character) code = character->code;
        if (!code) {
            return Error{ErrorCode::InvalidArgument,
                         u"\"%1\" is not a key I know. Try letters, digits, punctuation, or names like %2."_s
                             .arg(part, u"enter, tab, esc, f5, up, home, ctrl, alt, shift, super"_s)};
        }
        if (character && character->shift && !modifiers.contains(KEY_LEFTSHIFT)) modifiers.append(KEY_LEFTSHIFT);
        QList<int> &target = isModifier(*code) ? modifiers : keys;
        if (!target.contains(*code)) target.append(*code);
    }
    if (keys.isEmpty() && modifiers.isEmpty()) return Error{ErrorCode::InvalidArgument, u"No keys given."_s};

    // Modifiers first, in a fixed order: pressed in order and released in reverse, so they wrap
    // the other keys.
    std::sort(modifiers.begin(), modifiers.end(),
              [](int left, int right) { return modifierRank(left) < modifierRank(right); });
    QList<int> codes = modifiers;
    codes.append(keys);
    if (codes.size() > 5) {
        return Error{ErrorCode::InvalidArgument, u"That is %1 keys at once; five is the most."_s.arg(codes.size())};
    }
    return codes;
}

TypePlan forText(QStringView text) {
    TypePlan plan;
    plan.strokes.reserve(text.size());
    for (const QChar character : text) {
        if (const std::optional<KeyStroke> stroke = forCharacter(character)) {
            plan.strokes.append(*stroke);
        } else if (!plan.skipped.contains(character)) {
            plan.skipped.append(character);
        }
    }
    return plan;
}

QList<int> allCodes() {
    QList<int> codes{KEY_SPACE, KEY_ENTER, KEY_TAB, KEY_LEFTSHIFT};
    for (const Row &row : kRows) {
        for (qsizetype at = 0; at < row.plain.size(); ++at) codes.append(row.firstCode + int(at));
    }
    for (const int code : names()) {
        if (!codes.contains(code)) codes.append(code);
    }
    std::sort(codes.begin(), codes.end());
    codes.erase(std::unique(codes.begin(), codes.end()), codes.end());
    return codes;
}

QStringList knownNames() {
    QStringList all = names().keys();
    all.sort();
    return all;
}

} // namespace nexa::keymap
