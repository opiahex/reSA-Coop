// reSA-Coop Diagnostic System
// Copyright (c) 2026 opiahex
// Original source: https://github.com/opiahex/reSA-Coop
// SPDX-License-Identifier: LicenseRef-reSA-Coop-Diagnostics-Attribution-1.0
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "features/sync-diagnostics/TaskLifecycleNative.h"
#include "shared/api/sync-diagnostics/Intelligence.h"
#include <windows.h>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <new>
#include "common/lib/util/Util.h"
#include "entities/player/Players.h"
#include "entities/ped/Peds.h"
#include "shared/api/net-session/Session.h"
#include "shared/game/Game.h"
#include "shared/lib/core/Core.h"
#include "shared/lib/memory/Memory.h"
#include "shared/lib/memory/HookRegistry.h"

namespace coop::task_lifecycle::native {
namespace {
constexpr uintptr_t kBase=0x400000;
constexpr uint32_t kImageSize=18313216,kTimestamp=1114702282,kChecksum=14441450;
constexpr size_t kTaskStride=128;
static_assert(sizeof(void*)==4,"TaskLifecycle native adapters require x86");
std::atomic<InstallState> install{InstallState::NotAttempted};
std::atomic<uint32_t> attempted{0},installedMask{0},registryFailures{0},selected{0},revision{0};
std::atomic<uint32_t> activeSelections{0};
std::atomic<uint32_t> expired{0},invalidSelection{0},wrongThread{0},rejectedAdmission{0};
std::atomic<uint32_t> unknownTask{0},unknownEvent{0},readFailed{0},afterUnavailable{0},ordinaryThrottled{0};
struct Range{uintptr_t first=0,last=0;uint32_t flags=0;};
std::array<Range,16>sections{};unsigned sectionCount=0;uintptr_t selfBase=0;uint32_t selfSize=0;
struct Binding{Selection selection{};Subject subject{};void*body=nullptr;void*intelligence=nullptr;uint32_t recording=0,localWorld=0;std::array<uint64_t,2>ordinaryAt{};std::array<bool,2>ordinaryKnown{};};
std::array<Binding,kSelections>bindings{};
bool GameThread()noexcept{auto t=core::DiagnosticGameThreadId();return t&&t==GetCurrentThreadId();}
bool Read(uintptr_t at,void*out,size_t size)noexcept{
    SIZE_T got=0;if(!at||!out||!size||size>256||size-1>UINTPTR_MAX-at)return false;
    if(ReadProcessMemory(GetCurrentProcess(),(const void*)at,out,size,&got)&&got==size)return true;
    ++readFailed;return false;
}
template<class T>bool ReadValue(uintptr_t at,T&value)noexcept{return Read(at,&value,sizeof value);}
bool InRange(uintptr_t at,size_t size,uint32_t required,uint32_t forbidden=0)noexcept{
    if(!at||!size||size-1>UINTPTR_MAX-at)return false;
    for(unsigned i=0;i<sectionCount;++i){const auto&r=sections[i];if(at>=r.first&&at<r.last&&size<=r.last-at&&(r.flags&required)==required&&!(r.flags&forbidden))return true;}
    return false;
}
bool ImageIdentity(uintptr_t base,IMAGE_NT_HEADERS32&nt)noexcept{
    IMAGE_DOS_HEADER dos{};
    return base&&ReadValue(base,dos)&&dos.e_magic==IMAGE_DOS_SIGNATURE&&dos.e_lfanew>=0&&dos.e_lfanew<=4096&&
        ReadValue(base+(uintptr_t)dos.e_lfanew,nt)&&nt.Signature==IMAGE_NT_SIGNATURE&&
        nt.FileHeader.Machine==IMAGE_FILE_MACHINE_I386&&nt.OptionalHeader.Magic==IMAGE_NT_OPTIONAL_HDR32_MAGIC;
}
bool SupportedImage()noexcept{
    IMAGE_NT_HEADERS32 nt{};auto base=(uintptr_t)GetModuleHandleW(nullptr);
    if(base!=kBase||!ImageIdentity(base,nt)||nt.FileHeader.TimeDateStamp!=kTimestamp||
       nt.OptionalHeader.ImageBase!=kBase||nt.OptionalHeader.SizeOfImage!=kImageSize||
       nt.OptionalHeader.CheckSum!=kChecksum||nt.FileHeader.NumberOfSections==0||nt.FileHeader.NumberOfSections>sections.size())return false;
    IMAGE_DOS_HEADER dos{};if(!ReadValue(base,dos))return false;
    sectionCount=0;auto at=base+dos.e_lfanew+24+nt.FileHeader.SizeOfOptionalHeader;
    for(unsigned i=0;i<nt.FileHeader.NumberOfSections;++i){IMAGE_SECTION_HEADER sh{};if(!ReadValue(at+i*sizeof sh,sh))return false;
        auto size=sh.Misc.VirtualSize;if(!size||sh.VirtualAddress>kImageSize||size>kImageSize-sh.VirtualAddress)return false;
        sections[sectionCount++]={base+sh.VirtualAddress,base+sh.VirtualAddress+size,sh.Characteristics};}
    HMODULE module=nullptr;if(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCWSTR)&SupportedImage,&module)){
        IMAGE_NT_HEADERS32 own{};if(ImageIdentity((uintptr_t)module,own)&&own.OptionalHeader.SizeOfImage>=4096&&own.OptionalHeader.SizeOfImage<=0x10000000u&&
            (uintptr_t)module<=UINTPTR_MAX-own.OptionalHeader.SizeOfImage){selfBase=(uintptr_t)module;selfSize=own.OptionalHeader.SizeOfImage;}}
    return true;
}
struct Pool{uintptr_t objects=0,map=0;int32_t size=0;};
static_assert(sizeof(Pool)==12);
bool PoolHeader(uintptr_t global,Pool&pool)noexcept{
    uintptr_t p=0;return ReadValue(global,p)&&p&&Read(p,&pool,sizeof pool)&&pool.objects&&pool.map&&pool.size>0&&pool.size<=4096;
}
bool ResolvePed(uint32_t reference,void*&body,void*&intel)noexcept{
    body=intel=nullptr;Pool pool{};if(reference>INT32_MAX||!PoolHeader(0xB74490,pool))return false;
    const auto index=reference>>8;if(index>=(uint32_t)pool.size||index>(UINTPTR_MAX-pool.objects)/game::kPedPoolElemSize)return false;
    if(index>UINTPTR_MAX-pool.map)return false;
    uint8_t flag=0;if(!ReadValue(pool.map+index,flag)||flag>=128||flag!=(uint8_t)reference)return false;
    auto at=pool.objects+(uintptr_t)index*game::kPedPoolElemSize;if(at>UINTPTR_MAX-0x480)return false;
    if(!ReadValue(at+0x47C,intel)||!intel||(uintptr_t)intel>UINTPTR_MAX-0x68)return false;
    uint8_t again=0;if(!ReadValue(pool.map+index,again)||again!=flag)return false;body=(void*)at;return true;
}
bool TaskInPool(void*task)noexcept{
    Pool pool{};if(!task||!PoolHeader(0xB744A8,pool))return false;auto p=(uintptr_t)task;
    if(p<pool.objects||(p-pool.objects)%kTaskStride)return false;
    auto index=(p-pool.objects)/kTaskStride;if(index>=(uintptr_t)pool.size||index>UINTPTR_MAX-pool.map)return false;
    uint8_t flag=0;return ReadValue(pool.map+index,flag)&&flag<128;
}
Known<int32_t> ConstantType(void*object,unsigned slot,bool task)noexcept{
    if(!object)return {};if(task&&!TaskInPool(object)){++unknownTask;return {};}
    uintptr_t table=0,method=0;unsigned char code[6]{};uint32_t value=0;
    if(ReadValue((uintptr_t)object,table)&&InRange(table,(slot+1)*sizeof(void*),IMAGE_SCN_MEM_READ,IMAGE_SCN_MEM_WRITE|IMAGE_SCN_MEM_EXECUTE)&&
       ReadValue(table+slot*sizeof(void*),method)&&InRange(method,sizeof code,IMAGE_SCN_MEM_EXECUTE)&&Read(method,code,sizeof code)){
        if(code[0]==0xB8&&code[5]==0xC3){std::memcpy(&value,code+1,sizeof value);if(value<4096)return Known<int32_t>::Of((int32_t)value);}
        // Exact supported-file6857C0 constant-zero getter, not a virtual call.
        if(code[0]==0x33&&code[1]==0xC0&&code[2]==0xC3)return Known<int32_t>::Of(0);}
    if(task)++unknownTask;else ++unknownEvent;return {};
}
uint32_t AuthorityInc(uint8_t peer)noexcept{if(peer==Net().MyId())return Net().MyIncarnation();auto*p=Net().Peer(peer);return p?p->incarnation:0;}
uint32_t AuthorityWorld(uint8_t peer)noexcept{return peer==Net().MyId()?Net().WorldEpoch():Net().PeerWorldEpoch(peer);}
bool ResolveBinding(const Selection&s,Binding&out,bool refreshMotion=false)noexcept{
    void*body=nullptr;void*intel=nullptr;if(!ResolvePed(s.nativePedRef,body,intel))return false;
    if(s.peer>2||!s.world||!s.incarnation||AuthorityWorld(s.peer)!=s.world||AuthorityInc(s.peer)!=s.incarnation)return false;
    uint32_t actualMotion=0;
    if(s.kind==SubjectKind::Player){
        if(s.peer==Net().MyId()){uintptr_t local=0;if(!ReadValue(game::addr::Players,local)||(void*)local!=body||!(actualMotion=Players::Instance().MotionEpoch())||(!refreshMotion&&actualMotion!=s.motionEpoch))return false;}
        else{auto*p=Players::Instance().Get(s.peer);if(!p||!p->hasState||(void*)p->ped!=body||p->pedHandle<0||(uint32_t)p->pedHandle!=s.nativePedRef||!(actualMotion=p->state.motion.epoch)||(!refreshMotion&&actualMotion!=s.motionEpoch))return false;}
    }else if(s.kind==SubjectKind::Npc){
        const auto&all=Peds::Instance().All();auto i=all.find(s.entity);if(!s.entity||!s.generation||i==all.end())return false;auto&n=i->second;
        if((void*)n.ped!=body||n.handle<0||(uint32_t)n.handle!=s.nativePedRef||n.netId!=s.entity||n.owner!=s.peer||Net().EntityGeneration(ENTITY_PED,s.entity)!=s.generation)return false;
        uint16_t model=0;if(!ReadValue((uintptr_t)body+0x22,model)||model!=n.model)return false;
    }else return false;
    void*owner=nullptr;void*managerOwner=nullptr;void*handlerOwner=nullptr;
    if(!ReadValue((uintptr_t)intel,owner)||owner!=body||!ReadValue((uintptr_t)intel+0x30,managerOwner)||managerOwner!=body||
       !ReadValue((uintptr_t)intel+0x34,handlerOwner)||handlerOwner!=body)return false;
    out={};out.selection=s;out.body=body;out.intelligence=intel;out.recording=flight::ActiveRecordingToken();out.localWorld=Net().WorldEpoch();
    out.subject.kind=Known<SubjectKind>::Of(s.kind);out.subject.nativePedRef=Known<uint32_t>::Of(s.nativePedRef);out.subject.localPeer=Net().MyId();out.subject.owner=Known<uint8_t>::Of(s.peer);
    out.subject.context.source=s.peer;out.subject.context.incarnation=s.incarnation;out.subject.context.world=s.world;
    if(s.kind==SubjectKind::Player){out.subject.motionEpoch=Known<uint32_t>::Of(actualMotion);out.selection.motionEpoch=actualMotion;}
    if(s.kind==SubjectKind::Npc){out.subject.context.entity=s.entity;out.subject.context.generation=s.generation;}
    return out.recording&&out.localWorld;
}
bool Current(const Binding&before,Binding&after,bool sameOperation=true)noexcept{
    if(!flight::Enabled()||!before.recording||flight::ActiveRecordingToken()!=before.recording||Net().WorldEpoch()!=before.localWorld||NowMs()>before.selection.expiresMs)return false;
    if(!ResolveBinding(before.selection,after,true)||after.recording!=before.recording||after.body!=before.body||after.intelligence!=before.intelligence||after.localWorld!=before.localWorld)return false;
    return !sameOperation||before.subject.motionEpoch.available==after.subject.motionEpoch.available&&
        (!before.subject.motionEpoch.available||before.subject.motionEpoch.value==after.subject.motionEpoch.value);
}
// Tree provider: no virtual/native calls, no hooks, no records, no cached native
// pointers across captures. The firstFree field is included conservatively: an
// unrelated allocation may invalidate a capture rather than certify continuity.
struct TreePool{uintptr_t objects=0,map=0;int32_t size=0,firstFree=0;};
static_assert(sizeof(TreePool)==16);
struct TreeWitness{uintptr_t object=0,global=0,poolAddress=0,table=0;TreePool pool{};uint32_t index=0;uint8_t flag=0;};
bool TreePoolEqual(const TreePool&a,const TreePool&b)noexcept{return a.objects==b.objects&&a.map==b.map&&a.size==b.size&&a.firstFree==b.firstFree;}
bool TreePoolRead(uintptr_t global,uintptr_t&address,TreePool&pool)noexcept{
    return InRange(global,sizeof(uintptr_t),IMAGE_SCN_MEM_READ)&&ReadValue(global,address)&&address&&ReadValue(address,pool)&&
        pool.objects&&pool.map&&pool.size>0&&pool.size<=4096&&pool.firstFree>=-1&&pool.firstFree<pool.size;
}
tree::Read TreeValidate(const TreeWitness&w)noexcept{
    uintptr_t address=0,table=0;TreePool pool{};uint8_t flag=0;
    if(!TreePoolRead(w.global,address,pool))return tree::Read::Unavailable;
    if(address!=w.poolAddress||!TreePoolEqual(pool,w.pool))return tree::Read::Changed;
    if(!ReadValue(pool.map+w.index,flag))return tree::Read::Unavailable;
    if(flag!=w.flag||flag>=128)return tree::Read::Changed;
    if(!ReadValue(w.object,table))return tree::Read::Unavailable;
    if(table!=w.table)return tree::Read::Changed;
    return tree::Read::Ok;
}
tree::Read TreeWitnessRead(uintptr_t object,bool task,TreeWitness&out)noexcept{
    out={};const uintptr_t global=task?0xB744A8:0xB744AC;const size_t stride=task?128:68;
    uintptr_t address=0;TreePool pool{};
    if(!TreePoolRead(global,address,pool))return tree::Read::Unavailable;
    if((uintptr_t)pool.size>(UINTPTR_MAX-pool.objects)/stride)return tree::Read::Invalid;
    if(object<pool.objects||(object-pool.objects)%stride)return tree::Read::Invalid;
    const uintptr_t index=(object-pool.objects)/stride;
    if(index>=(uintptr_t)pool.size||index>UINTPTR_MAX-pool.map)return tree::Read::Invalid;
    uint8_t flag=0;uintptr_t table=0;
    if(!ReadValue(pool.map+index,flag))return tree::Read::Unavailable;
    if(flag>=128)return tree::Read::Invalid;
    if(!ReadValue(object,table))return tree::Read::Unavailable;
    if(!InRange(table,(task?5:3)*sizeof(uintptr_t),IMAGE_SCN_MEM_READ,IMAGE_SCN_MEM_WRITE|IMAGE_SCN_MEM_EXECUTE))return tree::Read::Invalid;
    out={object,global,address,table,pool,(uint32_t)index,flag};return TreeValidate(out);
}
bool TreeMethod(uintptr_t table,unsigned slot,std::array<uint8_t,6>&bytes)noexcept{
    uintptr_t method=0;return slot<5&&InRange(table,(slot+1)*sizeof(uintptr_t),IMAGE_SCN_MEM_READ,IMAGE_SCN_MEM_WRITE|IMAGE_SCN_MEM_EXECUTE)&&
        ReadValue(table+slot*sizeof(uintptr_t),method)&&InRange(method,bytes.size(),IMAGE_SCN_MEM_EXECUTE)&&Read(method,bytes.data(),bytes.size());
}
Known<bool> TreeSimple(uintptr_t table)noexcept{
    std::array<uint8_t,6>b{};if(!TreeMethod(table,3,b))return {};
    // Exact supported-file base getters43E310 /4211A0; never invoke a vcall.
    if(b[0]==0xB0&&b[1]==1&&b[2]==0xC3)return Known<bool>::Of(true);
    if(b[0]==0x32&&b[1]==0xC0&&b[2]==0xC3)return Known<bool>::Of(false);
    return {};
}
Known<int32_t> TreePriority(uintptr_t table)noexcept{
    std::array<uint8_t,6>b{};if(!TreeMethod(table,2,b))return {};
    if(b[0]==0xB8&&b[5]==0xC3){int32_t value=0;std::memcpy(&value,b.data()+1,4);return Known<int32_t>::Of(value);}
    // Constant EAX zero is distinct from an unavailable/dynamic getter.
    if(b[0]==0x33&&b[1]==0xC0&&b[2]==0xC3)return Known<int32_t>::Of(0);
    return {};
}
struct TreeReader{
    Binding binding{};uint32_t selectionRevision=0;
    std::array<TreeWitness,65>tasks{};std::array<TreeWitness,3>events{};size_t taskCount=0,eventCount=0;
    bool SubjectNow()noexcept{Binding after{};return revision.load(std::memory_order_relaxed)==selectionRevision&&Current(binding,after);}
    tree::Read Witness(uintptr_t object,bool task,uint64_t&cookie,TreeWitness*&w)noexcept{
        auto*all=task?tasks.data():events.data();auto&count=task?taskCount:eventCount;const auto capacity=task?tasks.size():events.size();
        for(size_t i=0;i<count;++i)if(all[i].object==object){w=&all[i];cookie=i+1;return TreeValidate(*w);}
        if(count>=capacity)return tree::Read::Unavailable;
        auto r=TreeWitnessRead(object,task,all[count]);if(r!=tree::Read::Ok)return r;
        w=&all[count];cookie=++count;return tree::Read::Ok;
    }
    static bool SubjectValid(void*user,const Subject&)noexcept{return ((TreeReader*)user)->SubjectNow();}
    static tree::Read Roots(void*user,tree::Roots&out)noexcept{
        auto&r=*(TreeReader*)user;out={};std::array<uintptr_t,11>values{};
        if(!r.SubjectNow()||!Read((uintptr_t)r.binding.intelligence+4,values.data(),sizeof values))return tree::Read::Unavailable;
        for(size_t i=0;i<5;++i)out.primary[i]=tree::Ref::Of(values[i]);
        for(size_t i=0;i<6;++i)out.secondary[i]=tree::Ref::Of(values[5+i]);return tree::Read::Ok;
    }
    static tree::Read History(void*user,tree::HistoryRefs&out)noexcept{
        auto&r=*(TreeReader*)user;out={};std::array<uintptr_t,4>values{};
        // Intelligence+34 is Handler; Handler+4 is History: aborted/nonTemp/temp/stored.
        if(!r.SubjectNow()||!Read((uintptr_t)r.binding.intelligence+0x38,values.data(),sizeof values))return tree::Read::Unavailable;
        out.lastAbortedTask=tree::Ref::Of(values[0]);out.nonTemporary=tree::Ref::Of(values[1]);out.temporary=tree::Ref::Of(values[2]);out.stored=tree::Ref::Of(values[3]);return tree::Read::Ok;
    }
    static tree::Read Task(void*user,uintptr_t object,tree::TaskFacts&out)noexcept{
        auto&r=*(TreeReader*)user;out={};if(!r.SubjectNow())return tree::Read::Changed;
        uint64_t cookie=0;TreeWitness*w=nullptr;auto result=r.Witness(object,true,cookie,w);if(result!=tree::Read::Ok)return result;
        uintptr_t parent=0;if(!ReadValue(object+4,parent))return tree::Read::Unavailable;
        out.cookie=Known<uint64_t>::Of(cookie);out.parent=tree::Ref::Of(parent);out.type=ConstantType((void*)object,4,true);out.simple=TreeSimple(w->table);
        std::array<uint8_t,6>b{};
        if(out.simple.available&&TreeMethod(w->table,2,b)){
            if(out.simple.value&&b[0]==0x33&&b[1]==0xC0&&b[2]==0xC3)out.subtask=tree::Ref::Of(0);
            else if(!out.simple.value&&b[0]==0x8B&&b[1]==0x41&&b[2]==0x08&&b[3]==0xC3){uintptr_t child=0;if(!ReadValue(object+8,child))return tree::Read::Unavailable;out.subtask=tree::Ref::Of(child);}
        }
        return TreeValidate(*w);
    }
    static tree::Read Event(void*user,uintptr_t object,tree::EventFacts&out)noexcept{
        auto&r=*(TreeReader*)user;out={};if(!r.SubjectNow())return tree::Read::Changed;
        uint64_t cookie=0;TreeWitness*w=nullptr;auto result=r.Witness(object,false,cookie,w);if(result!=tree::Read::Ok)return result;
        out.cookie=Known<uint64_t>::Of(cookie);out.type=ConstantType((void*)object,1,false);out.priority=TreePriority(w->table);return TreeValidate(*w);
    }
    static tree::Read TaskValid(void*user,uintptr_t object,uint64_t cookie)noexcept{
        auto&r=*(TreeReader*)user;if(!r.SubjectNow())return tree::Read::Changed;
        if(!cookie||cookie>r.taskCount||r.tasks[cookie-1].object!=object)return tree::Read::Invalid;return TreeValidate(r.tasks[cookie-1]);
    }
    static tree::Read EventValid(void*user,uintptr_t object,uint64_t cookie)noexcept{
        auto&r=*(TreeReader*)user;if(!r.SubjectNow())return tree::Read::Changed;
        if(!cookie||cookie>r.eventCount||r.events[cookie-1].object!=object)return tree::Read::Invalid;return TreeValidate(r.events[cookie-1]);
    }
    tree::Services Services()noexcept{return {this,&SubjectValid,&Roots,&History,&Task,&Event,&TaskValid,&EventValid};}
};
bool TreeLayoutVerified()noexcept{
    // Read-only layouts, file-bound to exact supported EXE in private preflight.
    struct Prefix{uintptr_t at;std::array<uint8_t,16>bytes;uint8_t size;};
    const std::array<Prefix,5>proofs{{
        {0x61A5A0,{0x8B,0x0D,0xA8,0x44,0xB7,0x00},6},
        {0x4B5630,{0x8B,0x4C,0x24,0x04,0x56,0x8B,0x35,0xAC,0x44,0xB7,0x00,0x2B,0x0E,0xB8,0x79,0x78},16},
        {0x43E310,{0xB0,0x01,0xC3},3},{0x4211A0,{0x32,0xC0,0xC3},3},
        {0x4B8CA0,{0x8B,0x41,0x08,0x85,0xC0,0x75,0x03,0x8B,0x41,0x04,0xC3},11}}};
    for(const auto&p:proofs){std::array<uint8_t,16>actual{};if(!InRange(p.at,p.size,IMAGE_SCN_MEM_EXECUTE)||!Read(p.at,actual.data(),p.size)||std::memcmp(actual.data(),p.bytes.data(),p.size))return false;}
    return true;
}
enum class Site:uint8_t{Intelligence,Handler,Manager,Body};
bool Acquire(void*site,Site kind,bool discrete,Binding&out,uint32_t&rev)noexcept{
    if(install.load(std::memory_order_relaxed)!=InstallState::Ready||!activeSelections.load(std::memory_order_relaxed)||!flight::Enabled())return false;
    if(!GameThread()){++wrongThread;return false;}rev=revision.load(std::memory_order_relaxed);
    auto now=NowMs();for(size_t i=0;i<selected.load(std::memory_order_relaxed);++i){auto&b=bindings[i];if(!b.intelligence)continue;
        auto address=kind==Site::Body?(uintptr_t)b.body:(uintptr_t)b.intelligence+(kind==Site::Manager?4:kind==Site::Handler?0x34:0);if(address!=(uintptr_t)site)continue;
        if(now>b.selection.expiresMs||flight::ActiveRecordingToken()!=b.recording){b.intelligence=nullptr;activeSelections.fetch_sub(1);++expired;return false;}
        if(!discrete){const unsigned ordinarySite=kind==Site::Intelligence?0:1;
            if(b.ordinaryKnown[ordinarySite]&&now-b.ordinaryAt[ordinarySite]<100){++ordinaryThrottled;return false;}
            b.ordinaryKnown[ordinarySite]=true;b.ordinaryAt[ordinarySite]=now;
            if(flight::AdmitNativeObservation(b.subject.context,(uint16_t)Stage::Before,b.subject.localPeer,false)!=flight::NativeAdmission::Admitted){++rejectedAdmission;return false;}}
        // Exact selected discrete operations bypass the shared ordinary read budget.
        // Their bounded critical records retain the recorder's reserve/loss counters.
        if(!Current(b,out,false)){b.intelligence=nullptr;activeSelections.fetch_sub(1);++expired;return false;}return true;}
    return false;
}
struct Snapshot{Observation observation{};void*pointer=nullptr;bool read=false;};
Snapshot Observe(void*intel,int slot,bool secondary=false)noexcept{
    Snapshot out{};uintptr_t manager=(uintptr_t)intel+4;
    if(slot==-1&&!secondary){std::array<void*,5>roots{};if(!Read(manager,roots.data(),sizeof roots))return out;for(int i=0;i<5;++i)if(roots[i]){slot=i;out.pointer=roots[i];break;}
        if(slot==-1){out.observation.taskSlot=Known<int32_t>::Of(-1);out.observation.taskType=Known<int32_t>::Of(-1);out.read=true;return out;}}
    else{if(slot<0||slot>=(secondary?6:5))return out;if(!ReadValue(manager+(secondary?0x14:0)+(uintptr_t)slot*sizeof(void*),out.pointer))return out;}
    out.observation.taskSlot=Known<int32_t>::Of(slot);out.observation.taskType=out.pointer?ConstantType(out.pointer,4,true):Known<int32_t>::Of(-1);out.read=true;return out;
}
Cause Caller(const void*address)noexcept{
    Cause cause{};auto at=(uintptr_t)address;
    if(InRange(at,1,IMAGE_SCN_MEM_EXECUTE)){cause.callerImageKind=Known<uint8_t>::Of(1);cause.callerRva=Known<uint32_t>::Of((uint32_t)(at-kBase));}
    else if(selfBase&&at>=selfBase&&at-selfBase<selfSize){cause.callerImageKind=Known<uint8_t>::Of(2);cause.callerRva=Known<uint32_t>::Of((uint32_t)(at-selfBase));}
    return cause;
}
using VoidFn=void(__fastcall*)(void*,void*);
using EventFn=void(__fastcall*)(void*,void*,const void*);
using PrimaryFn=void(__fastcall*)(void*,void*,void*,int,uint32_t);
using SecondaryFn=void(__fastcall*)(void*,void*,void*,int);
// Native601640 consumes one raw stack word (low byte bool), returns ret4.
using IntelligenceFlushFn=void(__fastcall*)(void*,void*,uint32_t);
// Native5E4110 consumes x/y/z/bool as four raw stack words and returns ret16.
using TeleportFn=void(__fastcall*)(void*,void*,uint32_t,uint32_t,uint32_t,uint32_t);
VoidFn originalEvent=nullptr,originalFlush=nullptr,originalImmediate=nullptr;EventFn originalResponse=nullptr;PrimaryFn originalPrimary=nullptr;SecondaryFn originalSecondary=nullptr;
IntelligenceFlushFn originalIntelligenceFlush=nullptr;
TeleportFn originalTeleport=nullptr;
// Local856 parts identify boundaries, not a reason WHY a task was cancelled.
// NativeFlags:1 state known,2 HP known,4 changed subject/no reads,8 bool input
// known,16 RPM gap,32 nonfinite HP,64 invalid state; bits8..15=actual bool byte.
enum BoundaryFlags:uint32_t {BoundaryState=1,BoundaryHealth=2,BoundaryChanged=4,BoundaryInput=8,BoundaryReadGap=16,BoundaryNonFinite=32,BoundaryInvalidState=64};
void FlushBoundary(Scope&scope,const Binding&binding,uint32_t rev,uint16_t part,Known<uint32_t>input={},Operation operation=Operation::FlushImmediate)noexcept{
    auto span=scope.Identity();if(!span.available)return;
    flight::Event e{};e.context=binding.subject.context;e.localPeer=binding.subject.localPeer;e.stage=856;e.reason=part;e.span=span.value;e.detailKnown=flight::SpanKnown;
    if(binding.subject.nativePedRef.available)e.nativeHandle=binding.subject.nativePedRef.value;
    if(binding.subject.motionEpoch.available)e.motionEpoch=binding.subject.motionEpoch.value;
    if(binding.subject.owner.available){e.owner=binding.subject.owner.value;e.detailKnown|=flight::OwnerKnown;}
    if(binding.subject.ownerEpoch.available){e.ownerEpoch=binding.subject.ownerEpoch.value;e.detailKnown|=flight::OwnerEpochKnown;}
    flight::AddUnsigned(e,flight::Metric::TaskRecordingToken,span.recordingToken);
    if(binding.subject.kind.available)flight::AddUnsigned(e,flight::Metric::TaskSubjectKind,unsigned(binding.subject.kind.value));
    flight::AddUnsigned(e,flight::Metric::TaskOperation,unsigned(operation));
    uint32_t flags=input.available?BoundaryInput|((input.value&255u)<<8):0;
    Binding fresh{};int32_t state=0;float health=0;bool stateRead=false,healthRead=false;
    if(rev!=revision.load(std::memory_order_relaxed)||!Current(binding,fresh))flags|=BoundaryChanged;
    else{
        stateRead=ReadValue((uintptr_t)fresh.body+0x530,state);healthRead=ReadValue((uintptr_t)fresh.body+0x540,health);
        Binding end{};if(rev!=revision.load(std::memory_order_relaxed)||!Current(fresh,end)){flags|=BoundaryChanged;stateRead=healthRead=false;}
        else{
            if(!stateRead||!healthRead)flags|=BoundaryReadGap;
            if(stateRead&&state<0){flags|=BoundaryInvalidState;stateRead=false;}
            if(healthRead&&!std::isfinite(health)){flags|=BoundaryNonFinite;healthRead=false;}
        }
    }
    if(stateRead){flags|=BoundaryState;flight::AddUnsigned(e,flight::Metric::ObservedPedState,uint32_t(state));}
    if(healthRead){flags|=BoundaryHealth;flight::AddFloat(e,flight::Metric::ObservedHealth,health);}
    flight::AddUnsigned(e,flight::Metric::NativeFlags,flags);
    // Record(false) is reflected by the existing critical/loss counters. This
    // is admission only; exported pairing/counts are required for completeness.
    if(scope.Active())flight::Record(e);
}
template<class Call>void FlushOperationCall(void*site,Site kind,uint16_t beforePart,Known<uint32_t>input,const void*caller,Call&&original,Operation operation=Operation::FlushImmediate)noexcept{
    Binding binding{};uint32_t rev=0;if(!Acquire(site,kind,true,binding,rev)){original();return;}
    auto before=Observe(binding.intelligence,-1);Scope scope(binding.subject,operation,before.observation,ParentRelation::SameSubjectOnly,Capture::SelectedDiscrete,binding.recording);
    if(scope.Active()){scope.RecordCause(Caller(caller));FlushBoundary(scope,binding,rev,beforePart,input,operation);}
    original(); // All native/task references must be reacquired after this call.
    if(!scope.Active())return;
    FlushBoundary(scope,binding,rev,beforePart+1,input,operation);
    Binding after{};if(rev!=revision.load(std::memory_order_relaxed)||!Current(binding,after)){++afterUnavailable;return;}
    auto observed=Observe(after.intelligence,-1);if(!observed.read){++afterUnavailable;return;}
    scope.Finish(observed.observation); // No native return/result or WHY invented.
}
template<class Call>void OperationCall(void*site,Site kind,Operation operation,bool discrete,int slot,bool secondary,const void*event,bool eventArgument,const void*caller,Call&&original)noexcept{
    Binding binding{};uint32_t rev=0;if(!Acquire(site,kind,discrete,binding,rev)){original();return;}
    auto before=Observe(binding.intelligence,slot,secondary);Scope scope(binding.subject,operation,before.observation,ParentRelation::SameSubjectOnly,discrete?Capture::SelectedDiscrete:Capture::OrdinaryAdmitted,binding.recording);
    if(scope.Active()){auto cause=Caller(caller);if(eventArgument){cause.eventPresent=Known<bool>::Of(event!=nullptr);if(event)cause.eventType=ConstantType((void*)event,1,false);}scope.RecordCause(cause);}
    original(); // Incoming task/event may have been destroyed here. Never dereference them below.
    if(!scope.Active())return;Binding after{};
    if(rev!=revision.load(std::memory_order_relaxed)||!Current(binding,after)){++afterUnavailable;return;}
    auto observed=Observe(after.intelligence,slot,secondary);if(!observed.read){++afterUnavailable;return;}
    if((operation==Operation::AssignPrimary||operation==Operation::AssignSecondary)&&before.read&&before.pointer!=observed.pointer)scope.Replace(before.observation,observed.observation);
    scope.Finish(observed.observation); // Void return: native result deliberately unavailable.
}
void __fastcall EventCycle(void*h,void*edx){auto caller=__builtin_return_address(0);OperationCall(h,Site::Handler,Operation::EventCycle,false,-1,false,nullptr,false,caller,[&](){originalEvent(h,edx);});}
void __fastcall EventResponse(void*h,void*edx,const void*event){auto caller=__builtin_return_address(0);OperationCall(h,Site::Handler,Operation::EventResponse,true,-1,false,event,true,caller,[&](){originalResponse(h,edx,event);});}
void __fastcall Flush(void*m,void*edx){auto caller=__builtin_return_address(0);OperationCall(m,Site::Manager,Operation::Flush,true,-1,false,nullptr,false,caller,[&](){originalFlush(m,edx);});}
void __fastcall Immediate(void*m,void*edx){auto caller=__builtin_return_address(0);FlushOperationCall(m,Site::Manager,3,{},caller,[&](){originalImmediate(m,edx);});}
void __fastcall IntelligenceFlush(void*intel,void*edx,uint32_t primaryDefault){auto caller=__builtin_return_address(0);FlushOperationCall(intel,Site::Intelligence,1,Known<uint32_t>::Of(primaryDefault),caller,[&](){originalIntelligenceFlush(intel,edx,primaryDefault);});}
void __fastcall Teleport(void*body,void*edx,uint32_t x,uint32_t y,uint32_t z,uint32_t resetRotation){auto caller=__builtin_return_address(0);FlushOperationCall(body,Site::Body,5,Known<uint32_t>::Of(resetRotation),caller,[&](){originalTeleport(body,edx,x,y,z,resetRotation);},Operation::Relocation);}
void __fastcall Primary(void*m,void*edx,void*task,int slot,uint32_t unused){auto caller=__builtin_return_address(0);OperationCall(m,Site::Manager,Operation::AssignPrimary,true,slot,false,nullptr,false,caller,[&](){originalPrimary(m,edx,task,slot,unused);});}
void __fastcall Secondary(void*m,void*edx,void*task,int slot){auto caller=__builtin_return_address(0);OperationCall(m,Site::Manager,Operation::AssignSecondary,true,slot,true,nullptr,false,caller,[&](){originalSecondary(m,edx,task,slot);});}
struct HookSite{const char*id;uintptr_t address;std::array<uint8_t,16>native;void*detour;void**original;};
std::array<HookSite,8>sites{{
    {"task.event.cycle",0x4C3F10,{0x83,0xec,0x1c,0x53,0x55,0x56,0x8b,0xd9,0x8b,0x43,0x10,0x85,0xc0,0x8d,0x73,0x04},(void*)&EventCycle,(void**)&originalEvent},
    {"task.event.response",0x4BC600,{0x53,0x55,0x56,0x8b,0xf1,0x8b,0x06,0x8b,0x98,0x7c,0x04,0x00,0x00,0x8b,0x46,0x20},(void*)&EventResponse,(void**)&originalResponse},
    {"task.manager.flush",0x681850,{0x56,0x57,0x8b,0xf9,0x33,0xf6,0x8b,0x0c,0xb7,0x85,0xc9,0x74,0x06,0x8b,0x01,0x6a},(void*)&Flush,(void**)&originalFlush},
    {"task.manager.immediate",0x6818A0,{0x53,0x56,0x57,0x8b,0xf9,0x33,0xf6,0x8b,0x0c,0xb7,0x85,0xc9,0x74,0x25,0x8b,0x57},(void*)&Immediate,(void**)&originalImmediate},
    {"task.manager.primary",0x681AF0,{0x53,0x8b,0x5c,0x24,0x08,0x85,0xdb,0x56,0x57,0x8b,0x7c,0x24,0x14,0x8b,0xf1,0x75},(void*)&Primary,(void**)&originalPrimary},
    {"task.manager.secondary",0x681B60,{0x53,0x8b,0x5c,0x24,0x08,0x56,0x57,0x8b,0x7c,0x24,0x14,0x8b,0xf1,0x8b,0x4c,0xbe},(void*)&Secondary,(void**)&originalSecondary},
    {"task.intelligence.immediate",0x601640,{0x6a,0xff,0x68,0x26,0xe8,0x83,0x00,0x64,0xa1,0x00,0x00,0x00,0x00,0x50,0x64,0x89},(void*)&IntelligenceFlush,(void**)&originalIntelligenceFlush},
    {"task.ped.teleport",0x5E4110,{0x56,0x8b,0xf1,0x8b,0x86,0x98,0x05,0x00,0x00,0x85,0xc0,0x57,0x74,0x1c,0x83,0xf8},(void*)&Teleport,(void**)&originalTeleport},
}};
bool Prefix(const HookSite&s)noexcept{std::array<uint8_t,16>bytes{};return Read(s.address,bytes.data(),bytes.size())&&bytes==s.native;}
}
Status SnapshotStatus()noexcept{
    return {install.load(),attempted.load(),installedMask.load(),registryFailures.load(),activeSelections.load(),revision.load(),expired.load(),invalidSelection.load(),wrongThread.load(),rejectedAdmission.load(),unknownTask.load(),unknownEvent.load(),readFailed.load(),afterUnavailable.load(),ordinaryThrottled.load()};
}
TreeCaptureStatus CaptureSelectedTree(size_t index,tree::Snapshot&out,tree::Limits limits)noexcept{
    out={};
    if(!GameThread()){out.flags=tree::InvalidInput;return TreeCaptureStatus::WrongThread;}
    if(install.load(std::memory_order_relaxed)!=InstallState::Ready||!flight::Enabled()){out.flags=tree::ReadUnavailable;return TreeCaptureStatus::NotReady;}
    if(index>=kSelections||index>=selected.load(std::memory_order_relaxed)||!bindings[index].intelligence){out.flags=tree::InvalidInput;return TreeCaptureStatus::Unselected;}
    if(!TreeLayoutVerified()){out.flags=tree::ReadUnavailable;return TreeCaptureStatus::LayoutUnavailable;}
    TreeReader reader{};reader.selectionRevision=revision.load(std::memory_order_relaxed);
    if(!Current(bindings[index],reader.binding,false)){out.flags=tree::SubjectChanged;return TreeCaptureStatus::SubjectChanged;}
    tree::Capture(reader.binding.subject,reader.Services(),out,limits);
    if(!reader.SubjectNow()){auto counters=out.counters;out={};out.subject=reader.binding.subject;out.counters=counters;out.flags=tree::SubjectChanged;return TreeCaptureStatus::SubjectChanged;}
    return TreeCaptureStatus::Copied;
}
void ClearSelections()noexcept{if(!GameThread()){++wrongThread;return;}activeSelections.store(0,std::memory_order_release);selected.store(0,std::memory_order_release);++revision;bindings={};}
bool SetSelections(const Selection*input,size_t count)noexcept{
    if(!GameThread()){++wrongThread;return false;}if(count>kSelections||(!input&&count)||install.load()!=InstallState::Ready||!flight::ActiveRecordingToken()){++invalidSelection;return false;}
    std::array<Selection,kSelections>requested{};if(count&&!Read((uintptr_t)input,requested.data(),count*sizeof(Selection))){++invalidSelection;return false;}
    std::array<Binding,kSelections>prepared{};auto now=NowMs();auto token=flight::ActiveRecordingToken();
    for(size_t i=0;i<count;++i){auto&s=requested[i];if(s.expiresMs<=now||s.expiresMs-now>60000||!ResolveBinding(s,prepared[i])||prepared[i].recording!=token){++invalidSelection;return false;}
        for(size_t j=0;j<i;++j)if(prepared[i].body==prepared[j].body){++invalidSelection;return false;}}
    if(!token||flight::ActiveRecordingToken()!=token){++invalidSelection;return false;}
    activeSelections.store(0,std::memory_order_release);selected.store(0,std::memory_order_release);++revision;bindings=prepared;selected.store((uint32_t)count,std::memory_order_release);activeSelections.store((uint32_t)count,std::memory_order_release);return true;
}
InstallState InstallNativeAdapters()noexcept{
    if(attempted.exchange(1))return install.load();
    std::array<hookreg::Ticket,8>tickets{};std::array<bool,8>ticketed{};std::array<std::array<uint8_t,16>,8>desired{};
    bool identity=SupportedImage(),preimages=true,registered=true;
    for(size_t i=0;i<sites.size();++i){auto&s=sites[i];desired[i]=s.native;desired[i][0]=0xE9;uint32_t relative=(uint32_t)((uintptr_t)s.detour-s.address-5);std::memcpy(desired[i].data()+1,&relative,sizeof relative);
        hookreg::Spec spec{};std::snprintf(spec.id,sizeof spec.id,"%s",s.id);std::snprintf(spec.module,sizeof spec.module,"gta_sa.exe");std::snprintf(spec.owner,sizeof spec.owner,"TaskLifecycle");std::snprintf(spec.purpose,sizeof spec.purpose,"Native observer;16B observation prefix,not displaced length;file ABI verified only");
        spec.site=s.address;spec.span=16;spec.kind=hookreg::Kind::Detour;
        // A matching16-byte preimage is not full chain/ABI certification.
        spec.chain=hookreg::Chain::NotChecked;hookreg::ManifestBytes(spec.expected,s.native.data(),16);hookreg::ManifestBytes(spec.desired,desired[i].data(),16);
        ticketed[i]=hookreg::CaptureBefore(spec,tickets[i])==hookreg::Put::Stored;if(!ticketed[i]){++registryFailures;registered=false;}
        if(!Prefix(s))preimages=false;
    }
    if(!identity||!preimages||!registered||mem::HasUnsafePatchFailure()){
        auto status=!registered?InstallState::RegistryUnavailable:!identity?InstallState::Unsupported:InstallState::Conflict;
        for(size_t i=0;i<sites.size();++i)if(ticketed[i]){if(hookreg::CaptureAfter(tickets[i],hookreg::Outcome::Conflict,!identity?ERROR_BAD_EXE_FORMAT:ERROR_REVISION_MISMATCH,0,0)!=hookreg::Put::Stored)++registryFailures;}
        install.store(status);return status;
    }
    for(size_t i=0;i<sites.size();++i){auto&s=sites[i];if(!Prefix(s)){if(hookreg::CaptureAfter(tickets[i],hookreg::Outcome::Conflict,ERROR_REVISION_MISMATCH,0,0)!=hookreg::Put::Stored)++registryFailures;install.store(InstallState::Conflict);return InstallState::Conflict;}
        const bool hooked=mem::Hook(s.address,s.detour,s.original,s.id);std::array<uint8_t,16>readback{};bool readbackOk=Read(s.address,readback.data(),readback.size());
        bool ready=hooked&&*s.original&&mem::ExecutableAddress(*s.original)&&readbackOk&&readback==desired[i]&&!mem::HasUnsafePatchFailure();
        auto outcome=ready?hookreg::Outcome::ApiSuccess:Prefix(s)?hookreg::Outcome::FailedUnchanged:hookreg::Outcome::Indeterminate;
        if(ready)installedMask.fetch_or(1u<<i);
        if(hookreg::CaptureAfter(tickets[i],outcome,ready?0:ERROR_GEN_FAILURE,0,(uintptr_t)*s.original)!=hookreg::Put::Stored){++registryFailures;install.store(InstallState::RegistryUnavailable);return InstallState::RegistryUnavailable;}
        if(!ready){auto state=outcome==hookreg::Outcome::FailedUnchanged?InstallState::FailedUnchanged:InstallState::Indeterminate;install.store(state);return state;}
    }
    install.store(InstallState::Ready,std::memory_order_release);return InstallState::Ready;
}
IntelligenceScope::IntelligenceScope(void*intelligence,const void*caller)noexcept{
    Binding b{};uint32_t rev=0;if(!Acquire(intelligence,Site::Intelligence,false,b,rev))return;
    auto before=Observe(b.intelligence,-1);subject_=b.subject;body_=b.body;intelligence_=b.intelligence;recording_=b.recording;revision_=rev;motionEpoch_=b.selection.motionEpoch;localWorld_=b.localWorld;selection_=b.selection;
    scope_=new(scopeStorage_)Scope(subject_,Operation::IntelligenceCycle,before.observation,ParentRelation::SameSubjectOnly,Capture::OrdinaryAdmitted,b.recording);
    if(scope_->Active())scope_->RecordCause(Caller(caller));
}
IntelligenceScope::~IntelligenceScope()noexcept{
    if(!scope_)return;
    if(scope_->Active()){Binding b{};b.subject=subject_;b.body=body_;b.intelligence=intelligence_;b.recording=recording_;b.localWorld=localWorld_;b.selection=selection_;Binding after{};
        if(revision_==revision.load(std::memory_order_relaxed)&&Current(b,after)){auto observed=Observe(after.intelligence,-1);if(observed.read)scope_->Finish(observed.observation);else ++afterUnavailable;}
        else ++afterUnavailable;}
    scope_->~Scope();
}
void ObserveIntelligenceCall(void* intelligence,void* context,const void* caller,IntelligenceBody body){
    if(!body)return;
    IntelligenceScope scope(intelligence,caller);
    body(intelligence,context);
}
namespace {struct AdapterModule:Module{const char*Name()const override{return "TaskLifecycleNative";}void OnInstallHooks()override{InstallNativeAdapters();}}module;
struct Register{Register(){core::RegisterModule(&module);}}registration;}
}
