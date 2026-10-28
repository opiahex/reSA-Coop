// reSA-Coop Diagnostic System
// Copyright (c) 2026 opiahex
// Original source: https://github.com/opiahex/reSA-Coop
// SPDX-License-Identifier: LicenseRef-reSA-Coop-Diagnostics-Attribution-1.0
// See DIAGNOSTICS-LICENSE.txt, DIAGNOSTICS-NOTICE.md and DIAGNOSTICS-COMPONENTS.md.

#pragma once
// Pre-opened, bounded, numeric-only crash snapshot. This is independent of
// crash.log: a bad stack or a locked CRT stream must not hide this evidence.
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <type_traits>
#include "common/config/version/Version.h"
#include "common/lib/diagnostics/FlightRecorder.h"

namespace coop::crash_tail {
// v1 is little-endian (supported GTA build is x86). Every field has a fixed
// width; no native pointers, addresses of buffers, or structure padding are
// persisted. A reader must validate magic/schema/header/event/frame lengths.
#pragma pack(push,1)
struct CountersV1 {
    uint32_t accepted,busyDrops,lifecycleDrops,historyOverwrites,drainDrops,ioErrors;
    uint32_t segmentsEvicted,eventsEvicted,rotationFailures,exportFailed;
};
struct HeaderV1 {
    char magic[4];uint16_t schema,headerBytes,eventBytes,reserved;
    uint32_t frameBytes,count,busy,pid,protocol,kind; // 1 first-chance, 2 unhandled
    uint64_t qpcFrequency;
    CountersV1 counters;
    char version[24];
};
struct EventV1 {
    uint64_t fingerprint,generation,ticks,sequence;
    uint32_t wireTime,incarnation,world,motionEpoch,nativeHandle,pid,tid;
    uint16_t entity,stage,reason;
    uint8_t msg,channel,source,recipient,localPeer;
    float health,maxHealth,armour,beforeHealth,afterHealth;
};
struct FrameV1 {HeaderV1 header;EventV1 events[64];};
#pragma pack(pop)
static_assert(std::is_trivially_copyable_v<HeaderV1> && std::is_trivially_copyable_v<EventV1>);
static_assert(sizeof(HeaderV1)<256 && sizeof(EventV1)<256);

// Initialized only during normal startup. This state intentionally remains
// alive until the OS closes the process handle; never close it from a handler.
inline HANDLE file=INVALID_HANDLE_VALUE;
inline volatile LONG busy=0, firstAttempts=0,unhandledAttempts=0,broken=0;
inline uint32_t processId=0;
inline uint64_t qpcFrequency=0;
inline FrameV1 frame{};
inline flight::Event snapshot[64]{};

inline bool Init(const std::wstring& coopDir) {
    char enabled[8]{};
    if(GetEnvironmentVariableA("COOP_FLIGHT_RECORDER",enabled,sizeof enabled)!=1 || enabled[0]!='1')return false;
    if(file!=INVALID_HANDLE_VALUE)return true;
    LARGE_INTEGER counter{};QueryPerformanceCounter(&counter);
    processId=GetCurrentProcessId();qpcFrequency=flight::TickFrequency();
    for(unsigned i=0;i<16;++i){
        wchar_t suffix[128]{};
        int n=swprintf(suffix,128,L"flight-tail-%lu-%llu-%u.bin",(unsigned long)processId,(unsigned long long)counter.QuadPart,i);
        if(n<=0||(size_t)n>=128)return false;
        std::wstring path=coopDir;if(!path.empty()&&path.back()!=L'\\'&&path.back()!=L'/')path+=L'\\';path+=suffix;
        HANDLE h=CreateFileW(path.c_str(),FILE_APPEND_DATA,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(h!=INVALID_HANDLE_VALUE){file=h;return true;}
        if(GetLastError()!=ERROR_FILE_EXISTS && GetLastError()!=ERROR_ALREADY_EXISTS)return false;
    }
    return false;
}

// Best effort and non-reentrant; it never dereferences exception context and
// never allocates, takes a mutex, formats text or scans native objects. A normal
// synchronous WriteFile can still block/fail in a damaged or stalled process.
inline bool Persist(bool firstChance) noexcept {
    if(file==INVALID_HANDLE_VALUE || InterlockedCompareExchange(&broken,0,0)!=0)return false;
    if(InterlockedCompareExchange(&busy,1,0)!=0)return false;
    volatile LONG* lane=firstChance?&firstAttempts:&unhandledAttempts;
    if(InterlockedCompareExchange(lane,0,0)>=(firstChance?6:2)){InterlockedExchange(&busy,0);return false;}
    InterlockedIncrement(lane);
    auto result=flight::Snapshot(snapshot,64);
    size_t n=std::min<size_t>(result.count,64);
    auto& h=frame.header;
    h.magic[0]='R';h.magic[1]='S';h.magic[2]='F';h.magic[3]='T';h.schema=1;
    h.headerBytes=(uint16_t)sizeof(HeaderV1);h.eventBytes=(uint16_t)sizeof(EventV1);h.reserved=0;
    h.frameBytes=(uint32_t)(sizeof(HeaderV1)+n*sizeof(EventV1));h.count=(uint32_t)n;
    h.busy=result.busy?1u:0u;h.pid=processId;h.protocol=kProtocolVersion;h.kind=firstChance?1u:2u;h.qpcFrequency=qpcFrequency;
    const auto& c=result.counters;
    h.counters={c.accepted,c.busyDrops,c.lifecycleDrops,c.historyOverwrites,c.drainDrops,c.ioErrors,
        c.segmentsEvicted,c.eventsEvicted,c.rotationFailures,c.exportFailed};
    constexpr char version[]=COOP_VERSION_STR;
    for(size_t i=0;i<sizeof h.version;++i)h.version[i]=i<sizeof(version)?version[i]:0;
    for(size_t i=0;i<n;++i){
        const auto& e=snapshot[i];const auto& x=e.context;auto& d=frame.events[i];
        d={x.fingerprint,x.generation,e.ticks,e.localSeq,x.wireTime,x.incarnation,x.world,
           e.motionEpoch,e.nativeHandle,e.pid,e.tid,x.entity,e.stage,e.reason,
           x.msg,x.channel,x.source,x.recipient,e.localPeer,
           e.health,e.maxHealth,e.armour,e.beforeHealth,e.afterHealth};
    }
    DWORD written=0;BOOL ok=WriteFile(file,&frame,h.frameBytes,&written,nullptr);
    if(!ok||written!=h.frameBytes)InterlockedExchange(&broken,1);
    InterlockedExchange(&busy,0);
    return ok && written==h.frameBytes;
}
}

namespace coop {
inline bool InitFlightTail(const std::wstring& coopDir){return crash_tail::Init(coopDir);}
inline bool PersistFlightTail(bool firstChance) noexcept{return crash_tail::Persist(firstChance);}
}
