#pragma once
#include <cstdint>
#include <set>
#include <vector>

namespace coop {
// A new generation cannot consume a previous scene's readiness acknowledgements.
struct SceneBarrier {
    uint32_t generation = 0;
    uint64_t since = 0;
    std::set<uint8_t> ready;
    void Begin(uint64_t now) { ++generation; since = now; ready.clear(); }
    void Ack(uint8_t player, uint32_t token) { if (token == generation) ready.insert(player); }
    bool CanStart(uint64_t now, const std::vector<uint8_t>& peers) const {
        if (now - since >= 20000) return true;  // one failed loader cannot trap a session forever
        for (uint8_t id : peers) if (!ready.count(id)) return false;
        return true;
    }
};
}  // namespace coop
