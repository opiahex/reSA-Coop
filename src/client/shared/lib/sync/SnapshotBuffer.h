#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>

namespace coop {
// Sender-clock history: arrival jitter changes the playout delay, never the
// spacing of samples. State changes remain authoritative outside this buffer.
template<class T> class SnapshotBuffer {
    struct Sample {int64_t tick;T state;};
    std::deque<Sample> samples;
    int64_t tick=0;
    uint32_t last=0;
    uint64_t arrived=0;
    double offset=0,jitter=0;
public:
    void Clear(){samples.clear();arrived=0;jitter=0;}
    size_t Size()const{return samples.size();}
    float DelayMs()const{return (float)std::clamp(40.0+jitter*2.5,40.0,180.0);}
    uint32_t PlayoutTick(uint64_t now)const{
        if(samples.empty())return 0;
        return (uint32_t)(int64_t)std::clamp((double)now-offset-DelayMs(),
            (double)samples.front().tick,(double)samples.back().tick+120.0);
    }
    bool Push(const T& state,uint64_t now){
        if(!samples.empty()&&(int32_t)(state.time-last)<=0)return false;
        if(samples.empty()){tick=state.time;offset=(double)now-tick;}
        else {
            int delta=(int32_t)(state.time-last);tick+=delta;
            double deviation=std::abs((double)(now-arrived)-delta);
            jitter+=(std::min(250.0,deviation)-jitter)*0.125;
            offset=std::min(offset+0.05,(double)now-tick);
        }
        samples.push_back({tick,state});last=state.time;arrived=now;
        while(samples.size()>32)samples.pop_front();
        return true;
    }
    template<class Blend,class Predict>
    T At(uint64_t now,Blend blend,Predict predict)const {
        if(samples.empty())return {};
        double target=(double)now-offset-DelayMs();
        if(target<=samples.front().tick)return samples.front().state;
        for(size_t i=1;i<samples.size();++i)if(target<=samples[i].tick){
            float f=(float)((target-samples[i-1].tick)/(samples[i].tick-samples[i-1].tick));
            return blend(samples[i-1].state,samples[i].state,f);
        }
        auto result=samples.back().state;
        float age=(float)std::max(0.0,target-samples.back().tick)/1000;
        predict(result,std::min(age,0.12f),age>0.25f);
        return result;
    }
};
}
