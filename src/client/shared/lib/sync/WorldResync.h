#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>
#include "common/lib/buffer/Buffer.h"

namespace coop {
struct WorldResync {
    std::vector<uint16_t> vehicles;
    bool Keeps(uint16_t id) const { return std::find(vehicles.begin(), vehicles.end(), id) != vehicles.end(); }
    void Write(Writer& w) const {
        size_t n = std::min<size_t>(vehicles.size(), 110);  // native vehicle pool limit
        w.U8((uint8_t)n);
        for (size_t i = 0; i < n; ++i) w.U16(vehicles[i]);
    }
    bool Read(Reader& r) {
        uint8_t n = r.U8();
        if (n > 110) return false;
        vehicles.clear();
        for (uint8_t i = 0; i < n; ++i) vehicles.push_back(r.U16());
        return r.Ok();
    }
};
inline uint8_t VehicleSuccessor(uint8_t host, uint8_t departed, const std::vector<uint8_t>& survivors) {
    if (host != departed) return host;
    return survivors.empty() ? 255 : *std::min_element(survivors.begin(), survivors.end());
}
}  // namespace coop
