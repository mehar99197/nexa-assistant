// system/a11y/ScreenTools: the screen as text for the model — snapshot, read, click, type and wait — over a scripted
// reader and a recording input backend.

#include "system/a11y/ScreenTools.h"

#include "core/tools/ToolRegistry.h"
#include "proto/KeyMap.h"
#include "system/input/IInputBackend.h"

#include "FakeScreenReader.h"
#include "QtStringMakers.h"

#include <QJsonObject>
#include <QTest>

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace Qt::StringLiterals;
using nexa::atspi::Node;
using nexa::atspi::State;
using ARole = nexa::atspi::Role;

namespace {

constexpr int kWaitMs = 5'000;
constexpr quint64 bit(State state) { return quint64(1) << int(state); }
const quint64 kShowing = bit(State::Showing) | bit(State::Visible) | bit(State::Sensitive);

Node node(int id, int parent, ARole role, const QString &name, quint64 states, const QStringList &interfaces = {}, int index = 0) {
    Node n;
    n.ref = {u":1.20"_s, u"/a/%1"_s.arg(id)};
    n.parent = {u":1.20"_s, parent < 0 ? u"/org/a11y/atspi/accessible/root"_s : u"/a/%1"_s.arg(parent)};
    n.role = uint(role);
    n.name = name;
    n.states = states;
    n.indexInParent = index;
    for (const QString &interface : interfaces) n.interfaces.append(u"org.a11y.atspi."_s + interface);
    return n;
}

// A "Save As" dialog: a label, an editable entry, a check box without an action, a terminal, two buttons.
AtspiApp dialog() {
    AtspiApp app{u":1.20"_s, u"org.gnome.Nautilus"_s, u"gtk"_s, {}};
    app.nodes = {
        node(1, -1, ARole::Dialog, u"Save As"_s, kShowing | bit(State::Active)),
        node(2, 1, ARole::Label, u"Name"_s, kShowing, {}, 0),
        node(3, 1, ARole::Entry, {}, kShowing | bit(State::Editable) | bit(State::Focusable), {u"Text"_s, u"EditableText"_s}, 1),
        node(4, 1, ARole::CheckBox, u"Overwrite"_s, kShowing | bit(State::Focusable) | bit(State::Checkable), {}, 2),
        node(5, 1, ARole::Terminal, u"Terminal"_s, kShowing | bit(State::Focusable), {u"Text"_s}, 3),
        node(6, 1, ARole::PushButton, u"Save"_s, kShowing | bit(State::Focusable), {u"Action"_s}, 4),
        node(7, 1, ARole::PushButton, u"Cancel"_s, kShowing | bit(State::Focusable), {u"Action"_s}, 5),
    };
    return app;
}

class RecordingInput : public IInputBackend {
public:
    QString name() const override { return u"fake keyboard"_s; }
    QString reach() const override { return {}; }
    void typeText(const QString &text, Done done) override {
        typed.append(text);
        done(QString());
    }
    void pressKeys(const QList<int> &keyCodes, Done done) override {
        pressed.append(keyCodes);
        done(QString());
    }
    void mouseMove(int, int, Done done) override { done(QString()); }
    void mouseMoveAbs(int, int, int, int, Done done) override { done(QString()); }
    void mouseClick(int, int, Done done) override { done(QString()); }
    void mouseScroll(int, int, Done done) override { done(QString()); }

    QStringList typed;
    QList<QList<int>> pressed;
};

struct Rig {
    Rig() {
        reader.scripted = {dialog()};
        reader.textByPath = {{u"/a/3"_s, u"notes.txt"_s}, {u"/a/5"_s, u"$ ls\nREADME.md\n$ "_s}};
        reader.actionsByPath = {{u"/a/6"_s, {u"click"_s, u"showContextMenu"_s}}, {u"/a/7"_s, {u"click"_s}}};
        registerScreenTools(registry, ScreenServices{.reader = &reader,
                                                     .input = [this] { return haveInput ? &input : nullptr; },
                                                     .allowed = [this] { return allowed; }});
    }

    Result<ToolResult> run(const QString &tool, const QJsonObject &arguments = {}) {
        Result<ToolResult> out = Error{ErrorCode::Unknown, u"never answered"_s};
        bool done = false;
        REQUIRE(registry.find(tool) != nullptr);
        registry.find(tool)->invoke(arguments, [&out, &done](Result<ToolResult> result) {
            out = std::move(result);
            done = true;
        });
        REQUIRE(QTest::qWaitFor([&done] { return done; }, kWaitMs));
        return out;
    }

    QString snapshot(const QJsonObject &arguments = {}) {
        const Result<ToolResult> result = run(u"screen_snapshot"_s, arguments);
        REQUIRE(result);
        return result.value().text;
    }

    test::FakeScreenReader reader;
    RecordingInput input;
    bool haveInput{true};
    bool allowed{true};
    ToolRegistry registry;
};

} // namespace

TEST_CASE("screen_snapshot writes the outline, with the elements' words, and remembers its refs") {
    Rig rig;
    const QString text = rig.snapshot();
    CHECK(text.startsWith(u"Dialog \"Save As\" — org.gnome.Nautilus (active) [ref=s1]"_s));
    CHECK(text.contains(u"- entry [editable] [ref=s3]: \"notes.txt\""_s));
    CHECK(text.contains(u"- terminal \"Terminal\" [ref=s5]: \"$ ls⏎README.md⏎$\""_s));
    CHECK(text.contains(u"- button \"Save\" [ref=s6]"_s));
    CHECK(rig.reader.appsCalls == 1);
    // The refs are what the other tools take.
    const Result<ToolResult> read = rig.run(u"screen_read"_s, {{u"ref"_s, u"s3"_s}});
    REQUIRE(read);
    CHECK(read.value().text == u"notes.txt"_s);
}

TEST_CASE("screen_read gives the element's text from an offset and says when more follows") {
    Rig rig;
    rig.snapshot();
    const Result<ToolResult> tail = rig.run(u"screen_read"_s, {{u"ref"_s, u"s3"_s}, {u"offset"_s, 6}});
    REQUIRE(tail);
    CHECK(tail.value().text == u"txt\n(the end; 9 characters in all)"_s);
    const Result<ToolResult> part = rig.run(u"screen_read"_s, {{u"ref"_s, u"s3"_s}, {u"max_chars"_s, 5}});
    REQUIRE(part);
    CHECK(part.value().text == u"notes\n… 4 more characters; ask again with offset=5."_s);
}

TEST_CASE("screen_click performs the element's action, or focuses it and presses Enter — Space for a check box") {
    Rig rig;
    rig.snapshot();
    SECTION("the first action by default, a named one when asked") {
        const Result<ToolResult> clicked = rig.run(u"screen_click"_s, {{u"ref"_s, u"s6"_s}});
        REQUIRE(clicked);
        CHECK(clicked.value().text == u"Clicked \"Save\" (s6) — its \"click\" action."_s);
        CHECK(rig.reader.log == QStringList{u"doAction /a/6 0"_s});
        const Result<ToolResult> menu = rig.run(u"screen_click"_s, {{u"ref"_s, u"s6"_s}, {u"action"_s, u"showContextMenu"_s}});
        REQUIRE(menu);
        CHECK(rig.reader.log.last() == u"doAction /a/6 1"_s);
        const Result<ToolResult> unknown = rig.run(u"screen_click"_s, {{u"ref"_s, u"s6"_s}, {u"action"_s, u"jump"_s}});
        REQUIRE_FALSE(unknown);
        CHECK(unknown.error().code == ErrorCode::InvalidArgument);
        CHECK(unknown.error().message.contains(u"click, showContextMenu"_s));
        CHECK(rig.input.pressed.isEmpty());
    }
    SECTION("no action: the focus and Enter") {
        const Result<ToolResult> clicked = rig.run(u"screen_click"_s, {{u"ref"_s, u"s5"_s}});
        REQUIRE(clicked);
        CHECK(clicked.value().text == u"Focused \"Terminal\" (s5) and pressed Enter."_s);
        CHECK(rig.reader.log == QStringList{u"grabFocus /a/5"_s});
        REQUIRE(rig.input.pressed.size() == 1);
        CHECK(rig.input.pressed.first() == keymap::parseChord(u"enter"_s).value());
    }
    SECTION("a check box without an action takes Space") {
        const Result<ToolResult> clicked = rig.run(u"screen_click"_s, {{u"ref"_s, u"s4"_s}});
        REQUIRE(clicked);
        CHECK(clicked.value().text == u"Focused \"Overwrite\" (s4) and pressed Space."_s);
        REQUIRE(rig.input.pressed.size() == 1);
        CHECK(rig.input.pressed.first() == keymap::parseChord(u"space"_s).value());
    }
    SECTION("no action and nothing to press keys with") {
        rig.haveInput = false;
        const Result<ToolResult> clicked = rig.run(u"screen_click"_s, {{u"ref"_s, u"s5"_s}});
        REQUIRE_FALSE(clicked);
        CHECK(clicked.error().code == ErrorCode::NotSupported);
        CHECK(clicked.error().message.contains(u"install-helper.sh"_s));
    }
}

TEST_CASE("screen_type sets an editable element's text, and types into a terminal after focusing it") {
    Rig rig;
    rig.snapshot();
    const Result<ToolResult> set = rig.run(u"screen_type"_s, {{u"ref"_s, u"s3"_s}, {u"text"_s, u"report.odt"_s}});
    REQUIRE(set);
    CHECK(set.value().text == u"Set the text of entry (s3): 10 characters."_s);
    CHECK(rig.reader.log == QStringList{u"setText /a/3 report.odt"_s});
    CHECK(rig.input.typed.isEmpty());
    const Result<ToolResult> typed = rig.run(u"screen_type"_s, {{u"ref"_s, u"s5"_s}, {u"text"_s, u"ls -la\n"_s}});
    REQUIRE(typed);
    CHECK(typed.value().text == u"Focused \"Terminal\" (s5) and typed 7 characters with the fake keyboard."_s);
    CHECK(rig.reader.log.last() == u"grabFocus /a/5"_s);
    CHECK(rig.input.typed == QStringList{u"ls -la\n"_s});
    // Text ending in Enter goes through the keyboard even into an editable field, so Enter is pressed.
    const Result<ToolResult> submit = rig.run(u"screen_type"_s, {{u"ref"_s, u"s3"_s}, {u"text"_s, u"a.txt\n"_s}});
    REQUIRE(submit);
    CHECK(rig.input.typed.last() == u"a.txt\n"_s);
}

TEST_CASE("A ref that is not in the last snapshot, or whose element is gone, says to snapshot again") {
    Rig rig;
    rig.snapshot();
    const Result<ToolResult> unknown = rig.run(u"screen_click"_s, {{u"ref"_s, u"s99"_s}});
    REQUIRE_FALSE(unknown);
    CHECK(unknown.error().code == ErrorCode::InvalidArgument);
    CHECK(unknown.error().message.contains(u"no element s99 in the last screen_snapshot"_s));
    rig.reader.gone.insert(u"/a/6"_s);
    const Result<ToolResult> gone = rig.run(u"screen_click"_s, {{u"ref"_s, u"s6"_s}});
    REQUIRE_FALSE(gone);
    CHECK(gone.error().code == ErrorCode::NotFound);
    CHECK(gone.error().message.contains(u"take screen_snapshot again"_s));
    CHECK(rig.reader.log.isEmpty());
    const Result<ToolResult> missing = rig.run(u"screen_type"_s, {{u"text"_s, u"x"_s}});
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code == ErrorCode::InvalidArgument);
}

TEST_CASE("screen_wait_for returns when the words appear, fails at the deadline, and stops when the screen is unshared") {
    Rig rig;
    SECTION("the words appear on the third look") {
        rig.reader.beforeApps = [&rig] {
            if (rig.reader.appsCalls == 3) {
                rig.reader.scripted.first().nodes.append(node(8, 1, ARole::Label, u"Saved to Documents"_s, kShowing, {}, 6));
            }
        };
        const Result<ToolResult> waited = rig.run(u"screen_wait_for"_s, {{u"text"_s, u"saved to"_s}, {u"timeout_ms"_s, 5000}});
        REQUIRE(waited);
        CHECK(waited.value().text.startsWith(u"\"saved to\" appeared after "_s));
        CHECK(waited.value().text.contains(u"- label \"Saved to Documents\""_s));
        CHECK(rig.reader.appsCalls == 3);
    }
    SECTION("the deadline") {
        const Result<ToolResult> waited = rig.run(u"screen_wait_for"_s, {{u"text"_s, u"never"_s}, {u"timeout_ms"_s, 700}});
        REQUIRE_FALSE(waited);
        CHECK(waited.error().code == ErrorCode::Timeout);
        CHECK(waited.error().message.startsWith(u"\"never\" did not appear within 0.7 s."_s));
        CHECK(waited.error().message.contains(u"Dialog \"Save As\""_s));
        CHECK(rig.reader.appsCalls >= 2);
    }
    SECTION("the screen is unshared meanwhile") {
        rig.reader.beforeApps = [&rig] { rig.allowed = false; };
        const Result<ToolResult> waited = rig.run(u"screen_wait_for"_s, {{u"text"_s, u"never"_s}, {u"timeout_ms"_s, 5000}});
        REQUIRE_FALSE(waited);
        CHECK(waited.error().code == ErrorCode::Cancelled);
        CHECK(rig.reader.appsCalls == 1);
    }
}

TEST_CASE("Without an accessibility bus every screen tool says why") {
    Rig rig;
    rig.reader.available = false;
    for (const QString &tool : {u"screen_snapshot"_s, u"screen_read"_s, u"screen_click"_s, u"screen_type"_s, u"screen_wait_for"_s}) {
        const Result<ToolResult> result = rig.run(tool, {{u"ref"_s, u"s1"_s}, {u"text"_s, u"x"_s}});
        REQUIRE_FALSE(result);
        CHECK(result.error().code == ErrorCode::NotSupported);
        CHECK(result.error().message.contains(u"no accessibility bus in this test"_s));
    }
}

TEST_CASE("The reading screen tools are not mutating and the acting ones are privileged") {
    Rig rig;
    for (const QString &tool : {u"screen_snapshot"_s, u"screen_read"_s, u"screen_wait_for"_s}) {
        CHECK_FALSE(rig.registry.find(tool)->spec().mutating);
        CHECK_FALSE(rig.registry.find(tool)->spec().privileged);
    }
    for (const QString &tool : {u"screen_click"_s, u"screen_type"_s}) {
        CHECK(rig.registry.find(tool)->spec().mutating);
        CHECK(rig.registry.find(tool)->spec().privileged);
        CHECK_FALSE(rig.registry.find(tool)->spec().risk.isEmpty());
    }
}
