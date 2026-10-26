#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace coop::worldscript {

struct ScriptLayout {
    uint32_t main = 0, largestMission = 0;
    uint16_t missions = 0, exclusiveMissions = 0;
    uint32_t largestMissionLocals = 0;
    bool operator==(const ScriptLayout&) const = default;
};
static_assert(sizeof(ScriptLayout) == 16);

struct SaveHeader {
    uint8_t missionPack = 0;
    ScriptLayout layout;
};

inline bool Read32(const std::vector<uint8_t>& data, size_t at, uint32_t& value) {
    if (at > data.size() || data.size() - at < sizeof(value)) return false;
    std::memcpy(&value, data.data() + at, sizeof(value));
    return true;
}

// CSimpleVariablesSaveStructure is the first native block (0x138 bytes).
// Its mission-pack member is a raw byte at +0x68: native save/load preserves
// the pack number, including numbers greater than one.
inline bool ReadMissionPack(const std::vector<uint8_t>& save, uint8_t& pack) {
    constexpr size_t nextBlock = 5 + 0x138;
    if (save.size() < nextBlock + 5 || std::memcmp(save.data(), "BLOCK", 5) ||
        std::memcmp(save.data() + nextBlock, "BLOCK", 5)) return false;
    pack = save[5 + 0x68];
    return true;
}

inline bool ReadScmLayout(const std::vector<uint8_t>& scm, ScriptLayout& layout) {
    size_t at = 0;
    for (int chunk = 0; chunk < 2; ++chunk) {
        uint32_t next = 0;
        if (at > scm.size() || scm.size() - at < 7 ||
            std::memcmp(scm.data() + at, "\x02\x00\x01", 3) ||
            !Read32(scm, at + 3, next) || next <= at || next > scm.size()) return false;
        at = next;
    }
    if (at > scm.size() || scm.size() - at < 8 + sizeof(layout) ||
        std::memcmp(scm.data() + at, "\x02\x00\x01", 3)) return false;
    std::memcpy(&layout, scm.data() + at + 8, sizeof(layout));
    return layout.main && layout.main <= scm.size() && layout.exclusiveMissions <= layout.missions;
}

inline bool ReadSaveHeader(const std::vector<uint8_t>& save, SaveHeader& header) {
    if (!ReadMissionPack(save, header.missionPack)) return false;
    constexpr size_t scriptBlock = 5 + 0x138 + 5;
    uint32_t globals = 0, jump = 0;
    if (!Read32(save, scriptBlock, globals) || globals < 7 ||
        globals > save.size() - scriptBlock - 4) return false;
    const size_t first = scriptBlock + 4;
    if (std::memcmp(save.data() + first, "\x02\x00\x01", 3) ||
        !Read32(save, first + 3, jump) || jump != globals) return false;
    // Native Save: 70 brains*20, two dwords, 25 building swaps*16,
    // 20 invisible entities*8, 20 vehicle models*4, 10 LOD links*8,
    // 8 animation-group links*20, then two byte flags and the SCM layout.
    constexpr size_t tables = 0x8F2;
    const size_t layoutAt = first + globals + tables;
    if (layoutAt > save.size() || save.size() - layoutAt < sizeof(header.layout)) return false;
    std::memcpy(&header.layout, save.data() + layoutAt, sizeof(header.layout));
    return header.layout.main >= globals && header.layout.main &&
           header.layout.exclusiveMissions <= header.layout.missions;
}

}  // namespace coop::worldscript
