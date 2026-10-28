#pragma once
#include <cstdint>

namespace coop {
// Audio tokens belong to the existing host world and story run. Token numbers
// restart after a world load and must never be compared across that boundary.
struct DialogueContext {
    uint32_t world = 0;
    uint64_t run = 0;
    bool operator==(const DialogueContext& other) const { return world == other.world && run == other.run; }
    bool operator!=(const DialogueContext& other) const { return !(*this == other); }
    template<class W> void Write(W& w) const { w.U32(world); w.U64(run); }
    template<class R> bool Read(R& r) { world = r.U32(); run = r.U64(); return r.Ok(); }
    enum Verdict { Current, Future, Stale };
    Verdict Compare(const DialogueContext& expected) const {
        if (!world || world != expected.world) return Stale;
        if (run == expected.run) return Current;
        // StoryCoop snapshots can wait for entity dependencies while dialogue
        // without entity references is already dispatchable on this channel.
        return run > expected.run ? Future : Stale;
    }
};
} // namespace coop
