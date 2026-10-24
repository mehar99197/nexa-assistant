// The app the user is in, as Nexa tracks it: what kind it is, what the model is told, and when a
// command asked for is typed into the terminal instead of run out of sight.

#include "core/agent/AppContext.h"

#include "QtStringMakers.h"

#include <QJsonObject>
#include <QTest>

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace Qt::StringLiterals;

namespace {

const OpenedApp kTerminal{u"GNOME Terminal"_s, u"org.gnome.Terminal.desktop"_s, u"gnome-terminal"_s,
                          {u"GNOME"_s, u"GTK"_s, u"System"_s, u"TerminalEmulator"_s, u"Utility"_s}};
const OpenedApp kWriter{u"LibreOffice Writer"_s, u"libreoffice-writer.desktop"_s, u"libreoffice"_s,
                        {u"Office"_s, u"WordProcessor"_s}};
const OpenedApp kEditor{u"Text Editor"_s, u"org.gnome.TextEditor.desktop"_s, u"gnome-text-editor"_s,
                        {u"GNOME"_s, u"Utility"_s, u"TextEditor"_s}};
const OpenedApp kBrowser{u"Firefox ESR"_s, u"firefox-esr.desktop"_s, u"firefox-esr"_s, {u"Network"_s, u"WebBrowser"_s}};

IntentMatch runCommand(const QString &command, bool background = false) {
    QJsonObject arguments{{u"command"_s, command}};
    if (background) arguments.insert(u"background"_s, true);
    return IntentMatch{.toolName = u"run_command"_s, .arguments = arguments, .verbatimPayload = true};
}

} // namespace

TEST_CASE("AppContext tells the kind of app from its categories or its program") {
    CHECK(AppContext::kindOf(kTerminal) == AppKind::Terminal);
    CHECK(AppContext::kindOf({u"Console"_s, u"org.gnome.Console.desktop"_s, u"kgx"_s, {}}) == AppKind::Terminal);
    CHECK(AppContext::kindOf(kWriter) == AppKind::Office);
    CHECK(AppContext::kindOf({u"LibreOffice"_s, u"libreoffice-startcenter.desktop"_s, u"libreoffice"_s, {u"Office"_s}})
          == AppKind::Office);
    CHECK(AppContext::kindOf({u"LibreOffice Impress"_s, u"libreoffice-impress.desktop"_s, u"soffice"_s, {}}) == AppKind::Office);
    CHECK(AppContext::kindOf(kEditor) == AppKind::Editor);
    CHECK(AppContext::kindOf(kBrowser) == AppKind::Browser);
    CHECK(AppContext::kindOf({u"Calculator"_s, u"org.gnome.Calculator.desktop"_s, u"gnome-calculator"_s, {u"Utility"_s}})
          == AppKind::Other);
}

TEST_CASE("AppContext holds the app opened last until it is closed, replaced or forgotten with time") {
    AppContext context;
    CHECK_FALSE(context.isActive());
    CHECK(context.promptSentence({}).isEmpty());
    CHECK(context.hint(true).isEmpty());

    context.noteOpened(kTerminal);
    REQUIRE(context.isActive());
    CHECK(context.app() == kTerminal);
    CHECK(context.kind() == AppKind::Terminal);
    CHECK(context.hint(true) == u"In GNOME Terminal: commands you ask for are typed there, unless Nexa's own window has the keyboard."_s);
    CHECK(context.hint(false) == u"In GNOME Terminal."_s);
    CHECK(context.remainingMs() > 0);

    // Closing another app changes nothing; closing this one ends it.
    context.noteClosed(kBrowser);
    CHECK(context.isActive());
    context.noteClosed({u"Terminal"_s, {}, u"GNOME-TERMINAL"_s, {}});
    CHECK_FALSE(context.isActive());
    CHECK(context.hint(true).isEmpty());
    CHECK(context.remainingMs() == 0);

    // Opening another replaces it; LibreOffice is one app behind several entries.
    context.noteOpened(kTerminal);
    context.noteOpened(kWriter);
    CHECK(context.app() == kWriter);
    context.noteClosed({u"LibreOffice Calc"_s, u"libreoffice-calc.desktop"_s, u"libreoffice"_s, {u"Office"_s, u"Spreadsheet"_s}});
    CHECK_FALSE(context.isActive());

    // Half an hour of silence (here: a millisecond) and the app no longer counts as in front; a
    // request in time keeps it.
    context.setTimeoutMs(300);
    context.noteOpened(kTerminal);
    QTest::qWait(150);
    context.touch();
    QTest::qWait(150);
    CHECK(context.isActive());
    QTest::qWait(400);
    CHECK_FALSE(context.isActive());
    context.touch();    // nothing to extend
    CHECK_FALSE(context.isActive());
}

TEST_CASE("AppContext tells the model what the app in front means for its tools") {
    AppContext context;
    context.noteOpened(kTerminal);
    const QString terminal = context.promptSentence({.nexaWindowActive = false, .canType = true});
    CHECK(terminal.contains(u"GNOME Terminal"_s));
    CHECK(terminal.contains(u"type_text"_s));
    CHECK(terminal.contains(u"instead of run_command"_s));
    CHECK_FALSE(terminal.contains(u"Nexa's own window"_s));
    const QString focused = context.promptSentence({.nexaWindowActive = true, .canType = true});
    CHECK(focused.contains(u"Nexa's own window has the keyboard focus"_s));
    const QString noTyping = context.promptSentence({.nexaWindowActive = false, .canType = false});
    CHECK(noTyping.contains(u"Typing is not available"_s));
    CHECK_FALSE(noTyping.contains(u"type each one with type_text"_s));
    // A confirmation dialog would take the keyboard; a phone user cannot see the terminal: in both
    // cases the model is told to run commands, never to type.
    const QString asked = context.promptSentence({.nexaWindowActive = false, .canType = true, .askedFirst = true});
    CHECK(asked.contains(u"confirmed by a dialog"_s));
    CHECK(asked.contains(u"run_command"_s));
    CHECK_FALSE(asked.contains(u"type each one with type_text"_s));
    const QString phone = context.promptSentence({.nexaWindowActive = false, .canType = true, .remote = true});
    CHECK(phone.contains(u"came from their phone"_s));
    CHECK_FALSE(phone.contains(u"type each one with type_text"_s));

    context.noteOpened(kWriter);
    const QString office = context.promptSentence({});
    CHECK(office.contains(u"create_document"_s));
    CHECK(office.contains(u"Never type the content"_s));
    CHECK(context.hint(true).contains(u"documents, slides and sheets"_s));

    context.noteOpened(kEditor);
    const AppContext::Situation typing{.nexaWindowActive = false, .canType = true};
    CHECK(context.promptSentence(typing).contains(u"type_text"_s));
    CHECK(context.hint(true).contains(u"typed there"_s));
    CHECK(context.hint(false) == u"In Text Editor."_s);
    // The editor is kept from the keystrokes for the terminal's four reasons.
    for (const AppContext::Situation &kept : {AppContext::Situation{.nexaWindowActive = true, .canType = true},
                                              AppContext::Situation{.nexaWindowActive = false, .canType = false},
                                              AppContext::Situation{.nexaWindowActive = false, .canType = true, .askedFirst = true},
                                              AppContext::Situation{.nexaWindowActive = false, .canType = true, .remote = true}}) {
        const QString sentence = context.promptSentence(kept);
        CHECK(sentence.contains(u"write_file"_s));
        CHECK_FALSE(sentence.contains(u"type_text"_s));
    }
    context.noteOpened(kBrowser);
    CHECK(context.promptSentence({}).contains(u"xdg-open"_s));
}

TEST_CASE("AppContext types a command into the terminal in front, and only then") {
    AppContext context;
    const AppContext::Situation handsFree{.nexaWindowActive = false, .canType = true};

    // No app in front: nothing changes.
    CHECK_FALSE(context.redirect(runCommand(u"ls -la"_s), handsFree, false).has_value());

    context.noteOpened(kTerminal);
    const std::optional<IntentMatch> typed = context.redirect(runCommand(u"ls -la"_s), handsFree, false);
    REQUIRE(typed.has_value());
    CHECK(typed->toolName == u"type_text"_s);
    CHECK(typed->arguments.value(u"text"_s).toString() == u"ls -la\n"_s);
    CHECK(typed->verbatimPayload);

    // Nexa's window has the keyboard: the keystrokes would land in Nexa.
    CHECK_FALSE(context.redirect(runCommand(u"ls"_s), {.nexaWindowActive = true, .canType = true}, false).has_value());
    // Nothing can type.
    CHECK_FALSE(context.redirect(runCommand(u"ls"_s), {.nexaWindowActive = false, .canType = false}, false).has_value());
    // The policy would ask first — and answering the question gives Nexa's window the keyboard.
    CHECK_FALSE(context.redirect(runCommand(u"ls"_s), {.nexaWindowActive = false, .canType = true, .askedFirst = true}, false)
                    .has_value());
    // From a phone: its user cannot see the PC's terminal.
    CHECK_FALSE(context.redirect(runCommand(u"ls"_s), {.nexaWindowActive = false, .canType = true, .remote = true}, false)
                    .has_value());
    // Characters a US keyboard cannot type would be dropped and the rest run anyway.
    CHECK_FALSE(context.redirect(runCommand(u"echo caf\u00e9"_s), handsFree, false).has_value());
    CHECK_FALSE(context.redirect(runCommand(u"ls\nrm x"_s), handsFree, false).has_value());
    // Destructive: run_command and its policy, as always.
    CHECK_FALSE(context.redirect(runCommand(u"rm -rf build"_s), handsFree, true).has_value());
    // Meant for the background, or not a command at all.
    CHECK_FALSE(context.redirect(runCommand(u"python3 -m http.server"_s, true), handsFree, false).has_value());
    CHECK_FALSE(context.redirect(IntentMatch{.toolName = u"open_app"_s}, handsFree, false).has_value());
    CHECK_FALSE(context.redirect(IntentMatch{.toolName = u"run_root_command"_s, .arguments = {{u"command"_s, u"id"_s}}},
                                 handsFree, false).has_value());

    // Another kind of app in front: commands run as before.
    context.noteOpened(kWriter);
    CHECK_FALSE(context.redirect(runCommand(u"ls"_s), handsFree, false).has_value());
}
