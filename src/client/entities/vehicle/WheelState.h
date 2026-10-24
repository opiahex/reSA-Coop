#pragma once
#include "shared/game/Game.h"
#include "common/lib/buffer/Buffer.h"
#include <algorithm>
#include <cmath>

namespace coop {
// Native wheel phase/rate (radians per game step) and tyre contact state.
// Aircraft use overlapping fields for rotors; they must never use this layout.
struct WheelState {
    uint8_t count=0, states=0, contact=0;
    float phase[4]{}, rate[4]{};
    static int Count(game::CVehicle* v) {
        switch(v->VehicleSubClass()) {
        case game::VEHICLE_TYPE_AUTOMOBILE: case game::VEHICLE_TYPE_MTRUCK:
        case game::VEHICLE_TYPE_QUAD: case game::VEHICLE_TYPE_TRAILER:return 4;
        case game::VEHICLE_TYPE_BIKE: case game::VEHICLE_TYPE_BMX:return 2;
        default:return 0;
        }
    }
    static float Wrap(float a){return std::isfinite(a)?std::remainder(a,6.2831853f):0.f;}
    static WheelState Capture(game::CVehicle* v) {
        WheelState s;s.count=(uint8_t)Count(v);bool bike=s.count==2;
        for(int i=0;i<s.count;++i){
            s.phase[i]=Wrap(game::Field<float>(v,(bike?0x750:0x828)+i*4));
            float rate=game::Field<float>(v,(bike?0x758:0x848)+i*4);
            s.rate[i]=std::isfinite(rate)?std::clamp(rate,-32.f,32.f):0.f;
            s.states|=(game::Field<uint32_t>(v,(bike?0x80C:0x968)+i*4)&3)<<(i*2);
            int line=bike?i*2:i;
            if(game::Field<float>(v,(bike?0x710:0x7D4)+line*4)<1.f ||
               (bike&&game::Field<float>(v,0x710+(line+1)*4)<1.f))s.contact|=1<<i;
        }
        return s;
    }
    void Apply(game::CVehicle* v) const {
        if(!count||Count(v)!=count)return;
        bool bike=count==2;
        for(int i=0;i<count;++i){
            game::Field<float>(v,(bike?0x750:0x828)+i*4)=phase[i];
            game::Field<float>(v,(bike?0x758:0x848)+i*4)=rate[i];
            // Wheel state also drives native squeal/skid particles. An airborne
            // wheel cannot skid against this client's different ground contact.
            game::Field<uint32_t>(v,(bike?0x80C:0x968)+i*4)=(contact&(1<<i))?((states>>(i*2))&3):0;
        }
    }
    void Write(Writer&w)const{
        w.U8(count);w.U8(states);w.U8(contact);
        for(int i=0;i<count;++i){w.Angle(phase[i]);w.I16(Writer::Quant(rate[i],32.f));}
    }
    bool Read(Reader&r){
        count=r.U8();states=r.U8();contact=r.U8();if(count!=2&&count!=4)return false;
        if(contact>>count)return false;
        for(int i=0;i<count;++i){phase[i]=Wrap(r.Angle());rate[i]=r.I16()/32767.f*32.f;}
        return r.Ok();
    }
    static WheelState Blend(const WheelState&a,const WheelState&b,float t,float seconds=0){
        auto s=b;if(a.count==b.count)for(int i=0;i<s.count;++i){
            float delta=Wrap(b.phase[i]-a.phase[i]);
            if(seconds>0&&seconds<.5f){float expected=(a.rate[i]+b.rate[i])*.5f*seconds*50.f;delta+=std::round((expected-delta)/6.2831853f)*6.2831853f;}
            s.phase[i]=Wrap(a.phase[i]+delta*t);s.rate[i]=a.rate[i]+(b.rate[i]-a.rate[i])*t;
        }
        return s;
    }
    void Predict(float seconds,bool stale){for(int i=0;i<count;++i){if(stale)rate[i]=0;phase[i]=Wrap(phase[i]+rate[i]*seconds*50.f);}}
};
}
