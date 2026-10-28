// reSA-Coop Diagnostic System
// Copyright (c) 2026 opiahex
// Original source: https://github.com/opiahex/reSA-Coop
// SPDX-License-Identifier: LicenseRef-reSA-Coop-Diagnostics-Attribution-1.0
// See DIAGNOSTICS-LICENSE.txt, DIAGNOSTICS-NOTICE.md and DIAGNOSTICS-COMPONENTS.md.

#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

#include "common/lib/diagnostics/FlightRecorder.h"
#include "shared/lib/memory/HookRegistry.h"
#include "shared/lib/memory/HookInspector.h"
#include "widgets/overlay/DiagnosticNative.h"

namespace coop::ui::diagnostic {
constexpr size_t kEvents = 128, kHooks = hookreg::kSlots, kRows = hookreg::kSlots+4;
enum class Tab : uint8_t { Network, Entities, Events, HooksScm, Performance };
struct Number {
    bool active=false, valid=true;
    uint64_t value=0;
    static Number Parse(std::wstring_view input) {
        Number n;
        if(input.empty())return n;
        n.active=true;
        unsigned base=10;size_t i=0;
        if(input.size()>2&&input[0]==L'0'&&(input[1]==L'x'||input[1]==L'X')){base=16;i=2;}
        for(;i<input.size();++i){
            wchar_t c=input[i];unsigned d=c>=L'0'&&c<=L'9'?unsigned(c-L'0'):c>=L'a'&&c<=L'f'?unsigned(c-L'a'+10):c>=L'A'&&c<=L'F'?unsigned(c-L'A'+10):99;
            if(d>=base||n.value>(UINT64_MAX-d)/base){n.valid=false;break;}
            n.value=n.value*base+d;
        }
        return n;
    }
    bool Match(uint64_t v,bool present=true)const{return !active||(valid&&present&&value==v);}
};
inline char Lower(char c){return c>='A'&&c<='Z'?char(c-'A'+'a'):c;}
inline bool Contains(std::string_view text,std::string_view term){
    if(term.size()>text.size())return false;
    for(size_t i=0;i+term.size()<=text.size();++i){size_t j=0;while(j<term.size()&&Lower(text[i+j])==Lower(term[j]))++j;if(j==term.size())return true;}
    return false;
}
template<size_t N> std::string_view Bounded(const char(&text)[N]){size_t n=0;while(n<N&&text[n])++n;return {text,n};}
template<size_t N> std::string_view Bounded(const std::array<char,N>&text){size_t n=0;while(n<N&&text[n])++n;return {text.data(),n};}
struct Filter {
    Number peer{},entity{},mission{},fingerprint{};
    std::string search,hookId;
    bool Valid()const{return peer.valid&&entity.valid&&mission.valid&&fingerprint.valid;}
    bool Match(const flight::Event&e)const {
        bool peerMatch=!peer.active||(peer.valid&&((e.localPeer!=255&&peer.value==e.localPeer)||(e.context.source!=255&&peer.value==e.context.source)||(e.context.recipient!=255&&peer.value==e.context.recipient)));
        // Recorder v1 has no mission field. Never turn an unavailable value into mission 0.
        return Valid()&&peerMatch&&entity.Match(e.context.entity)&&mission.Match(0,false)&&fingerprint.Match(e.context.fingerprint)&&hookId.empty();
    }
    bool Match(const hookreg::Record&r)const {
        return Valid()&&!peer.active&&!entity.active&&!mission.active&&!fingerprint.active&&Contains(Bounded(r.spec.id),hookId);
    }
    bool Match(const NativeRow&r)const {
        return Valid()&&peer.Match(r.peer,r.peerKnown)&&entity.Match(r.entity,r.entityKnown)&&!mission.active&&!fingerprint.active&&hookId.empty()&&Contains(Bounded(r.text),search);
    }
};
inline const char* Comparison(hookreg::Comparison c){switch(c){case hookreg::Comparison::Match:return "MATCH";case hookreg::Comparison::Mismatch:return "MISMATCH";default:return "UNKNOWN";}}
inline const char* Outcome(hookreg::Outcome o){switch(o){case hookreg::Outcome::ApiSuccess:return "API success";case hookreg::Outcome::FailedUnchanged:return "failed unchanged";case hookreg::Outcome::Conflict:return "conflict";case hookreg::Outcome::Indeterminate:return "indeterminate";default:return "UNKNOWN / result not observed";}}
inline const char* Chain(hookreg::Chain c){switch(c){case hookreg::Chain::NativeExpected:return "native expected";case hookreg::Chain::ExplicitVerified:return "explicit verified";case hookreg::Chain::ExternalUnverified:return "external UNVERIFIED";case hookreg::Chain::Conflict:return "conflict";case hookreg::Chain::Indeterminate:return "indeterminate";default:return "not checked";}}
inline const char* Abi(hookreg::Check c){switch(c){case hookreg::Check::NotChecked:return "not checked";case hookreg::Check::Passed:return "reported passed";case hookreg::Check::Failed:return "reported failed";case hookreg::Check::NotApplicable:return "not applicable";default:return "UNKNOWN";}}
inline std::string HookCollectorSummary(const hookreg::Counters&c){
    char text[320];std::snprintf(text,sizeof(text),"%s | Registry counters busy %u capacity %u invalid %u conflict %u stale %u alreadyFinalized %u",c.busy||c.capacity?"Observations INCOMPLETE":"Observation completeness UNKNOWN",c.busy,c.capacity,c.invalid,c.conflict,c.stale,c.alreadyFinalized);return text;
}
inline std::string EventText(const flight::Event&e){
    char text[384];
    std::snprintf(text,sizeof(text),"seq %llu stage %u reason %u peer %u src %u dst %u entity %u gen %llu world %u msg %u ch %u fp %016llX hp %.2f/%.2f arm %.2f",
        (unsigned long long)e.localSeq,e.stage,e.reason,e.localPeer,e.context.source,e.context.recipient,e.context.entity,(unsigned long long)e.context.generation,e.context.world,e.context.msg,e.context.channel,(unsigned long long)e.context.fingerprint,e.health,e.maxHealth,e.armour);
    return text;
}
inline std::string HookText(const hookreg::Record&r){
    char text[256],rva[32];
    if(r.rvaKnown)std::snprintf(rva,sizeof(rva),"+%08X",r.rva);else std::snprintf(rva,sizeof(rva),"RVA UNKNOWN");
    std::snprintf(text,sizeof(text)," %s INSTALL pre %s post %s %s chain %s error %u SHA256 %s ABI %s",rva,Comparison(r.pre),Comparison(r.post),Outcome(r.outcome),Chain(r.spec.chain),r.error,r.fullSha256Verified?"reported verified":"UNVERIFIED",Abi(r.abi));
    return std::string(Bounded(r.spec.id))+" "+std::string(Bounded(r.spec.module))+text;
}
struct Rows {
    std::array<std::string,kRows> text{};size_t count=0;
    void Add(std::string s){if(count<text.size())text[count++]=std::move(s);}
    std::string Copy(size_t first,size_t limit)const {
        std::string out;for(size_t i=first;i<count&&i-first<limit;++i){out+=text[i];out+='\n';}return out;
    }
};
inline const char* ReadStatus(hookreg::ReadStatus s){switch(s){case hookreg::ReadStatus::NotRequested:return "not requested";case hookreg::ReadStatus::Complete:return "complete";case hookreg::ReadStatus::Partial:return "partial";case hookreg::ReadStatus::Inaccessible:return "inaccessible";case hookreg::ReadStatus::InvalidRange:return "invalid range";default:return "UNKNOWN";}}
inline const char* InspectStatus(hookinspect::Status s){switch(s){case hookinspect::Status::Busy:return "BUSY / UNKNOWN";case hookinspect::Status::NotFound:return "NOT FOUND";case hookinspect::Status::Invalid:return "INVALID";case hookinspect::Status::Pending:return "PENDING / UNKNOWN result not observed";case hookinspect::Status::Unreadable:return "UNREADABLE";case hookinspect::Status::Partial:return "PARTIAL";case hookinspect::Status::ChangedDuringRead:return "CHANGED DURING READ / UNKNOWN";case hookinspect::Status::Match:return "MATCH";case hookinspect::Status::Mismatch:return "MISMATCH";default:return "UNKNOWN";}}
inline std::string BytesText(const char*label,const hookreg::Bytes&bytes){
    char metadata[160];std::snprintf(metadata,sizeof(metadata),"%s len %u captured %u read %s truncated %s [",label,bytes.total,bytes.captured,ReadStatus(bytes.read),bytes.truncated?"yes":"no");
    std::string out=metadata;for(size_t n=0;n<std::min<size_t>(bytes.captured,hookreg::kBytes);++n){char hex[4];std::snprintf(hex,sizeof(hex),"%02X",bytes.data[n]);if(n)out+=' ';out+=hex;}return out+"]";
}
// Fixed-size owned metadata. No native entity pointers, names, packet payloads or file paths.
struct Snapshot {
    std::array<flight::Event,kEvents> events{};
    std::array<hookreg::Record,kHooks> hooks{};
    size_t eventCount=0,hookCount=0;
    bool eventsBusy=false,hooksBusy=false,hooksTruncated=false,recorderEnabled=false;
    flight::Counters flightCounters{};hookreg::Counters hookCounters{};
    NativeSnapshot native{};
    hookinspect::Sample inspection{};
    std::array<char,hookreg::kId> inspectionId{};
    uint64_t inspectionAttempt=0;
    bool inspectionRequested=false;
    uint64_t sampledAtMs=0,sequence=0;
    double snapshotUs=0,formatUs=0;
    void Capture(uint64_t now){
        auto e=flight::Snapshot(events.data(),events.size());eventCount=e.count;eventsBusy=e.busy;flightCounters=e.counters;
        CaptureHooks();
        recorderEnabled=flight::Enabled();sampledAtMs=now;++sequence;
    }
    // Collector4b adapter copies owned install-time metadata only. No memory
    // inspection or status inference occurs here; AlreadyFinalized is a counter.
    void CaptureHooks(){
        auto h=hookreg::Snapshot(hooks.data(),hooks.size());hookCount=h.count;hooksBusy=h.busy;hooksTruncated=h.truncated;hookCounters=h.counters;
    }
    const hookreg::Record* ExactHook(std::string_view id)const {
        if(id.empty()||id.size()>=hookreg::kId)return nullptr;
        for(size_t n=0;n<hookCount;++n)if(Bounded(hooks[n].spec.id)==id)return &hooks[n];return nullptr;
    }
    // Invoke only from the existing owner-thread 250 ms update gate. The ID
    // passed to Inspector is the trusted registry copy, never free-form input.
    void CaptureInspection(std::string_view requested){
        inspection={};inspectionId={};inspectionAttempt=0;inspectionRequested=false;
        auto*record=ExactHook(requested);if(!record)return;
        std::copy(std::begin(record->spec.id),std::end(record->spec.id),inspectionId.begin());
        inspectionAttempt=record->attempt;inspectionRequested=true;
        inspection=hookinspect::Inspect(inspectionId.data(),0);
        hookCounters=hookreg::Stats(); // include an Inspector lookup BUSY drop
    }
    void Detail(Rows&rows,std::string_view requested)const {
        auto*record=ExactHook(requested);
        if(!record){rows.Add("Fresh inspection: NO SAMPLE (enter an exact registry hook ID)");return;}
        char meta[256];std::snprintf(meta,sizeof(meta),"INSTALL attempt %llu previous 0x%llX trampoline 0x%llX (historical metadata)",(unsigned long long)record->attempt,(unsigned long long)record->previous,(unsigned long long)record->trampoline);rows.Add(meta);
        rows.Add(BytesText("INSTALL original",record->original));rows.Add(BytesText("INSTALL expected",record->spec.expected));
        rows.Add(BytesText("INSTALL desired",record->spec.desired));rows.Add(BytesText("INSTALL readback",record->readback));
        if(!inspectionRequested||Bounded(inspectionId)!=requested||inspectionAttempt!=record->attempt){rows.Add("Fresh inspection: NO SAMPLE for this exact ID/attempt (await owning-thread refresh)");return;}
        bool bound=inspection.install.attempt==record->attempt&&Bounded(inspection.install.spec.id)==requested;
        bool positive=inspection.status==hookinspect::Status::Match||inspection.status==hookinspect::Status::Mismatch;
        const char*status=!bound&&positive?"UNKNOWN / installation attempt changed":InspectStatus(inspection.status);
        const char*comparison=bound?Comparison(inspection.desiredComparison):"UNKNOWN";
        std::snprintf(meta,sizeof(meta),"FRESH status %s desired %s | expected attempt %llu observed %llu bound %s",status,comparison,(unsigned long long)record->attempt,(unsigned long long)inspection.install.attempt,bound?"yes":"no");rows.Add(meta);
        if(inspection.ticksValid)std::snprintf(meta,sizeof(meta),"FRESH QPC %llu | lifetime UNKNOWN (epoch 0); point-in-time bytes, no stable-memory/ABI proof",(unsigned long long)inspection.ticks);
        else std::snprintf(meta,sizeof(meta),"FRESH QPC UNKNOWN | lifetime UNKNOWN (epoch 0); result may be unobserved");rows.Add(meta);
        rows.Add(BytesText("FRESH current",inspection.current));
    }
    Rows Select(Tab tab,const Filter&filter)const {
        Rows rows;
        if(!filter.Valid()){rows.Add("Invalid numeric filter (decimal or 0x hexadecimal required)");return rows;}
        if(tab==Tab::Entities||tab==Tab::Network){for(size_t n=0;n<native.count&&n<native.rows.size();++n){auto&r=native.rows[n];if((tab==Tab::Network)==(r.kind==NativeKind::Network)&&filter.Match(r))rows.Add(std::string(Bounded(r.text)));}}
        if(tab==Tab::Events||tab==Tab::Entities){
            // Newest first; entity view means observed recorder identities, not a live pool inspector.
            for(size_t n=eventCount;n>0;--n){auto&e=events[n-1];if(!filter.Match(e))continue;
                if(tab==Tab::Entities){bool seen=false;for(size_t j=n;j<eventCount;++j)if(filter.Match(events[j])&&events[j].context.entity==e.context.entity&&events[j].context.generation==e.context.generation&&events[j].context.world==e.context.world&&events[j].context.msg==e.context.msg&&events[j].localPeer==e.localPeer){seen=true;break;}if(seen)continue;}
                auto text=EventText(e);if(Contains(text,filter.search))rows.Add(std::move(text));
            }
        }else if(tab==Tab::HooksScm){auto*exact=ExactHook(filter.hookId);for(size_t n=0;n<hookCount;++n){auto&r=hooks[n];if((exact&&exact!=&r)||!filter.Match(r))continue;auto text=HookText(r);if(Contains(text,filter.search))rows.Add(std::move(text));}}
        return rows;
    }
};
}
