// reSA-Coop Diagnostic System
// Copyright (c) 2026 opiahex
// Original source: https://github.com/opiahex/reSA-Coop
// SPDX-License-Identifier: LicenseRef-reSA-Coop-Diagnostics-Attribution-1.0
// See DIAGNOSTICS-LICENSE.txt, DIAGNOSTICS-NOTICE.md and DIAGNOSTICS-COMPONENTS.md.

#include "widgets/overlay/DiagnosticPanel.h"
#include "widgets/overlay/DiagnosticModel.h"
#include "widgets/overlay/DiagnosticTaskBridge.h"
#include <windows.h>
#include <atomic>
#include <cstring>
#include "common/lib/util/Util.h"
#include "shared/config/Config.h"
#include "shared/ui/renderer/Renderer.h"
#include "shared/ui/widgets/Widgets.h"
#include "common/lib/log/Log.h"
#include "shared/lib/core/Core.h"

namespace coop::ui {
namespace {
using namespace diagnostic;
std::atomic<DWORD> g_thread{0};
Snapshot g_snapshot;
Tab g_tab=Tab::Network;
std::wstring g_peer,g_entity,g_mission,g_fingerprint,g_search,g_hook;
size_t g_page=0;
bool g_interactiveDiagnosticFrame=false;
std::string g_copyStatus;
flight::SnapshotExportResult g_exportRequest=flight::SnapshotExportResult::None;
const char* g_controlRequest="none";
DiagnosticTimings g_timings;
const char* PhaseLabel(log::DiagnosticPhase phase){
    using P=log::DiagnosticPhase;
    switch(phase){case P::Idle:return "idle";case P::Starting:return "starting";case P::Recording:return "recording";
    case P::Stopping:return "stopping";case P::Stopped:return "stopped";case P::Failed:return "failed";case P::TimedOut:return "timed out";}
    return "unknown";
}
void SubmitControl(log::DiagnosticCommand command){
    switch(log::SubmitDiagnostics(command)){
    case log::DiagnosticSubmit::Queued:g_controlRequest="queued";break;
    case log::DiagnosticSubmit::Busy:g_controlRequest="busy - retry";break;
    case log::DiagnosticSubmit::Unavailable:g_controlRequest="worker unavailable";break;
    case log::DiagnosticSubmit::ShuttingDown:g_controlRequest="shutting down";break;
    }
}
const char* ExportLabel(flight::SnapshotExportResult value){
    using R=flight::SnapshotExportResult;
    switch(value){case R::Queued:return "queued";case R::Writing:return "writing";case R::Saved:return "saved";
    case R::Busy:return "busy - retry";case R::Disabled:return "recorder off";case R::Limit:return "request limit reached";
    case R::IoError:return "IO error - incomplete file possible";case R::Stopped:return "stopped - incomplete file possible";default:return "no request";}
}
LARGE_INTEGER Qpc(){LARGE_INTEGER t{};QueryPerformanceCounter(&t);return t;}
double Micros(LARGE_INTEGER start){auto f=flight::TickFrequency();return f?double(Qpc().QuadPart-start.QuadPart)*1000000.0/double(f):0;}
struct DrawTiming {
    LARGE_INTEGER start=Qpc();
    ~DrawTiming(){double us=Micros(start);if(flight::TickFrequency()&&start.QuadPart>0&&us>=0){++g_timings.draws;g_timings.drawTotalUs+=us;g_timings.drawMaxUs=std::max(g_timings.drawMaxUs,us);}}
};
bool CopyText(const std::string&utf8){
    auto text=Widen(utf8);const size_t bytes=(text.size()+1)*sizeof(wchar_t);
    HGLOBAL mem=GlobalAlloc(GMEM_MOVEABLE,bytes);if(!mem)return false;
    void*dst=GlobalLock(mem);if(!dst){GlobalFree(mem);return false;}
    std::memcpy(dst,text.c_str(),bytes);GlobalUnlock(mem);
    if(!OpenClipboard(nullptr)){GlobalFree(mem);return false;}
    bool ok=EmptyClipboard()&&SetClipboardData(CF_UNICODETEXT,mem)!=nullptr;
    CloseClipboard();if(!ok)GlobalFree(mem);return ok;
}
}

void UpdateDiagnosticSnapshot(uint64_t nowMs){
    if(GetCurrentThreadId()!=core::DiagnosticGameThreadId())return;
    const bool open=Cfg().debugOverlay&&core::ConnectMenuVisible()&&g_interactiveDiagnosticFrame;
    g_interactiveDiagnosticFrame=false;
    auto peer=Number::Parse(g_peer),entity=Number::Parse(g_entity);
    task_bridge::Update({open,peer.valid&&entity.valid,peer.active,entity.active,peer.value,entity.value},GetCurrentThreadId(),nowMs);
    if(!Cfg().debugOverlay)return;
    DWORD self=GetCurrentThreadId(),unbound=0;
    g_thread.compare_exchange_strong(unbound,self,std::memory_order_release,std::memory_order_relaxed);
    if(g_thread.load(std::memory_order_acquire)!=self)return;
    if(g_snapshot.sequence&&nowMs>=g_snapshot.sampledAtMs&&nowMs-g_snapshot.sampledAtMs<250)return;
    auto start=Qpc();g_snapshot.Capture(nowMs);auto selected=Number::Parse(g_entity);
    CaptureNativeSnapshot(g_snapshot.native,self,selected.valid&&selected.active&&selected.value<=65535?uint16_t(selected.value):0);
    g_snapshot.CaptureInspection(g_tab==Tab::HooksScm?Narrow(g_hook):std::string{});
    CacheDiagnosticInspection(g_snapshot.inspection,g_snapshot.inspectionRequested);g_snapshot.snapshotUs=Micros(start);
    if(flight::TickFrequency()&&start.QuadPart>0&&g_snapshot.snapshotUs>=0){++g_timings.samples;g_timings.snapshotTotalUs+=g_snapshot.snapshotUs;g_timings.snapshotMaxUs=std::max(g_timings.snapshotMaxUs,g_snapshot.snapshotUs);}
}

DiagnosticTimings GetDiagnosticTimings(){
    if(g_thread.load(std::memory_order_acquire)!=GetCurrentThreadId())return {};
    auto result=g_timings;result.available=flight::TickFrequency()!=0;return result;
}

void RenderDiagnosticPanel(bool interactive){
    // Reject before touching any renderer, widgets, input or owned UI state.
    // A diagnostic warning itself would issue D3D work on the rejected thread.
    if(g_thread.load(std::memory_order_acquire)!=GetCurrentThreadId())return;
    if(!Cfg().debugOverlay)return;
    if(interactive)g_interactiveDiagnosticFrame=true;
    auto&g=Gfx();float s=g.Scale();
    // This panel never calls SetMouseCapture, SetKeyboardCapture, DrawCursor,
    // or dereferences a native entity. Widget frame ownership stays with caller.
    const float x=24*s,y=90*s,W=std::min(1400*s,g.Width()-48*s),H=std::min(850*s,g.Height()-114*s);
    if(W<=0||H<360*s)return;
    DrawTiming drawTiming;
    g.Rect(x,y,W,H,0xEE000000);g.RectOutline(x,y,W,H,w::kSaLine,s);
    float cx=x+20*s,cy=y+14*s,cw=W-40*s;
    g.Text(FONT_MENU_SMALL,cx,cy,"DIAGNOSTICS",w::kSaBlue);
    g.Text(FONT_SMALL,x+W-20*s,cy,interactive?"Input owned by open menu":"Read only - open diagnostic controls in menu",w::kSaMuted,TEXT_RIGHT);
    cy+=30*s;
    const char*tabs[]={"Network","Entities","Events","Hooks / SCM","Performance"};
    for(int n=0;n<5;++n){float tx=cx+cw*n/5,tw=cw/5;
        if(interactive){if(w::Tab(tabs[n],tx,cy,tw,30*s,tabs[n],int(g_tab)==n,true)){g_tab=Tab(n);g_page=0;}}
        else g.Text(FONT_MENU_SMALL,tx+tw*.5f,cy+5*s,tabs[n],int(g_tab)==n?w::kSaBlue:w::kSaSlate,TEXT_CENTER);
    }
    cy+=36*s;
    auto field=[&](const char*id,const char*label,std::wstring&value,int n,int total,size_t max){
        float fx=cx+cw*n/total,fw=cw/total-10*s;
        g.Text(FONT_SMALL,fx,cy,label,w::kSaMuted);
        if(interactive){auto previous=value;w::TextBox(id,fx,cy+18*s,fw,28*s,value,max,false,false,label);if(value!=previous)g_page=0;}
        else g.TextW(FONT_SMALL,fx,cy+20*s,value.empty()?L"*":value,w::kSaMuted);
    };
    field("diag_peer","peer",g_peer,0,4,20);field("diag_entity","entity",g_entity,1,4,20);
    field("diag_mission","mission run (where known)",g_mission,2,4,20);field("diag_fp","fingerprint (not unique ID)",g_fingerprint,3,4,20);
    cy+=52*s;
    field("diag_search","search",g_search,0,2,96);field("diag_hook","hook ID contains",g_hook,1,2,79);cy+=56*s;
    Filter filter{Number::Parse(g_peer),Number::Parse(g_entity),Number::Parse(g_mission),Number::Parse(g_fingerprint),Narrow(g_search),Narrow(g_hook)};
    auto started=Qpc();auto rows=g_snapshot.Select(g_tab,filter);g_snapshot.formatUs=Micros(started);
    const float rowHeight=g.LineHeight(FONT_SMALL)+3*s;
    const float footerY=y+H-174*s;
    if(g_tab==Tab::Network){
        auto&c=g_snapshot.flightCounters;
        auto control=log::GetDiagnosticsStatus();
        rows.Add(control.busy?"Recording controls: status unavailable / retry":Format("Recording controls: %s | %u/%u completed | error %u | tail last command %s | last submit %s",PhaseLabel(control.phase),control.completed,control.requested,control.error,control.tailConfigured?(control.tailReady?"armed":"disarmed"):"not configured",g_controlRequest));
        if(!control.busy)rows.Add(Format("Text log drops %u | critical %u | reentrant %u | IO errors %u | callback errors %u | worker errors %u",control.logDropped,control.criticalDropped,control.reentrantDropped,control.ioErrors,control.callbackErrors,control.workerErrors));
        if(!control.busy)rows.Add(Format("Sync JSONL drops %u | stale %u | begin failures %u | IO errors %u | rotations %u",control.legacyDropped,control.legacyStale,control.legacyBeginFailures,control.legacyIoErrors,control.legacyRotations));
        rows.Add(Format("Recorder %s | sampled events %u / cap %u | snapshot %s",g_snapshot.recorderEnabled?"ON":"OFF",unsigned(g_snapshot.eventCount),unsigned(kEvents),g_snapshot.eventsBusy?"BUSY / no sample":"copied"));
        rows.Add(Format("accepted %u busyDrops %u lifecycleDrops %u historyOverwrites %u drainDrops %u IO errors %u",c.accepted,c.busyDrops,c.lifecycleDrops,c.historyOverwrites,c.drainDrops,c.ioErrors));
        rows.Add(Format("segmentsEvicted %u eventsEvicted %u rotationFailures %u exportFailed %u",c.segmentsEvicted,c.eventsEvicted,c.rotationFailures,c.exportFailed));
        auto sampling=flight::CaptureStats();
        rows.Add(sampling.busy?"Capture policy BUSY / unavailable":Format("Capture attempted %u | filtered %u sampled out %u critical drops %u | state 1/%u SCM 1/%u mask %X",sampling.attempted,sampling.filtered,sampling.sampledOut,sampling.criticalDrops,sampling.policy.stateEvery,sampling.policy.scmEvery,sampling.policy.categories));
        if(!sampling.busy)rows.Add(Format("SCM pre-gate spans %u | sampled %u filtered %u rate limited %u busy %u | budget %u/s (0 unlimited)",sampling.scmPreAttempted,sampling.scmPreSampled,sampling.scmPreFiltered,sampling.scmPreRateLimited,sampling.scmPreBusy,sampling.policy.scmSpansPerSecond));
        rows.Add(Format("Critical reserve buffered %u | pending %u | full losses %u",sampling.criticalBuffered,sampling.criticalPending,sampling.criticalReserveFull));
        rows.Add(Format("Native observations %u | sampled %u filtered %u busy %u budget rejects %u | incomplete metadata %u",sampling.nativePreAttempted,sampling.nativePreSampled,sampling.nativePreFiltered,sampling.nativePreBusy,sampling.nativePreBudget,sampling.incompleteMetadata));
        if(!sampling.busy)rows.Add(Format("Native query budget %u/s (0 unlimited)",sampling.policy.nativeObservationsPerSecond));
        rows.Add("Queue/reject counters are cumulative; packet loss percentage is not measured");
        rows.Add("Peer, entity and fingerprint filters apply to Events / Entities; hook ID applies to Hooks");
    }else if(g_tab==Tab::Performance){
        rows.Add(Format("Snapshot %.2f us | formatting %.2f us | refresh 250 ms | sample %llu @ %llu ms",g_snapshot.snapshotUs,g_snapshot.formatUs,(unsigned long long)g_snapshot.sequence,(unsigned long long)g_snapshot.sampledAtMs));
        rows.Add(Format("CPU draw calls %llu mean %.2f us peak %.2f us | snapshot calls %llu mean %.2f us peak %.2f us",(unsigned long long)g_timings.draws,g_timings.draws?g_timings.drawTotalUs/g_timings.draws:0,g_timings.drawMaxUs,(unsigned long long)g_timings.samples,g_timings.samples?g_timings.snapshotTotalUs/g_timings.samples:0,g_timings.snapshotMaxUs));
        rows.Add("CPU call timings include formatting; GPU time and comparative FPS are not measured by this panel");
        rows.Add(Format("Fixed caps: %u recorder events, %u hook records, %u formatted rows",unsigned(kEvents),unsigned(kHooks),unsigned(kRows)));
    }
    if(g_tab==Tab::Entities){
        rows.Add("Exact entity search: current owner/generation/pending. Event mission and ownerEpoch only where explicitly recorded");
        if(g_snapshot.native.truncated)rows.Add("Native value rows TRUNCATED (fixed row/text cap)");
    }
    if(g_tab==Tab::HooksScm){
        if(filter.Valid()){Rows detail;g_snapshot.Detail(detail,filter.hookId);for(size_t n=0;n<rows.count;++n)detail.Add(std::move(rows.text[n]));rows=std::move(detail);}
        rows.Add(Format("Registry snapshot %s | records %u | truncated %s | SCM spans in Events (covered paths only)",g_snapshot.hooksBusy?"BUSY / no sample":"copied",unsigned(g_snapshot.hookCount),g_snapshot.hooksTruncated?"YES":"no"));
        rows.Add(HookCollectorSummary(g_snapshot.hookCounters));
        rows.Add("INSTALL bytes are historical. FRESH exact-ID sample refreshes at 250 ms; lifetime/ABI are not verified");
    }
    const size_t pageRows=std::max(size_t(1),size_t(std::max(0.0f,footerY-cy)/rowHeight));
    const size_t pages=std::max(size_t(1),(rows.count+pageRows-1)/pageRows);g_page=std::min(g_page,pages-1);
    auto clipped=[&](const std::string&text){
        std::wstring t=Widen(text);if(g.TextWidthW(FONT_SMALL,t)<=cw)return t;
        size_t lo=0,hi=t.size();while(lo<hi){size_t mid=lo+(hi-lo+1)/2;
            if(g.TextWidthW(FONT_SMALL,std::wstring_view(t).substr(0,mid))+g.TextWidthW(FONT_SMALL,L"...")<=cw)lo=mid;else hi=mid-1;}
        t.resize(lo);return t+L"...";
    };
    for(size_t i=g_page*pageRows;i<rows.count&&i<(g_page+1)*pageRows;++i){g.TextW(FONT_SMALL,cx,cy,clipped(rows.text[i]),w::kSaMuted);cy+=rowHeight;}
    if(!rows.count)g.Text(FONT_SMALL,cx,cy,filter.Valid()?"No matching sampled rows":"Invalid filter",w::kSaMuted);
    cy=footerY;
    if(interactive){
        if(w::Button("diag_prev",cx,cy,100*s,28*s,"Prev",g_page>0))--g_page;
        if(w::Button("diag_next",cx+110*s,cy,100*s,28*s,"Next",g_page+1<pages))++g_page;
        if(w::Button("diag_copy",cx+240*s,cy,210*s,28*s,"Copy visible page"))g_copyStatus=CopyText(rows.Copy(g_page*pageRows,pageRows))?"Copied":"Clipboard unavailable";
        if(w::Button("diag_start",cx+465*s,cy,140*s,28*s,"Start recording"))SubmitControl(log::DiagnosticCommand::Start);
        if(w::Button("diag_stop",cx+615*s,cy,140*s,28*s,"Stop recording"))SubmitControl(log::DiagnosticCommand::Stop);
    }
    g.Text(FONT_SMALL,x+W-20*s,cy,Format("%u rows | page %u/%u %s",unsigned(rows.count),unsigned(g_page+1),unsigned(pages),g_copyStatus.c_str()),w::kSaMuted,TEXT_RIGHT);
    auto exportState=flight::GetSnapshotExportStatus();
    if(interactive){
        if(w::Button("diag_save_recent",cx,cy+30*s,175*s,22*s,"Save recent"))g_exportRequest=flight::RequestSnapshotExport();
    }else g.Text(FONT_SMALL,cx,cy+34*s,"Recent snapshot",w::kSaMuted);
    g.Text(FONT_SMALL,cx+190*s,cy+34*s,exportState.busy?"Snapshot status busy - await refresh":Format("%s | %u/4 attempts | request: %s | recent 4096 only",ExportLabel(exportState.result),exportState.requested,ExportLabel(g_exportRequest)),w::kSaMuted);
    g.Text(FONT_SMALL,cx,cy+52*s,"Recorder snapshots contain numeric metadata. Files stay local; no automatic upload.",w::kSaMuted);
    g.Text(FONT_SMALL,cx,cy+74*s,"About / Credits",w::kSaBlue);
    g.Text(FONT_SMALL,cx,cy+94*s,"reSA-Coop Diagnostic System",w::kSaMuted);
    g.Text(FONT_SMALL,cx,cy+114*s,"Developed and maintained as part of reSA-Coop by opiahex",w::kSaMuted);
    g.Text(FONT_SMALL,cx,cy+134*s,"Source: https://github.com/opiahex/reSA-Coop",w::kSaMuted);
}
}
