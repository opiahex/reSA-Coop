#pragma once
#include "shared/lib/sync/Quat.h"
#include "shared/lib/sync/TrainSupport.h"
namespace coop {
struct ObjectMotion {
    game::CVector velocity,turn,offset,euler;
    Quat rotation,bodyRotation;game::CVector bodyOffset;
    uint8_t parentKind=0,holder=255,simulator=255;
    uint16_t supportVehicle=0;
    uint16_t parentId=0;
    TrainAttachment train;
    uint32_t revision=0;
    bool dynamic=false;
    void Write(Writer&w)const{
        w.Vec({velocity.x,velocity.y,velocity.z});w.Vec({turn.x,turn.y,turn.z});
        w.U8(parentKind);if(parentKind==1)w.Entity(ENTITY_VEHICLE,parentId);else if(parentKind==2)w.Entity(ENTITY_PED,parentId);else if(parentKind==4)w.Entity(ENTITY_OBJECT,parentId);else if(parentKind==5)train.Write(w);else w.U16(parentId);w.Vec({offset.x,offset.y,offset.z});w.Vec({euler.x,euler.y,euler.z});rotation.Write(w);
        w.Vec({bodyOffset.x,bodyOffset.y,bodyOffset.z});bodyRotation.Write(w);w.U8(holder);w.U32(revision);w.Bool(dynamic);w.U8(simulator);w.Entity(ENTITY_VEHICLE,supportVehicle);
    }
    bool Read(Reader&r){
        auto v=r.Vec(),t=r.Vec();velocity={v.x,v.y,v.z};turn={t.x,t.y,t.z};parentKind=r.U8();
        if(parentKind==5){if(!train.Read(r)||!train.Valid())return false;parentId=train.id;}else{parentId=r.U16();train={};}
        auto o=r.Vec(),e=r.Vec();offset={o.x,o.y,o.z};euler={e.x,e.y,e.z};rotation.Read(r);auto b=r.Vec();bodyOffset={b.x,b.y,b.z};bodyRotation.Read(r);holder=r.U8();revision=r.U32();dynamic=r.Bool();simulator=r.U8();supportVehicle=r.U16();
        return r.Ok()&&parentKind<=5&&(holder<3||holder==255)&&(simulator<3||simulator==255)&&(simulator==255||supportVehicle)&&velocity.Length()<100&&turn.Length()<100&&bodyOffset.Length()<100&&offset.Length()<100&&euler.Length()<100;
    }
};
}
