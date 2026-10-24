#pragma once
#include "common/lib/buffer/Buffer.h"
#include "shared/game/Types.h"
namespace coop {
// Orientation as a quaternion (compressed to 4 x int16 on the wire).
struct Quat {
    float x = 0, y = 0, z = 0, w = 1;
    static Quat FromMatrix(const game::CMatrix& m);
    void ToMatrix(game::CMatrix& m) const;  // writes right/forward/up, keeps pos
    static Quat Slerp(const Quat& a, const Quat& b, float t);
    static Quat Turned(const Quat& q,const game::CVector& radiansPerSecond,float seconds);
    void Write(Writer& w) const;
    void Read(Reader& r);
};

}
