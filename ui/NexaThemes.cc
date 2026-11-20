#include "ui/NexaThemes.h"

#include <algorithm>
#include <cmath>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

// How a pack is made: the knobs that make one look unlike the next, beyond its hue. Two packs on the same
// hue still differ in the tint and depth of their ground, in how far the states wander and how bright they
// glow, and in the motion of the core.
struct Recipe {
    const char *id;
    const char *name;
    const char *category;
    double hue;          // the accent's hue, degrees: idle, fast and the states take it
    double saturation;   // the accent's saturation, 0 … 1
    double groundHue;    // the tint of the ground and the panels, degrees — warm or cool, not always the accent's
    double groundSat;    // how tinted they are: 0 pure grey … 0.45 deep colour
    double depth;        // the ground's lightness: 0.025 near black … 0.12 soft
    double spread;       // how far the states wander from the accent: 0.2 one colour … 1.5 a rainbow
    double glow;         // how bright the states are: 0.55 dim … 0.74 bright
    CoreStyle style;
};

// The two house packs are hand-tuned (below), the HUD packs are drawn from pictures (below too); the rest are
// recipes. Twelve categories, each a look of its own: HUD is the heads-up displays of the films — bracketed
// panels, neon edges, a grid behind; Neon is hot colour on near black; Nature is green and bark on a mossy
// ground; Ocean is blue-green on deep water; Space is dark and far; Fire & Ember is warm; Minimal & Mono is
// grey with a trace; Retro is phosphor and synthwave; Cyber is green-on-black and chrome; Pastel Night is soft
// and gentle; Gems are jewel tones.
constexpr const char *kNexa = "Nexa";
constexpr const char *kHud = "HUD";
constexpr const char *kNeon = "Neon";
constexpr const char *kNature = "Nature";
constexpr const char *kOcean = "Ocean";
constexpr const char *kSpace = "Space";
constexpr const char *kFire = "Fire & Ember";
constexpr const char *kMono = "Minimal & Mono";
constexpr const char *kRetro = "Retro";
constexpr const char *kCyber = "Cyber";
constexpr const char *kPastel = "Pastel Night";
constexpr const char *kGems = "Gems";

constexpr Recipe kRecipes[] = {
    // Nexa (after the two hand-tuned ones)
    {"nexa-midnight", "Nexa Midnight", kNexa, 174, 0.90, 215, 0.35, 0.040, 1.0, 0.62, CoreStyle::Orbit},
    {"nexa-slate", "Nexa Slate", kNexa, 174, 0.60, 210, 0.12, 0.100, 0.9, 0.60, CoreStyle::Minimal},
    // Neon
    {"neon-cyan", "Neon Cyan", kNeon, 185, 1.00, 200, 0.20, 0.035, 0.8, 0.68, CoreStyle::Pulse},
    {"neon-magenta", "Neon Magenta", kNeon, 310, 1.00, 290, 0.20, 0.035, 0.7, 0.66, CoreStyle::Storm},
    {"neon-lime", "Neon Lime", kNeon, 90, 1.00, 110, 0.15, 0.035, 0.8, 0.62, CoreStyle::Comet},
    {"neon-orange", "Neon Orange", kNeon, 28, 1.00, 20, 0.20, 0.035, 0.9, 0.64, CoreStyle::Pulse},
    {"neon-violet", "Neon Violet", kNeon, 268, 1.00, 255, 0.25, 0.035, 0.8, 0.68, CoreStyle::Aurora},
    {"neon-pink", "Hot Pink", kNeon, 335, 1.00, 320, 0.20, 0.040, 0.7, 0.68, CoreStyle::Orbit},
    {"neon-yellow", "Neon Yellow", kNeon, 55, 1.00, 45, 0.12, 0.035, 1.0, 0.60, CoreStyle::Storm},
    // Nature
    {"nature-forest", "Forest", kNature, 130, 0.60, 140, 0.30, 0.060, 1.0, 0.60, CoreStyle::Classic},
    {"nature-moss", "Moss", kNature, 95, 0.45, 80, 0.18, 0.070, 0.9, 0.60, CoreStyle::Minimal},
    {"nature-pine", "Pine", kNature, 160, 0.55, 170, 0.30, 0.045, 0.8, 0.62, CoreStyle::Nebula},
    {"nature-bark", "Bark", kNature, 30, 0.50, 25, 0.25, 0.070, 1.1, 0.62, CoreStyle::Orbit},
    {"nature-meadow", "Meadow", kNature, 110, 0.70, 120, 0.15, 0.085, 1.2, 0.64, CoreStyle::Aurora},
    {"nature-fern", "Fern", kNature, 145, 0.50, 150, 0.20, 0.090, 0.7, 0.60, CoreStyle::Pulse},
    {"nature-autumn", "Autumn", kNature, 22, 0.80, 15, 0.30, 0.060, 0.9, 0.62, CoreStyle::Comet},
    // Ocean
    {"ocean", "Ocean", kOcean, 205, 0.90, 205, 0.27, 0.055, 1.0, 0.62, CoreStyle::Orbit},
    {"ocean-abyss", "Abyss", kOcean, 215, 0.80, 225, 0.40, 0.030, 0.7, 0.58, CoreStyle::Nebula},
    {"ocean-lagoon", "Lagoon", kOcean, 180, 0.70, 195, 0.30, 0.080, 0.9, 0.64, CoreStyle::Aurora},
    {"ocean-reef", "Reef", kOcean, 170, 0.85, 200, 0.25, 0.060, 1.4, 0.66, CoreStyle::Comet},
    {"ocean-arctic", "Arctic", kOcean, 196, 0.50, 205, 0.15, 0.100, 0.9, 0.70, CoreStyle::Minimal},
    {"ocean-sapphire", "Sapphire", kOcean, 225, 0.90, 230, 0.30, 0.050, 1.0, 0.62, CoreStyle::Pulse},
    {"ocean-tide", "Tide", kOcean, 190, 0.60, 215, 0.20, 0.070, 0.6, 0.60, CoreStyle::Classic},
    // Space
    {"space-nebula", "Nebula", kSpace, 280, 0.80, 265, 0.35, 0.045, 1.2, 0.64, CoreStyle::Nebula},
    {"space-void", "Void", kSpace, 250, 0.50, 240, 0.20, 0.025, 0.8, 0.60, CoreStyle::Minimal},
    {"space-starlight", "Starlight", kSpace, 220, 0.40, 230, 0.25, 0.050, 1.0, 0.74, CoreStyle::Orbit},
    {"space-mars", "Mars", kSpace, 15, 0.75, 10, 0.30, 0.050, 0.8, 0.60, CoreStyle::Storm},
    {"space-aurora", "Aurora", kSpace, 150, 0.80, 230, 0.30, 0.045, 1.3, 0.64, CoreStyle::Aurora},
    {"space-eclipse", "Eclipse", kSpace, 40, 0.90, 240, 0.12, 0.030, 0.5, 0.62, CoreStyle::Pulse},
    {"space-cosmos", "Cosmos", kSpace, 300, 0.70, 275, 0.30, 0.060, 1.1, 0.66, CoreStyle::Comet},
    // Fire & Ember
    {"fire-ember", "Ember", kFire, 18, 0.95, 15, 0.28, 0.055, 1.0, 0.62, CoreStyle::Pulse},
    {"fire-inferno", "Inferno", kFire, 8, 1.00, 5, 0.35, 0.040, 0.6, 0.60, CoreStyle::Storm},
    {"fire-amber", "Amber", kFire, 36, 0.95, 35, 0.28, 0.055, 1.0, 0.62, CoreStyle::Comet},
    {"fire-copper", "Copper", kFire, 26, 0.75, 25, 0.22, 0.060, 1.0, 0.60, CoreStyle::Aurora},
    {"fire-coal", "Coal", kFire, 20, 0.90, 20, 0.08, 0.050, 0.4, 0.60, CoreStyle::Minimal},
    {"fire-lava", "Lava", kFire, 355, 0.90, 0, 0.30, 0.050, 0.7, 0.60, CoreStyle::Nebula},
    {"fire-gold", "Gold", kFire, 48, 0.90, 45, 0.25, 0.055, 1.0, 0.60, CoreStyle::Orbit},
    // Minimal & Mono
    {"mono-graphite", "Graphite", kMono, 0, 0.00, 0, 0.00, 0.055, 1.0, 0.62, CoreStyle::Minimal},
    {"mono-charcoal", "Charcoal", kMono, 210, 0.12, 210, 0.04, 0.090, 1.0, 0.66, CoreStyle::Classic},
    {"mono-ink", "Ink", kMono, 230, 0.20, 230, 0.08, 0.030, 0.8, 0.70, CoreStyle::Pulse},
    {"mono-silver", "Silver", kMono, 200, 0.08, 200, 0.03, 0.120, 1.0, 0.72, CoreStyle::Orbit},
    {"mono-slate", "Slate", kMono, 222, 0.18, 222, 0.06, 0.055, 1.0, 0.62, CoreStyle::Storm},
    {"mono-steel", "Steel", kMono, 212, 0.30, 212, 0.09, 0.070, 1.0, 0.64, CoreStyle::Nebula},
    {"mono-sepia", "Sepia", kMono, 40, 0.30, 38, 0.10, 0.080, 0.9, 0.64, CoreStyle::Comet},
    // Retro
    {"retro-terminal", "Terminal", kRetro, 120, 0.90, 120, 0.10, 0.030, 0.25, 0.58, CoreStyle::Minimal},
    {"retro-amber-tube", "Amber Tube", kRetro, 40, 1.00, 35, 0.10, 0.030, 0.2, 0.60, CoreStyle::Classic},
    {"retro-synthwave", "Synthwave", kRetro, 320, 0.90, 265, 0.35, 0.050, 1.1, 0.66, CoreStyle::Comet},
    {"retro-vaporwave", "Vaporwave", kRetro, 185, 0.70, 290, 0.30, 0.070, 1.4, 0.70, CoreStyle::Aurora},
    {"retro-arcade", "Arcade", kRetro, 60, 1.00, 240, 0.30, 0.040, 1.5, 0.62, CoreStyle::Storm},
    {"retro-cassette", "Cassette", kRetro, 20, 0.60, 30, 0.15, 0.080, 1.2, 0.62, CoreStyle::Orbit},
    // Cyber
    {"cyber-matrix", "Matrix", kCyber, 135, 0.95, 135, 0.20, 0.030, 0.5, 0.60, CoreStyle::Storm},
    {"cyber-hacker", "Hacker", kCyber, 160, 0.80, 180, 0.20, 0.035, 0.6, 0.58, CoreStyle::Pulse},
    {"cyber-circuit", "Circuit", kCyber, 170, 0.90, 200, 0.30, 0.045, 1.2, 0.66, CoreStyle::Comet},
    {"cyber-glitch", "Glitch", kCyber, 345, 0.90, 200, 0.25, 0.040, 1.5, 0.64, CoreStyle::Nebula},
    {"cyber-chrome", "Chrome", kCyber, 200, 0.40, 205, 0.12, 0.075, 1.0, 0.70, CoreStyle::Orbit},
    {"cyber-grid", "Grid", kCyber, 260, 0.90, 250, 0.30, 0.030, 0.9, 0.64, CoreStyle::Aurora},
    // Pastel Night
    {"pastel-lavender", "Lavender", kPastel, 258, 0.55, 255, 0.15, 0.090, 1.0, 0.72, CoreStyle::Classic},
    {"pastel-mint", "Mint", kPastel, 165, 0.50, 165, 0.12, 0.085, 1.0, 0.72, CoreStyle::Pulse},
    {"pastel-peach", "Peach", kPastel, 22, 0.60, 20, 0.12, 0.090, 0.9, 0.74, CoreStyle::Minimal},
    {"pastel-sky", "Sky", kPastel, 205, 0.55, 210, 0.15, 0.095, 0.9, 0.74, CoreStyle::Orbit},
    {"pastel-rose", "Rose", kPastel, 340, 0.55, 340, 0.12, 0.085, 1.0, 0.72, CoreStyle::Nebula},
    {"pastel-lilac", "Lilac", kPastel, 290, 0.45, 280, 0.12, 0.100, 1.1, 0.74, CoreStyle::Aurora},
    // Gems
    {"gem-ruby", "Ruby", kGems, 355, 0.90, 350, 0.30, 0.050, 1.0, 0.62, CoreStyle::Classic},
    {"gem-emerald", "Emerald", kGems, 145, 0.85, 150, 0.30, 0.045, 0.9, 0.60, CoreStyle::Orbit},
    {"gem-amethyst", "Amethyst", kGems, 275, 0.80, 270, 0.30, 0.050, 0.9, 0.64, CoreStyle::Nebula},
    {"gem-topaz", "Topaz", kGems, 42, 0.90, 40, 0.25, 0.060, 0.9, 0.62, CoreStyle::Comet},
    {"gem-jade", "Jade", kGems, 155, 0.75, 160, 0.22, 0.060, 1.0, 0.62, CoreStyle::Pulse},
    {"gem-onyx", "Onyx", kGems, 270, 0.30, 260, 0.10, 0.025, 1.0, 0.70, CoreStyle::Minimal},
    {"gem-garnet", "Garnet", kGems, 345, 0.70, 350, 0.22, 0.060, 0.8, 0.60, CoreStyle::Storm},
};

// A HUD pack, as the owner's reference pictures draw them: a ground, the accent that lights everything, a
// second hue for the words and the thought, a warm one for action and what needs the user, how much of the
// accent the panels and lines carry, and the chrome. The rest of the palette follows (hudTokens).
struct Hud {
    const char *id;
    const char *name;
    const char *ground;
    const char *accent;
    const char *second;
    const char *warm;
    double tint;         // of the accent in the panels and lines: 0.05 almost none … 0.4 deep
    PanelChrome chrome;
    Backdrop backdrop;
    CoreStyle style;
};

constexpr Hud kHuds[] = {
    // The owner's own console: cyan on navy, orange for what waits on them, bracketed panels on a grid.
    {"hud-console", "Nexa Console", "#07111F", "#4FD1FF", "#7FA8FF", "#F5A742", 0.22, PanelChrome::Brackets, Backdrop::Grid, CoreStyle::Orbit},
    // A hologram in blue light, the panels edged in it.
    {"hud-jarvis", "Jarvis", "#04101C", "#38C6F4", "#6EE7FF", "#F0B454", 0.30, PanelChrome::Glow, Backdrop::Grid, CoreStyle::Comet},
    // Black, white and cobalt, a bar down each panel: the rainmeter desk.
    {"hud-mark-three", "Mark III", "#000000", "#3B82F6", "#9CA3AF", "#F59E0B", 0.05, PanelChrome::Rail, Backdrop::Plain, CoreStyle::Classic},
    // Teal on black, thin lines, scan lines, nothing else.
    {"hud-stark", "Stark", "#020B0C", "#22D3E6", "#5EEAD4", "#F2C14E", 0.25, PanelChrome::Brackets, Backdrop::Scan, CoreStyle::Minimal},
    // Magenta on purple-black with the corners cut.
    {"hud-violet-circuit", "Violet Circuit", "#120818", "#E040FB", "#B388FF", "#FFB74D", 0.30, PanelChrome::Chamfer, Backdrop::Vignette, CoreStyle::Storm},
    // Indigo and cyan, soft panes: the assistant that listens.
    {"hud-indigo", "Indigo Assistant", "#151533", "#3DD9F2", "#8B8CF5", "#F5B263", 0.25, PanelChrome::Glass, Backdrop::Vignette, CoreStyle::Nebula},
    // Violet, cyan and pink neon over a horizon.
    {"hud-synth-deck", "Synth Deck", "#160A2A", "#C04CF5", "#3DE8F5", "#F55AB0", 0.30, PanelChrome::Glow, Backdrop::Horizon, CoreStyle::Aurora},
    // Teal and coral on charcoal, a rail on every panel.
    {"hud-coral", "Cyber Coral", "#0B0D10", "#2EE6D6", "#7CF3EA", "#FF7A6B", 0.12, PanelChrome::Rail, Backdrop::Dots, CoreStyle::Pulse},
    // A pane of blue light.
    {"hud-holo", "Holo Blue", "#03122E", "#4FC3FF", "#8FE3FF", "#FFC65C", 0.40, PanelChrome::Glass, Backdrop::Grid, CoreStyle::Aurora},
    // Pale reactor light and amber on near black.
    {"hud-reactor", "Arc Reactor", "#050A0D", "#9FF3FF", "#38BDF8", "#F6A03C", 0.20, PanelChrome::Glow, Backdrop::Vignette, CoreStyle::Pulse},
    // Amber on charcoal: an aircraft's display.
    {"hud-sentinel", "Sentinel", "#0D0B08", "#F2A93B", "#FFD27A", "#FF7043", 0.12, PanelChrome::Brackets, Backdrop::Scan, CoreStyle::Storm},
    // Silver and ice on black, cut corners, a lattice of dots.
    {"hud-ghost", "Ghost", "#0A0B0D", "#CFD8E3", "#8FD3FF", "#FFB454", 0.05, PanelChrome::Chamfer, Backdrop::Dots, CoreStyle::Minimal},
    // The display in green.
    {"hud-emerald", "Emerald HUD", "#04110B", "#2EE88A", "#8BF5C8", "#F5C542", 0.25, PanelChrome::Brackets, Backdrop::Grid, CoreStyle::Comet},
    // Violet, electric blue and pink with the corners cut, over a grid.
    {"hud-ultraviolet", "Ultraviolet", "#0B0618", "#8B5CF6", "#38BDF8", "#F472B6", 0.30, PanelChrome::Chamfer, Backdrop::Grid, CoreStyle::Aurora},
};

// The catalogue before this one was 26 colour families, each in dark and light: a setting from then
// ("sapphire-dark", "sand-light") finds the pack nearest its family here.
struct Alias {
    const char *family;
    const char *id;
};

constexpr Alias kAliases[] = {
    {"nexa", "nexa-dark"},          {"mission", "mission-dark"},     {"ocean", "ocean"},
    {"sapphire", "ocean-sapphire"}, {"indigo", "cyber-grid"},        {"violet", "neon-violet"},
    {"orchid", "space-cosmos"},     {"magenta", "neon-magenta"},     {"rose", "pastel-rose"},
    {"ruby", "gem-ruby"},           {"coral", "space-mars"},         {"ember", "fire-ember"},
    {"copper", "fire-copper"},      {"amber", "fire-amber"},         {"gold", "fire-gold"},
    {"sand", "mono-sepia"},         {"lime", "neon-lime"},           {"forest", "nature-forest"},
    {"jade", "gem-jade"},           {"mint", "pastel-mint"},         {"aqua", "ocean-lagoon"},
    {"ice", "ocean-arctic"},        {"steel", "mono-steel"},         {"slate", "mono-slate"},
    {"graphite", "mono-graphite"},  {"lavender", "pastel-lavender"}, {"plum", "pastel-lilac"},
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

// A navy at mid lightness all but vanishes on near-black: every colour is lightened until it stands out
// from the ground by at least `minimum`, so the recipes never have to know a hue's weight.
QColor standingOut(QColor colour, const QColor &ground, double minimum) {
    for (int step = 0; step < 40 && contrast(colour, ground) < minimum; ++step) {
        const double lightness = double(colour.lightnessF()) + 0.02;
        colour = QColor::fromHslF(std::max(0.0, double(colour.hslHueF())), colour.hslSaturationF(),
                                  std::clamp(lightness, 0.0, 1.0));
    }
    return colour;
}

// The contrast every pack is held to: text on the panels, muted text and the state colours on the ground.
void guaranteed(ThemeTokens &t) {
    t.text = standingOut(t.text, t.panelRaised, 7.0);
    t.muted = standingOut(t.muted, t.ground, 3.5);
    for (QColor *state : {&t.idle, &t.wake, &t.listen, &t.stt, &t.think, &t.act, &t.speak, &t.fast, &t.error}) {
        *state = standingOut(*state, t.ground, 3.0);
    }
}

// The design's shape of palette, from a recipe: the neutrals carry the ground's tint, the states keep their
// meaning (idle on the accent, acting across from it, error warm) in the accent's light, as far from it as
// the spread says and as bright as the glow.
ThemeTokens tokensFor(const Recipe &r) {
    const double sat = r.saturation;
    const double glow = r.glow;
    ThemeTokens t;
    t.ground = hsl(r.groundHue, r.groundSat, r.depth);
    t.panel = hsl(r.groundHue, r.groundSat * 0.9, r.depth + 0.035);
    t.panelRaised = hsl(r.groundHue, r.groundSat * 0.8, r.depth + 0.065);
    t.line = hsl(r.groundHue, r.groundSat * 0.6, r.depth + 0.145);
    t.text = hsl(r.groundHue, std::min(r.groundSat, 0.15) * 0.8, 0.93);
    t.muted = hsl(r.groundHue, r.groundSat * 0.35, 0.66);
    t.idle = hsl(r.hue, 0.55 * sat + 0.05, glow - 0.12);
    t.wake = hsl(r.hue + 40 * r.spread, 0.55 * sat + 0.10, glow);
    t.listen = hsl(r.hue + 8 * r.spread, 0.70 * sat + 0.10, glow + 0.01);
    t.stt = hsl(r.hue + 75 * r.spread, 0.70 * sat + 0.10, glow + 0.14);
    t.think = hsl(r.hue + 125 * r.spread, 0.65 * sat + 0.10, glow + 0.16);
    t.act = hsl(r.hue + 185 * r.spread, 0.85 * sat + 0.10, glow);
    t.speak = hsl(r.hue - 22 * r.spread, 0.65 * sat + 0.10, glow + 0.06);
    t.fast = hsl(r.hue, 0.6 * sat, 0.94);
    t.error = hsl(10, 0.92, 0.68);
    t.onAccent = hsl(r.hue, 0.5 * sat, 0.08);
    guaranteed(t);
    return t;
}

// Mission Control's palette, as the mock-up drew it: violet idle on near-black blue, green for the wake
// word, sky for the words, indigo thought, amber action, orchid speech.
ThemeTokens missionTokens() {
    ThemeTokens t;
    t.ground = QColor(0x0B, 0x0D, 0x12);
    t.panel = QColor(0x12, 0x14, 0x1B);
    t.panelRaised = QColor(0x19, 0x1C, 0x25);
    t.line = QColor(0x23, 0x27, 0x36);
    t.text = QColor(0xE7, 0xE9, 0xF2);
    t.muted = QColor(0x8A, 0x90, 0xA6);
    t.idle = QColor(0xA8, 0x55, 0xF7);
    t.wake = QColor(0x22, 0xC5, 0x5E);
    t.listen = QColor(0xC0, 0x84, 0xFC);
    t.stt = QColor(0x38, 0xBD, 0xF8);
    t.think = QColor(0x81, 0x8C, 0xF8);
    t.act = QColor(0xF5, 0x9E, 0x0B);
    t.speak = QColor(0xE8, 0x79, 0xF9);
    t.fast = QColor(0xF5, 0xF3, 0xFF);
    t.error = QColor(0xF8, 0x71, 0x71);
    t.onAccent = QColor(0x0B, 0x0D, 0x12);
    guaranteed(t);
    return t;
}

QColor mix(const QColor &from, const QColor &to, double amount) {
    amount = std::clamp(amount, 0.0, 1.0);
    return QColor::fromRgbF(from.redF() + (to.redF() - from.redF()) * amount,
                            from.greenF() + (to.greenF() - from.greenF()) * amount,
                            from.blueF() + (to.blueF() - from.blueF()) * amount);
}

QColor lighter(const QColor &colour, double by) {
    return QColor::fromHslF(std::max(0.0, double(colour.hslHueF())), colour.hslSaturationF(),
                            std::clamp(double(colour.lightnessF()) + by, 0.0, 1.0));
}

// A HUD pack's palette from its picture: the panels and lines are the ground lifted towards the accent, the
// text is white with a breath of it, the states are the accent, the second hue and the warm one in their roles
// (idle, listening and the fast path on the accent; the words and the thought on the second; action and what
// needs the user warm), and the whole held to the same contrast as every other pack.
ThemeTokens hudTokens(const Hud &h) {
    const QColor ground(QString::fromLatin1(h.ground));
    const QColor accent(QString::fromLatin1(h.accent));
    const QColor second(QString::fromLatin1(h.second));
    const QColor warm(QString::fromLatin1(h.warm));
    ThemeTokens t;
    t.ground = ground;
    t.panel = lighter(mix(ground, accent, 0.03 + h.tint * 0.10), 0.030);
    t.panelRaised = lighter(mix(ground, accent, 0.04 + h.tint * 0.14), 0.060);
    t.line = lighter(mix(ground, accent, 0.10 + h.tint * 0.35), 0.110);
    t.text = mix(QColor(Qt::white), accent, 0.08);
    t.muted = mix(t.text, ground, 0.48);
    t.idle = accent;
    t.listen = lighter(accent, 0.08);
    t.fast = mix(QColor(Qt::white), accent, 0.25);
    t.wake = mix(accent, second, 0.5);
    t.stt = second;
    t.think = lighter(mix(second, accent, 0.35), 0.06);
    t.act = warm;
    t.speak = lighter(mix(accent, second, 0.3), 0.12);
    t.error = hsl(10, 0.92, 0.68);
    t.onAccent = ground.lightness() < 8 ? QColor(0x06, 0x08, 0x0C) : ground;
    guaranteed(t);
    return t;
}

QList<ThemePack> generate() {
    QList<ThemePack> packs;
    // Nexa's own look first, exactly as the design draws it, then Mission Control's; then the HUD packs from the
    // pictures; then the recipes.
    packs.append(ThemePack{u"nexa-dark"_s, u"Nexa"_s, QString::fromLatin1(kNexa), CoreStyle::Classic,
                           NexaTheme::tokens(ThemeScheme::Dark), {}});
    packs.append(ThemePack{u"mission-dark"_s, u"Mission Control"_s, QString::fromLatin1(kNexa), CoreStyle::Comet,
                           missionTokens(), {}});
    for (const Hud &hud : kHuds) {
        packs.append(ThemePack{QString::fromLatin1(hud.id), QString::fromLatin1(hud.name), QString::fromLatin1(kHud),
                               hud.style, hudTokens(hud), ThemeChrome{hud.chrome, hud.backdrop}});
    }
    for (const Recipe &recipe : kRecipes) {
        packs.append(ThemePack{QString::fromLatin1(recipe.id), QString::fromLatin1(recipe.name),
                               QString::fromLatin1(recipe.category), recipe.style, tokensFor(recipe), {}});
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

QString NexaThemes::defaultId() {
    return u"nexa-dark"_s;
}

ThemePack NexaThemes::resolve(const QString &setting) {
    if (const std::optional<ThemePack> pack = find(setting)) return *pack;
    // The old scheme words, and the old catalogue's "<family>-dark" / "<family>-light".
    QString family = setting.toLower();
    for (const QString &suffix : {u"-dark"_s, u"-light"_s}) {
        if (family.endsWith(suffix)) family.chop(suffix.size());
    }
    for (const Alias &alias : kAliases) {
        if (family == QLatin1StringView(alias.family)) {
            if (const std::optional<ThemePack> pack = find(QString::fromLatin1(alias.id))) return *pack;
        }
    }
    return *find(defaultId());
}

QString NexaThemes::settingFor(const ThemePack &pack) {
    return pack.id;
}

QStringList NexaThemes::categories() {
    QStringList names;
    for (const ThemePack &pack : all()) {
        if (!names.contains(pack.category)) names.append(pack.category);
    }
    return names;
}

QList<ThemePack> NexaThemes::inCategory(const QString &category) {
    QList<ThemePack> packs;
    for (const ThemePack &pack : all()) {
        if (pack.category == category) packs.append(pack);
    }
    return packs;
}

ThemePack NexaThemes::nextInCategory(const ThemePack &pack) {
    const QList<ThemePack> packs = inCategory(pack.category);
    for (qsizetype i = 0; i < packs.size(); ++i) {
        if (packs.at(i).id == pack.id) return packs.at((i + 1) % packs.size());
    }
    return packs.isEmpty() ? resolve(pack.id) : packs.first();
}

} // namespace nexa
