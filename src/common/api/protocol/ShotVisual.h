#pragma once
#include "common/lib/buffer/Buffer.h"

namespace coop {
// Visuals follow the shooter's actual ray. Damage remains a separate, owner-validated G_DAMAGE event.
struct ShotVisual {
    uint32_t sequence = 0;
    uint8_t weapon = 22, flags = 0, surface = 0;
    Vec3 start, end, normal;
    enum : uint8_t { BLOOD = 1, IMPACT = 2 };
    void Write(Writer& w) const {
        w.U32(sequence); w.U8(weapon); w.U8(flags); w.U8(surface);
        w.Vec(start); w.Vec(end); w.Vec(normal);
    }
    bool Read(Reader& r) {
        sequence = r.U32(); weapon = r.U8(); flags = r.U8(); surface = r.U8();
        start = r.Vec(); end = r.Vec(); normal = r.Vec();
        float x = end.x - start.x, y = end.y - start.y, z = end.z - start.z;
        return r.Ok() && ((weapon >= 22 && weapon <= 34) || weapon == 38 || weapon == 52) &&
               !(flags & ~(BLOOD | IMPACT)) && x*x + y*y + z*z < 10000.0f * 10000.0f;
    }
};
}  // namespace coop
