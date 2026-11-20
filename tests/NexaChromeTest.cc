// ui/NexaChrome: the chrome of a theme — how panels are framed (brackets, neon, cut corners, glass, a rail) and
// what lies behind them (a grid, dots, a glow, a horizon, scan lines) — painted and checked pixel by pixel.

#include "ui/NexaChrome.h"

#include "QtStringMakers.h"

#include <QImage>
#include <QPainter>

#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace nexa;
using namespace Qt::StringLiterals;

namespace {

const ChromeColours kColours{QColor(0x07, 0x11, 0x1F), QColor(0x0B, 0x1A, 0x2C), QColor(0x1B, 0x33, 0x50),
                             QColor(0x4F, 0xD1, 0xFF), QColor(0xE6, 0xF1, 0xFF)};

QImage ground(int width, int height) {
    QImage image(width, height, QImage::Format_ARGB32_Premultiplied);
    image.fill(kColours.ground);
    return image;
}

QImage backdropPicture(Backdrop backdrop) {
    QImage image = ground(240, 180);
    QPainter painter(&image);
    paintBackdrop(painter, QRectF(0, 0, 240, 180), backdrop, kColours);
    return image;
}

QImage panelPicture(PanelChrome chrome, double radius = 10) {
    QImage image = ground(140, 90);
    QPainter painter(&image);
    paintPanelChrome(painter, QRectF(0, 0, 140, 90), chrome, radius, kColours);
    return image;
}

// How far two colours are apart, in RGB.
double apart(const QColor &a, const QColor &b) {
    return std::abs(a.red() - b.red()) + std::abs(a.green() - b.green()) + std::abs(a.blue() - b.blue());
}

bool nearer(const QColor &colour, const QColor &to, const QColor &than) {
    return apart(colour, to) < apart(colour, than);
}

int changedPixels(const QImage &image) {
    int changed = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (image.pixelColor(x, y) != kColours.ground) ++changed;
        }
    }
    return changed;
}

int brightest(const QImage &image) {
    int most = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) most = std::max(most, image.pixelColor(x, y).lightness());
    }
    return most;
}

} // namespace

TEST_CASE("Every chrome and backdrop has a name, a key that round-trips, and the panel chromes a blurb") {
    for (const PanelChrome chrome : kPanelChromes) {
        INFO(panelChromeKey(chrome).toStdString());
        CHECK_FALSE(panelChromeName(chrome).isEmpty());
        CHECK_FALSE(panelChromeBlurb(chrome).isEmpty());
        CHECK(panelChromeFromKey(panelChromeKey(chrome)) == chrome);
        CHECK(panelChromeKey(chrome) == panelChromeKey(chrome).toLower());
    }
    for (const Backdrop backdrop : kBackdrops) {
        INFO(backdropKey(backdrop).toStdString());
        CHECK_FALSE(backdropName(backdrop).isEmpty());
        CHECK(backdropFromKey(backdropKey(backdrop)) == backdrop);
    }
    CHECK_FALSE(panelChromeFromKey(u"neon"_s).has_value());
    CHECK_FALSE(backdropFromKey(u"stars"_s).has_value());
    CHECK(panelChromeFromKey(u"brackets"_s) == PanelChrome::Brackets);
    CHECK(backdropFromKey(u"grid"_s) == Backdrop::Grid);
    const ThemeChrome none;
    CHECK(none.panels == PanelChrome::Flat);
    CHECK(none.backdrop == Backdrop::Plain);
}

TEST_CASE("A backdrop draws over the ground, stays dark, and Plain leaves the ground alone") {
    CHECK(changedPixels(backdropPicture(Backdrop::Plain)) == 0);
    for (const Backdrop backdrop : kBackdrops) {
        if (backdrop == Backdrop::Plain) continue;
        INFO(backdropKey(backdrop).toStdString());
        const QImage picture = backdropPicture(backdrop);
        // Something is drawn, over a good part of the window — but never anything bright: the panels and the
        // words must stay what the eye goes to.
        CHECK(changedPixels(picture) > 240 * 180 / 20);
        CHECK(brightest(picture) < 90);
        // The middle is lifted towards the accent (the glow behind the core), the corners are not.
        const QColor middle = picture.pixelColor(120, 76);
        CHECK(apart(middle, kColours.ground) > 0);
        if (backdrop != Backdrop::Horizon) CHECK(apart(middle, kColours.accent) < apart(picture.pixelColor(2, 2), kColours.accent));
    }
    // The grid is lines: a point on a line differs from the point just beside it.
    const QImage grid = backdropPicture(Backdrop::Grid);
    bool line = false;
    for (int x = 1; x < 60 && !line; ++x) line = apart(grid.pixelColor(x, 100), grid.pixelColor(x + 1, 100)) > 3;
    CHECK(line);
    // Scan lines are rows: along a row nothing changes but the glow; down a column it does.
    const QImage scan = backdropPicture(Backdrop::Scan);
    bool rows = false;
    for (int y = 1; y < 12 && !rows; ++y) rows = apart(scan.pixelColor(10, y), scan.pixelColor(10, y + 1)) > 3;
    CHECK(rows);
    // The horizon runs to the bottom: lines cross the lower band, none the top one (the glow there is smooth).
    const QImage horizon = backdropPicture(Backdrop::Horizon);
    int lower = 0;
    int upper = 0;
    for (int x = 1; x < 240; ++x) {
        if (apart(horizon.pixelColor(x, 172), horizon.pixelColor(x - 1, 172)) > 6) ++lower;
        if (apart(horizon.pixelColor(x, 6), horizon.pixelColor(x - 1, 6)) > 6) ++upper;
    }
    CHECK(lower > 0);
    CHECK(upper == 0);
}

TEST_CASE("Each panel chrome fills its panel and frames it its own way; Flat leaves it to the stylesheet") {
    CHECK(changedPixels(panelPicture(PanelChrome::Flat)) == 0);
    for (const PanelChrome chrome : kPanelChromes) {
        if (chrome == PanelChrome::Flat) continue;
        INFO(panelChromeKey(chrome).toStdString());
        const QImage picture = panelPicture(chrome);
        // The inside is the panel's colour (glass lets a little ground through), never the bare ground.
        const QColor middle = picture.pixelColor(70, 45);
        CHECK(nearer(middle, kColours.panel, kColours.ground));
        // Dark everywhere but the chrome itself.
        CHECK(brightest(picture) <= kColours.accent.lightness() + 1);
    }
    // Brackets: the accent at the corners, a hairline between them, square.
    const QImage brackets = panelPicture(PanelChrome::Brackets);
    CHECK(nearer(brackets.pixelColor(1, 1), kColours.accent, kColours.line));
    CHECK(nearer(brackets.pixelColor(138, 88), kColours.accent, kColours.line));
    CHECK(nearer(brackets.pixelColor(70, 0), kColours.line, kColours.accent));
    // Chamfer: the corner itself is cut away — still the ground — and the cut is stroked in the accent.
    const QImage chamfer = panelPicture(PanelChrome::Chamfer);
    CHECK(chamfer.pixelColor(0, 0) == kColours.ground);
    CHECK(chamfer.pixelColor(139, 89) == kColours.ground);
    CHECK(nearer(chamfer.pixelColor(6, 6), kColours.accent, kColours.panel));
    CHECK(chamfer.pixelColor(70, 45) == kColours.panel);
    CHECK_FALSE(panelOutline(QRectF(0, 0, 140, 90), PanelChrome::Chamfer, 10).contains(QPointF(1, 1)));
    CHECK(panelOutline(QRectF(0, 0, 140, 90), PanelChrome::Brackets, 10).contains(QPointF(1, 1)));
    // Glow: the edge is the accent, and just inside it the panel is lit.
    const QImage glow = panelPicture(PanelChrome::Glow);
    CHECK(nearer(glow.pixelColor(70, 0), kColours.accent, kColours.line));
    CHECK(apart(glow.pixelColor(70, 3), kColours.panel) > apart(glow.pixelColor(70, 45), kColours.panel));
    // Glass: the pane lets the ground through, and the light along the top edge is the text's colour, faintly.
    const QImage glass = panelPicture(PanelChrome::Glass);
    CHECK(glass.pixelColor(70, 45) != kColours.panel);
    CHECK(glass.pixelColor(70, 1).lightness() > glass.pixelColor(70, 45).lightness());
    // Rail: the accent down the left edge, nowhere else.
    const QImage rail = panelPicture(PanelChrome::Rail);
    CHECK(nearer(rail.pixelColor(2, 45), kColours.accent, kColours.panel));
    CHECK(nearer(rail.pixelColor(137, 45), kColours.panel, kColours.accent));
    // A chrome keeps as much of the design's corner as suits it.
    CHECK(panelRadius(PanelChrome::Flat, 10) == 10);
    CHECK(panelRadius(PanelChrome::Glow, 10) == 10);
    CHECK(panelRadius(PanelChrome::Glass, 16) == 16);
    CHECK(panelRadius(PanelChrome::Brackets, 10) <= 2);
    CHECK(panelRadius(PanelChrome::Chamfer, 10) == 0);
    CHECK(panelRadius(PanelChrome::Rail, 22) <= 6);
}
