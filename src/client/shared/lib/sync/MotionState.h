#pragma once
#include <cmath>
#include "common/lib/buffer/Buffer.h"
namespace coop {
struct MotionState {
    enum:uint8_t {NORMAL, JUMP, FALL, GETUP, SWIM, CLIMB, PARACHUTE, MOUNTED};
    uint32_t epoch=0;
    uint8_t kind=NORMAL,phase=0;
    uint16_t support=0;
    bool objectSupport=false;
    Vec3 offset;
    float heading=0,pitch=0,roll=0,torsoPitch=0,swimHeading=0;
    void Write(Writer&w)const{
        w.U32(epoch);w.U8(kind);w.U8(phase);w.Bool(objectSupport);w.U16(support);if(objectSupport)w.Reference(ENTITY_OBJECT,support);else if(support&0x8000u)w.Reference(ENTITY_TRAIN,(support>>4)&255u,ENTITY_USE,((support>>12)&7u)-1u);else w.Reference(ENTITY_VEHICLE,support);
        if(support){w.Vec(offset);w.Angle(heading);}
        if(kind==SWIM){w.F32(pitch);w.F32(roll);w.F32(torsoPitch);w.F32(swimHeading);}
    }
    bool Read(Reader&r){
        *this={};
        epoch=r.U32();kind=r.U8();phase=r.U8();objectSupport=r.Bool();support=r.U16();
        if(support){offset=r.Vec();heading=r.Angle();}
        if(kind==SWIM){pitch=r.F32();roll=r.F32();torsoPitch=r.F32();swimHeading=r.F32();}
        return r.Ok()&&kind<=MOUNTED&&(!objectSupport||(support&&kind==MOUNTED))&&(kind!=SWIM||phase<=5)&&(kind!=CLIMB||phase<=6)&&std::isfinite(heading)&&std::isfinite(pitch)&&std::isfinite(roll)&&
            std::isfinite(torsoPitch)&&std::isfinite(swimHeading)&&std::fabs(torsoPitch)<7&&std::fabs(swimHeading)<7&&std::fabs(pitch)<7&&std::fabs(roll)<7&&std::isfinite(offset.x)&&std::isfinite(offset.y)&&std::isfinite(offset.z)&&
            std::fabs(offset.x)<100&&std::fabs(offset.y)<100&&std::fabs(offset.z)<40;
    }
};
}
