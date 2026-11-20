// ui/NexaThemes: the catalogue of themes — fifty and more, every one dark, each readable, each with a
// motion style, in categories — what an old setting resolves to, and what the styles do to the core's frames.

#include "ui/NexaChrome.h"
#include "ui/NexaCoreSpec.h"
#include "ui/NexaDesigns.h"
#include "ui/NexaTheme.h"
#include "ui/NexaThemes.h"

#include "QtStringMakers.h"

#include <QRegularExpression>
#include <QSet>

#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace nexa;
using namespace Qt::StringLiterals;

namespace {

// Relative luminance, as contrast is judged.
double luminance(const QColor &colour) {
    const auto channel = [](double c) { return c <= 0.03928 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); };
    return 0.2126 * channel(colour.redF()) + 0.7152 * channel(colour.greenF()) + 0.0722 * channel(colour.blueF());
}

double contrast(const QColor &a, const QColor &b) {
    const double la = luminance(a);
    const double lb = luminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

} // namespace

TEST_CASE("There are more than eighty themes, every one dark and readable, in categories, every style in use") {
    const QList<ThemePack> &packs = NexaThemes::all();
    CHECK(packs.size() >= 80);
    QSet<QString> ids;
    QSet<QString> names;
    QSet<QString> looks;
    QSet<int> styles;
    const QRegularExpression kebab(u"^[a-z]+(-[a-z]+)*$"_s);
    for (const ThemePack &pack : packs) {
        INFO(pack.id.toStdString());
        ids.insert(pack.id);
        names.insert(pack.name);
        // No two packs look the same: the ground, its accent and its states together.
        looks.insert(u"%1/%2/%3/%4"_s.arg(pack.tokens.ground.name(), pack.tokens.idle.name(), pack.tokens.listen.name(),
                                           pack.tokens.act.name()));
        styles.insert(static_cast<int>(pack.style));
        CHECK(kebab.match(pack.id).hasMatch());
        CHECK_FALSE(pack.name.isEmpty());
        CHECK(NexaThemes::categories().contains(pack.category));
        // Dark: a deep ground, panels barely lighter, light text.
        CHECK(pack.tokens.ground.lightness() < 40);
        CHECK(pack.tokens.panel.lightness() < 70);
        CHECK(pack.tokens.text.lightness() > 200);
        // Text on the ground and on the panels reads; the states stand out from the ground.
        CHECK(contrast(pack.tokens.text, pack.tokens.ground) >= 7.0);
        CHECK(contrast(pack.tokens.text, pack.tokens.panel) >= 7.0);
        CHECK(contrast(pack.tokens.text, pack.tokens.panelRaised) >= 7.0);
        CHECK(contrast(pack.tokens.muted, pack.tokens.ground) >= 3.0);
        for (const CoreMode mode : kCoreModes) CHECK(contrast(pack.tokens.stateColor(mode), pack.tokens.ground) >= 2.5);
        // The stylesheet fills in, and carries the motion style for the core and the chrome for the panels and the
        // ground; a pack with a chrome of its own has the sheet leave the panels bare for the frames to paint.
        const QString sheet = NexaTheme::styleSheet(pack.tokens, pack.style, NexaDesigns::resolve(u"mission"_s), pack.chrome);
        CHECK_FALSE(sheet.contains(u'@'));
        CHECK(sheet.contains(u"qproperty-coreStyle: %1;"_s.arg(static_cast<int>(pack.style))));
        CHECK(sheet.contains(u"qproperty-panelChrome: %1;"_s.arg(static_cast<int>(pack.chrome.panels))));
        CHECK(sheet.contains(u"qproperty-backdrop: %1;"_s.arg(static_cast<int>(pack.chrome.backdrop))));
        if (pack.chrome.panels == PanelChrome::Flat) {
            CHECK(sheet.contains(u"nexa--MissionPanel {\n    background: %1;\n    border: 1px solid %2;"_s.arg(
                pack.tokens.panel.name(), pack.tokens.line.name())));
        } else {
            CHECK(sheet.contains(u"nexa--MissionPanel {\n    background: transparent;\n    border: 0;"_s));
        }
        // The setting round-trips.
        CHECK(NexaThemes::resolve(NexaThemes::settingFor(pack)).id == pack.id);
    }
    CHECK(ids.size() == packs.size());
    CHECK(names.size() == packs.size());
    CHECK(looks.size() == packs.size());
    CHECK(styles.size() == kCoreStyleCount);
    // Not one light pack anywhere.
    for (const ThemePack &pack : packs) CHECK(pack.id.endsWith(u"-light"_s) == false);
}

TEST_CASE("The categories: ten and more, none empty, each a mix of motion styles, and the step through one") {
    const QStringList categories = NexaThemes::categories();
    CHECK(categories.size() >= 10);
    CHECK(categories.first() == u"Nexa"_s);
    CHECK(categories.at(1) == u"HUD"_s);      // the owner's pictures, right after the house packs
    for (const QString &name : {u"Neon"_s, u"Nature"_s, u"Ocean"_s, u"Space"_s, u"Fire & Ember"_s, u"Minimal & Mono"_s,
                                u"Retro"_s, u"Cyber"_s, u"Pastel Night"_s, u"Gems"_s}) {
        CHECK(categories.contains(name));
    }
    qsizetype counted = 0;
    for (const QString &category : categories) {
        INFO(category.toStdString());
        const QList<ThemePack> packs = NexaThemes::inCategory(category);
        CHECK(packs.size() >= 2);
        counted += packs.size();
        QSet<int> styles;
        for (const ThemePack &pack : packs) {
            CHECK(pack.category == category);
            styles.insert(static_cast<int>(pack.style));
        }
        CHECK(styles.size() >= 2);
        // The toolbar's button walks the category and comes back to the start.
        ThemePack walked = packs.first();
        for (qsizetype i = 0; i < packs.size(); ++i) {
            CHECK(walked.id == packs.at(i).id);
            walked = NexaThemes::nextInCategory(walked);
            CHECK(walked.category == category);
        }
        CHECK(walked.id == packs.first().id);
    }
    CHECK(counted == NexaThemes::all().size());
    CHECK(NexaThemes::inCategory(u"No such"_s).isEmpty());
}

TEST_CASE("The setting resolves to a pack and back, old settings included") {
    // The scheme words, from when there was a light look: Nexa's own, dark.
    CHECK(NexaThemes::resolve(u"dark"_s).id == u"nexa-dark"_s);
    CHECK(NexaThemes::resolve(u"light"_s).id == u"nexa-dark"_s);
    CHECK(NexaThemes::resolve(u"system"_s).id == u"nexa-dark"_s);
    CHECK(NexaThemes::resolve(QString()).id == u"nexa-dark"_s);
    CHECK(NexaThemes::resolve(u"no-such-theme"_s).id == u"nexa-dark"_s);
    CHECK(NexaThemes::defaultId() == u"nexa-dark"_s);
    // The old catalogue's ids, dark and light, land on the nearest pack of the new one.
    CHECK(NexaThemes::resolve(u"ocean-dark"_s).id == u"ocean"_s);
    CHECK(NexaThemes::resolve(u"ocean-light"_s).id == u"ocean"_s);
    CHECK(NexaThemes::resolve(u"ocean-light"_s).style == CoreStyle::Orbit);
    CHECK(NexaThemes::resolve(u"violet-dark"_s).id == u"neon-violet"_s);
    CHECK(NexaThemes::resolve(u"steel-light"_s).id == u"mono-steel"_s);
    CHECK(NexaThemes::resolve(u"sand-light"_s).id == u"mono-sepia"_s);
    CHECK(NexaThemes::resolve(u"graphite-dark"_s).id == u"mono-graphite"_s);
    CHECK(NexaThemes::resolve(u"mission-light"_s).id == u"mission-dark"_s);
    CHECK(NexaThemes::resolve(u"nexa-light"_s).id == u"nexa-dark"_s);
    for (const QString &family : {u"sapphire"_s, u"indigo"_s, u"orchid"_s, u"magenta"_s, u"rose"_s, u"ruby"_s, u"coral"_s,
                                  u"ember"_s, u"copper"_s, u"amber"_s, u"gold"_s, u"lime"_s, u"forest"_s, u"jade"_s,
                                  u"mint"_s, u"aqua"_s, u"ice"_s, u"slate"_s, u"lavender"_s, u"plum"_s}) {
        INFO(family.toStdString());
        CHECK(NexaThemes::resolve(family + u"-dark"_s).id != u"nexa-dark"_s);
        CHECK(NexaThemes::resolve(family + u"-dark"_s).id == NexaThemes::resolve(family + u"-light"_s).id);
    }
    // A pack's setting is its id; the house packs keep theirs.
    CHECK(NexaThemes::settingFor(*NexaThemes::find(u"neon-cyan"_s)) == u"neon-cyan"_s);
    CHECK(NexaThemes::settingFor(*NexaThemes::find(u"nexa-dark"_s)) == u"nexa-dark"_s);
    CHECK(NexaThemes::find(u"mission-dark"_s).has_value());
    CHECK_FALSE(NexaThemes::find(u"ocean-dark"_s).has_value());
    // Nexa's own pack is the design's own, untouched.
    CHECK(NexaThemes::find(u"nexa-dark"_s)->tokens.ground.name() == u"#0d1014"_s);
    CHECK(NexaThemes::find(u"nexa-dark"_s)->tokens.idle.name() == u"#46b7b0"_s);
    CHECK(NexaThemes::find(u"nexa-dark"_s)->category == u"Nexa"_s);
    for (const CoreStyle style : kCoreStyles) {
        CHECK(coreStyleFromKey(coreStyleKey(style)) == style);
        CHECK_FALSE(coreStyleBlurb(style).isEmpty());
    }
}

TEST_CASE("Each motion style bends the frames its own way") {
    const CoreFrame classic = coreFrame(CoreMode::Idle, 0, 3.3, 3.3, CoreStyle::Classic);
    CHECK_FALSE(classic.satelliteAngles);
    CHECK(classic.sonarRadius == 0);
    CHECK(classic.hueShift == 0);
    CHECK(classic.ring);

    CHECK(coreFrame(CoreMode::Idle, 0, 3.3, 3.3, CoreStyle::Orbit).satelliteAngles.has_value());
    CHECK(coreFrame(CoreMode::Idle, 0, 3.3, 3.3, CoreStyle::Pulse).sonarRadius > 0);
    CHECK(std::abs(coreFrame(CoreMode::Idle, 0, 2.25, 2.25, CoreStyle::Aurora).hueShift) > 10.0);
    CHECK(coreFrame(CoreMode::Idle, 0, 2.25, 2.25, CoreStyle::Aurora).haloScale > 1.0);
    const CoreFrame comet = coreFrame(CoreMode::Listening, 0, 3.3, 3.3, CoreStyle::Comet);
    REQUIRE(comet.cometAngle);
    REQUIRE(comet.secondCometAngle);
    CHECK(std::abs(std::fmod(*comet.secondCometAngle - *comet.cometAngle + 360.0, 360.0) - 180.0) < 1e-6);
    CHECK(coreFrame(CoreMode::Idle, 0, 1.3, 1.3, CoreStyle::Nebula).haloScale > 1.05);
    CHECK(coreFrame(CoreMode::Idle, 0, 1.3, 1.3, CoreStyle::Nebula).coreScale > classic.coreScale);
    CHECK(coreFrame(CoreMode::Idle, 0, 3.3, 3.3, CoreStyle::Storm).tickSwell > 0);
    CHECK(coreStyleFrameRate(CoreStyle::Storm, CoreMode::Idle) == 60);
    CHECK(coreStyleFrameRate(CoreStyle::Classic, CoreMode::Idle) == 30);
    const CoreFrame minimal = coreFrame(CoreMode::Idle, 0, 3.3, 3.3, CoreStyle::Minimal);
    CHECK_FALSE(minimal.ring);
    CHECK_FALSE(minimal.cometAngle);
    CHECK(minimal.coreScale - 1.0 < classic.coreScale - 1.0 + 1e-9);
    // The breath stays within sight in every style and state.
    for (const CoreStyle style : kCoreStyles) {
        for (const CoreMode mode : kCoreModes) {
            const double scale = coreFrame(mode, 0.5, 1.7, 1.7, style).coreScale;
            CHECK(scale >= 0.97 - 1e-9);
            CHECK(scale <= 1.14 + 1e-9);
        }
    }
}

TEST_CASE("The HUD packs are the owner's pictures: each with a chrome or a backdrop, every chrome and backdrop in use") {
    const QList<ThemePack> huds = NexaThemes::inCategory(u"HUD"_s);
    CHECK(huds.size() >= 12);
    QSet<int> chromes;
    QSet<int> backdrops;
    QSet<int> styles;
    for (const ThemePack &pack : huds) {
        INFO(pack.id.toStdString());
        CHECK(pack.id.startsWith(u"hud-"_s));
        CHECK((pack.chrome.panels != PanelChrome::Flat || pack.chrome.backdrop != Backdrop::Plain));
        chromes.insert(static_cast<int>(pack.chrome.panels));
        backdrops.insert(static_cast<int>(pack.chrome.backdrop));
        styles.insert(static_cast<int>(pack.style));
        // Dark text on the accent (the Send button, the chips): the ground, or near it.
        CHECK(pack.tokens.onAccent.lightness() < 40);
    }
    CHECK(chromes.size() == kPanelChromeCount - 1);     // every chrome but Flat
    CHECK(backdrops.size() == kBackdropCount);          // every backdrop, Plain included (Mark III)
    CHECK(styles.size() == kCoreStyleCount);
    // Every pack outside HUD is as it was: the sheet's rounded panels on the bare ground.
    for (const ThemePack &pack : NexaThemes::all()) {
        if (pack.category == u"HUD"_s) continue;
        INFO(pack.id.toStdString());
        CHECK(pack.chrome.panels == PanelChrome::Flat);
        CHECK(pack.chrome.backdrop == Backdrop::Plain);
    }
    // The owner's own console: cyan on navy, orange for what waits on them, bracketed panels over a grid.
    const std::optional<ThemePack> console = NexaThemes::find(u"hud-console"_s);
    REQUIRE(console.has_value());
    CHECK(console->tokens.ground == QColor(0x07, 0x11, 0x1F));
    CHECK(console->tokens.idle == QColor(0x4F, 0xD1, 0xFF));
    CHECK(console->tokens.act == QColor(0xF5, 0xA7, 0x42));
    CHECK(console->chrome.panels == PanelChrome::Brackets);
    CHECK(console->chrome.backdrop == Backdrop::Grid);
    CHECK(console->style == CoreStyle::Orbit);
    // The black one keeps the ground black, with a rail and nothing behind.
    const std::optional<ThemePack> mark = NexaThemes::find(u"hud-mark-three"_s);
    REQUIRE(mark.has_value());
    CHECK(mark->tokens.ground == QColor(Qt::black));
    CHECK(mark->chrome.panels == PanelChrome::Rail);
    CHECK(mark->chrome.backdrop == Backdrop::Plain);
    CHECK(mark->tokens.panel.lightness() > 0);
}

TEST_CASE("The Mission palette is the mock-up's: violet on near-black, green online, amber for the user") {
    const std::optional<ThemePack> pack = NexaThemes::find(u"mission-dark"_s);
    REQUIRE(pack.has_value());
    CHECK(pack->tokens.ground == QColor(0x0B, 0x0D, 0x12));
    CHECK(pack->tokens.idle == QColor(0xA8, 0x55, 0xF7));
    CHECK(pack->tokens.wake == QColor(0x22, 0xC5, 0x5E));
    CHECK(pack->tokens.act == QColor(0xF5, 0x9E, 0x0B));
    CHECK(pack->category == u"Nexa"_s);
    CHECK(pack->style == CoreStyle::Comet);
}
