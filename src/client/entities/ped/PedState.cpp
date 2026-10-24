#include "entities/ped/Peds.h"
#include <cmath>
namespace coop {
constexpr uint16_t kNoAnim=0xFFFF;
void PedSyncState::Write(Writer&w)const{const auto&s=*this;
    w.U32(s.time);
    w.Vec({s.pos.x,s.pos.y,s.pos.z});
    w.VecQ({s.vel.x,s.vel.y,s.vel.z}, 30.0f);
    w.Angle(s.heading);
    w.U8(s.moveState);
    w.F32(s.health);
    w.F32(s.armour);
    w.U8(s.weapon);
    w.U8(s.flags);
    w.U8(s.entityFlags);
    w.Entity(ENTITY_VEHICLE,s.vehNetId);
    w.I8(s.seat);
    w.I8(s.area);
    if (s.flags & PedSyncState::F_AIMING) {
        w.VecQ({s.aim.x,s.aim.y,s.aim.z}, 200.0f);
        w.U8(s.aimKind);
        if(s.aimKind==2)w.Entity(ENTITY_PED,s.aimId);else if(s.aimKind)w.U16(s.aimId);
    }
    if (s.flags & PedSyncState::F_DRIVEBY) { w.U8(s.drivebyStyle); w.U8(s.drivebyFrequency); }
    w.U16(s.animBlock);
    if (s.animBlock != kNoAnim) {
        w.U16(s.animIdx);
        w.U16(s.animFlags);
        w.F32(s.animTime);
        w.F32(s.animSpeed);
    }
    s.motion.Write(w);
    s.interaction.Write(w);
}
bool PedSyncState::Read(Reader&r){auto&s=*this;
    s.time = r.U32();
    auto p=r.Vec();s.pos={p.x,p.y,p.z};
    auto v=r.VecQ(30.0f);s.vel={v.x,v.y,v.z};
    s.heading=r.Angle();
    s.moveState = r.U8();
    s.health = r.F32();
    s.armour = r.F32();
    s.weapon = r.U8();
    if (s.weapon > 46) s.weapon = 0;
    s.flags = r.U8();
    s.entityFlags = r.U8();
    s.vehNetId = r.U16();
    s.seat = r.I8();
    s.area = r.I8();
    if (s.flags & PedSyncState::F_AIMING) {
        auto a=r.VecQ(200.0f);s.aim={a.x,a.y,a.z};
        s.aimKind = r.U8();
        if (s.aimKind) s.aimId = r.U16();
    }
    if (s.flags & PedSyncState::F_DRIVEBY) { s.drivebyStyle = std::min<uint8_t>(r.U8(),8); s.drivebyFrequency = std::min<uint8_t>(r.U8(),100); }
    s.animBlock = r.U16();
    if (s.animBlock != kNoAnim) {
        s.animIdx = r.U16();
        s.animFlags = r.U16();
        s.animTime = r.F32();
        s.animSpeed = std::clamp(r.F32(), 0.0f, 4.0f);
    }
    bool motionOk=s.motion.Read(r)&&s.interaction.Read(r);
    return r.Ok()&&motionOk&&std::isfinite(s.pos.x)&&std::isfinite(s.pos.y)&&std::isfinite(s.pos.z)&&
        std::isfinite(s.health)&&s.health>=0&&s.health<=1000000&&std::isfinite(s.armour)&&s.armour>=0&&s.armour<=1000000&&
        std::isfinite(s.animTime)&&std::isfinite(s.animSpeed)&&s.moveState<=7&&s.seat>=-1&&s.seat<8&&s.aimKind<=2&&
        (!(s.flags&F_IN_VEHICLE)||s.vehNetId!=0)&&(!(s.entityFlags&~15u));
}
}
