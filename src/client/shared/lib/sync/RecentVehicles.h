#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>
namespace coop {
struct RecentVehicles {
    static constexpr size_t kLimit = 3;
    std::vector<uint16_t> ids;
    bool Touch(uint16_t id) {
        if (!id || (!ids.empty() && ids.front() == id)) return false;
        ids.erase(std::remove(ids.begin(), ids.end(), id), ids.end());
        ids.insert(ids.begin(), id);
        if (ids.size() > kLimit) ids.resize(kLimit);
        return true;
    }
    bool Contains(uint16_t id) const { return std::find(ids.begin(), ids.end(), id) != ids.end(); }
};
}
