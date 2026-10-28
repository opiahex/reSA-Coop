// reSA-Coop Diagnostic System
// Copyright (c) 2026 opiahex
// Original source: https://github.com/opiahex/reSA-Coop
// SPDX-License-Identifier: LicenseRef-reSA-Coop-Diagnostics-Attribution-1.0
// See DIAGNOSTICS-LICENSE.txt, DIAGNOSTICS-NOTICE.md and DIAGNOSTICS-COMPONENTS.md.

#include "shared/lib/memory/HookInspector.h"
#include <windows.h>
#include <algorithm>
#include <cstring>

namespace coop::hookinspect {
namespace {
hookreg::Bytes Read(uintptr_t address,uint32_t size)noexcept {
    hookreg::Bytes out{};out.total=size;out.truncated=size>hookreg::kBytes;
    if(!size)return out;
    if(!address||size-1>UINTPTR_MAX-address){out.read=hookreg::ReadStatus::InvalidRange;return out;}
    const size_t want=std::min<size_t>(size,hookreg::kBytes);SIZE_T got=0;
    // Reject every guard/no-access region before attempting a read. These
    // checks cannot pin a mapping or eliminate concurrent protection changes.
    size_t checked=0;
    while(checked<want){
        MEMORY_BASIC_INFORMATION region{};const uintptr_t pos=address+checked;
        if(!VirtualQuery((const void*)pos,&region,sizeof region)||region.State!=MEM_COMMIT||
           (region.Protect&PAGE_GUARD)||(region.Protect&0xff)==PAGE_NOACCESS){
            out.read=checked?hookreg::ReadStatus::Partial:hookreg::ReadStatus::Inaccessible;return out;
        }
        const uintptr_t base=(uintptr_t)region.BaseAddress;
        if(pos<base||pos-base>=region.RegionSize){out.read=hookreg::ReadStatus::InvalidRange;return out;}
        const size_t available=region.RegionSize-(pos-base);
        checked+=std::min(want-checked,available);
    }
    const BOOL ok=ReadProcessMemory(GetCurrentProcess(),(const void*)address,out.data.data(),want,&got);
    out.captured=(uint8_t)std::min<SIZE_T>(got,want);
    out.read=ok&&got==want?hookreg::ReadStatus::Complete:got?hookreg::ReadStatus::Partial:hookreg::ReadStatus::Inaccessible;
    return out;
}
Status LookupFailure(hookreg::Put p)noexcept {
    return p==hookreg::Put::Busy?Status::Busy:p==hookreg::Put::NotFound?Status::NotFound:Status::Invalid;
}
}
Sample Inspect(const char*id,uint64_t epoch)noexcept {
    Sample result{};result.contextEpoch=epoch;
    const auto first=hookreg::Lookup(id,result.install);
    if(first!=hookreg::Put::Stored){result.status=LookupFailure(first);return result;}
    if(result.install.outcome==hookreg::Outcome::NotAttempted){result.status=Status::Pending;return result;}
    result.current=Read(result.install.spec.site,result.install.spec.span);
    hookreg::Bytes second{};
    if(result.current.read==hookreg::ReadStatus::Complete)second=Read(result.install.spec.site,result.install.spec.span);
    LARGE_INTEGER at{};result.ticksValid=QueryPerformanceCounter(&at)!=FALSE;if(result.ticksValid)result.ticks=(uint64_t)at.QuadPart;
    hookreg::Record now{};const auto last=hookreg::Lookup(id,now);
    if(last!=hookreg::Put::Stored){result.status=LookupFailure(last);return result;}
    if(now.attempt!=result.install.attempt){result.status=Status::ChangedDuringRead;return result;}
    if(result.current.read==hookreg::ReadStatus::Partial){result.status=Status::Partial;return result;}
    if(result.current.read!=hookreg::ReadStatus::Complete){result.status=Status::Unreadable;return result;}
    if(second.read!=hookreg::ReadStatus::Complete||second.captured!=result.current.captured||
       std::memcmp(result.current.data.data(),second.data.data(),result.current.captured)!=0){result.status=Status::ChangedDuringRead;return result;}
    result.desiredComparison=hookreg::Compare(result.install.spec.desired,result.current);
    result.status=result.desiredComparison==hookreg::Comparison::Match?Status::Match:
        result.desiredComparison==hookreg::Comparison::Mismatch?Status::Mismatch:Status::Unknown;
    return result;
}
}
