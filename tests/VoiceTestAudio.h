#pragma once

// Synthetic 16 kHz mono Int16 audio for the voice tests: quiet room noise and loud "speech"
// (a tone with noise), deterministic so every run sees the same samples.

#include <QByteArray>
#include <QtGlobal>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>

namespace nexa::testaudio {

inline constexpr int kRate = 16'000;

inline QByteArray make(int ms, double toneAmplitude, double noiseAmplitude, std::uint32_t seed = 1) {
    const int samples = kRate * ms / 1000;
    QByteArray pcm(samples * 2, Qt::Uninitialized);
    auto *out = reinterpret_cast<qint16 *>(pcm.data());
    std::uint32_t state = seed * 2654435761u + 1;
    for (int i = 0; i < samples; ++i) {
        state = state * 1664525u + 1013904223u;
        const double noise = (double(state >> 8) / double(1u << 24)) * 2.0 - 1.0;
        const double tone = std::sin(2.0 * std::numbers::pi * 220.0 * i / kRate);
        const double value = (toneAmplitude * tone + noiseAmplitude * noise) * 32767.0;
        out[i] = qint16(std::clamp(value, -32768.0, 32767.0));
    }
    return pcm;
}

inline QByteArray quiet(int ms, std::uint32_t seed = 1) { return make(ms, 0.0, 0.002, seed); }   // ≈ -59 dBFS
inline QByteArray speech(int ms, std::uint32_t seed = 2) { return make(ms, 0.25, 0.02, seed); } // ≈ -15 dBFS

} // namespace nexa::testaudio
