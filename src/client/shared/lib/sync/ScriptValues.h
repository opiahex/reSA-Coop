#pragma once
#include <array>
#include <cstdint>
namespace coop {
// Borrow host values only while native HUD formatting/countdown code runs.
// A guest's actual script globals are restored before its next instruction.
template<size_t N>class ScriptValueOverlay {
    int32_t*base;std::array<uint32_t,N>offsets;std::array<int32_t,N>saved{};std::array<bool,N>valid{};
public:
    ScriptValueOverlay(int32_t*data,uint32_t bytes,const std::array<uint32_t,N>&off,const std::array<int32_t,N>&values):base(data),offsets(off){
        for(size_t i=0;i<N;++i){auto o=off[i];bool duplicate=false;for(size_t j=0;j<i;++j)duplicate|=valid[j]&&off[j]==o;
            valid[i]=!duplicate&&o>=4&&!(o&3)&&bytes>=4&&o<=bytes-4;
            if(valid[i]){saved[i]=base[o/4];base[o/4]=values[i];}
        }
    }
    void Capture(std::array<int32_t,N>&values)const{for(size_t i=0;i<N;++i)if(valid[i])values[i]=base[offsets[i]/4];}
    ~ScriptValueOverlay(){for(size_t i=N;i>0;--i)if(valid[i-1])base[offsets[i-1]/4]=saved[i-1];}
};
}
