#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

// Control-flow / value obfuscation primitives for licence paths.
// Two shapes toggled by NEXA_OBFUSCATE_LICENSE: flattened + masked in shipping release
// builds, straight passthrough in dev/test builds. GuardTest compiles both shapes and
// proves identical outputs across every check vector.
namespace nexa::obf {

#ifndef NEXA_CHECK_ROTATION_SEED
#define NEXA_CHECK_ROTATION_SEED 0x4e455841u
#endif

inline constexpr std::uint32_t kRotationSeed = NEXA_CHECK_ROTATION_SEED;

constexpr std::uint32_t mix(std::uint32_t x) {
    x += 0x9e3779b9u;
    x = (x ^ (x >> 16)) * 0x21f0aaadu;
    x = (x ^ (x >> 15)) * 0x735a2d97u;
    return x ^ (x >> 15);
}

constexpr std::uint32_t seedAt(std::uint32_t i) {
    return mix(kRotationSeed ^ (i * 0x9e3779b9u + 0x85ebca6bu));
}

template <std::size_t N>
constexpr std::array<int, N> permutation(std::uint32_t seed) {
    std::array<int, N> order{};
    for (std::size_t i = 0; i < N; ++i) order[i] = static_cast<int>(i);
    std::uint32_t s = seed;
    for (std::size_t i = N; i > 1; --i) {
        s = mix(s);
        const std::size_t j = s % i;
        const int tmp = order[i - 1];
        order[i - 1] = order[j];
        order[j] = tmp;
    }
    return order;
}

struct MaskedBool {
    std::uint32_t word{0};
    std::uint32_t key{0};
    std::uint32_t mask{0};

    static MaskedBool make(bool value, std::uint32_t key, std::uint32_t mask) {
        const std::uint32_t m = mask ? mask : 0xA5A5A5A5u;
        return MaskedBool{value ? key : (key ^ m), key, m};
    }

    [[nodiscard]] bool decode() const { return word == key; }
};

#ifdef NEXA_OBFUSCATE_LICENSE

inline bool opaqueTrue(std::uint32_t salt) {
    volatile std::uint32_t sink = salt ^ seedAt(1);
    const std::uint32_t x = sink;
    const bool parityHolds = ((x * x) & 1u) == (x & 1u);
    const bool oddHolds = ((x | 1u) & 1u) == 1u;
    return parityHolds && oddHolds;
}

inline bool opaqueFalse(std::uint32_t salt) { return !opaqueTrue(salt); }

inline bool guardedTrue(bool cond, std::uint32_t salt) {
    return cond && opaqueTrue(salt);
}

#else

inline bool opaqueTrue(std::uint32_t) { return true; }
inline bool opaqueFalse(std::uint32_t) { return false; }
inline bool guardedTrue(bool cond, std::uint32_t) { return cond; }

#endif

} // namespace nexa::obf
