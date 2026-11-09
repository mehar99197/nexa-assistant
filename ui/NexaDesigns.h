#pragma once

#include "ui/NexaCoreSpec.h"

#include <QList>
#include <QString>

#include <optional>

namespace nexa {

// How the window is arranged.
enum class WindowLayout {
    Studio,     // the design as drawn: header beside the chat, Actions on the right
    Focus,      // one centred column, the core large above it, Actions hidden until asked
    Console,    // a terminal: no header, the core in the status line, Actions as a bottom pane
    Sidebar,    // the core and the state in a column on the left, Actions beside it
    Orb,        // voice first: a huge core in the middle, the state in large type
    Dashboard,  // every part a card
    Bottom,     // the header as a band, Actions along the bottom
    MissionControl,     // a control room: the agents on the left, the board in the middle, live activity on the right
};
inline constexpr int kWindowLayoutCount = 8;

// What the transcript draws around what you said.
enum class BubbleStyle { Bubbles, Lines, Cards };

enum class FontFace { Sans, Mono, Serif };

// A design: one of the looks Nexa can wear — a layout, a core shape and its motion, the corners,
// the type, the transcript's shape — drawn in a palette of its own (any other palette fits it).
struct DesignSpec {
    QString id;             // "console"
    QString name;           // "Console"
    QString blurb;          // one line for the gallery
    WindowLayout layout{WindowLayout::Studio};
    CoreShape shape{CoreShape::Ring};
    CoreStyle motion{CoreStyle::Classic};
    BubbleStyle bubbles{BubbleStyle::Bubbles};
    FontFace font{FontFace::Sans};
    int fontPoints{0};      // added to the system's size
    int radius{10};         // the corners, in pixels
    QString palette;        // the ui.theme setting the design was drawn in
};

// The catalogue of designs (Registry). NexaThemes holds the palettes; a design is the shape of
// everything else, and the two combine freely.
class NexaDesigns {
public:
    static const QList<DesignSpec> &all();
    static std::optional<DesignSpec> find(const QString &id);
    static DesignSpec resolve(const QString &id);          // unknown → Studio
    static QString layoutName(WindowLayout layout);
    static QString fontFamily(FontFace face);               // what the stylesheet asks for
};

} // namespace nexa
