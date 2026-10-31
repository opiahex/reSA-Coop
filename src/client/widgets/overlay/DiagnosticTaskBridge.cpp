// reSA-Coop Diagnostic System
// Copyright (c) 2026 opiahex
// Original source: https://github.com/opiahex/reSA-Coop
// SPDX-License-Identifier: LicenseRef-reSA-Coop-Diagnostics-Attribution-1.0
#include "widgets/overlay/DiagnosticTaskBridge.h"
#include "widgets/overlay/DiagnosticNative.h"
#include "shared/lib/core/Core.h"
#include <windows.h>
#include <cstdio>
#include <cstring>

namespace coop::ui::diagnostic::task_bridge {
namespace {
namespace tn=task_lifecycle::native;
namespace tt=task_lifecycle::tree;
constexpr uint64_t kCaptureMs=1000,kLeaseMs=5000,kRenewMs=1500,kFreshMs=1500;
bool SameRequest(const Request&a,const Request&b)noexcept{
    return a.open==b.open&&a.valid==b.valid&&a.peerSet==b.peerSet&&a.entitySet==b.entitySet&&
        (!a.peerSet||a.peer==b.peer)&&(!a.entitySet||a.entity==b.entity);
}
bool SameBody(const tn::Selection&a,const tn::Selection&b)noexcept{
    return a.kind==b.kind&&a.peer==b.peer&&a.entity==b.entity&&a.nativePedRef==b.nativePedRef&&
        a.incarnation==b.incarnation&&a.world==b.world&&a.generation==b.generation;
}
State AdmissionState(flight::NativeAdmission a)noexcept{
    switch(a){case flight::NativeAdmission::Disabled:return State::AdmissionDisabled;
    case flight::NativeAdmission::Filtered:return State::AdmissionFiltered;
    case flight::NativeAdmission::Sampled:return State::AdmissionSampled;
    case flight::NativeAdmission::Busy:return State::AdmissionBusy;
    case flight::NativeAdmission::Budget:return State::AdmissionBudget;
    default:return State::NotReady;}
}
const char* Label(State s)noexcept{
    switch(s){case State::None:return "none (enter peer or NPC entity)";case State::Closed:return "closed";
    case State::Invalid:return "invalid target filter";case State::WrongThread:return "wrong thread";
    case State::Disabled:return "recorder off";case State::NotReady:return "native observers not ready";
    case State::OwnershipConflict:return "selection owned elsewhere; edit filter to retry";
    case State::TargetUnavailable:return "target unavailable";case State::ReferenceUnavailable:return "native reference unavailable";
    case State::RecordingChanged:return "recording changed; edit filter to reselect";
    case State::SubjectChanged:return "subject changed; edit filter to reselect";
    case State::AdmissionDisabled:return "observation disabled";case State::AdmissionFiltered:return "observation filtered";
    case State::AdmissionSampled:return "observation sampled";case State::AdmissionBusy:return "observation busy";
    case State::AdmissionBudget:return "observation budget";case State::SelectionRejected:return "selection rejected";
    case State::CaptureUnselected:return "selection unavailable";case State::CaptureNotReady:return "capture not ready";
    case State::LayoutUnavailable:return "native layout unavailable";case State::CaptureChanged:return "subject changed during capture";
    case State::Copied:return "copied";case State::TracePartial:return "copied; trace admission incomplete";
    case State::Stale:return "copied snapshot stale";case State::InvalidSnapshot:return "copied snapshot invalid";}
    return "unknown";
}
const char* LinkLabel(tt::State s)noexcept{
    switch(s){case tt::State::Unavailable:return "UNKNOWN";case tt::State::Absent:return "absent";
    case tt::State::Node:return "node";case tt::State::External:return "external";case tt::State::Truncated:return "LIMIT";
    case tt::State::Invalid:return "invalid";case tt::State::Changed:return "changed";case tt::State::Present:return "present";}
    return "UNKNOWN";
}
struct Text {
    char value[320]{};size_t used=0;bool truncated=false;
    void Add(const char*text)noexcept{Add("%s",text);}
    template<class...A>void Add(const char*f,A...a)noexcept{
        if(used>=sizeof(value)-1){truncated=true;return;}
        int n=std::snprintf(value+used,sizeof(value)-used,f,a...);
        if(n<0){truncated=true;return;}if(size_t(n)>=sizeof(value)-used){used=sizeof(value)-1;truncated=true;return;}used+=size_t(n);
    }
};
void Chain(Text&text,const tt::Snapshot&s,tt::Link link)noexcept{
    bool seen[tt::kNodes]{};
    for(size_t depth=0;depth<=tt::kDepth;++depth){
        if(link.state!=tt::State::Node){text.Add("%s",LinkLabel(link.state));return;}
        if(link.node>=s.count||link.node>=s.nodes.size()){text.Add("INVALID_INDEX");return;}
        if(seen[link.node]){text.Add("CYCLE#%u",unsigned(link.node));return;}seen[link.node]=true;
        auto&n=s.nodes[link.node];text.Add("#%u:",unsigned(link.node));
        if(n.type.available)text.Add("%d",int(n.type.value));else text.Add("UNKNOWN");
        text.Add("[p=");if(n.parent.state==tt::State::Node)text.Add("#%u",unsigned(n.parent.node));else text.Add("%s",LinkLabel(n.parent.state));
        text.Add("]");if(!n.validatedAtRead)text.Add("?");
        if(n.subtask.state==tt::State::Absent)return;
        text.Add(" > ");link=n.subtask;
    }
    text.Add("DEPTH_LIMIT");
}
void Event(Text&text,const tt::EventObservation&e)noexcept{
    text.Add("%s type=",LinkLabel(e.state));if(e.type.available)text.Add("%d",int(e.type.value));else text.Add("UNKNOWN");
    text.Add(" priority=");if(e.priority.available)text.Add("%d",int(e.priority.value));else text.Add("UNKNOWN");
    text.Add(" script=");if(e.scriptCommand.available)text.Add("%u",e.scriptCommand.value?1u:0u);else text.Add("UNKNOWN");
}
bool OwnerThread(uint32_t owner)noexcept{return owner&&owner==GetCurrentThreadId()&&owner==core::DiagnosticGameThreadId();}
flight::NativeAdmission Admit(const Prepared&p)noexcept{return flight::AdmitNativeObservation(p.context,850,p.localPeer,false);}
tn::TreeCaptureStatus Capture(size_t index,tt::Snapshot&s)noexcept{return tn::CaptureSelectedTree(index,s);}
task_lifecycle::tree_trace::Result Record(const tt::Snapshot&s,uint32_t token)noexcept{return task_lifecycle::tree_trace::RecordSnapshot(s,token,{});}
Controller controller;
const Services production{OwnerThread,flight::ActiveRecordingToken,tn::SnapshotStatus,PrepareTarget,Admit,ResolveReference,tn::SetSelections,tn::ClearSelections,Capture,Record};
}
void Controller::Release(const Services&s)noexcept{
    if(owned_&&s.status&&s.clear&&s.status().selectionRevision==revision_)s.clear();
    owned_=false;copied_=false;selection_={};snapshot_={};trace_={};revision_=token_=localWorld_=0;
}
void Controller::Update(const Request&r,uint32_t owner,uint64_t now,const Services&s)noexcept{
    // No Net, selection, native or recorder service may precede this guard.
    if(!s.thread||!s.thread(owner)){state_=State::WrongThread;copied_=false;return;}
    owningThread_=owner;
    if(!s.token||!s.status||!s.prepare||!s.admit||!s.reference||!s.select||!s.clear||!s.capture||!s.record){state_=State::NotReady;return;}
    if(!hasRequest_||!SameRequest(r,request_)){Release(s);request_=r;hasRequest_=true;blocked_=false;attempted_=false;}
    if(!r.open){Release(s);state_=State::Closed;return;}
    if(!r.valid||(r.peerSet&&r.peer>2)||(r.entitySet&&(!r.entity||r.entity>65535))){Release(s);state_=State::Invalid;return;}
    if(!r.peerSet&&!r.entitySet){Release(s);state_=State::None;return;}
    auto token=s.token();
    if(owned_&&token!=token_){Release(s);blocked_=true;state_=State::RecordingChanged;return;}
    if(!token){state_=State::Disabled;copied_=false;return;}
    auto status=s.status();
    if(owned_&&status.selectionRevision!=revision_){owned_=false;copied_=false;blocked_=true;state_=State::OwnershipConflict;return;}
    if(blocked_)return;
    if(owned_&&now>selection_.expiresMs){Release(s);state_=State::SubjectChanged;blocked_=true;return;}
    if(!owned_&&status.selected){state_=State::OwnershipConflict;blocked_=true;return;}
    if(status.install!=tn::InstallState::Ready){state_=State::NotReady;copied_=false;return;}
    if(attempted_&&now>=attemptedAt_&&now-attemptedAt_<kCaptureMs)return;
    attempted_=true;attemptedAt_=now;
    Prepared p{};
    if(!s.prepare(r,p)){state_=State::TargetUnavailable;copied_=false;return;}
    auto admission=s.admit(p);if(admission!=flight::NativeAdmission::Admitted){state_=AdmissionState(admission);copied_=false;return;}
    if(s.token()!=token){Release(s);state_=State::RecordingChanged;blocked_=true;return;}
    if(!s.reference(p)){state_=State::ReferenceUnavailable;copied_=false;return;}
    if(owned_&&(!SameBody(p.selection,selection_)||p.localWorld!=localWorld_||now>selection_.expiresMs)){
        Release(s);state_=State::SubjectChanged;blocked_=true;return;
    }
    // The metadata setter performs additional guarded reads; it is called only
    // after admission and only for first selection or a nearly-expired lease.
    if(!owned_||selection_.expiresMs-now<=kRenewMs){
        if(s.token()!=token){Release(s);state_=State::RecordingChanged;blocked_=true;return;}
        status=s.status();if((owned_&&status.selectionRevision!=revision_)||(!owned_&&status.selected)){
            owned_=false;copied_=false;blocked_=true;state_=State::OwnershipConflict;return;
        }
        if(now>UINT64_MAX-kLeaseMs){state_=State::Invalid;copied_=false;return;}
        p.selection.expiresMs=now+kLeaseMs;
        if(!s.select(&p.selection,1)){state_=State::SelectionRejected;copied_=false;return;}
        status=s.status();selection_=p.selection;revision_=status.selectionRevision;token_=token;localWorld_=p.localWorld;owned_=true;
        if(status.selected!=1||s.token()!=token){Release(s);state_=State::RecordingChanged;blocked_=true;return;}
    }
    tt::Snapshot value{};auto result=s.capture(0,value);
    if(s.token()!=token||s.status().selectionRevision!=revision_){Release(s);state_=State::RecordingChanged;blocked_=true;return;}
    copied_=false;
    if(result!=tn::TreeCaptureStatus::Copied){
        switch(result){case tn::TreeCaptureStatus::WrongThread:state_=State::WrongThread;break;
        case tn::TreeCaptureStatus::Unselected:state_=State::CaptureUnselected;break;
        case tn::TreeCaptureStatus::NotReady:state_=State::CaptureNotReady;break;
        case tn::TreeCaptureStatus::LayoutUnavailable:state_=State::LayoutUnavailable;break;
        default:state_=State::CaptureChanged;break;}return;
    }
    if(value.count>value.nodes.size()||!value.subject.kind.available||value.subject.kind.value!=selection_.kind||
       !value.subject.nativePedRef.available||value.subject.nativePedRef.value!=selection_.nativePedRef||
       value.subject.context.source!=selection_.peer||value.subject.context.incarnation!=selection_.incarnation||
       value.subject.context.world!=selection_.world||value.subject.context.entity!=selection_.entity||
       value.subject.context.generation!=selection_.generation||value.subject.localPeer!=p.localPeer||
       value.subject.context.msg||value.subject.context.channel||value.subject.context.fingerprint||value.subject.context.wireTime){state_=State::InvalidSnapshot;return;}
    trace_=s.record(value,token);
    if(s.token()!=token||s.status().selectionRevision!=revision_){Release(s);state_=State::RecordingChanged;blocked_=true;return;}
    snapshot_=value;copiedAt_=now;copied_=true;state_=trace_.AllRowsAdmitted()?State::Copied:State::TracePartial;
}
void Controller::Append(NativeSnapshot&out,uint64_t now,const Services&s)const noexcept{
    if(!owningThread_||!s.thread||!s.thread(owningThread_))return;
    const uint8_t peer=request_.peerSet&&request_.peer<=2?uint8_t(request_.peer):selection_.peer;
    const bool entity=request_.entitySet&&request_.entity>0&&request_.entity<=65535;
    auto add=[&](const char*text){
        auto before=out.count;out.Add(NativeKind::Remote,peer,entity,entity?uint16_t(request_.entity):0,"%s",text);
        // These local task observations carry no validated mission provenance.
        if(out.count>before){out.rows[before].mission=0;out.rows[before].missionKnown=false;}
    };
    const bool tokenValid=!copied_||(s.token&&s.token()==token_);
    const bool selectionValid=!copied_||(s.status&&s.status().selectionRevision==revision_);
    bool fresh=copied_&&tokenValid&&selectionValid&&now>=copiedAt_&&now-copiedAt_<=kFreshMs;
    const State visible=!tokenValid?State::RecordingChanged:!selectionValid?State::OwnershipConflict:copied_&&!fresh?State::Stale:state_;
    Text summary;summary.Add("Task tree: %s",Label(visible));
    if(copied_&&tokenValid&&selectionValid)summary.Add(" | age %llums | nodes %u | complete %s flags 0x%X | trace %u/%u",(unsigned long long)(now>=copiedAt_?now-copiedAt_:0),unsigned(snapshot_.count),snapshot_.complete?"yes":"no",unsigned(snapshot_.flags),unsigned(trace_.admitted),unsigned(trace_.attempted));
    if(fresh)summary.Add(" | read-time indices; WHY UNKNOWN");
    add(summary.value);if(!fresh)return;
    auto root=[&](const char*kind,size_t i,tt::Link link){Text text;text.Add("Task %s[%u]: ",kind,unsigned(i));Chain(text,snapshot_,link);add(text.value);if(text.truncated)out.truncated=true;};
    for(size_t n=0;n<snapshot_.primary.size();++n)root("primary",n,snapshot_.primary[n]);
    for(size_t n=0;n<snapshot_.secondary.size();++n)root("secondary",n,snapshot_.secondary[n]);
    const tt::EventObservation*events[]={&snapshot_.history.nonTemporary,&snapshot_.history.temporary,&snapshot_.history.stored};
    const char*names[]={"non-temporary","temporary","stored"};
    for(size_t n=0;n<3;++n){Text text;text.Add("History %s: ",names[n]);Event(text,*events[n]);add(text.value);}
    Text last;last.Add("History current: ");Event(last,snapshot_.history.current);last.Add(" | last-aborted %s type=",LinkLabel(snapshot_.history.lastAborted.state));
    if(snapshot_.history.lastAborted.type.available)last.Add("%d",int(snapshot_.history.lastAborted.type.value));else last.Add("UNKNOWN");add(last.value);
}
void Update(const Request&r,uint32_t owner,uint64_t now)noexcept{controller.Update(r,owner,now,production);}
void Append(NativeSnapshot&out,uint64_t now)noexcept{
    if(OwnerThread(core::DiagnosticGameThreadId()))controller.Append(out,now,production);
}
}
