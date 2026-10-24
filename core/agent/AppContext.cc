#include "core/agent/AppContext.h"

#include <QJsonObject>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

bool hasCategory(const QStringList &categories, QLatin1StringView wanted) {
    for (const QString &category : categories) {
        if (category.compare(wanted, Qt::CaseInsensitive) == 0) return true;
    }
    return false;
}

bool isOneOf(const QString &executable, std::initializer_list<QLatin1StringView> names) {
    for (const QLatin1StringView name : names) {
        if (executable.compare(name, Qt::CaseInsensitive) == 0) return true;
    }
    return false;
}

} // namespace

void AppContext::noteOpened(const OpenedApp &app) {
    app_ = app;
    kind_ = kindOf(app);
    since_.start();
}

void AppContext::noteClosed(const OpenedApp &app) {
    if (!since_.isValid()) return;
    const bool same = (!app.id.isEmpty() && app.id == app_.id)
                      || (!app.executable.isEmpty() && app.executable.compare(app_.executable, Qt::CaseInsensitive) == 0)
                      // LibreOffice is one application behind several entries and executables.
                      || (kind_ == AppKind::Office && kindOf(app) == AppKind::Office);
    if (same) clear();
}

void AppContext::touch() {
    if (isActive()) since_.start();
}

void AppContext::clear() {
    app_ = {};
    kind_ = AppKind::Other;
    since_.invalidate();
}

bool AppContext::isActive() const {
    return since_.isValid() && !app_.name.isEmpty() && since_.elapsed() <= timeoutMs_;
}

qint64 AppContext::remainingMs() const {
    return isActive() ? std::max<qint64>(0, timeoutMs_ - since_.elapsed()) : 0;
}

AppKind AppContext::kindOf(const OpenedApp &app) {
    const QString executable = app.executable.trimmed();
    if (hasCategory(app.categories, "TerminalEmulator"_L1)
        || isOneOf(executable, {"gnome-terminal"_L1, "kgx"_L1, "ptyxis"_L1, "konsole"_L1, "xterm"_L1, "alacritty"_L1,
                                "kitty"_L1, "foot"_L1, "tilix"_L1, "terminator"_L1, "wezterm"_L1, "xfce4-terminal"_L1,
                                "qterminal"_L1, "mate-terminal"_L1, "lxterminal"_L1, "urxvt"_L1, "st"_L1})) {
        return AppKind::Terminal;
    }
    if (hasCategory(app.categories, "Office"_L1) || hasCategory(app.categories, "WordProcessor"_L1)
        || hasCategory(app.categories, "Presentation"_L1) || hasCategory(app.categories, "Spreadsheet"_L1)
        || isOneOf(executable, {"libreoffice"_L1, "soffice"_L1, "lowriter"_L1, "localc"_L1, "loimpress"_L1,
                                "onlyoffice-desktopeditors"_L1})) {
        return AppKind::Office;
    }
    if (hasCategory(app.categories, "WebBrowser"_L1)) return AppKind::Browser;
    if (hasCategory(app.categories, "TextEditor"_L1) || hasCategory(app.categories, "IDE"_L1)) return AppKind::Editor;
    return AppKind::Other;
}

QString AppContext::kindName(AppKind kind) {
    switch (kind) {
    case AppKind::Terminal: return u"a terminal"_s;
    case AppKind::Office: return u"an office suite"_s;
    case AppKind::Editor: return u"a text editor"_s;
    case AppKind::Browser: return u"a web browser"_s;
    case AppKind::Other: return u"an application"_s;
    }
    return {};
}

QString AppContext::promptSentence(const Situation &situation) const {
    if (!isActive()) return {};
    QString sentence = u"The user is working in %1 (%2), which you opened for them at their request: it is the window in "
                       "front of them, and their requests are meant for it unless they say otherwise."_s
                           .arg(app_.name, kindName(kind_));
    switch (kind_) {
    case AppKind::Terminal:
        // Typing into the terminal is the way only when the keystrokes would land there: not
        // while Nexa's own window has the keyboard, not when a confirmation would take it, not for
        // a phone user who cannot see the terminal, and not without an input backend.
        if (situation.remote) {
            sentence += u" This request came from their phone, so they cannot see that terminal: run shell "
                        "commands with run_command and tell them the result; do not type into the terminal."_s;
        } else if (situation.nexaWindowActive) {
            sentence += u" Right now Nexa's own window has the keyboard focus, so typing would land in Nexa: run "
                        "shell commands with run_command and tell them the result, until the terminal is in "
                        "front again."_s;
        } else if (situation.askedFirst) {
            sentence += u" Typing on the keyboard is confirmed by a dialog first, which would take the keyboard "
                        "away from the terminal: run shell commands with run_command and tell them the result."_s;
        } else if (!situation.canType) {
            sentence += u" Typing is not available on this PC (no input backend): run shell commands with "
                        "run_command and tell them the result."_s;
        } else {
            sentence += u" Shell commands they ask for belong in that terminal, where they can watch them run: type "
                        "each one with type_text, ending in \\n for Enter, one command per call, instead of "
                        "run_command. Use run_command only when you need the output yourself, and then say what "
                        "it showed."_s;
        }
        break;
    case AppKind::Office:
        sentence += u" When they ask for a document, letter, report, presentation, slides or a spreadsheet, make it "
                    "with create_document (kind document, presentation or spreadsheet): it writes the file and opens "
                    "it in LibreOffice Writer, Impress or Calc. Never type the content in with type_text."_s;
        break;
    case AppKind::Editor:
        // The same four reasons as the terminal's keep the keystrokes away from the editor.
        if (situation.remote || situation.nexaWindowActive || situation.askedFirst || !situation.canType) {
            sentence += u" Typing into it is not the way right now"_s;
            sentence += situation.remote           ? u" (this request came from their phone)"_s
                        : situation.nexaWindowActive ? u" (Nexa's own window has the keyboard focus)"_s
                        : situation.askedFirst       ? u" (typing is confirmed by a dialog first, which takes the keyboard)"_s
                                                     : u" (nothing can type on this PC)"_s;
            sentence += u": text they ask you to write goes into a file with write_file, opened for them afterwards."_s;
        } else {
            sentence += u" Text they ask you to write goes into it with type_text; for more than a few lines, "
                        "write_file and open the file instead."_s;
        }
        break;
    case AppKind::Browser:
        sentence += u" A page or a search they ask for opens there with run_command \"xdg-open <url>\" (for a "
                    "search, https://duckduckgo.com/?q=<words>)."_s;
        break;
    case AppKind::Other:
        break;
    }
    return sentence;
}

QString AppContext::hint(bool typing) const {
    if (!isActive()) return {};
    switch (kind_) {
    case AppKind::Terminal:
        return typing ? u"In %1: commands you ask for are typed there, unless Nexa's own window has the keyboard."_s.arg(app_.name)
                      : u"In %1."_s.arg(app_.name);
    case AppKind::Office: return u"In %1: documents, slides and sheets are made as files and opened there."_s.arg(app_.name);
    case AppKind::Editor:
        return typing ? u"In %1: text you ask for is typed there."_s.arg(app_.name) : u"In %1."_s.arg(app_.name);
    case AppKind::Browser:
    case AppKind::Other: return u"In %1."_s.arg(app_.name);
    }
    return {};
}

std::optional<IntentMatch> AppContext::redirect(const IntentMatch &match, const Situation &situation,
                                                bool destructive) const {
    if (!isActive() || kind_ != AppKind::Terminal) return std::nullopt;
    if (match.toolName != u"run_command"_s) return std::nullopt;
    if (situation.nexaWindowActive || !situation.canType || situation.askedFirst || situation.remote || destructive) {
        return std::nullopt;
    }
    if (match.arguments.value(u"background"_s).toBool()) return std::nullopt;
    const QString command = match.arguments.value(u"command"_s).toString().trimmed();
    if (command.isEmpty()) return std::nullopt;
    // Only what a US keyboard types: a character left out would still be run with Enter.
    for (const QChar character : command) {
        if (character.unicode() < 0x20 || character.unicode() > 0x7e) return std::nullopt;
    }
    return IntentMatch{.toolName = u"type_text"_s,
                       .arguments = QJsonObject{{u"text"_s, command + u'\n'}},
                       .verbatimPayload = true};
}

} // namespace nexa
