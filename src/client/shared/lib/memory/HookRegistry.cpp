// reSA-Coop Diagnostic System
// Copyright (c) 2026 opiahex
// Original source: https://github.com/opiahex/reSA-Coop
// SPDX-License-Identifier: LicenseRef-reSA-Coop-Diagnostics-Attribution-1.0
// See DIAGNOSTICS-LICENSE.txt, DIAGNOSTICS-NOTICE.md and DIAGNOSTICS-COMPONENTS.md.

#include "shared/lib/memory/HookRegistry.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>
#include <type_traits>

namespace coop::hookreg {
namespace {
struct Registry {
    std::atomic_flag lock=ATOMIC_FLAG_INIT;
    std::array<Record,kSlots> records{};
    size_t count=0;
    uint64_t nextAttempt=0;
    std::atomic<uint32_t> busy{0},capacity{0},invalid{0},conflict{0},stale{0},alreadyFinalized{0};
};
static_assert(std::atomic<uint32_t>::is_always_lock_free,"hook counters require lock-free atomics");
static_assert(std::is_trivially_destructible_v<Registry>,"collector must have no shutdown destructor");
Registry g;
bool Lock()noexcept{return !g.lock.test_and_set(std::memory_order_acquire);}
void Unlock()noexcept{g.lock.clear(std::memory_order_release);}
bool Label(const char*text,size_t max,bool required)noexcept{
    bool first=true;
    for(size_t i=0;i<max;++i){unsigned char c=(unsigned char)text[i];
        if(!c)return !first||!required;
        if(c<32||c>126||c=='/'||c=='\\'||c==':')return false;
        first=false;
    }
    return false;
}
bool Purpose(const char*text,size_t max)noexcept{
    // UTF-8 display text is accepted as opaque bytes, never dereferenced later.
    for(size_t i=0;i<max;++i)if(!text[i])return true;
    return false;
}
bool ValidBytes(const Bytes&b)noexcept{
    if((unsigned)b.read>(unsigned)ReadStatus::InvalidRange||b.captured>kBytes||b.captured>b.total)return false;
    if(b.truncated!=(b.total>kBytes))return false;
    if(!b.total)return b.captured==0&&b.read==ReadStatus::NotRequested;
    if(b.read!=ReadStatus::Complete)return false; // manifest must be a trusted complete local copy
    return b.captured==std::min<uint32_t>(b.total,kBytes);
}
bool ValidSpec(const Spec&s)noexcept{
    if(!Label(s.id,sizeof s.id,true)||!Label(s.module,sizeof s.module,true)||!Label(s.owner,sizeof s.owner,false)||!Purpose(s.purpose,sizeof s.purpose))return false;
    if(!s.site||!s.span||s.span-1>UINTPTR_MAX-s.site||(unsigned)s.kind>(unsigned)Kind::Other||(unsigned)s.chain>(unsigned)Chain::Indeterminate)return false;
    // No positive chain certification until a separate full fingerprint and
    // ABI-aware predecessor verification have actually been implemented.
    if(s.chain==Chain::NativeExpected||s.chain==Chain::ExplicitVerified)return false;
    return ValidBytes(s.expected)&&ValidBytes(s.desired);
}
Bytes ReadBytes(uintptr_t address,uint32_t size)noexcept{
    Bytes out{};out.total=size;out.truncated=size>kBytes;
    if(!size)return out;
    if(!address||size-1>UINTPTR_MAX-address){out.read=ReadStatus::InvalidRange;return out;}
    const size_t want=std::min<size_t>(size,kBytes);MEMORY_BASIC_INFORMATION m{};
    if(!VirtualQuery((const void*)address,&m,sizeof m)||m.State!=MEM_COMMIT||(m.Protect&PAGE_GUARD)||(m.Protect&0xff)==PAGE_NOACCESS){out.read=ReadStatus::Inaccessible;return out;}
    SIZE_T got=0;BOOL ok=ReadProcessMemory(GetCurrentProcess(),(const void*)address,out.data.data(),want,&got);
    out.captured=(uint8_t)std::min<SIZE_T>(got,want);
    out.read=ok&&got==want?ReadStatus::Complete:got?ReadStatus::Partial:ReadStatus::Inaccessible;
    return out;
}
bool ReadExact(uintptr_t address,void*out,size_t size)noexcept{
    if(!address||!size||size-1>UINTPTR_MAX-address)return false;
    SIZE_T got=0;return ReadProcessMemory(GetCurrentProcess(),(const void*)address,out,size,&got)&&got==size;
}
PeIdentity PeAt(uintptr_t address)noexcept{
    PeIdentity p{};MEMORY_BASIC_INFORMATION m{};
    if(!VirtualQuery((const void*)address,&m,sizeof m))return p;
    p.base=(uintptr_t)m.AllocationBase;
    if(m.Type!=MEM_IMAGE||!p.base)return p;
    IMAGE_DOS_HEADER dos{};if(!ReadExact(p.base,&dos,sizeof dos)||dos.e_magic!=IMAGE_DOS_SIGNATURE||dos.e_lfanew<0||dos.e_lfanew>1024*1024)return p;
    uintptr_t offset=(uintptr_t)dos.e_lfanew;if(offset>UINTPTR_MAX-p.base)return p;
    IMAGE_NT_HEADERS32 nt{};if(!ReadExact(p.base+offset,&nt,sizeof nt)||nt.Signature!=IMAGE_NT_SIGNATURE||nt.OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR32_MAGIC)return p;
    if(!nt.OptionalHeader.SizeOfImage||address<p.base||address-p.base>=nt.OptionalHeader.SizeOfImage)return p;
    p.imageSize=nt.OptionalHeader.SizeOfImage;p.timestamp=nt.FileHeader.TimeDateStamp;p.checksum=nt.OptionalHeader.CheckSum;p.machine=nt.FileHeader.Machine;p.valid=true;return p;
}
bool SameKey(const Record&a,const Record&b)noexcept{
    return !std::strcmp(a.spec.module,b.spec.module)&&a.spec.site==b.spec.site&&a.spec.kind==b.spec.kind;
}
}
bool ManifestBytes(Bytes&out,const uint8_t*data,size_t size)noexcept{
    out={};if((!data&&size)||size>std::numeric_limits<uint32_t>::max())return false;
    if(!size)return true;out.total=(uint32_t)size;out.captured=(uint8_t)std::min(size,kBytes);out.truncated=size>kBytes;out.read=ReadStatus::Complete;
    std::memcpy(out.data.data(),data,out.captured);return true;
}
Comparison Compare(const Bytes&a,const Bytes&b)noexcept{
    if(!a.total||!b.total||a.read!=ReadStatus::Complete||b.read!=ReadStatus::Complete)return Comparison::Unknown;
    if(a.captured>kBytes||b.captured>kBytes||a.captured!=std::min<uint32_t>(a.total,kBytes)||b.captured!=std::min<uint32_t>(b.total,kBytes)||
       a.truncated!=(a.total>kBytes)||b.truncated!=(b.total>kBytes))return Comparison::Unknown;
    if(a.total!=b.total)return Comparison::Mismatch;
    if(a.captured!=b.captured)return Comparison::Unknown;
    if(std::memcmp(a.data.data(),b.data.data(),a.captured))return Comparison::Mismatch;
    return a.truncated||b.truncated?Comparison::Unknown:Comparison::Match;
}
Counters Stats()noexcept{return {g.busy.load(),g.capacity.load(),g.invalid.load(),g.conflict.load(),g.stale.load(),g.alreadyFinalized.load()};}
Put CaptureBefore(const Spec&spec,Ticket&ticket)noexcept{
    ticket={};if(!ValidSpec(spec)){g.invalid.fetch_add(1,std::memory_order_relaxed);return Put::Invalid;}
    Record r{};r.spec=spec;r.pe=PeAt(spec.site);
    if(r.pe.valid&&spec.site>=r.pe.base&&spec.site-r.pe.base<=UINT32_MAX){r.rva=(uint32_t)(spec.site-r.pe.base);r.rvaKnown=true;}
    r.original=ReadBytes(spec.site,spec.span);r.pre=Compare(spec.expected,r.original);
    if(!Lock()){g.busy.fetch_add(1,std::memory_order_relaxed);return Put::Busy;}
    size_t index=0;for(;index<g.count;++index)if(!std::strcmp(g.records[index].spec.id,spec.id))break;
    if(index<g.count&&!SameKey(g.records[index],r)){g.conflict.fetch_add(1,std::memory_order_relaxed);Unlock();return Put::IdConflict;}
    if(index==g.count&&g.count==kSlots){g.capacity.fetch_add(1,std::memory_order_relaxed);Unlock();return Put::Capacity;}
    if(index==g.count)++g.count;
    if(++g.nextAttempt==0)++g.nextAttempt;r.attempt=g.nextAttempt;g.records[index]=r;ticket.record=r;Unlock();return Put::Stored;
}
Put CaptureAfter(Ticket&ticket,Outcome outcome,uint32_t error,uintptr_t previous,uintptr_t trampoline)noexcept{
    if(!ticket.record.attempt||!ValidSpec(ticket.record.spec)||(unsigned)outcome>(unsigned)Outcome::Indeterminate||outcome==Outcome::NotAttempted){g.invalid.fetch_add(1);return Put::Invalid;}
    if(!Lock()){g.busy.fetch_add(1,std::memory_order_relaxed);return Put::Busy;}
    size_t index=0;for(;index<g.count;++index)if(!std::strcmp(g.records[index].spec.id,ticket.record.spec.id))break;
    if(index==g.count||g.records[index].attempt!=ticket.record.attempt||!SameKey(g.records[index],ticket.record)){
        g.stale.fetch_add(1,std::memory_order_relaxed);Unlock();return Put::StaleAttempt;
    }
    if(g.records[index].outcome!=Outcome::NotAttempted){g.alreadyFinalized.fetch_add(1,std::memory_order_relaxed);Unlock();return Put::AlreadyFinalized;}
    Record r=g.records[index];Unlock();
    r.error=error;r.previous=previous;r.trampoline=trampoline;r.outcome=outcome;
    r.readback=ReadBytes(r.spec.site,r.spec.span);r.post=Compare(r.spec.desired,r.readback);
    // Mismatch after an API success contradicts the supplied intended patch.
    if(outcome==Outcome::ApiSuccess&&r.post==Comparison::Mismatch)r.outcome=Outcome::Indeterminate;
    if(outcome==Outcome::FailedUnchanged&&Compare(r.original,r.readback)==Comparison::Mismatch)r.outcome=Outcome::Indeterminate;
    if(!Lock()){g.busy.fetch_add(1,std::memory_order_relaxed);return Put::Busy;}
    size_t i=0;for(;i<g.count;++i)if(!std::strcmp(g.records[i].spec.id,r.spec.id))break;
    if(i==g.count||g.records[i].attempt!=r.attempt||!SameKey(g.records[i],r)){
        g.stale.fetch_add(1,std::memory_order_relaxed);Unlock();return Put::StaleAttempt;
    }
    if(g.records[i].outcome!=Outcome::NotAttempted){g.alreadyFinalized.fetch_add(1,std::memory_order_relaxed);Unlock();return Put::AlreadyFinalized;}
    g.records[i]=r;ticket.record=r;Unlock();return Put::Stored;
}
Put Lookup(const char*id,Record&out)noexcept{
    // id is an internal trusted NUL-terminated stable identifier.
    if(!id||strnlen(id,kId)==kId)return Put::Invalid;if(!Lock()){g.busy.fetch_add(1);return Put::Busy;}
    for(size_t i=0;i<g.count;++i)if(!std::strcmp(g.records[i].spec.id,id)){out=g.records[i];Unlock();return Put::Stored;}
    Unlock();return Put::NotFound;
}
SnapshotResult Snapshot(Record*out,size_t capacity)noexcept{
    SnapshotResult result{};result.counters=Stats();
    if(!Lock()){result.busy=true;return result;}
    result.count=out?std::min(capacity,g.count):0;result.truncated=result.count<g.count;
    for(size_t i=0;i<result.count;++i)out[i]=g.records[i];Unlock();return result;
}
}
