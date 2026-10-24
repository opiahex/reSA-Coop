#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <array>
#include "shared/game/Types.h"
namespace coop {
// Owner-clock evidence, bounded to one second. Never rewinds the live world or
// tests an old ray against the target's new position.
class HitHistory {
    struct Sample {uint32_t tick;game::CVector pos;int8_t area;bool alive;};
    std::array<Sample,48> samples{};
    size_t next=0,count=0;
    uint32_t lastRevive=0;
public:
    void Push(uint32_t tick,const game::CVector&p,int8_t area,bool alive){
        if(count&&!samples[(next+47)%48].alive&&alive)lastRevive=tick;
        samples[next]={tick,p,area,alive};next=(next+1)%48;count=std::min<size_t>(48,count+1);
        while(count&&(int32_t)(tick-samples[(next+48-count)%48].tick)>1200)--count;
    }
    bool Accept(uint32_t now,uint32_t tick,const game::CVector&p,int8_t area,float tolerance)const{
        int32_t age=(int32_t)(now-tick);
        if(!tick||age < -100||age>1200||(lastRevive&&(int32_t)(tick-lastRevive)<0))return false;
        const Sample* nearest=nullptr;int distance=0x7fffffff;
        for(size_t i=0;i<count;++i){const auto&s=samples[(next+48-count+i)%48];int d=std::abs((int32_t)(tick-s.tick));if(d<distance){nearest=&s;distance=d;}}
        return nearest&&nearest->alive&&nearest->area==area&&distance<=140&&(nearest->pos-p).Length()<=tolerance;
    }
    size_t Size()const{return count;}
};
}
