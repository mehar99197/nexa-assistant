#include "ui/NexaThemes.h"

#include <algorithm>
#include <cmath>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

struct Family {
    const char *name;
    double hue;             // degrees
    double saturation;      // 0 … 1: how much colour the neutrals and states carry
    CoreStyle style;
};

// Nexa first, as the design draws it; then the families, each with its own motion.
constexpr Family kFamilies[] = {
    {"Nexa", 174, 1.0, CoreStyle::Classic},
    {"Ocean", 205, 0.9, CoreStyle::Orbit},
    {"Sapphire", 225, 0.9, CoreStyle::Pulse},
    {"Indigo", 243, 0.85, CoreStyle::Aurora},
    {"Violet", 265, 0.85, CoreStyle::Comet},
    {"Orchid", 285, 0.8, CoreStyle::Nebula},
    {"Magenta", 310, 0.85, CoreStyle::Storm},
    {"Rose", 340, 0.85, CoreStyle::Minimal},
    {"Ruby", 355, 0.9, CoreStyle::Classic},
    {"Coral", 10, 0.9, CoreStyle::Orbit},
    {"Ember", 18, 0.95, CoreStyle::Pulse},
    {"Copper", 26, 0.75, CoreStyle::Aurora},
    {"Amber", 36, 0.95, CoreStyle::Comet},
    {"Gold", 48, 0.9, CoreStyle::Nebula},
    {"Sand", 42, 0.35, CoreStyle::Storm},
    {"Lime", 82, 0.8, CoreStyle::Minimal},
    {"Forest", 130, 0.6, CoreStyle::Classic},
    {"Jade", 155, 0.75, CoreStyle::Orbit},
    {"Mint", 165, 0.7, CoreStyle::Pulse},
    {"Aqua", 186, 0.9, CoreStyle::Aurora},
    {"Ice", 196, 0.7, CoreStyle::Comet},
    {"Steel", 212, 0.3, CoreStyle::Nebula},
    {"Slate", 222, 0.18, CoreStyle::Storm},
    {"Graphite", 0, 0.0, CoreStyle::Minimal},
    {"Lavender", 258, 0.55, CoreStyle::Classic},
    {"Plum", 300, 0.55, CoreStyle::Orbit},
};

QColor hsl(double hue, double saturation, double lightness) {
    hue = std::fmod(std::fmod(hue, 360.0) + 360.0, 360.0);
    return QColor::fromHslF(hue / 360.0, std::clamp(saturation, 0.0, 1.0), std::clamp(lightness, 0.0, 1.0));
}

double luminance(const QColor &colour) {
    const auto channel = [](double c) { return c <= 0.03928 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); };
    return 0.2126 * channel(colour.redF()) + 0.7152 * channel(colour.greenF()) + 0.0722 * channel(colour.blueF());
}

double contrast(const QColor &a, const QColor &b) {
    const double la = luminance(a);
    const double lb = luminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

// A yellow at mid lightness all but vanishes on white, a navy on near-black: every colour is
// pushed away from the ground until it stands out by at least `minimum` (lighter on dark
// grounds, darker on light ones), so the formulas above never have to know the hue's weight.
QColor standingOut(QColor colour, const QColor &ground, double minimum, ThemeScheme scheme) {
    for (int step = 0; step < 40 && contrast(colour, ground) < minimum; ++step) {
        const double lightness = double(colour.lightnessF()) + (scheme == ThemeScheme::Dark ? 0.02 : -0.02);
        colour = QColor::fromHslF(std::max(0.0, double(colour.hslHueF())), colour.hslSaturationF(),
                                  std::clamp(lightness, 0.0, 1.0));
    }
    return colour;
}

// The design's shape of palette, around another hue: the neutrals carry a trace of it, the
// states keep their meaning (idle on the hue, acting on its complement, error warm) in its light.
ThemeTokens tokensAround(double hue, double sat, ThemeScheme scheme) {
    ThemeTokens t;
    if (scheme == ThemeScheme::Dark) {
        t.ground = hsl(hue, 0.30 * sat, 0.055);
        t.panel = hsl(hue, 0.26 * sat, 0.09);
        t.panelRaised = hsl(hue, 0.24 * sat, 0.12);
        t.line = hsl(hue, 0.20 * sat, 0.20);
        t.text = hsl(hue, 0.12 * sat, 0.93);
        t.muted = hsl(hue, 0.10 * sat, 0.66);
        t.idle = hsl(hue, 0.55 * sat + 0.05, 0.50);
        t.wake = hsl(hue + 40, 0.55 * sat + 0.1, 0.62);
        t.listen = hsl(hue + 8, 0.70 * sat + 0.1, 0.63);
        t.stt = hsl(hue + 75, 0.70 * sat + 0.1, 0.76);
        t.think = hsl(hue + 125, 0.65 * sat + 0.1, 0.78);
        t.act = hsl(hue + 185, 0.85 * sat + 0.1, 0.62);
        t.speak = hsl(hue - 22, 0.65 * sat + 0.1, 0.68);
        t.fast = hsl(hue, 0.6 * sat, 0.94);
        t.error = hsl(10, 0.92, 0.68);
        t.onAccent = hsl(hue, 0.5 * sat, 0.08);
    } else {
        t.ground = hsl(hue, 0.25 * sat, 0.97);
        t.panel = hsl(hue, 0.30 * sat, 0.995);
        t.panelRaised = hsl(hue, 0.25 * sat, 0.94);
        t.line = hsl(hue, 0.22 * sat, 0.86);
        t.text = hsl(hue, 0.25 * sat, 0.11);
        t.muted = hsl(hue, 0.15 * sat, 0.42);
        t.idle = hsl(hue, 0.60 * sat + 0.05, 0.38);
        t.wake = hsl(hue + 40, 0.60 * sat + 0.1, 0.40);
        t.listen = hsl(hue + 8, 0.75 * sat + 0.1, 0.42);
        t.stt = hsl(hue + 75, 0.65 * sat + 0.1, 0.52);
        t.think = hsl(hue + 125, 0.60 * sat + 0.1, 0.50);
        t.act = hsl(hue + 185, 0.85 * sat + 0.1, 0.42);
        t.speak = hsl(hue - 22, 0.60 * sat + 0.1, 0.40);
        t.fast = hsl(hue, 0.60 * sat, 0.30);
        t.error = hsl(8, 0.75, 0.48);
        t.onAccent = hsl(hue, 0.3 * sat, 0.06);
    }
    t.muted = standingOut(t.muted, t.ground, 3.5, scheme);
    for (QColor *state : {&t.idle, &t.wake, &t.listen, &t.stt, &t.think, &t.act, &t.speak, &t.fast, &t.error}) {
        *state = standingOut(*state, t.ground, 3.0, scheme);
    }
    return t;
}

QList<ThemePack> generate() {
    QList<ThemePack> packs;
    for (const Family &family : kFamilies) {
        for (const ThemeScheme scheme : {ThemeScheme::Dark, ThemeScheme::Light}) {
            ThemePack pack;
            pack.family = QString::fromLatin1(family.name);
            pack.scheme = scheme;
            pack.style = family.style;
            pack.id = pack.family.toLower() + u'-' + NexaTheme::schemeKey(scheme);
            pack.name = u"%1 · %2"_s.arg(pack.family, scheme == ThemeScheme::Dark ? u"Dark"_s : u"Light"_s);
            pack.tokens = pack.family == u"Nexa"_s ? NexaTheme::tokens(scheme) : tokensAround(family.hue, family.saturation, scheme);
            packs.append(pack);
        }
    }
    return packs;
}

} // namespace

const QList<ThemePack> &NexaThemes::all() {
    static const QList<ThemePack> packs = generate();
    return packs;
}

std::optional<ThemePack> NexaThemes::find(const QString &id) {
    for (const ThemePack &pack : all()) {
        if (pack.id == id) return pack;
    }
    return std::nullopt;
}

ThemePack NexaThemes::resolve(const QString &setting) {
    if (setting == u"system"_s || setting == u"dark"_s || setting == u"light"_s || setting.isEmpty()) {
        return *find(u"nexa-"_s + NexaTheme::schemeKey(NexaTheme::schemeFor(setting)));
    }
    if (const std::optional<ThemePack> pack = find(setting)) return *pack;
    return *find(u"nexa-dark"_s);
}

QString NexaThemes::settingFor(const ThemePack &pack) {
    return pack.family == u"Nexa"_s ? NexaTheme::schemeKey(pack.scheme) : pack.id;
}

QString NexaThemes::siblingId(const ThemePack &pack) {
    const ThemeScheme other = pack.scheme == ThemeScheme::Dark ? ThemeScheme::Light : ThemeScheme::Dark;
    return pack.family.toLower() + u'-' + NexaTheme::schemeKey(other);
}

QStringList NexaThemes::families() {
    QStringList names;
    for (const Family &family : kFamilies) names.append(QString::fromLatin1(family.name));
    return names;
}

} // namespace nexa
