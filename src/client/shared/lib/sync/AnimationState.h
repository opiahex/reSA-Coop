#pragma once
#include <algorithm>
#include <cmath>
#include "common/lib/buffer/Buffer.h"

namespace coop {
// Association identity plus its clock; pointers and task callbacks are never transmitted.
struct AnimationState {
    uint16_t block = 0xFFFF, index = 0, flags = 0;
    int16_t group = -1, id = -1;
    float time = 0, speed = 1;
    bool Present() const { return block != 0xFFFF; }
    bool Same(const AnimationState& other) const { return block == other.block && index == other.index; }
    void Write(Writer& w) const {
        w.U16(block);
        if (!Present()) return;
        w.U16(index); w.U16(flags); w.I16(group); w.I16(id); w.F32(time); w.F32(speed);
    }
    bool Read(Reader& r) {
        *this = {};
        block = r.U16();
        if (Present()) {
            index = r.U16(); flags = r.U16(); group = r.I16(); id = r.I16();
            time = std::clamp(r.F32(), 0.0f, 600.0f);
            speed = std::clamp(r.F32(), 0.0f, 4.0f);
        }
        return r.Ok()&&std::isfinite(time)&&std::isfinite(speed);
    }
    float At(float age, float total) const {
        float t = time + std::max(0.0f, age) * speed * ((flags & 1) ? 1.0f : 0.0f);
        return total > 0 ? ((flags & 2) ? std::fmod(t, total) : std::min(t, total)) : 0;
    }
};
}  // namespace coop
