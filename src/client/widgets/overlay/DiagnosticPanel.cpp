// reSA-Coop Diagnostic System
// Copyright (c) 2026 opiahex
// Original source: https://github.com/opiahex/reSA-Coop
// SPDX-License-Identifier: LicenseRef-reSA-Coop-Diagnostics-Attribution-1.0
// See DIAGNOSTICS-LICENSE.txt, DIAGNOSTICS-NOTICE.md and DIAGNOSTICS-COMPONENTS.md.

#include "widgets/overlay/DiagnosticPanel.h"
#include "widgets/overlay/DiagnosticModel.h"
#include <windows.h>
#include <atomic>
#include <cstring>
#include "common/lib/util/Util.h"
#include "shared/config/Config.h"
#include "shared/ui/renderer/Renderer.h"
#include "shared/ui/widgets/Widgets.h"

namespace coop::ui {
namespace {
using namespace diagnostic;
std::atomic<DWORD> g_thread{0};
Snapshot g_snapshot;
Tab g_tab=Tab::Network;
std::wstring g_peer,g_entity,g_mission,g_fingerprint,g_search,g_hook;
size_t g_page=0;
std::string g_copyStatus;
LARGE_INTEGER Qpc(){LARGE_INTEGER t{};QueryPerformanceCounter(&t);return t;}
double Micros(LARGE_INTEGER start){auto f=flight::TickFrequency();return f?double(Qpc().QuadPart-start.QuadPart)*1000000.0/double(f):0;}
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
    if(!Cfg().debugOverlay)return;
    DWORD self=GetCurrentThreadId(),unbound=0;
    g_thread.compare_exchange_strong(unbound,self,std::memory_order_release,std::memory_order_relaxed);
    if(g_thread.load(std::memory_order_acquire)!=self)return;
    if(g_snapshot.sequence&&nowMs>=g_snapshot.sampledAtMs&&nowMs-g_snapshot.sampledAtMs<250)return;
    auto start=Qpc();g_snapshot.Capture(nowMs);CaptureNativeSnapshot(g_snapshot.native,self);
    g_snapshot.CaptureInspection(g_tab==Tab::HooksScm?Narrow(g_hook):std::string{});g_snapshot.snapshotUs=Micros(start);
}

void RenderDiagnosticPanel(bool interactive){
    // Reject before touching any renderer, widgets, input or owned UI state.
    // A diagnostic warning itself would issue D3D work on the rejected thread.
    if(g_thread.load(std::memory_order_acquire)!=GetCurrentThreadId())return;
    if(!Cfg().debugOverlay)return;
    auto&g=Gfx();float s=g.Scale();
    // This panel never calls SetMouseCapture, SetKeyboardCapture, DrawCursor,
    // or dereferences a native entity. Widget frame ownership stays with caller.
    const float x=24*s,y=90*s,W=std::min(1400*s,g.Width()-48*s),H=std::min(850*s,g.Height()-114*s);
    if(W<=0||H<360*s)return;
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
    field("diag_mission","mission (unavailable)",g_mission,2,4,20);field("diag_fp","fingerprint (not unique ID)",g_fingerprint,3,4,20);
    cy+=52*s;
    field("diag_search","search",g_search,0,2,96);field("diag_hook","hook ID contains",g_hook,1,2,79);cy+=56*s;
    Filter filter{Number::Parse(g_peer),Number::Parse(g_entity),Number::Parse(g_mission),Number::Parse(g_fingerprint),Narrow(g_search),Narrow(g_hook)};
    auto started=Qpc();auto rows=g_snapshot.Select(g_tab,filter);g_snapshot.formatUs=Micros(started);
    const float rowHeight=g.LineHeight(FONT_SMALL)+3*s;
    const float footerY=y+H-174*s;
    if(g_tab==Tab::Network){
        auto&c=g_snapshot.flightCounters;
        rows.Add(Format("Recorder %s | sampled events %u / cap %u | snapshot %s",g_snapshot.recorderEnabled?"ON":"OFF",unsigned(g_snapshot.eventCount),unsigned(kEvents),g_snapshot.eventsBusy?"BUSY / no sample":"copied"));
        rows.Add(Format("accepted %u busyDrops %u lifecycleDrops %u historyOverwrites %u drainDrops %u IO errors %u",c.accepted,c.busyDrops,c.lifecycleDrops,c.historyOverwrites,c.drainDrops,c.ioErrors));
        rows.Add(Format("segmentsEvicted %u eventsEvicted %u rotationFailures %u exportFailed %u",c.segmentsEvicted,c.eventsEvicted,c.rotationFailures,c.exportFailed));
        rows.Add("Transport queues / packet loss / stale-reject totals: not instrumented by this snapshot");
        rows.Add("Peer, entity and fingerprint filters apply to Events / Entities; hook ID applies to Hooks");
    }else if(g_tab==Tab::Performance){
        rows.Add(Format("Snapshot %.2f us | formatting %.2f us | refresh 250 ms | sample %llu @ %llu ms",g_snapshot.snapshotUs,g_snapshot.formatUs,(unsigned long long)g_snapshot.sequence,(unsigned long long)g_snapshot.sampledAtMs));
        rows.Add("These are local snapshot/format costs; full draw/frame cost and ON/OFF game overhead not measured");
        rows.Add(Format("Fixed caps: %u recorder events, %u hook records, %u formatted rows",unsigned(kEvents),unsigned(kHooks),unsigned(kRows)));
    }
    if(g_tab==Tab::Entities){
        rows.Add("Native car/remote values + observed recorder identities. Live owner/streaming/pending creation: not instrumented");
        if(g_snapshot.native.truncated)rows.Add("Native value rows TRUNCATED (fixed row/text cap)");
    }
    if(g_tab==Tab::HooksScm){
        if(filter.Valid()){Rows detail;g_snapshot.Detail(detail,filter.hookId);for(size_t n=0;n<rows.count;++n)detail.Add(std::move(rows.text[n]));rows=std::move(detail);}
        rows.Add(Format("Registry snapshot %s | records %u | truncated %s | SCM runtime state not instrumented",g_snapshot.hooksBusy?"BUSY / no sample":"copied",unsigned(g_snapshot.hookCount),g_snapshot.hooksTruncated?"YES":"no"));
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
    }
    g.Text(FONT_SMALL,x+W-20*s,cy,Format("%u rows | page %u/%u %s",unsigned(rows.count),unsigned(g_page+1),unsigned(pages),g_copyStatus.c_str()),w::kSaMuted,TEXT_RIGHT);
    g.Text(FONT_SMALL,cx,cy+32*s,"Trace lifecycle/export controls unavailable here. Mission, owner and native ABI remain explicit gaps.",w::kSaMuted);
    g.Text(FONT_SMALL,cx,cy+52*s,"Local diagnostic metadata. No names, packet contents, save data or automatic upload.",w::kSaMuted);
    g.Text(FONT_SMALL,cx,cy+74*s,"About / Credits",w::kSaBlue);
    g.Text(FONT_SMALL,cx,cy+94*s,"reSA-Coop Diagnostic System",w::kSaMuted);
    g.Text(FONT_SMALL,cx,cy+114*s,"Developed and maintained as part of reSA-Coop by opiahex",w::kSaMuted);
    g.Text(FONT_SMALL,cx,cy+134*s,"Source: https://github.com/opiahex/reSA-Coop",w::kSaMuted);
}
}
