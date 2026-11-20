#include "core/account/Guard.h"
#include "core/account/Obfuscation.h"

namespace nexa::guard {

namespace {

Result combine(const bool (&r)[SlotCount]) {
    Result out;
    out.paid = r[KeyIntact] && (r[LivePaid] || r[CachedGrace]);
    out.tamper = !out.paid && r[MemoryClaimsPaid] && !(r[KeyIntact] && r[SignedGrant]);
    return out;
}

} // namespace

#ifdef NEXA_OBFUSCATE_LICENSE

Result evaluate(const Checks &checks) {
    constexpr auto order = obf::permutation<SlotCount>(obf::seedAt(7));
    obf::MaskedBool masked[SlotCount];

    int state = 0;
    for (;;) {
        if (state >= static_cast<int>(SlotCount)) break;
        if (obf::guardedTrue(true, obf::seedAt(20u + static_cast<std::uint32_t>(state)))) {
            const int slot = order[static_cast<std::size_t>(state)];
            const std::uint32_t key = obf::seedAt(40u + static_cast<std::uint32_t>(slot));
            const std::uint32_t mask = obf::seedAt(60u + static_cast<std::uint32_t>(slot)) | 1u;
            const bool value = checks[static_cast<std::size_t>(slot)]();
            masked[slot] = obf::MaskedBool::make(value, key, mask);
        }
        state += 1;
    }

    bool r[SlotCount]{};
    for (int i = 0; i < static_cast<int>(SlotCount); ++i) r[i] = masked[i].decode();
    return combine(r);
}

#else

Result evaluate(const Checks &checks) {
    bool r[SlotCount]{};
    for (int i = 0; i < static_cast<int>(SlotCount); ++i) {
        r[i] = checks[static_cast<std::size_t>(i)]();
    }
    return combine(r);
}

#endif

} // namespace nexa::guard
