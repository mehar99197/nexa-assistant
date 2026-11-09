#include "ui/NexaDesigns.h"

#include <QFontDatabase>

using namespace Qt::StringLiterals;

namespace nexa {

const QList<DesignSpec> &NexaDesigns::all() {
    static const QList<DesignSpec> designs{
        DesignSpec{u"studio"_s, u"Studio"_s,
                   u"The design as drawn: the core beside the chat, pills in the toolbar, Actions on the right."_s,
                   WindowLayout::Studio, CoreShape::Ring, CoreStyle::Classic, BubbleStyle::Bubbles, FontFace::Sans, 0, 10,
                   u"system"_s},
        DesignSpec{u"focus"_s, u"Focus"_s,
                   u"One centred column under a large sphere; Actions stay hidden until you ask."_s,
                   WindowLayout::Focus, CoreShape::Orb, CoreStyle::Nebula, BubbleStyle::Bubbles, FontFace::Sans, 0, 16,
                   u"ocean-dark"_s},
        DesignSpec{u"console"_s, u"Console"_s,
                   u"A terminal: monospace, square corners, prompt lines, a bar meter in the status line."_s,
                   WindowLayout::Console, CoreShape::Bars, CoreStyle::Storm, BubbleStyle::Lines, FontFace::Mono, 0, 0,
                   u"graphite-dark"_s},
        DesignSpec{u"sidebar"_s, u"Sidebar"_s,
                   u"A hex core and the state in a left column, Actions beside them, the conversation wide."_s,
                   WindowLayout::Sidebar, CoreShape::Hex, CoreStyle::Orbit, BubbleStyle::Cards, FontFace::Sans, 0, 6,
                   u"slate-dark"_s},
        DesignSpec{u"orb"_s, u"Orb"_s,
                   u"Voice first: a huge iris in the middle, the state in large type, a floating input."_s,
                   WindowLayout::Orb, CoreShape::Iris, CoreStyle::Pulse, BubbleStyle::Bubbles, FontFace::Sans, 1, 22,
                   u"violet-dark"_s},
        DesignSpec{u"dashboard"_s, u"Dashboard"_s,
                   u"Every part a card: the faceted core, the chat, the Actions, on a light board."_s,
                   WindowLayout::Dashboard, CoreShape::Crystal, CoreStyle::Comet, BubbleStyle::Cards, FontFace::Sans, 0, 12,
                   u"steel-light"_s},
        DesignSpec{u"gauge"_s, u"Gauge"_s,
                   u"An arc gauge with a needle and a VU meter for the microphone; Actions along the bottom."_s,
                   WindowLayout::Bottom, CoreShape::Arc, CoreStyle::Aurora, BubbleStyle::Bubbles, FontFace::Mono, 0, 4,
                   u"amber-dark"_s},
        DesignSpec{u"mission"_s, u"Mission Control"_s,
                   u"A control room: the agents on the left, the mission board in the middle, live activity on the right."_s,
                   WindowLayout::MissionControl, CoreShape::Ring, CoreStyle::Comet, BubbleStyle::Cards, FontFace::Sans, 0, 10,
                   u"mission-dark"_s},
        DesignSpec{u"paper"_s, u"Paper"_s,
                   u"Light and flat: a serif face, a plain dot for a core, ruled lines instead of bubbles."_s,
                   WindowLayout::Studio, CoreShape::Dot, CoreStyle::Minimal, BubbleStyle::Lines, FontFace::Serif, 1, 2,
                   u"sand-light"_s},
    };
    return designs;
}

std::optional<DesignSpec> NexaDesigns::find(const QString &id) {
    for (const DesignSpec &design : all()) {
        if (design.id == id) return design;
    }
    return std::nullopt;
}

DesignSpec NexaDesigns::resolve(const QString &id) {
    if (const std::optional<DesignSpec> design = find(id)) return *design;
    return all().first();
}

QString NexaDesigns::layoutName(WindowLayout layout) {
    switch (layout) {
    case WindowLayout::Studio: return u"Studio"_s;
    case WindowLayout::Focus: return u"Focus"_s;
    case WindowLayout::Console: return u"Console"_s;
    case WindowLayout::Sidebar: return u"Sidebar"_s;
    case WindowLayout::Orb: return u"Orb"_s;
    case WindowLayout::Dashboard: return u"Dashboard"_s;
    case WindowLayout::Bottom: return u"Bottom"_s;
    case WindowLayout::MissionControl: return u"Mission Control"_s;
    }
    return {};
}

QString NexaDesigns::fontFamily(FontFace face) {
    switch (face) {
    case FontFace::Sans: return QFontDatabase::systemFont(QFontDatabase::GeneralFont).family();
    case FontFace::Mono: return QFontDatabase::systemFont(QFontDatabase::FixedFont).family();
    case FontFace::Serif: return u"serif"_s;    // fontconfig's alias: Noto Serif, DejaVu Serif, …
    }
    return {};
}

} // namespace nexa
