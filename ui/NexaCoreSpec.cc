#include "ui/NexaCoreSpec.h"

#include <QtGlobal>

#include <algorithm>
#include <cmath>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kTau = 2 * kPi;

double turns(double seconds, double period) {
    return period > 0 ? std::fmod(360.0 * seconds / period, 360.0) : 0.0;
}

double phase(double seconds, double period) {
    return std::fmod(seconds, period) / period;
}

} // namespace

QString coreModeName(CoreMode mode) {
    switch (mode) {
    case CoreMode::Idle: return u"Idle"_s;
    case CoreMode::WakeWord: return u"Wake word"_s;
    case CoreMode::Listening: return u"Listening"_s;
    case CoreMode::Transcribing: return u"Transcribing"_s;
    case CoreMode::Thinking: return u"Thinking"_s;
    case CoreMode::Acting: return u"Acting"_s;
    case CoreMode::Speaking: return u"Speaking"_s;
    case CoreMode::FastPath: return u"Fast path"_s;
    case CoreMode::Offline: return u"Offline"_s;
    }
    return {};
}

QString coreModeKey(CoreMode mode) {
    switch (mode) {
    case CoreMode::Idle: return u"idle"_s;
    case CoreMode::WakeWord: return u"wake"_s;
    case CoreMode::Listening: return u"listen"_s;
    case CoreMode::Transcribing: return u"stt"_s;
    case CoreMode::Thinking: return u"think"_s;
    case CoreMode::Acting: return u"act"_s;
    case CoreMode::Speaking: return u"speak"_s;
    case CoreMode::FastPath: return u"fast"_s;
    case CoreMode::Offline: return u"offline"_s;
    }
    return {};
}

std::optional<CoreMode> coreModeFromKey(QStringView key) {
    for (const CoreMode mode : kCoreModes) {
        if (coreModeKey(mode) == key) return mode;
    }
    return std::nullopt;
}

CoreNodes coreNodesForTool(QStringView tool) {
    if (tool.isEmpty()) return NoNode;
    if (tool.startsWith(u"android_")) return PhoneNode;
    if (tool == u"speak") return VoiceNode;
    return PcNode | ToolsNode;
}

QString coreNodeName(CoreNode node) {
    switch (node) {
    case VoiceNode: return u"Voice"_s;
    case PcNode: return u"PC"_s;
    case ToolsNode: return u"Tools"_s;
    case PhoneNode: return u"Phone"_s;
    case NoNode: break;
    }
    return {};
}

CoreTiming coreTiming(CoreMode mode) {
    switch (mode) {
    case CoreMode::Idle: return {5.2, 70.0, 16.0, 30};
    case CoreMode::WakeWord: return {5.2, 50.0, 16.0, 30};
    case CoreMode::Listening: return {1.1, 70.0, 0.0, 60};
    case CoreMode::Transcribing: return {2.4, 70.0, 0.0, 60};
    case CoreMode::Thinking: return {2.8, 30.0, 8.0, 60};
    case CoreMode::Acting: return {1.6, 70.0, 2.2, 60};
    case CoreMode::Speaking: return {0.8, 70.0, 0.0, 60};
    case CoreMode::FastPath: return {1.6, 70.0, 0.0, 60};
    case CoreMode::Offline: return {3.4, 70.0, 0.0, 30};
    }
    return {5.2, 70.0, 16.0, 30};
}

CoreMode resolveCoreMode(const CoreActivity &a) {
    if (a.offline) return CoreMode::Offline;
    if (a.speaking) return CoreMode::Speaking;
    if (a.listening) return CoreMode::Listening;
    if (a.transcribing) return CoreMode::Transcribing;
    if (a.acting) return CoreMode::Acting;
    if (a.fastPath) return CoreMode::FastPath;
    if (a.thinking) return CoreMode::Thinking;
    if (a.wakeWord) return CoreMode::WakeWord;
    return CoreMode::Idle;
}

double easeInOut(double x) {
    x = std::clamp(x, 0.0, 1.0);
    return x < 0.5 ? 4 * x * x * x : 1 - std::pow(-2 * x + 2, 3) / 2;
}

double easeOut(double x) {
    x = std::clamp(x, 0.0, 1.0);
    return 1 - std::pow(1 - x, 3);
}

double easeIn(double x) {
    x = std::clamp(x, 0.0, 1.0);
    return x * x;
}

double easeCross(double x) {
    x = std::clamp(x, 0.0, 1.0);
    return x * x * (3 - 2 * x);
}

CoreFrame coreFrame(CoreMode mode, double level, double seconds, double secondsInMode) {
    const CoreTiming timing = coreTiming(mode);
    level = std::clamp(level, 0.0, 1.0);
    CoreFrame frame;

    // The breath: a sine eased at the turns, scale 1.02 … 1.07; offline, 0.97 … 1.0.
    const double breath = easeInOut(0.5 + 0.5 * std::sin(kTau * seconds / timing.breathSeconds));
    frame.coreScale = mode == CoreMode::Offline ? 0.97 + 0.03 * breath : 1.02 + 0.05 * breath;

    frame.ringAngle = turns(seconds, timing.ringTurnSeconds);
    if (timing.cometLapSeconds > 0) frame.cometAngle = turns(seconds, timing.cometLapSeconds);

    switch (mode) {
    case CoreMode::Listening:
        // Two rings of 48 ticks swell with the microphone, each breathing at its own pace, out of phase.
        frame.tickSwell = 0.25 + 0.75 * level;
        frame.tickPhaseInner = kTau * seconds / 0.9;
        frame.tickPhaseOuter = kTau * seconds / 1.3 + kPi;
        break;
    case CoreMode::Speaking:
        // Nothing measures the voice, so the ticks follow a 1.6 s swell, the outer ring 0.8 s behind.
        frame.tickSwell = 0.45 + 0.35 * std::sin(kTau * seconds / 1.6);
        frame.tickPhaseInner = kTau * seconds / 1.6;
        frame.tickPhaseOuter = kTau * (seconds + 0.8) / 1.6;
        break;
    case CoreMode::Transcribing:
        frame.scanAngle = turns(seconds, 1.1);
        break;
    case CoreMode::Thinking:
        frame.satelliteAngles = {{turns(seconds, 3.2), -turns(seconds, 4.7), turns(seconds, 6.6)}};
        break;
    case CoreMode::WakeWord: {
        // A sonar ping every 3.2 s: grows from 0.4 to 2.9 core radii and fades.
        const double p = phase(seconds, 3.2);
        frame.sonarRadius = 0.4 + 2.5 * easeOut(p);
        frame.sonarOpacity = 0.55 * (1 - p);
        break;
    }
    case CoreMode::Acting:
        // Packets leave the core for the lit nodes, one every 1.1 s.
        frame.packetProgress = easeIn(phase(seconds, 1.1));
        break;
    case CoreMode::FastPath: {
        // Once per request: a packet in 0.3 s and a ping in 0.9 s, then rest.
        if (secondsInMode < 0.3) frame.packetProgress = easeIn(secondsInMode / 0.3);
        if (secondsInMode < 0.9) {
            const double p = secondsInMode / 0.9;
            frame.sonarRadius = 0.4 + 2.5 * easeOut(p);
            frame.sonarOpacity = 0.7 * (1 - p);
        }
        break;
    }
    case CoreMode::Idle:
    case CoreMode::Offline:
        break;
    }
    return frame;
}

} // namespace nexa
