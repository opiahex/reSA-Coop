#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <tuple>

namespace coop::sync {
struct GarageDoorCandidate {
    uint8_t id;
    bool inside, driver;
    float distance;
};

// One common winner for all three views. Pairwise "within five metres" tests
// are not transitive: three players could otherwise follow each other in a loop.
inline uint8_t GarageDoorOwner(const GarageDoorCandidate* candidates, size_t count, uint8_t host) {
    uint8_t owner = 255;
    std::tuple<int, int, int, int> best{2, 2, 1000000, 256};
    for (size_t i = 0; i < count; ++i) {
        const auto& c = candidates[i];
        if (!std::isfinite(c.distance) || c.distance < 0 || (!c.inside && c.distance > 30)) continue;
        auto rank = std::make_tuple(c.inside ? 0 : 1, c.driver ? 0 : 1,
            c.inside ? 0 : (int)(c.distance / 5), c.id == host ? 0 : (int)c.id + 1);
        if (rank < best) { best = rank; owner = c.id; }
    }
    return owner;
}
}  // namespace coop::sync
