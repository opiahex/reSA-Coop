#pragma once
#include <cmath>
#include <cstdint>
#include <vector>
#include "common/lib/buffer/Buffer.h"
namespace coop {
struct GroupMember {
    Vec3 position;
    int8_t area = 0;
    bool fresh = false, alive = false;
};
inline bool GroupReady(const Vec3& point, int8_t area, float radius, const std::vector<GroupMember>& members, float vertical = 4.0f) {
    for (const auto& m : members)
        if (!m.fresh || !m.alive || m.area != area || std::fabs(m.position.z - point.z) > vertical ||
            std::hypot(m.position.x - point.x, m.position.y - point.y) > radius) return false;
    return true;
}
struct SeatTransition {
    uint16_t vehicle = 0;
    uint32_t senderTime = 0;
    uint64_t began = 0, confirmed = 0;
    bool leaving = false, jacked = false;
    bool Begin(uint16_t id, uint32_t time, uint64_t now, bool jack = false) {
        if (leaving && (int32_t)(time - senderTime) < 0) return false;
        vehicle = id; senderTime = time; began = now; confirmed = 0; leaving = true; jacked = jack;
        return true;
    }
    bool HoldSeat(uint16_t id, uint64_t now) const { return leaving && vehicle == id && now - began < 6000; }
    void OnFoot(uint64_t now) { if (leaving && !confirmed) confirmed = now; }
    bool MustDetach(uint64_t now) const { return confirmed && now - confirmed >= 750; }
};
}
