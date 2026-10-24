#include "shared/lib/sync/Quat.h"
#include <algorithm>
#include <cmath>
namespace coop {
using game::CMatrix;
Quat Quat::FromMatrix(const CMatrix& m) {
    // columns: right (X), forward (Y), up (Z)
    float m00 = m.right.x, m01 = m.forward.x, m02 = m.up.x;
    float m10 = m.right.y, m11 = m.forward.y, m12 = m.up.y;
    float m20 = m.right.z, m21 = m.forward.z, m22 = m.up.z;
    Quat q;
    float tr = m00 + m11 + m22;
    if (tr > 0) {
        float s = std::sqrt(tr + 1.0f) * 2.0f;
        q.w = 0.25f * s;
        q.x = (m21 - m12) / s;
        q.y = (m02 - m20) / s;
        q.z = (m10 - m01) / s;
    } else if (m00 > m11 && m00 > m22) {
        float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
        q.w = (m21 - m12) / s;
        q.x = 0.25f * s;
        q.y = (m01 + m10) / s;
        q.z = (m02 + m20) / s;
    } else if (m11 > m22) {
        float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
        q.w = (m02 - m20) / s;
        q.x = (m01 + m10) / s;
        q.y = 0.25f * s;
        q.z = (m12 + m21) / s;
    } else {
        float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
        q.w = (m10 - m01) / s;
        q.x = (m02 + m20) / s;
        q.y = (m12 + m21) / s;
        q.z = 0.25f * s;
    }
    float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (n > 1e-6f) {
        q.x /= n;
        q.y /= n;
        q.z /= n;
        q.w /= n;
    }
    return q;
}

void Quat::ToMatrix(CMatrix& m) const {
    float xx = x * x, yy = y * y, zz = z * z, xy = x * y, xz = x * z, yz = y * z, wx = w * x, wy = w * y, wz = w * z;
    m.right = {1 - 2 * (yy + zz), 2 * (xy + wz), 2 * (xz - wy)};
    m.forward = {2 * (xy - wz), 1 - 2 * (xx + zz), 2 * (yz + wx)};
    m.up = {2 * (xz + wy), 2 * (yz - wx), 1 - 2 * (xx + yy)};
}

Quat Quat::Slerp(const Quat& a, const Quat& bIn, float t) {
    Quat b = bIn;
    float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (d < 0) {
        d = -d;
        b.x = -b.x;
        b.y = -b.y;
        b.z = -b.z;
        b.w = -b.w;
    }
    Quat r;
    if (d > 0.9995f) {
        r.x = a.x + (b.x - a.x) * t;
        r.y = a.y + (b.y - a.y) * t;
        r.z = a.z + (b.z - a.z) * t;
        r.w = a.w + (b.w - a.w) * t;
    } else {
        float th = std::acos(d), s = std::sin(th);
        float wa = std::sin((1 - t) * th) / s, wb = std::sin(t * th) / s;
        r.x = a.x * wa + b.x * wb;
        r.y = a.y * wa + b.y * wb;
        r.z = a.z * wa + b.z * wb;
        r.w = a.w * wa + b.w * wb;
    }
    float n = std::sqrt(r.x * r.x + r.y * r.y + r.z * r.z + r.w * r.w);
    r.x /= n;
    r.y /= n;
    r.z /= n;
    r.w /= n;
    return r;
}

Quat Quat::Turned(const Quat&q,const game::CVector&v,float seconds){
    float speed=v.Length();if(!std::isfinite(speed)||!std::isfinite(seconds)||speed<1e-6f||seconds<=0)return q;
    float half=speed*seconds*.5f,s=std::sin(half)/speed;Quat d{v.x*s,v.y*s,v.z*s,std::cos(half)};
    return {d.w*q.x+d.x*q.w+d.y*q.z-d.z*q.y,d.w*q.y-d.x*q.z+d.y*q.w+d.z*q.x,d.w*q.z+d.x*q.y-d.y*q.x+d.z*q.w,d.w*q.w-d.x*q.x-d.y*q.y-d.z*q.z};
}
void Quat::Write(Writer& out) const {
    out.I16(Writer::Quant(x, 1.0f));
    out.I16(Writer::Quant(y, 1.0f));
    out.I16(Writer::Quant(z, 1.0f));
    out.I16(Writer::Quant(this->w, 1.0f));
}

void Quat::Read(Reader& r) {
    x = r.I16() / 32767.0f;
    y = r.I16() / 32767.0f;
    z = r.I16() / 32767.0f;
    w = r.I16() / 32767.0f;
    float n = std::sqrt(x * x + y * y + z * z + w * w);
    if (n < 1e-4f) {
        x = y = z = 0;
        w = 1;
    } else {
        x /= n;
        y /= n;
        z /= n;
        w /= n;
    }
}

}
