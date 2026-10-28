// reSA-Coop Diagnostic System
// Copyright (c) 2026 opiahex
// Original source: https://github.com/opiahex/reSA-Coop
// SPDX-License-Identifier: LicenseRef-reSA-Coop-Diagnostics-Attribution-1.0
// See DIAGNOSTICS-LICENSE.txt, DIAGNOSTICS-NOTICE.md and DIAGNOSTICS-COMPONENTS.md.

#include "common/lib/diagnostics/FlightRecorder.h"
#include <windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <locale.h>
#include <type_traits>

namespace coop::flight {
namespace {
constexpr size_t kCapacity=4096,kBatch=128;
constexpr uint64_t kFileLimit=4u*1024u*1024u;
constexpr unsigned kFiles=4;
constexpr uint64_t kControlReserve=2048;
static_assert(kFileLimit>kControlReserve+512,"segment must leave room for counters");
static_assert(std::atomic<uint32_t>::is_always_lock_free,"recorder counters must not hide locks");
static_assert(std::atomic<bool>::is_always_lock_free,"recorder switch must not hide locks");
struct Recorder {
    struct Segment {unsigned number=0;DWORD volume=0,indexHigh=0,indexLow=0;uint32_t events=0;uint64_t first=0,last=0;HANDLE handle=INVALID_HANDLE_VALUE;};
    std::atomic_flag guard=ATOMIC_FLAG_INIT;
    std::array<Event,kCapacity> history{};
    uint64_t total=0,frequency=0;
    std::atomic<uint32_t> accepted{0},busyDrops{0},lifecycleDrops{0},historyOverwrites{0},drainDrops{0},ioErrors{0},epoch{0};
    std::atomic<uint32_t> segmentsEvicted{0},eventsEvicted{0},rotationFailures{0},exportFailed{0};
    std::atomic<bool> enabled{false},running{false};
    HANDLE writer=nullptr;
    wchar_t path[2048]{};
    _locale_t locale=nullptr;
    HANDLE file=INVALID_HANDLE_VALUE;
    uint64_t written=0;
    unsigned segment=0;
    std::array<Segment,kFiles> segments{};
    unsigned segmentCount=0;
};
static_assert(std::is_trivially_destructible_v<Recorder>,"recorder must outlive a timed-out writer during CRT exit");
Recorder g_recorder;
Recorder& Get()noexcept{return g_recorder;}
bool TryLock(Recorder&r)noexcept{return !r.guard.test_and_set(std::memory_order_acquire);}
void Unlock(Recorder&r)noexcept{r.guard.clear(std::memory_order_release);}
void Add(std::atomic<uint32_t>& counter,uint64_t n)noexcept{
    counter.fetch_add((uint32_t)std::min<uint64_t>(n,0xffffffffu),std::memory_order_relaxed);
}
bool WriteBytes(Recorder&r,const char*data,DWORD size){
    DWORD done=0;
    if(r.file==INVALID_HANDLE_VALUE||!WriteFile(r.file,data,size,&done,nullptr)||done!=size){
        if(r.file!=INVALID_HANDLE_VALUE){CloseHandle(r.file);if(r.segmentCount)r.segments[r.segmentCount-1].handle=INVALID_HANDLE_VALUE;r.file=INVALID_HANDLE_VALUE;}
        r.ioErrors.fetch_add(1,std::memory_order_relaxed);r.exportFailed.store(1,std::memory_order_relaxed);return false;
    }
    r.written+=size;return true;
}
bool SegmentName(const Recorder&r,unsigned index,wchar_t(&name)[2064]){
    int chars=index?swprintf(name,2064,L"%ls.%u",r.path,index):swprintf(name,2064,L"%ls",r.path);
    return chars>=0&&(size_t)chars<2064;
}
uint64_t RetainedFrom(const Recorder&r){
    for(unsigned i=0;i<r.segmentCount;++i)if(r.segments[i].events)return r.segments[i].first;
    return 0;
}
uint64_t RetainedThrough(const Recorder&r){
    for(unsigned i=r.segmentCount;i>0;--i)if(r.segments[i-1].events)return r.segments[i-1].last;
    return 0;
}
bool EvictOldest(Recorder&r){
    if(!r.segmentCount)return false;
    const auto old=r.segments[0];wchar_t name[2064];if(!SegmentName(r,old.number,name))return false;
    HANDLE h=CreateFileW(name,DELETE|FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                         nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if(h==INVALID_HANDLE_VALUE)return false;
    BY_HANDLE_FILE_INFORMATION info{};
    bool same=GetFileInformationByHandle(h,&info)&&info.dwVolumeSerialNumber==old.volume&&
              info.nFileIndexHigh==old.indexHigh&&info.nFileIndexLow==old.indexLow;
    FILE_DISPOSITION_INFO disposition{};disposition.DeleteFile=TRUE;
    bool removed=same&&SetFileInformationByHandle(h,FileDispositionInfo,&disposition,sizeof disposition);
    CloseHandle(h);
    if(!removed)return false;
    if(old.handle!=INVALID_HANDLE_VALUE)CloseHandle(old.handle);
    for(unsigned i=1;i<r.segmentCount;++i)r.segments[i-1]=r.segments[i];
    --r.segmentCount;r.segmentsEvicted.fetch_add(1,std::memory_order_relaxed);Add(r.eventsEvicted,old.events);
    return true;
}
bool OpenSegment(Recorder&r,unsigned index,bool initial){
    HANDLE next=INVALID_HANDLE_VALUE;BY_HANDLE_FILE_INFORMATION info{};unsigned chosen=index;
    for(unsigned tries=0;tries<64;++tries){
        wchar_t name[2064];if(!SegmentName(r,chosen,name))break;
        next=CreateFileW(name,GENERIC_WRITE|FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_DELETE,
                         nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(next!=INVALID_HANDLE_VALUE)break;
        DWORD error=GetLastError();if(initial||(error!=ERROR_FILE_EXISTS&&error!=ERROR_ALREADY_EXISTS)||chosen==UINT32_MAX)break;
        ++chosen;
    }
    if(next==INVALID_HANDLE_VALUE){r.ioErrors.fetch_add(1,std::memory_order_relaxed);return false;}
    if(!GetFileInformationByHandle(next,&info)){CloseHandle(next);r.ioErrors.fetch_add(1,std::memory_order_relaxed);return false;}
    r.file=next;r.written=0;r.segment=chosen;
    auto&segment=r.segments[r.segmentCount++];segment={chosen,info.dwVolumeSerialNumber,info.nFileIndexHigh,info.nFileIndexLow,0,0,0,next};
    char b[256];int n=snprintf(b,sizeof b,"{\"event\":\"flight_meta\",\"version\":1,\"qpc_frequency\":%llu,\"segment\":%u,\"segments_evicted\":%u,\"events_evicted\":%u,\"retained_from_seq\":%llu,\"retained_through_seq\":%llu}\n",
        (unsigned long long)r.frequency,chosen,r.segmentsEvicted.load(),r.eventsEvicted.load(),(unsigned long long)RetainedFrom(r),(unsigned long long)RetainedThrough(r));
    return n>0&&WriteBytes(r,b,(DWORD)n);
}
void RotationFailed(Recorder&r){
    r.rotationFailures.fetch_add(1,std::memory_order_relaxed);r.exportFailed.store(1,std::memory_order_relaxed);
    // Reserve space before rotation so readers may see why export stopped.
    // If the device itself refuses this write, missing status means UNKNOWN.
    if(r.file!=INVALID_HANDLE_VALUE){char b[256];int n=snprintf(b,sizeof b,"{\"event\":\"flight_export_failed\",\"rotation_failures\":%u,\"segments_evicted\":%u,\"events_evicted\":%u,\"retained_from_seq\":%llu,\"retained_through_seq\":%llu}\n",
        r.rotationFailures.load(),r.segmentsEvicted.load(),r.eventsEvicted.load(),(unsigned long long)RetainedFrom(r),(unsigned long long)RetainedThrough(r));
        if(n>0&&(size_t)n<sizeof b&&r.written+(unsigned)n<=kFileLimit)WriteBytes(r,b,(DWORD)n);
        if(r.file!=INVALID_HANDLE_VALUE){CloseHandle(r.file);if(r.segmentCount)r.segments[r.segmentCount-1].handle=INVALID_HANDLE_VALUE;r.file=INVALID_HANDLE_VALUE;}}
}
bool Rotate(Recorder&r){
    if(r.segment==UINT32_MAX||(r.segmentCount==kFiles&&!EvictOldest(r))){RotationFailed(r);return false;}
    if(!OpenSegment(r,r.segment+1,false)){RotationFailed(r);return false;}
    return true;
}
bool Output(Recorder&r,const Event&e){
    if(r.file==INVALID_HANDLE_VALUE)return false;
    // Fixed stack storage on the drain thread, large enough even for decimal
    // UINT64_MAX in every field and scientific notation for every finite float.
    char b[2048],f[5][32];
    const float values[]={e.health,e.maxHealth,e.armour,e.beforeHealth,e.afterHealth};
    for(size_t i=0;i<5;++i)if(std::isfinite(values[i]))_snprintf_l(f[i],sizeof f[i],"%.9g",r.locale,values[i]);else snprintf(f[i],sizeof f[i],"null");
    const auto&c=e.context;
    int n=snprintf(b,sizeof b,"{\"event\":\"flight\",\"ticks\":%llu,\"local_seq\":%llu,\"pid\":%u,\"tid\":%u,\"stage\":%u,\"reason\":%u,\"msg\":%u,\"channel\":%u,\"source\":%u,\"recipient\":%u,\"local_peer\":%u,\"wire_time\":%u,\"incarnation\":%u,\"world\":%u,\"id\":%u,\"generation\":%llu,\"fingerprint\":%llu,\"motion_epoch\":%u,\"native_handle\":%u,\"health\":%s,\"max_health\":%s,\"armour\":%s,\"before_health\":%s,\"after_health\":%s}\n",
        (unsigned long long)e.ticks,(unsigned long long)e.localSeq,e.pid,e.tid,e.stage,e.reason,c.msg,c.channel,c.source,c.recipient,e.localPeer,c.wireTime,c.incarnation,c.world,c.entity,(unsigned long long)c.generation,(unsigned long long)c.fingerprint,e.motionEpoch,e.nativeHandle,f[0],f[1],f[2],f[3],f[4]);
    if(n<=0||(size_t)n>=sizeof b){r.ioErrors.fetch_add(1,std::memory_order_relaxed);return false;}
    if(r.written+(unsigned)n>kFileLimit-kControlReserve&&!Rotate(r))return false;
    if(!WriteBytes(r,b,(DWORD)n))return false;
    auto&segment=r.segments[r.segmentCount-1];if(!segment.events)segment.first=e.localSeq;
    segment.last=e.localSeq;++segment.events;return true;
}
void Drain(Recorder&r,uint64_t&cursor){
    std::array<Event,kBatch> batch{};size_t count=0;
    if(!TryLock(r))return;
    uint64_t oldest=r.total>kCapacity?r.total-kCapacity:0;
    if(cursor<oldest){Add(r.drainDrops,oldest-cursor);cursor=oldest;}
    while(cursor<r.total&&count<kBatch)batch[count++]=r.history[(size_t)(cursor++%kCapacity)];
    Unlock(r);
    for(size_t i=0;i<count;++i)if(!Output(r,batch[i]))Add(r.drainDrops,1);
}
void OutputCounters(Recorder&r){
    if(r.file==INVALID_HANDLE_VALUE)return;
    // A rotation may evict the oldest segment and change both retained bounds
    // and eviction counters. Build the footer only after that transition.
    if(r.written+512>kFileLimit-kControlReserve&&!Rotate(r))return;
    char b[512];int n=snprintf(b,sizeof b,"{\"event\":\"flight_counters\",\"accepted\":%u,\"busy_drops\":%u,\"lifecycle_drops\":%u,\"history_overwrites\":%u,\"drain_drops\":%u,\"io_errors\":%u,\"segments_evicted\":%u,\"events_evicted\":%u,\"rotation_failures\":%u,\"export_failed\":%u,\"retained_from_seq\":%llu,\"retained_through_seq\":%llu}\n",
        r.accepted.load(),r.busyDrops.load(),r.lifecycleDrops.load(),r.historyOverwrites.load(),r.drainDrops.load(),r.ioErrors.load(),r.segmentsEvicted.load(),r.eventsEvicted.load(),r.rotationFailures.load(),r.exportFailed.load(),(unsigned long long)RetainedFrom(r),(unsigned long long)RetainedThrough(r));
    if(n<=0||(size_t)n>=sizeof b){r.ioErrors.fetch_add(1);return;}
    WriteBytes(r,b,(DWORD)n);
}
DWORD WINAPI Worker(void*param){
    auto*r=static_cast<Recorder*>(param);
    uint64_t cursor=0,lastCounters=0;
    while(r->running.load(std::memory_order_acquire)){
        Drain(*r,cursor);auto now=GetTickCount64();
        if(now-lastCounters>=1000){OutputCounters(*r);lastCounters=now;}
        Sleep(20);
    }
    // Avoid issuing new I/O during shutdown. Keep history for Snapshot and
    // expose any records not yet copied to the writer as drain drops.
    if(TryLock(*r)) {if(cursor<r->total)Add(r->drainDrops,r->total-cursor);Unlock(*r);}
    OutputCounters(*r); // best effort: Stop has a bounded wait and reports timeout
    for(unsigned i=0;i<r->segmentCount;++i)if(r->segments[i].handle!=INVALID_HANDLE_VALUE){CloseHandle(r->segments[i].handle);r->segments[i].handle=INVALID_HANDLE_VALUE;}
    r->file=INVALID_HANDLE_VALUE;
    if(r->locale){_free_locale(r->locale);r->locale=nullptr;}
    return 0;
}
}
bool Start(const std::wstring&path){
    auto&r=Get();if(path.empty()||path.size()>=std::size(r.path)||r.enabled.load(std::memory_order_acquire)||r.writer)return false;
    LARGE_INTEGER f{};if(!QueryPerformanceFrequency(&f)||f.QuadPart<=0)return false;
    _locale_t locale=_create_locale(LC_NUMERIC,"C");if(!locale){r.ioErrors.fetch_add(1);return false;}
    if(!TryLock(r)){_free_locale(locale);return false;}
    r.total=0;r.frequency=(uint64_t)f.QuadPart;std::wmemcpy(r.path,path.c_str(),path.size()+1);r.locale=locale;r.segment=0;r.segmentCount=0;
    r.accepted=0;r.busyDrops=0;r.lifecycleDrops=0;r.historyOverwrites=0;r.drainDrops=0;r.ioErrors=0;
    r.segmentsEvicted=0;r.eventsEvicted=0;r.rotationFailures=0;r.exportFailed=0;
    Unlock(r);
    if(!OpenSegment(r,0,true)){_free_locale(r.locale);r.locale=nullptr;return false;}
    HMODULE pinned=nullptr;
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,(LPCWSTR)&Worker,&pinned)){
        CloseHandle(r.file);r.file=INVALID_HANDLE_VALUE;_free_locale(r.locale);r.locale=nullptr;r.ioErrors.fetch_add(1);return false;
    }
    r.epoch.fetch_add(1,std::memory_order_release);
    r.running.store(true,std::memory_order_release);
    r.writer=CreateThread(nullptr,0,Worker,&r,0,nullptr);
    if(!r.writer){r.running=false;CloseHandle(r.file);r.file=INVALID_HANDLE_VALUE;_free_locale(r.locale);r.locale=nullptr;r.ioErrors.fetch_add(1);return false;}
    r.enabled.store(true,std::memory_order_release);return true;
}
bool Stop(uint32_t timeoutMs){
    auto&r=Get();r.enabled.store(false,std::memory_order_release);r.epoch.fetch_add(1,std::memory_order_acq_rel);
    if(!r.writer)return true;
    const auto began=GetTickCount64();
    // A producer that already passed its second epoch check owns this guard.
    // Do not let the writer close its footer until that producer has committed.
    while(!TryLock(r)){
        if(GetTickCount64()-began>=timeoutMs)return false;
        Sleep(1); // lifecycle only, never a game/network callback
    }
    Unlock(r);r.running.store(false,std::memory_order_release);
    CancelSynchronousIo(r.writer);
    auto elapsed=GetTickCount64()-began;DWORD remaining=elapsed>=timeoutMs?0:timeoutMs-(DWORD)elapsed;
    if(WaitForSingleObject(r.writer,remaining)!=WAIT_OBJECT_0)return false;
    CloseHandle(r.writer);r.writer=nullptr;return true;
}
bool Enabled()noexcept{return Get().enabled.load(std::memory_order_acquire);}
bool Record(Event event)noexcept{
    auto&r=Get();auto epoch=r.epoch.load(std::memory_order_acquire);if(!r.enabled.load(std::memory_order_acquire))return false;
    LARGE_INTEGER t{};if(!QueryPerformanceCounter(&t))return false;
    event.ticks=(uint64_t)t.QuadPart;event.pid=GetCurrentProcessId();event.tid=GetCurrentThreadId();
    if(!TryLock(r)){r.busyDrops.fetch_add(1,std::memory_order_relaxed);return false;}
    if(!r.enabled.load(std::memory_order_acquire)||r.epoch.load(std::memory_order_acquire)!=epoch){r.lifecycleDrops.fetch_add(1,std::memory_order_relaxed);Unlock(r);return false;}
    if(r.total>=kCapacity)r.historyOverwrites.fetch_add(1,std::memory_order_relaxed);
    event.localSeq=r.total+1;r.history[(size_t)(r.total++%kCapacity)]=event;r.accepted.fetch_add(1,std::memory_order_relaxed);Unlock(r);return true;
}
Counters GetCounters()noexcept{auto&r=Get();return {r.accepted.load(),r.busyDrops.load(),r.lifecycleDrops.load(),r.historyOverwrites.load(),r.drainDrops.load(),r.ioErrors.load(),r.segmentsEvicted.load(),r.eventsEvicted.load(),r.rotationFailures.load(),r.exportFailed.load()};}
SnapshotResult Snapshot(Event*out,size_t cap)noexcept{
    auto&r=Get();SnapshotResult result;result.counters=GetCounters();if(!out||!cap)return result;
    if(!TryLock(r)){result.busy=true;return result;}
    result.count=(size_t)std::min<uint64_t>(std::min<size_t>(cap,kCapacity),r.total);
    uint64_t start=r.total-result.count;
    for(size_t i=0;i<result.count;++i)out[i]=r.history[(size_t)((start+i)%kCapacity)];
    Unlock(r);return result;
}
uint64_t TickFrequency()noexcept{LARGE_INTEGER f{};return QueryPerformanceFrequency(&f)&&f.QuadPart>0?(uint64_t)f.QuadPart:0;}
}
