#pragma once

#include "system/a11y/AtspiNode.h"

#include <QHash>
#include <QList>
#include <QSet>
#include <QString>

#include <functional>

namespace nexa {

// One application on the accessibility bus, with the tree its cache gave.
struct AtspiApp {
    QString bus;            // its connection (":1.13")
    QString name;           // the application node's name: "org.gnome.Terminal", "Google Chrome"
    QString toolkit;        // "gtk", "Chromium", "Qt"
    QList<atspi::Node> nodes;
};

// The desktop as text, the way Playwright writes a page: one line per element, a ref on each.
struct ScreenOutline {
    QString text;
    QHash<QString, atspi::NodeRef> refs;    // "s12" → the element
    QHash<QString, uint> roles;             // "s12" → its role (screen_click presses Space on a check box)
    QHash<QString, QString> names;          // "s12" → the name shown, for the tools' sentences
    QSet<QString> editable;                 // the refs whose text screen_type can set (EditableText, editable)
    int windows{0};                         // windows written out in full
    bool truncated{false};
};

// What the cache does not carry: an element's words (asked only for the elements written out).
using TextLookup = std::function<QString(const atspi::NodeRef &ref, int maxChars, bool fromEnd)>;

struct OutlineOptions {
    QString scope{QStringLiteral("active")};            // active | all | words of an app or a window title
    QString filter;                                     // keep the lines containing these words
    QString ownApp{QStringLiteral("nexa-assistant")};   // Nexa's own windows are never in the outline
    int maxLines{300};
    int maxChars{12000};
    int excerptChars{160};
    int terminalTailChars{400};
};

// Builds the outline (pure): the active window first, every other named window on an "Also open" line (scope
// `active`), or every window (scope `all`), or the windows whose title or app the scope's words name. Structural
// elements with no name are lifted out so their children stand in their place; a password's text is never asked for.
ScreenOutline buildScreenOutline(const QList<AtspiApp> &apps, const OutlineOptions &options, const TextLookup &text);

// What to do about a window that exposes nothing, for the outline and the tools' sentences.
QString exposesNothingHint();

} // namespace nexa
