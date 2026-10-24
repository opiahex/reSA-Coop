#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#include "common/lib/buffer/EntityRef.h"

namespace coop {

struct Vec3 {
    float x = 0, y = 0, z = 0;
};

// Little-endian binary writer used for all network messages.
class Writer {
public:
    Writer() { m_data.reserve(64); }
    explicit Writer(uint8_t msgId) {
        m_data.reserve(64);
        U8(msgId);
    }

    void U8(uint8_t v) { m_data.push_back(v); }
    void I8(int8_t v) { U8((uint8_t)v); }
    void Bool(bool v) { U8(v ? 1 : 0); }
    void U16(uint16_t v) { Raw(&v, 2); }
    void Entity(uint8_t kind,uint16_t id,uint8_t flags=ENTITY_USE,uint8_t scope=255,uint64_t generation=0){
        U16(id);Reference(kind,id,flags,scope,generation);
    }
    void Reference(uint8_t kind,uint16_t id,uint8_t flags=ENTITY_USE,uint8_t scope=255,uint64_t generation=0){
        if(!id||(kind==ENTITY_VEHICLE&&id==0xFFFF))return;
        for(auto&ref:m_entities)if(ref.kind==kind&&ref.id==id&&ref.scope==scope){ref.flags|=flags;return;}
        m_entities.push_back({kind,scope,id,generation,flags});
    }
    const std::vector<EntityRef>& Entities()const{return m_entities;}
    void I16(int16_t v) { Raw(&v, 2); }
    void U32(uint32_t v) { Raw(&v, 4); }
    void I32(int32_t v) { Raw(&v, 4); }
    void U64(uint64_t v) { Raw(&v, 8); }
    void F32(float v) { Raw(&v, 4); }
    void Vec(const Vec3& v) {
        F32(v.x);
        F32(v.y);
        F32(v.z);
    }
    // Velocity-like vector quantized to int16 with given range (+-range).
    void VecQ(const Vec3& v, float range) {
        I16(Quant(v.x, range));
        I16(Quant(v.y, range));
        I16(Quant(v.z, range));
    }
    // Angle in radians quantized to 16 bits.
    void Angle(float rad) {
        float a = std::fmod(rad, 6.2831853f);
        if (a < 0) a += 6.2831853f;
        U16((uint16_t)(a / 6.2831853f * 65535.0f + 0.5f));
    }
    // Value in [0,1] (or [-1,1] when signed) quantized to 8 bits.
    void Unit8(float v) { U8((uint8_t)(std::fmin(std::fmax(v, 0.0f), 1.0f) * 255.0f + 0.5f)); }
    void SUnit8(float v) { I8((int8_t)(std::fmin(std::fmax(v, -1.0f), 1.0f) * 127.0f)); }
    void VarU32(uint32_t v) {
        while (v >= 0x80) {
            U8((uint8_t)(v | 0x80));
            v >>= 7;
        }
        U8((uint8_t)v);
    }
    void Str(std::string_view s, size_t maxLen = 4096) {
        if (s.size() > maxLen) s = s.substr(0, maxLen);
        VarU32((uint32_t)s.size());
        Raw(s.data(), s.size());
    }
    void Bytes(const void* p, size_t n) {
        VarU32((uint32_t)n);
        Raw(p, n);
    }
    void Raw(const void* p, size_t n) {
        const uint8_t* b = (const uint8_t*)p;
        m_data.insert(m_data.end(), b, b + n);
    }

    const uint8_t* Data() const { return m_data.data(); }
    size_t Size() const { return m_data.size(); }
    std::vector<uint8_t>& Buffer() { return m_data; }

    static int16_t Quant(float v, float range) {
        float q = v / range * 32767.0f;
        if (q > 32767.0f) q = 32767.0f;
        if (q < -32767.0f) q = -32767.0f;
        return (int16_t)std::lround(q);
    }

private:
    std::vector<uint8_t> m_data;
    std::vector<EntityRef> m_entities;
};

// Bounds-checked reader. On underflow it sets the error flag and returns zeros.
class Reader {
public:
    Reader(const uint8_t* data, size_t size) : m_p(data), m_end(data + size) {}

    uint8_t U8() {
        uint8_t v = 0;
        Raw(&v, 1);
        return v;
    }
    int8_t I8() { return (int8_t)U8(); }
    bool Bool() { return U8() != 0; }
    uint16_t U16() {
        uint16_t v = 0;
        Raw(&v, 2);
        return v;
    }
    int16_t I16() {
        int16_t v = 0;
        Raw(&v, 2);
        return v;
    }
    uint32_t U32() {
        uint32_t v = 0;
        Raw(&v, 4);
        return v;
    }
    int32_t I32() {
        int32_t v = 0;
        Raw(&v, 4);
        return v;
    }
    uint64_t U64() {
        uint64_t v = 0;
        Raw(&v, 8);
        return v;
    }
    float F32() {
        float v = 0;
        Raw(&v, 4);
        if (!std::isfinite(v)) { m_error=true;v=0.0f; } // reject corrupt state instead of teleporting an axis to zero
        return v;
    }
    Vec3 Vec() {
        Vec3 v;
        v.x = F32();
        v.y = F32();
        v.z = F32();
        return v;
    }
    Vec3 VecQ(float range) {
        Vec3 v;
        v.x = I16() / 32767.0f * range;
        v.y = I16() / 32767.0f * range;
        v.z = I16() / 32767.0f * range;
        return v;
    }
    float Angle() { return U16() / 65535.0f * 6.2831853f; }
    float Unit8() { return U8() / 255.0f; }
    float SUnit8() { return I8() / 127.0f; }
    uint32_t VarU32() {
        uint32_t v = 0;
        for (int shift = 0; shift < 35; shift += 7) {
            uint8_t b = U8();
            v |= (uint32_t)(b & 0x7F) << shift;
            if (!(b & 0x80)) return v;
        }
        m_error = true;
        return 0;
    }
    std::string Str(size_t maxLen = 4096) {
        uint32_t n = VarU32();
        if (n > maxLen || n > Remaining()) {
            m_error = true;
            return {};
        }
        std::string s((const char*)m_p, n);
        m_p += n;
        return s;
    }
    std::vector<uint8_t> Bytes(size_t maxLen = 1 << 20) {
        uint32_t n = VarU32();
        if (n > maxLen || n > Remaining()) {
            m_error = true;
            return {};
        }
        std::vector<uint8_t> v(m_p, m_p + n);
        m_p += n;
        return v;
    }
    void Raw(void* out, size_t n) {
        if (Remaining() < n) {
            m_error = true;
            std::memset(out, 0, n);
            m_p = m_end;
            return;
        }
        std::memcpy(out, m_p, n);
        m_p += n;
    }
    size_t Remaining() const { return (size_t)(m_end - m_p); }
    const uint8_t* Cursor() const { return m_p; }
    bool Ok() const { return !m_error; }

private:
    const uint8_t* m_p;
    const uint8_t* m_end;
    bool m_error = false;
};

}  // namespace coop
