#pragma once
#include <cmath>
#include <array>
#include <vector>
#include "entities/vehicle/VehicleState.h"
namespace coop {
struct HeldProp {
    uint16_t model=0;
    uint8_t bone=255;
    Vec3 offset;
    Quat rotation;
    void Write(Writer&w)const{w.U16(model);w.U8(bone);w.Vec(offset);rotation.Write(w);}
    bool Read(Reader&r){
        model=r.U16();bone=r.U8();offset=r.Vec();rotation.Read(r);
        return r.Ok()&&model>0&&model<20000&&(bone==255||bone<19)&&
            std::isfinite(offset.x)&&std::isfinite(offset.y)&&std::isfinite(offset.z)&&
            std::fabs(offset.x)<20&&std::fabs(offset.y)<20&&std::fabs(offset.z)<20;
    }
};
struct PlayerExtrasState {
    bool phone=false;
    std::array<uint8_t,13> weapons{};
    std::array<uint32_t,13> ammo{};
    std::vector<HeldProp> props;
    void Write(Writer&w)const{
        w.Bool(phone);for(int i=0;i<13;++i){w.U8(weapons[i]);w.U32(ammo[i]);}
        w.U8((uint8_t)props.size());for(auto&p:props)p.Write(w);
    }
    bool Read(Reader&r){
        phone=r.Bool();for(int i=0;i<13;++i){weapons[i]=r.U8();ammo[i]=r.U32();if(weapons[i]>46||ammo[i]>1000000)return false;}
        auto n=r.U8();if(n>3)return false;props.resize(n);for(auto&p:props)if(!p.Read(r))return false;
        return r.Ok();
    }
};
}
