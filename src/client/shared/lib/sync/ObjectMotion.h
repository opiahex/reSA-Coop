#pragma once
#include <cmath>
#include "shared/lib/sync/Quat.h"
#include "shared/lib/sync/TrainSupport.h"
namespace coop {
struct ObjectPhysics {
    // Exclude transient contact/attachment bits and the replica's local
    // DisableCollisionForce/DontApplySpeed freeze. These are configured
    // physical properties; the current simulator owns the local freeze.
    static constexpr uint32_t PhysicalMask=0x00FC00FBu,InteractionMask=0x00042000u,FreezeMask=0x00002004u;
    float mass=1,turnMass=1;
    uint32_t physicalFlags=0,interactionFlags=0;
    uint16_t freezeFlags=0; // native configured freeze, separate from a replica's local freeze
    void Write(Writer&w)const{w.F32(mass);w.F32(turnMass);w.U32(physicalFlags);w.U32(interactionFlags);w.U16(freezeFlags);}
    bool Read(Reader&r){mass=r.F32();turnMass=r.F32();physicalFlags=r.U32();interactionFlags=r.U32();freezeFlags=r.U16();
        return r.Ok()&&std::isfinite(mass)&&mass>=0&&mass<=1e10f&&std::isfinite(turnMass)&&turnMass>=0&&turnMass<=1e10f&&
            !(physicalFlags&~PhysicalMask)&&!(interactionFlags&~InteractionMask)&&!(freezeFlags&~FreezeMask);}
    bool SameAs(const ObjectPhysics&other)const{return mass==other.mass&&turnMass==other.turnMass&&physicalFlags==other.physicalFlags&&interactionFlags==other.interactionFlags&&freezeFlags==other.freezeFlags;}
};
struct ObjectMotion {
    game::CVector velocity,turn,offset,euler;
    Quat rotation,bodyRotation;game::CVector bodyOffset;
    uint8_t parentKind=0,holder=255,simulator=255;
    uint16_t supportVehicle=0;
    uint16_t parentId=0;
    TrainAttachment train;
    uint32_t revision=0;
    bool dynamic=false;
    ObjectPhysics physics;
    void Write(Writer&w)const{
        w.Vec({velocity.x,velocity.y,velocity.z});w.Vec({turn.x,turn.y,turn.z});
        w.U8(parentKind);if(parentKind==1)w.Entity(ENTITY_VEHICLE,parentId);else if(parentKind==2)w.Entity(ENTITY_PED,parentId);else if(parentKind==4)w.Entity(ENTITY_OBJECT,parentId);else if(parentKind==5)train.Write(w);else w.U16(parentId);w.Vec({offset.x,offset.y,offset.z});w.Vec({euler.x,euler.y,euler.z});rotation.Write(w);
        w.Vec({bodyOffset.x,bodyOffset.y,bodyOffset.z});bodyRotation.Write(w);w.U8(holder);w.U32(revision);w.Bool(dynamic);w.U8(simulator);w.Entity(ENTITY_VEHICLE,supportVehicle);physics.Write(w);
    }
    bool Read(Reader&r){
        auto v=r.Vec(),t=r.Vec();velocity={v.x,v.y,v.z};turn={t.x,t.y,t.z};parentKind=r.U8();
        if(parentKind==5){if(!train.Read(r)||!train.Valid())return false;parentId=train.id;}else{parentId=r.U16();train={};}
        auto o=r.Vec(),e=r.Vec();offset={o.x,o.y,o.z};euler={e.x,e.y,e.z};rotation.Read(r);auto b=r.Vec();bodyOffset={b.x,b.y,b.z};bodyRotation.Read(r);holder=r.U8();revision=r.U32();dynamic=r.Bool();simulator=r.U8();supportVehicle=r.U16();if(!physics.Read(r))return false;
        return r.Ok()&&parentKind<=5&&(holder<3||holder==255)&&(simulator<3||simulator==255)&&(simulator==255||supportVehicle)&&velocity.Length()<100&&turn.Length()<100&&bodyOffset.Length()<100&&offset.Length()<100&&euler.Length()<100;
    }
};
}
