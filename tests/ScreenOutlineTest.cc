// system/a11y/ScreenOutline: the desktop's accessibility trees as the text outline a model works on.

#include "system/a11y/ScreenOutline.h"

#include "QtStringMakers.h"

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace nexa::atspi;
using namespace Qt::StringLiterals;

namespace {

constexpr quint64 bit(State state) { return quint64(1) << int(state); }
const quint64 kShowing = bit(State::Showing) | bit(State::Visible) | bit(State::Sensitive);
const quint64 kActive = kShowing | bit(State::Active);
const QString kRoot = u"/org/a11y/atspi/accessible/root"_s;

Node node(const QString &bus, int id, int parent, Role role, const QString &name, quint64 states = kShowing,
          const QStringList &interfaces = {}, int index = 0) {
    Node n;
    n.ref = {bus, u"/org/a11y/atspi/accessible/%1"_s.arg(id)};
    n.parent = {bus, parent < 0 ? kRoot : u"/org/a11y/atspi/accessible/%1"_s.arg(parent)};
    n.role = uint(role);
    n.name = name;
    n.states = states;
    n.interfaces = interfaces;
    n.indexInParent = index;
    for (const QString &interface : interfaces) n.interfaces.append(u"org.a11y.atspi."_s + interface);
    return n;
}

// GNOME Terminal as the live sample gave it: a maximized, active window.
AtspiApp terminal(bool active = true) {
    AtspiApp app{u":1.13"_s, u"org.gnome.Terminal"_s, u"gtk"_s, {}};
    app.nodes = {
        node(app.bus, 75, -1, Role::Frame, u"◑ nexa-mission-control-dashboard"_s, active ? kActive : kShowing),
        node(app.bus, 1, 75, Role::Panel, {}, kShowing, {}, 0),
        node(app.bus, 2, 1, Role::Filler, {}, kShowing, {}, 0),
        node(app.bus, 3, 2, Role::PushButton, u"Minimize"_s, kShowing | bit(State::Focusable), {u"Action"_s}, 0),
        node(app.bus, 4, 2, Role::PushButton, u"Close"_s, kShowing | bit(State::Focusable), {u"Action"_s}, 1),
        node(app.bus, 5, 1, Role::PushButton, u"Find"_s, kShowing | bit(State::Focusable), {u"Action"_s}, 1),
        node(app.bus, 6, 1, Role::ToggleButton, u"Menu"_s, kShowing | bit(State::Focusable), {u"Action"_s}, 2),
        node(app.bus, 7, 1, Role::Filler, {}, kShowing, {}, 3),
        node(app.bus, 8, 7, Role::Label, u"◑ nexa-mission-control-dashboard"_s, kShowing),
        node(app.bus, 9, 1, Role::PushButton, u"New Tab"_s, kShowing | bit(State::Focusable), {u"Action"_s}, 4),
        node(app.bus, 10, 75, Role::Filler, {}, kShowing, {}, 1),
        node(app.bus, 11, 10, Role::PageTabList, {}, kShowing),
        node(app.bus, 12, 11, Role::PageTab, {}, kShowing),
        node(app.bus, 13, 12, Role::Panel, {}, kShowing),
        node(app.bus, 14, 13, Role::Terminal, u"Terminal"_s, kShowing | bit(State::Focusable) | (active ? bit(State::Focused) : 0),
             {u"Text"_s, u"Action"_s}),
        node(app.bus, 15, 13, Role::ScrollBar, {}, kShowing),
        node(app.bus, 16, 75, Role::Panel, {}, 0),     // not showing: a hidden search bar and its button
        node(app.bus, 17, 16, Role::PushButton, u"Hidden"_s, bit(State::Visible), {u"Action"_s}),
    };
    return app;
}

// Chrome before GNOME's switch: frames that count a child the cache does not carry.
AtspiApp chrome(bool active = false) {
    AtspiApp app{u":1.11"_s, u"Google Chrome"_s, u"Chromium"_s, {}};
    Node frame = node(app.bus, 1, -1, Role::Frame, u"Codecademy - Google Chrome"_s, active ? kActive : kShowing, {u"Action"_s});
    frame.childCount = 1;
    Node popup = node(app.bus, 2, -1, Role::Frame, {}, kShowing, {u"Action"_s});
    popup.childCount = 1;
    app.nodes = {frame, popup};
    return app;
}

AtspiApp nexaItself() {
    AtspiApp app{u":1.1001"_s, u"nexa-assistant"_s, u"Qt"_s, {}};
    app.nodes = {node(app.bus, 1, -1, Role::Frame, u"Nexa"_s, kActive),
                 node(app.bus, 2, 1, Role::PushButton, u"Send"_s, kShowing, {u"Action"_s})};
    return app;
}

AtspiApp shell() {
    AtspiApp app{u":1.1"_s, u"gnome-shell"_s, u"clutter"_s, {}};
    app.nodes = {node(app.bus, 1, -1, Role::Window, u"Main stage"_s, kShowing | bit(State::Focused))};
    return app;
}

AtspiApp dialog() {
    AtspiApp app{u":1.20"_s, u"org.gnome.Nautilus"_s, u"gtk"_s, {}};
    app.nodes = {
        node(app.bus, 1, -1, Role::Dialog, u"Save As"_s, kActive),
        node(app.bus, 2, 1, Role::Label, u"Name"_s, kShowing),
        node(app.bus, 3, 1, Role::Entry, {}, kShowing | bit(State::Editable) | bit(State::Focusable), {u"Text"_s, u"EditableText"_s}),
        node(app.bus, 4, 1, Role::CheckBox, u"Overwrite"_s, kShowing | bit(State::Checked) | bit(State::Checkable), {u"Action"_s}),
        node(app.bus, 5, 1, Role::PasswordText, {}, kShowing | bit(State::Editable), {u"Text"_s, u"EditableText"_s}),
        node(app.bus, 6, 1, Role::PushButton, u"Save"_s, kShowing | bit(State::Focusable), {u"Action"_s}),
        node(app.bus, 7, 1, Role::PushButton, u"Cancel"_s, bit(State::Showing) | bit(State::Visible) | bit(State::Focusable), {u"Action"_s}),
    };
    return app;
}

QString lookup(const NodeRef &ref, int maxChars, bool fromEnd) {
    if (ref.path.endsWith(u"/14"_s)) return fromEnd ? u"$ ls\nREADME.md\n$ "_s : u"first"_s;
    if (ref.path.endsWith(u"/3"_s)) return u"notes.txt"_s;
    if (ref.path.endsWith(u"/5"_s)) return u"hunter2"_s;
    return QString(maxChars, u'x');
}

QString outlineOf(const QList<AtspiApp> &apps, OutlineOptions options = {}) {
    return buildScreenOutline(apps, options, lookup).text;
}

} // namespace

TEST_CASE("The active window comes first with its buttons, the terminal's tail quoted and every element a ref") {
    const ScreenOutline outline = buildScreenOutline({chrome(), terminal()}, {}, lookup);
    const QStringList lines = outline.text.split(u'\n');
    REQUIRE(!lines.isEmpty());
    CHECK(lines.first() == u"Window \"◑ nexa-mission-control-dashboard\" — org.gnome.Terminal (active) [ref=s1]"_s);
    CHECK(outline.text.contains(u"  - button \"New Tab\" [ref=s"_s));
    CHECK(outline.text.contains(u"  - toggle button \"Menu\" [ref=s"_s));
    CHECK(outline.text.contains(u"  - terminal \"Terminal\" [focused] [ref=s"_s));
    CHECK(outline.text.contains(u": \"$ ls⏎README.md⏎$\""_s));          // the tail, newlines marked
    CHECK(outline.text.contains(u"Also open: \"Codecademy - Google Chrome\" — Google Chrome"_s));
    CHECK_FALSE(outline.text.contains(u"Hidden"_s));                     // not showing
    CHECK_FALSE(outline.text.contains(u"scroll bar"_s));
    CHECK(outline.windows == 1);
    CHECK(outline.refs.size() == 8);        // the window, Minimize, Close, Find, Menu, the label, New Tab, the terminal
    CHECK(outline.refs.value(u"s1"_s).path == u"/org/a11y/atspi/accessible/75"_s);
    CHECK(outline.roles.value(u"s1"_s) == uint(Role::Frame));
    for (const QString &ref : outline.refs.keys()) CHECK(outline.text.contains(u"[ref=%1]"_s.arg(ref)));
}

TEST_CASE("Fillers, panels and unnamed tabs are lifted out, so nothing is nested for nothing") {
    const QString text = outlineOf({terminal()});
    for (const QString &line : text.split(u'\n')) {
        if (line.startsWith(u"Window"_s)) continue;
        CHECK(line.startsWith(u"  - "_s));          // every element one level under the window
    }
    CHECK_FALSE(text.contains(u"filler"_s));
    CHECK_FALSE(text.contains(u"panel"_s));
    CHECK_FALSE(text.contains(u"- tab"_s));
}

TEST_CASE("A window that exposes nothing says how to change that, and a nameless empty frame is left out") {
    const QString text = outlineOf({chrome(true)});
    CHECK(text.startsWith(u"Window \"Codecademy - Google Chrome\" — Google Chrome (active) [ref=s1]"_s));
    CHECK(text.contains(exposesNothingHint()));
    CHECK(text.count(u"Window "_s) == 1);
}

TEST_CASE("Nexa's own window and the shell are never in the outline") {
    const QString text = outlineOf({shell(), nexaItself(), terminal(false)});
    CHECK_FALSE(text.contains(u"\"Nexa\""_s));
    CHECK_FALSE(text.contains(u"Send"_s));
    CHECK_FALSE(text.contains(u"Main stage"_s));
    // The terminal is not active (Nexa is), so there is no active window: every window follows the note.
    CHECK(text.contains(u"No window is active"_s));
    CHECK(text.contains(u"Window \"◑ nexa-mission-control-dashboard\" — org.gnome.Terminal [ref="_s));
    CHECK_FALSE(text.contains(u"(active)"_s));
}

TEST_CASE("A focused element makes its window the active one when no window says so") {
    AtspiApp app = dialog();
    app.nodes[0].states = kShowing;                                 // the dialog does not say it is active
    app.nodes[2].states |= bit(State::Focused);                     // but its entry has the keyboard
    const QString text = outlineOf({app, terminal(false)});
    CHECK(text.startsWith(u"Dialog \"Save As\" — org.gnome.Nautilus (active) [ref=s1]"_s));
    CHECK(text.contains(u"Also open: \"◑ nexa-mission-control-dashboard\""_s));
}

TEST_CASE("A dialog: the entry's words, the checked box, the disabled button, and a password without its text") {
    const QString text = outlineOf({dialog()});
    CHECK(text.startsWith(u"Dialog \"Save As\" — org.gnome.Nautilus (active) [ref=s1]"_s));
    CHECK(text.contains(u"  - label \"Name\" [ref="_s));
    CHECK(text.contains(u"  - entry [editable] [ref=s3]: \"notes.txt\""_s));
    CHECK(text.contains(u"  - check box \"Overwrite\" [checked] [ref="_s));
    CHECK(text.contains(u"  - password [password] [ref="_s));
    CHECK_FALSE(text.contains(u"hunter2"_s));
    CHECK(text.contains(u"  - button \"Cancel\" [disabled] [ref="_s));
    CHECK(text.contains(u"  - button \"Save\" [ref="_s));
    CHECK_FALSE(text.contains(u"Save\" [disabled]"_s));
}

TEST_CASE("filter keeps the window header and the matching lines") {
    OutlineOptions options;
    options.filter = u"new TAB"_s;
    const QString text = outlineOf({terminal()}, options);
    const QStringList lines = text.split(u'\n');
    REQUIRE(lines.size() == 2);
    CHECK(lines.first().startsWith(u"Window"_s));
    CHECK(lines.last().contains(u"New Tab"_s));
}

TEST_CASE("scope names a window by words of its title or app, and says what is open when none matches") {
    OutlineOptions options;
    options.scope = u"chrome"_s;
    QString text = outlineOf({chrome(), terminal()}, options);
    CHECK(text.startsWith(u"Window \"Codecademy - Google Chrome\""_s));
    CHECK_FALSE(text.contains(u"New Tab"_s));
    options.scope = u"all"_s;
    text = outlineOf({chrome(), terminal()}, options);
    CHECK(text.contains(u"New Tab"_s));
    CHECK(text.contains(u"Window \"Codecademy - Google Chrome\""_s));
    CHECK_FALSE(text.contains(u"Also open"_s));
    options.scope = u"spreadsheet"_s;
    text = outlineOf({chrome(), terminal()}, options);
    CHECK(text.startsWith(u"No window matches \"spreadsheet\". Open: "_s));
    CHECK(text.contains(u"\"Codecademy - Google Chrome\" (Google Chrome)"_s));
}

TEST_CASE("The cap says how many elements were left out, and the excerpt says when there is more") {
    AtspiApp big{u":1.30"_s, u"Big"_s, u"gtk"_s, {}};
    big.nodes.append(node(big.bus, 1, -1, Role::Frame, u"Many"_s, kActive));
    for (int i = 0; i < 400; ++i) {
        big.nodes.append(node(big.bus, 10 + i, 1, Role::PushButton, u"Button %1"_s.arg(i), kShowing, {u"Action"_s}, i));
    }
    big.nodes.append(node(big.bus, 500, 1, Role::Text, {}, kShowing, {u"Text"_s}, 500));
    OutlineOptions options;
    options.maxLines = 50;
    const ScreenOutline outline = buildScreenOutline({big}, options, lookup);
    CHECK(outline.truncated);
    CHECK(outline.text.split(u'\n').size() == 51);
    CHECK(outline.text.contains(u"… 352 more elements"_s));
    CHECK(outline.refs.size() == 402);      // every element keeps a ref, shown or not
    options.maxLines = 1000;
    options.maxChars = 100'000;
    const QString full = buildScreenOutline({big}, options, lookup).text;
    CHECK(full.contains(u"- text [ref=s402]: \"xxxx"_s));
    CHECK(full.contains(u"\" (the first 160 characters; screen_read for all of it)"_s));
}

TEST_CASE("No window at all is said in words") {
    const QString text = outlineOf({shell(), nexaItself()});
    CHECK(text.startsWith(u"No window exposes itself on the accessibility bus"_s));
}
