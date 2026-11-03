#pragma once

#include "core/intent/IIntentHandler.h"

#include <QElapsedTimer>
#include <QString>
#include <QStringList>

#include <optional>

namespace nexa {

// What was opened: the facts a desktop entry gives, as plain strings so core stays free of system/.
struct OpenedApp {
    QString name;               // "GNOME Terminal"
    QString id;                 // "org.gnome.Terminal.desktop"
    QString executable;         // "gnome-terminal"
    QStringList categories;     // the entry's Categories, e.g. "System", "TerminalEmulator"

    bool operator==(const OpenedApp &) const = default;
};

enum class AppKind { Terminal, Office, Editor, Browser, Other };

// The application the user is working in, as far as Nexa can tell. GNOME tells no app which
// window is in front (the Shell's introspection is reserved for the portals), so this is the one
// Nexa itself opened last at the user's request — "open terminal", "open libreoffice" — until it
// is closed, another is opened, or half an hour passes without a request. It turns the request
// after into the right action (Strategy by kind): a shell command into the terminal the user is
// looking at, a document or presentation into LibreOffice's own files, text into the editor — as a
// sentence for the model, and as a rewrite of the fast path's match.
class AppContext {
public:
    struct Situation {
        bool nexaWindowActive{false};   // Nexa's own window has the keyboard: typing would land here
        bool canType{false};            // an input backend exists (the root helper, or the portal)
        bool askedFirst{false};         // the policy would ask before typing — and answering gives Nexa the keyboard
        bool remote{false};             // the request came from a phone, whose user cannot see the PC's windows
        bool browserTools{false};       // the browser_* tools (Playwright) are there
        QString browserTarget;          // …and where they work: "the user's own Google Chrome (profile "Ahmad"), …"
    };

    void noteOpened(const OpenedApp &app);      // open_app ran, from the fast path or the model
    void noteClosed(const OpenedApp &app);      // close_app was asked for it: it is no longer in front
    void touch();                               // a request came while it is in front
    void clear();
    // How long it counts as in front after the last request (default 30 minutes).
    void setTimeoutMs(qint64 ms) { timeoutMs_ = ms; }

    bool isActive() const;
    qint64 remainingMs() const;                 // until it is forgotten; 0 when nothing is in front
    const OpenedApp &app() const { return app_; }
    AppKind kind() const { return kind_; }

    static AppKind kindOf(const OpenedApp &app);
    static QString kindName(AppKind kind);

    // For the model: who is in front and what that means for its tools. Empty when nothing is.
    QString promptSentence(const Situation &situation) const;
    // For the window, one line: "In GNOME Terminal: commands you ask for are typed there, …" —
    // `typing` says whether commands can be typed at all (an input backend, no dialog first).
    QString hint(bool typing) const;
    // The fast path: a shell command asked for while a terminal is in front is typed into it
    // (type_text, ending in Enter) so the user sees it run — unless Nexa's own window has the
    // keyboard, typing is not available or would be asked about first, the request came from a
    // phone, the command is meant for the background, is destructive, or has characters a US
    // keyboard cannot type (then it runs through run_command and its policy as usual). The match
    // to run instead, or nothing.
    std::optional<IntentMatch> redirect(const IntentMatch &match, const Situation &situation, bool destructive) const;

private:
    OpenedApp app_;
    AppKind kind_{AppKind::Other};
    QElapsedTimer since_;
    qint64 timeoutMs_{30 * 60'000};
};

} // namespace nexa
