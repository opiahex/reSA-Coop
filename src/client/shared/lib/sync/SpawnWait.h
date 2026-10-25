#pragma once
#include <cstdint>
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
namespace coop {
// Local diagnostics only: never changes network identity or forces a blocking load.
struct SpawnWait {
    uint64_t since=0,reported=0;
    const char* reason="queued";
    int priority=0;
    void Block(const char*kind,uint16_t id,const char*why,int rank=0){
        auto now=NowMs();if(!since)since=now;reason=why;priority=rank;
        if(now-since>=1000&&now-reported>=3000){
            LOGI("Spawn waiting kind=%s id=%04X reason=%s priority=%d elapsed=%llu",kind,id,reason,priority,(unsigned long long)(now-since));reported=now;
        }
    }
    void Done(const char*kind,uint16_t id){
        if(since&&NowMs()-since>=1000)LOGI("Spawn ready kind=%s id=%04X elapsed=%llu",kind,id,(unsigned long long)(NowMs()-since));
        *this={};
    }
};
}
