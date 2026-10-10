// ui/NexaThemes: the catalogue of themes — enough of them, each readable, each with a motion
// style — and what the styles do to the core's frames.

#include "ui/NexaCoreSpec.h"
#include "ui/NexaTheme.h"
#include "ui/NexaThemes.h"

#include "QtStringMakers.h"

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

TEST_CASE("There are more than fifty themes, each readable, each in both schemes, every style in use") {
    const QList<ThemePack> &packs = NexaThemes::all();
    CHECK(packs.size() >= 50);
    QSet<QString> ids;
    QSet<QString> names;
    QSet<int> styles;
    int dark = 0;
    for (const ThemePack &pack : packs) {
        ids.insert(pack.id);
        names.insert(pack.name);
        styles.insert(static_cast<int>(pack.style));
        if (pack.scheme == ThemeScheme::Dark) ++dark;
        INFO(pack.id.toStdString());
        // Text on the ground and on a panel reads; the states stand out from the ground.
        CHECK(contrast(pack.tokens.text, pack.tokens.ground) >= 7.0);
        CHECK(contrast(pack.tokens.text, pack.tokens.panel) >= 7.0);
        CHECK(contrast(pack.tokens.muted, pack.tokens.ground) >= 3.0);
        for (const CoreMode mode : kCoreModes) CHECK(contrast(pack.tokens.stateColor(mode), pack.tokens.ground) >= 2.5);
        // The stylesheet fills in, and carries the motion style for the core.
        const QString sheet = NexaTheme::styleSheet(pack.tokens, pack.style);
        CHECK_FALSE(sheet.contains(u'@'));
        CHECK(sheet.contains(u"qproperty-coreStyle: %1;"_s.arg(static_cast<int>(pack.style))));
        CHECK(NexaThemes::find(NexaThemes::siblingId(pack)).has_value());
        CHECK(NexaThemes::find(NexaThemes::siblingId(pack))->scheme != pack.scheme);
    }
    CHECK(ids.size() == packs.size());
    CHECK(names.size() == packs.size());
    CHECK(styles.size() == kCoreStyleCount);
    CHECK(dark * 2 == packs.size());
    CHECK(NexaThemes::families().size() * 2 == packs.size());
}

TEST_CASE("The setting resolves to a pack and back") {
    CHECK(NexaThemes::resolve(u"dark"_s).id == u"nexa-dark"_s);
    CHECK(NexaThemes::resolve(u"light"_s).id == u"nexa-light"_s);
    CHECK(NexaThemes::resolve(u"system"_s).family == u"Nexa"_s);
    CHECK(NexaThemes::resolve(u"ocean-light"_s).id == u"ocean-light"_s);
    CHECK(NexaThemes::resolve(u"ocean-light"_s).style == CoreStyle::Orbit);
    CHECK(NexaThemes::resolve(u"no-such-theme"_s).id == u"nexa-dark"_s);
    CHECK(NexaThemes::settingFor(*NexaThemes::find(u"nexa-light"_s)) == u"light"_s);
    CHECK(NexaThemes::settingFor(*NexaThemes::find(u"violet-dark"_s)) == u"violet-dark"_s);
    CHECK(NexaThemes::siblingId(*NexaThemes::find(u"violet-dark"_s)) == u"violet-light"_s);
    // The Nexa pair is the design's own.
    CHECK(NexaThemes::find(u"nexa-dark"_s)->tokens.ground.name() == u"#0d1014"_s);
    CHECK(NexaThemes::find(u"nexa-dark"_s)->tokens.idle.name() == u"#46b7b0"_s);
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
