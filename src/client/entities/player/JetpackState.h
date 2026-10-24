#pragma once
#include <array>
#include <algorithm>
#include <cmath>
#include "common/lib/buffer/Buffer.h"
#include "shared/game/Game.h"
namespace coop {
struct JetpackState {
    bool stop=false,attack=false;int8_t forward=0;
    float strafe=0,angle=0,fx=0;std::array<float,6>legs{};
    static JetpackState Capture(void*task){
        JetpackState s;if(!task)return s;
        s.stop=game::Field<uint8_t>(task,0xD)!=0;s.attack=game::Field<uint8_t>(task,0xB)!=0;
        s.forward=std::clamp<int8_t>(game::Field<int8_t>(task,0xE),-1,1);
        s.strafe=game::Field<float>(task,0x10);s.angle=game::Field<float>(task,0x14);s.fx=game::Field<float>(task,0x6C);
        for(int i=0;i<6;++i)s.legs[i]=game::Field<float>(task,0x18+i*4);return s;
    }
    void Apply(void*task)const{
        game::Field<uint8_t>(task,0xD)=stop;game::Field<uint8_t>(task,0xB)=attack;game::Field<int8_t>(task,0xE)=forward;
        game::Field<float>(task,0x10)=strafe;game::Field<float>(task,0x14)=angle;game::Field<float>(task,0x6C)=fx;
        for(int i=0;i<6;++i)game::Field<float>(task,0x18+i*4)=legs[i];
    }
    void Write(Writer&w)const{
        w.U8((stop?1:0)|(attack?2:0));w.I8(forward);w.I16(Writer::Quant(strafe,3.f));w.I16(Writer::Quant(angle,3.2f));w.Unit8(fx);
        for(auto v:legs)w.I16(Writer::Quant(v,3.2f));
    }
    bool Read(Reader&r){
        auto flags=r.U8();stop=flags&1;attack=flags&2;forward=r.I8();strafe=r.I16()/32767.f*3.f;angle=r.I16()/32767.f*3.2f;fx=r.Unit8();
        for(auto&v:legs)v=r.I16()/32767.f*3.2f;return r.Ok()&&flags<4&&forward>=-1&&forward<=1;
    }
    static JetpackState Blend(const JetpackState&a,const JetpackState&b,float t){
        auto s=b;s.strafe=a.strafe+(b.strafe-a.strafe)*t;s.angle=a.angle+(b.angle-a.angle)*t;s.fx=a.fx+(b.fx-a.fx)*t;
        for(int i=0;i<6;++i)s.legs[i]=a.legs[i]+(b.legs[i]-a.legs[i])*t;return s;
    }
};
}
