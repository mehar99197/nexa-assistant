#include "core/account/Guard.h"

#include <catch2/catch_test_macros.hpp>

using namespace nexa::guard;

TEST_CASE("Guard: evaluates all 32 check vectors identically in plain and obfuscated builds", "[account][guard]") {
    for (int mask = 0; mask < (1 << SlotCount); ++mask) {
        const auto bit = [mask](Slot slot) { return ((mask >> int(slot)) & 1) != 0; };
        const Checks checks{{
            [&] { return bit(KeyIntact); },
            [&] { return bit(LivePaid); },
            [&] { return bit(CachedGrace); },
            [&] { return bit(MemoryClaimsPaid); },
            [&] { return bit(SignedGrant); },
        }};

        const Result actual = evaluate(checks);
        const bool expectedPaid = bit(KeyIntact) && (bit(LivePaid) || bit(CachedGrace));
        const bool expectedTamper = !expectedPaid && bit(MemoryClaimsPaid) && !(bit(KeyIntact) && bit(SignedGrant));

        CHECK(actual.paid == expectedPaid);
        CHECK(actual.tamper == expectedTamper);
    }
}
