// ui/NexaDesigns: the designs — each a different arrangement and core, not another colour — and
// what each one asks of the stylesheet.

#include "ui/NexaDesigns.h"
#include "ui/NexaTheme.h"
#include "ui/NexaThemes.h"

#include "QtStringMakers.h"

#include <QSet>

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace Qt::StringLiterals;

TEST_CASE("Every design is its own arrangement and core, drawn in a palette that exists") {
    const QList<DesignSpec> &designs = NexaDesigns::all();
    CHECK(designs.size() >= 8);
    QSet<QString> ids;
    QSet<QString> arrangements;     // layout + shape
    QSet<int> shapes;
    QSet<int> layouts;
    for (const DesignSpec &design : designs) {
        INFO(design.id.toStdString());
        ids.insert(design.id);
        arrangements.insert(u"%1/%2"_s.arg(NexaDesigns::layoutName(design.layout), coreShapeName(design.shape)));
        shapes.insert(static_cast<int>(design.shape));
        layouts.insert(static_cast<int>(design.layout));
        CHECK_FALSE(design.name.isEmpty());
        CHECK_FALSE(design.blurb.isEmpty());
        CHECK(design.radius >= 0);
        CHECK(design.radius <= 24);
        // The palette it was drawn in is a real one, and the sheet comes out whole.
        const ThemePack pack = NexaThemes::resolve(design.palette);
        if (design.palette != u"system"_s) CHECK(NexaThemes::settingFor(pack) == design.palette);
        const QString sheet = NexaTheme::styleSheet(pack.tokens, pack.style, design);
        CHECK_FALSE(sheet.contains(u'@'));
        CHECK(sheet.contains(u"qproperty-coreShape: %1;"_s.arg(static_cast<int>(design.shape))));
        CHECK(sheet.contains(u"qproperty-bubbleStyle: %1;"_s.arg(static_cast<int>(design.bubbles))));
        CHECK(sheet.contains(u"border-radius: %1px"_s.arg(design.radius)));
        CHECK(sheet.contains(u"font-family: \"%1\""_s.arg(NexaDesigns::fontFamily(design.font))));
    }
    CHECK(ids.size() == designs.size());
    CHECK(arrangements.size() == designs.size());    // no two designs share a layout and a core
    CHECK(shapes.size() == kCoreShapeCount);          // every core shape is someone's
    CHECK(layouts.size() == kWindowLayoutCount);      // and every arrangement too
}

TEST_CASE("A design is found by id, and the type faces resolve") {
    CHECK(NexaDesigns::resolve(u"console"_s).shape == CoreShape::Bars);
    CHECK(NexaDesigns::resolve(u"console"_s).font == FontFace::Mono);
    CHECK(NexaDesigns::resolve(u"no-such-design"_s).id == u"studio"_s);
    CHECK(NexaDesigns::find(u"orb"_s)->layout == WindowLayout::Orb);
    CHECK(NexaDesigns::fontFamily(FontFace::Mono) == NexaTheme::monoFamily());
    CHECK_FALSE(NexaDesigns::fontFamily(FontFace::Sans).isEmpty());
    CHECK(NexaDesigns::fontFamily(FontFace::Serif) == u"serif"_s);
    for (int i = 0; i < kWindowLayoutCount; ++i) CHECK_FALSE(NexaDesigns::layoutName(static_cast<WindowLayout>(i)).isEmpty());
}
