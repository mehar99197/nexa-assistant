#pragma once

#include <array>
#include <functional>

namespace nexa::guard {

// Five independent, redundant checks re-deriving entitlement ground truth from
// the signed token and compiled-in public key.
enum Slot {
    KeyIntact = 0,
    LivePaid,
    CachedGrace,
    MemoryClaimsPaid,
    SignedGrant,
    SlotCount
};

using Check = std::function<bool()>;
using Checks = std::array<Check, SlotCount>;

struct Result {
    // KeyIntact && (LivePaid || CachedGrace)
    bool paid{false};
    // !paid && MemoryClaimsPaid && !(KeyIntact && SignedGrant)
    bool tamper{false};
};

[[nodiscard]] Result evaluate(const Checks &checks);

} // namespace nexa::guard
