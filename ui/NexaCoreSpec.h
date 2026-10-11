#pragma once

#include <QString>
#include <QStringView>

#include <array>
#include <optional>

namespace nexa {

// The motion spec of the Nexa core, pure: which states exist, what each one is called, how fast it
// breathes and turns, and where every moving part is at a given moment. Everything the core does is
// built from circles, arcs and dashes, so the same numbers paint with QPainter in the window
// (NexaCorePainter) and with SVG and CSS on the phone (remote/web/app.js follows this file).

// The nine states, in the order of the design's state strip.
enum class CoreMode { Idle, WakeWord, Listening, Transcribing, Thinking, Acting, Speaking, FastPath, Offline };
inline constexpr int kCoreModeCount = 9;
inline constexpr std::array<CoreMode, kCoreModeCount> kCoreModes{
    CoreMode::Idle,     CoreMode::WakeWord, CoreMode::Listening, CoreMode::Transcribing, CoreMode::Thinking,
    CoreMode::Acting,   CoreMode::Speaking, CoreMode::FastPath,  CoreMode::Offline};

// The four satellites around the core, as flags: an action can light more than one.
enum CoreNode : unsigned { NoNode = 0, VoiceNode = 1, PcNode = 2, ToolsNode = 4, PhoneNode = 8 };
using CoreNodes = unsigned;

// How the motion is styled — every theme pack picks one. Classic is the spec as written; the
// others bend it: satellites that never land, a ping that never stops, a hue that drifts…
enum class CoreStyle { Classic, Orbit, Pulse, Aurora, Comet, Nebula, Storm, Minimal };
inline constexpr int kCoreStyleCount = 8;
inline constexpr std::array<CoreStyle, kCoreStyleCount> kCoreStyles{
    CoreStyle::Classic, CoreStyle::Orbit, CoreStyle::Pulse,  CoreStyle::Aurora,
    CoreStyle::Comet,   CoreStyle::Nebula, CoreStyle::Storm, CoreStyle::Minimal};
QString coreStyleName(CoreStyle style);             // "Aurora"
QString coreStyleKey(CoreStyle style);              // "aurora"
QString coreStyleBlurb(CoreStyle style);            // one line for the theme gallery
std::optional<CoreStyle> coreStyleFromKey(QStringView key);

// What the core is drawn as — every design picks one. The same frames (breath, ring angle,
// ticks, satellites, sonar, comet, packets) are read by each shape in its own geometry.
enum class CoreShape { Ring, Orb, Hex, Bars, Iris, Crystal, Arc, Dot };
inline constexpr int kCoreShapeCount = 8;
inline constexpr std::array<CoreShape, kCoreShapeCount> kCoreShapes{
    CoreShape::Ring, CoreShape::Orb,     CoreShape::Hex, CoreShape::Bars,
    CoreShape::Iris, CoreShape::Crystal, CoreShape::Arc, CoreShape::Dot};
QString coreShapeName(CoreShape shape);             // "Crystal"
QString coreShapeKey(CoreShape shape);              // "crystal"
std::optional<CoreShape> coreShapeFromKey(QStringView key);

QString coreModeName(CoreMode mode);                // "Fast path": shown beside the core, never colour alone
QString coreModeKey(CoreMode mode);                 // "fast": the colour token (--nx-fast) and the wire name
std::optional<CoreMode> coreModeFromKey(QStringView key);
// Which satellites an action lights: the phone for android_*, Voice for speak, PC and Tools otherwise.
CoreNodes coreNodesForTool(QStringView tool);
QString coreNodeName(CoreNode node);                // "Voice", "PC", "Tools", "Phone"

// Timing and easing, per state.
struct CoreTiming {
    double breathSeconds;       // one in-and-out of the core (ease-in-out)
    double ringTurnSeconds;     // one turn of the dotted ring (linear)
    double cometLapSeconds;     // one lap of the comet on the ring; 0: no comet
    int frameRateHz;            // 60 while something happens, 30 for idle breathing
};
CoreTiming coreTiming(CoreMode mode);

inline constexpr int kCrossFadeMs = 350;        // a state change cross-fades colour and opacity
inline constexpr int kFastPathHoldMs = 900;     // the fast-path snap shows for at least its ping
inline constexpr int kTickCount = 48;           // per ring of level ticks

// What is going on, as the controller knows it; resolveCoreMode() picks the one state to show.
struct CoreActivity {
    bool offline{false};        // the provider cannot be reached
    bool speaking{false};
    bool listening{false};
    bool transcribing{false};
    bool acting{false};         // a tool is running
    bool fastPath{false};       // a request is being answered without the model
    bool thinking{false};       // the agent is streaming
    bool wakeWord{false};       // the microphone is open for "Hey Nexa"
};
CoreMode resolveCoreMode(const CoreActivity &activity);

// One frame of motion: where everything is at `seconds` (a clock that never resets, so continuous
// motion never jumps) and `secondsInMode` (since the state began, for the things that happen once).
// `level` is the microphone level, 0 … 1. Radii are in units of the core's own radius.
struct CoreFrame {
    double coreScale{1.0};                      // the breath: 1.02 … 1.07, offline 0.97 … 1.0
    double ringAngle{0.0};                      // degrees the dotted ring has turned
    std::optional<double> cometAngle;           // degrees along the ring
    double tickSwell{0.0};                      // 0 … 1 for the level ticks; 0: no ticks
    double tickPhaseInner{0.0};                 // radians
    double tickPhaseOuter{0.0};
    std::optional<double> scanAngle;            // transcribing: degrees
    std::optional<std::array<double, 3>> satelliteAngles;   // thinking: degrees, the middle one reversed
    double sonarRadius{0.0};                    // in core radii; 0: no ping
    double sonarOpacity{0.0};
    std::optional<double> packetProgress;       // 0 (at the core) … 1 (at the node)
    // What a style adds.
    double hueShift{0.0};                       // Aurora: degrees to turn the state's colour by
    double haloScale{1.0};                      // Nebula: the halo breathes wider
    std::optional<double> secondCometAngle;     // Comet: a second one, opposite the first
    bool ring{true};                            // Minimal: no dotted ring
};
CoreFrame coreFrame(CoreMode mode, double level, double seconds, double secondsInMode,
                    CoreStyle style = CoreStyle::Classic);
// The repaint rate a style needs when the state alone would be happy with less.
int coreStyleFrameRate(CoreStyle style, CoreMode mode);

double easeInOut(double x);     // cubic, for the breath and the ticks
double easeOut(double x);       // for the sonar ping
double easeIn(double x);        // for the packets
double easeCross(double x);     // the state change

} // namespace nexa
