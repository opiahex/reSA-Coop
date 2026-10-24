#include <windows.h>
#include <psapi.h>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <map>
#include <string>
#include <vector>
#include "common/api/protocol/Protocol.h"
#include "common/config/version/Version.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "common/lib/profile/Profile.h"
#include "entities/ped/Peds.h"
#include "entities/activity/Activities.h"
#include "entities/player/Players.h"
#include "entities/vehicle/Vehicles.h"
#include "shared/api/net-session/Session.h"
#include "shared/config/Config.h"
#include "shared/lib/core/Core.h"
#include "shared/lib/sync/PopulationShare.h"
#include "shared/lib/sync/SyncTrace.h"

namespace coop {
namespace {
using namespace game;
using Entry=SyncAuditEntry;
uint32_t PedIdentity(CPed*p){
    if(!p)return 0;
    if(p==FindPlayerPed(0))return 0x10000u+Net().MyId()+1;
    if(auto*rp=Players::Instance().FindByPed(p))return 0x10000u+rp->id+1;
    if(auto*np=Peds::Instance().Find(p))return np->netId;
    return 0x20000u+(IsPedValid(p)?p->ModelIndex():0);
}
uint32_t Look(CVehicle*v){
    uint32_t h=2166136261u;
    for(int i=0;i<4;++i)h=(h^v->Colors()[i])*16777619u;
    for(int i=0;i<2;++i)h=(h^(uint8_t)Field<int8_t>(v,0x438+i))*16777619u;
    return h;
}
std::string Escape(const std::string&s){std::string r;for(unsigned char c:s){if(c=='"'||c=='\\')r+='\\';if(c>=32)r+=c;}return r;}
class Diagnostics:public Module {
    FILE*file=nullptr;std::wstring path;uint64_t last=0,seq=0,written=0;
    std::map<uint32_t,uint64_t> requests,warnings;
    std::map<uint32_t,std::pair<uint64_t,uint64_t>> observations;
    std::map<uint8_t,std::map<uint32_t,Entry>> remote;
    std::vector<Entry> ownedAudit;
    size_t auditCursor=0;
    uint64_t auditAt=0;
    std::vector<Entry> Collect(){
        std::vector<Entry> out;
        for(auto&[id,n]:Vehicles::Instance().All()){
            Entry e;e.kind=0;e.id=id;e.owner=n.owner;e.model=n.model;e.alive=Vehicles::Alive(n);
            if(e.alive){auto*v=n.veh;e.pos=v->GetPosition();e.area=v->AreaCode();e.relation=PedIdentity(v->Driver());e.look=Look(v);}out.push_back(e);
        }
        for(auto&[id,n]:Vehicles::Instance().Pending())if(!Vehicles::Instance().All().count(id)){Entry e;e.id=id;e.model=n.model;e.owner=n.owner;e.pos=n.pos;e.area=n.area;out.push_back(e);}
        for(auto&[id,n]:Peds::Instance().All()){
            Entry e;e.kind=1;e.id=id;e.owner=n.owner;e.model=n.model;e.alive=n.ped&&IsPedValid(n.ped)&&GetPedRef(n.ped)==n.handle;
            if(e.alive){auto*p=n.ped;e.pos=p->GetPosition();e.area=AreaOf(p);if(p->InVehicle())if(auto*v=Vehicles::Instance().Find(p->Vehicle())){e.relation=v->netId;e.pos=v->veh->GetPosition();e.area=v->veh->AreaCode();}}
            else if(n.hasState){e.pos=n.state.pos;e.area=n.state.area;}
            out.push_back(e);
        }
        core::CollectSyncAudit(out);
        return out;
    }
    void Write(const std::string&s){
        if(!file)return;
        profile::Scope timing(profile::LOG_IO);
        if(written+s.size()>8*1024*1024){fclose(file);std::wstring old=path+L".previous";MoveFileExW(path.c_str(),old.c_str(),MOVEFILE_REPLACE_EXISTING);file=_wfopen(path.c_str(),L"wb");written=0;}
        if(file){fwrite(s.data(),1,s.size(),file);written+=s.size();}
    }
    void Warn(const char*reason,const Entry&e,uint8_t source){
        uint32_t key=((uint32_t)source<<24)|((uint32_t)e.kind<<16)|e.id;auto now=last;
        auto&seen=observations[key];if(!seen.first)seen.first=e.received?e.received:now;seen.second=now;
        if(!e.received||e.received-seen.first<3000)return; // require fresh authoritative evidence across multiple manifests, not one cached old entry
        if(now-warnings[key]<5000)return;warnings[key]=now;
        char b[512];snprintf(b,sizeof(b),"{\"event\":\"anomaly\",\"ms\":%llu,\"peer\":%u,\"source\":%u,\"kind\":%u,\"id\":%u,\"reason\":\"%s\"}\n",(unsigned long long)now,Net().MyId(),source,e.kind,e.id,reason);Write(b);
        LOGW("Sync audit: %s kind=%u id=%04X source=%u",reason,e.kind,e.id,source);
    }
    void Audit(uint8_t from,Reader&r){
        auto count=r.U8();if(count>20||from==Net().MyId()||!Net().Peer(from))return;
        std::vector<Entry> batch;
        for(int i=0;i<count&&r.Ok();++i){Entry e;e.kind=r.U8();e.id=r.U16();e.model=r.U16();e.area=r.I8();auto p=r.Vec();e.pos={p.x,p.y,p.z};e.relation=r.U32();e.look=r.U32();e.owner=from;e.alive=true;if(e.kind>AUDIT_MISSION||!e.id)return;batch.push_back(e);}
        if(!r.Ok())return;
        for(auto&e:batch){e.received=NowMs();auto&records=remote[from];if(records.size()<4096||records.count(((uint32_t)e.kind<<16)|e.id))records[((uint32_t)e.kind<<16)|e.id]=e;}
    }
public:
    const char*Name()const override{return "SyncDiagnostics";}
    void OnInstallHooks()override{
        Net().SetHandler(proto::G_SYNC_AUDIT,[](uint8_t f,Reader&r){Instance().Audit(f,r);});
        Net().SetHandler(proto::G_ENTITY_REQUEST,[](uint8_t f,Reader&r){
            auto owner=r.U8();auto kind=r.U8();auto id=r.U16();
            if(!r.Ok()||!Net().Peer(f)||owner!=Net().MyId()||!core::IsPlayerReady()||kind>AUDIT_MISSION)return;
            auto&self=Instance();auto key=0x80000000u|((uint32_t)f<<24)|((uint32_t)kind<<16)|id;
            auto now=NowMs();if(now-self.requests[key]<2000)return;self.requests[key]=now;
            if(kind==0)Vehicles::Instance().ReannounceOne(id);else if(kind==1)Peds::Instance().ReannounceOne(id);
            else core::RepairSyncEntity(kind,id);
        });
    }
    void OnSessionStart()override{
        OnSessionEnd();SyncTrace()={};auto dir=Cfg().coopDir+L"diagnostics\\";CreateDirectoryW(dir.c_str(),nullptr);
        SYSTEMTIME t;GetLocalTime(&t);wchar_t name[128];swprintf(name,128,L"sync-%04u%02u%02u-%02u%02u%02u-p%u-%lu.jsonl",t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond,Net().MyId(),GetCurrentProcessId());path=dir+name;
        // Bounded sessions and file rotation; never include configs, invite codes or voice audio.
        std::vector<std::filesystem::path> files;std::error_code ec;
        for(std::filesystem::directory_iterator it(dir,ec),end;!ec&&it!=end;it.increment(ec))if(it->is_regular_file(ec)&&it->path().filename().wstring().rfind(L"sync-",0)==0)files.push_back(it->path());
        std::sort(files.begin(),files.end());while(files.size()>10){std::filesystem::remove(files.front(),ec);files.erase(files.begin());}
        file=_wfopen(path.c_str(),L"wb");written=0;
        Write("{\"event\":\"session\",\"version\":\"" COOP_VERSION_STR "\",\"protocol\":"+std::to_string(kProtocolVersion)+",\"pid\":"+std::to_string(GetCurrentProcessId())+",\"peer\":"+std::to_string(Net().MyId())+",\"host\":"+std::to_string(Net().HostId())+",\"nickname\":\""+Escape(Cfg().nickname)+"\"}\n");
        if(file)fflush(file);last=0;seq=0;
    }
    void OnSessionEnd()override{if(file){fclose(file);file=nullptr;}OnGameShutdown();}
    void OnGameShutdown()override{remote.clear();warnings.clear();requests.clear();observations.clear();ownedAudit.clear();auditCursor=0;auditAt=0;last=0;}
    void OnPeerLeave(uint8_t id)override{remote.erase(id);}
    bool OnCommand(const std::string&cmd,const std::vector<std::string>&)override{if(cmd!="syncdiag")return false;last=0;core::Notify("Sync diagnostics: coop/diagnostics; /syncdiag records the current state");return true;}
    void OnPostProcess()override{
        if(!Net().Online()||!core::IsPlayerReady())return;
        // Spread the manifests across frames; rotate through every identity,
        // including entries past 320, rather than repeatedly sending a prefix.
        if(!ownedAudit.empty()&&NowMs()>=auditAt){
            auditAt=NowMs()+125;Writer w(proto::G_SYNC_AUDIT);
            size_t n=std::min<size_t>(20,ownedAudit.size()-auditCursor);w.U8((uint8_t)n);
            for(size_t i=0;i<n;++i){auto&e=ownedAudit[auditCursor+i];w.U8(e.kind);w.U16(e.id);w.U16(e.model);w.I8(e.area);w.Vec({e.pos.x,e.pos.y,e.pos.z});w.U32(e.relation);w.U32(e.look);}
            Net().Send(proto::CH_STATE,w);auditCursor=(auditCursor+n)%ownedAudit.size();
        }
        if(NowMs()-last<2000)return;last=NowMs();++seq;
        auto entries=Collect();auto*me=FindPlayerPed(0);char b[1000];
        for(int phase=0;phase<profile::PHASE_COUNT;++phase){auto&m=profile::Metrics()[phase];
            auto count=m.count.exchange(0),us=m.total.exchange(0),peak=m.maximum.exchange(0);
            snprintf(b,sizeof(b),"{\"event\":\"performance\",\"seq\":%llu,\"phase\":\"%s\",\"count\":%llu,\"total_us\":%llu,\"peak_us\":%llu,\"stale_packets\":%llu,\"trace_dropped\":%u}\n",(unsigned long long)seq,profile::Name(phase),(unsigned long long)count,(unsigned long long)us,(unsigned long long)peak,(unsigned long long)Net().StalePacketsDiscarded(),SyncTrace().dropped);Write(b);
        }
        SyncTraceEvent event;
        while(SyncTrace().Pop(event)){
            snprintf(b,sizeof(b),"{\"event\":\"%s\",\"ms\":%llu,\"peer\":%u,\"slot\":%u,\"token\":%u,\"asset\":%d,\"stage\":%u}\n",event.type,(unsigned long long)event.ms,event.peer,event.slot,event.token,event.asset,event.stage);Write(b);
        }
        PROCESS_MEMORY_COUNTERS_EX memory{};memory.cb=sizeof(memory);
        using MemoryFn=BOOL(WINAPI*)(HANDLE,PPROCESS_MEMORY_COUNTERS,DWORD);
        static auto memoryFn=(MemoryFn)GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"K32GetProcessMemoryInfo");
        if(memoryFn)memoryFn(GetCurrentProcess(),(PPROCESS_MEMORY_COUNTERS)&memory,sizeof(memory));
        snprintf(b,sizeof(b),"{\"event\":\"resources\",\"seq\":%llu,\"private_bytes\":%llu,\"working_set\":%llu,\"queued_in\":%llu,\"queued_out\":%llu,\"expired_packets\":%llu,\"entity_ref_rejects\":%llu,\"entity_ref_waiting\":%u}\n",(unsigned long long)seq,(unsigned long long)memory.PrivateUsage,(unsigned long long)memory.WorkingSetSize,(unsigned long long)Net().Net().QueuedInBytes(),(unsigned long long)Net().Net().QueuedOutBytes(),(unsigned long long)Net().QueuedPacketsDiscarded(),(unsigned long long)Net().EntityPacketsDiscarded(),(unsigned)Net().EntityPacketsWaiting());Write(b);
        FILETIME ft;GetSystemTimeAsFileTime(&ft);uint64_t utc=(((uint64_t)ft.dwHighDateTime<<32)|ft.dwLowDateTime)/10000-11644473600000ULL;
        snprintf(b,sizeof(b),"{\"event\":\"clock\",\"seq\":%llu,\"utc_ms\":%llu,\"ms\":%llu,\"controls_disabled\":%d}\n",(unsigned long long)seq,(unsigned long long)utc,(unsigned long long)last,GetPad(0)->DisablePlayerControls!=0);Write(b);
        snprintf(b,sizeof(b),"{\"event\":\"frame\",\"seq\":%llu,\"ms\":%llu,\"peer\":%u,\"host\":%u,\"rtt\":%u,\"area\":%d,\"pos\":[%.2f,%.2f,%.2f],\"bytes_in\":%llu,\"bytes_out\":%llu,\"mission\":%d}\n",(unsigned long long)seq,(unsigned long long)last,Net().MyId(),Net().HostId(),Net().Rtt(),me->AreaCode(),me->GetPosition().x,me->GetPosition().y,me->GetPosition().z,(unsigned long long)Net().Net().BytesIn(),(unsigned long long)Net().Net().BytesOut(),core::IsOnMission());Write(b);
        for(int kind=0;kind<2;++kind){auto*pool=kind?PedPool():VehiclePool();int free=0,unmapped=0;
            for(int i=0;pool&&i<pool->size;++i){if(pool->byteMap[i]<0){++free;continue;}
                CEntity*entity=nullptr;uint32_t relation=0;int handle=-1;
                if(kind){auto*p=(CPed*)(pool->objects+(size_t)i*kPedPoolElemSize);if(p->IsPlayer()||Players::Instance().FindByPed(p)||Peds::Instance().Find(p))continue;entity=p;handle=GetPedRef(p);}
                else{auto*v=(CVehicle*)(pool->objects+(size_t)i*kVehiclePoolElemSize);if(Vehicles::Instance().Find(v))continue;entity=v;handle=GetVehicleRef(v);relation=PedIdentity(v->Driver());}
                auto p=entity->GetPosition();++unmapped;
                snprintf(b,sizeof(b),"{\"event\":\"native_only\",\"seq\":%llu,\"peer\":%u,\"kind\":%d,\"handle\":%d,\"model\":%d,\"area\":%d,\"pos\":[%.2f,%.2f,%.2f],\"relation\":%u}\n",(unsigned long long)seq,Net().MyId(),kind,handle,entity->ModelIndex(),entity->AreaCode(),p.x,p.y,p.z,relation);Write(b);
            }
            snprintf(b,sizeof(b),"{\"event\":\"pool\",\"seq\":%llu,\"peer\":%u,\"kind\":%d,\"capacity\":%d,\"free\":%d,\"unmapped\":%d,\"shared_source_only\":%d}\n",(unsigned long long)seq,Net().MyId(),kind,pool?pool->size:0,free,unmapped,NearbyPopulationShare().active);Write(b);
        }
        std::map<uint32_t,Entry> local;
        for(auto&e:entries){local[((uint32_t)e.kind<<16)|e.id]=e;
            snprintf(b,sizeof(b),"{\"event\":\"entity\",\"seq\":%llu,\"ms\":%llu,\"peer\":%u,\"kind\":%u,\"id\":%u,\"owner\":%u,\"model\":%u,\"alive\":%d,\"area\":%d,\"pos\":[%.2f,%.2f,%.2f],\"relation\":%u,\"look\":%u}\n",(unsigned long long)seq,(unsigned long long)last,Net().MyId(),e.kind,e.id,e.owner,e.model,e.alive,e.area,e.pos.x,e.pos.y,e.pos.z,e.relation,e.look);Write(b);
        }
        for(auto&[id,n]:Vehicles::Instance().All())if(Vehicles::Alive(n)){
            auto*v=n.veh;auto*driver=v->Driver();
            snprintf(b,sizeof(b),"{\"event\":\"vehicle_identity\",\"seq\":%llu,\"id\":%u,\"owner\":%u,\"native\":%d,\"generation\":%llu,\"script\":%d,\"mirror_created\":%d,\"lease_ms\":%llu}\n",(unsigned long long)seq,id,n.owner,n.handle,(unsigned long long)Net().EntityGeneration(ENTITY_VEHICLE,id),n.script,n.createdByUs,(unsigned long long)(n.handoverUntil>last?n.handoverUntil-last:0));Write(b);
            auto wheels=WheelState::Capture(v);
            auto damageCrc=Crc32(n.look.damage,sizeof(n.look.damage));
            damageCrc=Crc32(n.look.bikeTyres,sizeof(n.look.bikeTyres),damageCrc);
            snprintf(b,sizeof(b),"{\"event\":\"vehicle_damage\",\"seq\":%llu,\"id\":%u,\"owner\":%u,\"repair_revision\":%u,\"crc32\":%u}\n",(unsigned long long)seq,id,n.owner,n.repairRevision,damageCrc);Write(b);
            if(wheels.count){snprintf(b,sizeof(b),"{\"event\":\"vehicle_wheels\",\"seq\":%llu,\"id\":%u,\"owner\":%u,\"handbrake\":%d,\"brake\":%.3f,\"states\":%u,\"contact\":%u,\"phase\":[%.3f,%.3f,%.3f,%.3f],\"rate\":[%.3f,%.3f,%.3f,%.3f]}\n",(unsigned long long)seq,id,n.owner,v->HandbrakeOn(),v->BrakePedal(),wheels.states,wheels.contact,wheels.phase[0],wheels.phase[1],wheels.phase[2],wheels.phase[3],wheels.rate[0],wheels.rate[1],wheels.rate[2],wheels.rate[3]);Write(b);}
            snprintf(b,sizeof(b),"{\"event\":\"vehicle_physics\",\"seq\":%llu,\"peer\":%u,\"id\":%u,\"owner\":%u,\"status\":%u,\"created_by\":%u,\"engine\":%d,\"flags\":%u,\"physical\":%u,\"speed\":%.3f,\"gas\":%.3f,\"health\":%.1f,\"driver\":%u,\"driver_health\":%.1f,\"state_age_ms\":%llu,\"held\":%d,\"recent\":%d}\n",(unsigned long long)seq,Net().MyId(),id,n.owner,v->Status(),v->CreatedBy(),v->EngineOn(),v->VehicleFlagsHi(),v->PhysicalFlags(),v->MoveSpeed().Length()*50,v->GasPedal(),v->Health(),PedIdentity(driver),driver&&IsPedValid(driver)?driver->Health():0.f,(unsigned long long)(n.hasState?last-n.recvMs:0),n.physicsHeld,n.playerUsed);Write(b);
        }
        for(auto&[id,n]:Peds::Instance().All())if(n.owner!=Net().MyId()&&n.hasState&&(!n.ped||!IsPedValid(n.ped)||GetPedRef(n.ped)!=n.handle)){
            auto*v=(n.state.flags&PedSyncState::F_IN_VEHICLE)?Vehicles::Instance().Find(n.state.vehNetId):nullptr;
            const char*reason=!IsModelLoaded(n.model)?"model_not_loaded":(n.state.flags&PedSyncState::F_IN_VEHICLE)?((!v||!Vehicles::Alive(*v))?"vehicle_missing":n.state.seat<0&&v->veh->Driver()?"driver_seat_occupied":"awaiting_spawn"):"awaiting_spawn";
            auto*tasks=Ref<CPoolHeader*>(0xB744A8);int free=0;for(int i=0;tasks&&i<tasks->size;++i)free+=tasks->byteMap[i]<0;
            snprintf(b,sizeof(b),"{\"event\":\"ped_pending\",\"seq\":%llu,\"id\":%u,\"vehicle\":%u,\"reason\":\"%s\",\"task_free\":%d}\n",(unsigned long long)seq,id,n.state.vehNetId,reason,free);Write(b);
        }
        snprintf(b,sizeof(b),"{\"event\":\"population\",\"seq\":%llu,\"source\":%u,\"suppressed\":%d,\"ped_density\":%.2f,\"car_density\":%.2f,\"random_cars\":%d,\"mission_cars\":%d,\"parked_cars\":%u,\"native_ped_count\":%u}\n",(unsigned long long)seq,NearbyPopulationShare().active?NearbyPopulationShare().source:Net().MyId(),NearbyPopulationShare().active,Ref<float>(0x8D2530),Ref<float>(0x8A5B20),Ref<int32_t>(0x969094),Ref<int32_t>(0x96909C),Ref<uint32_t>(0x9690A0),Ref<uint32_t>(0xC0EC28));Write(b);
        for(uint8_t id=0;id<3;++id)if(id==Net().MyId()||Net().Peer(id)){
            auto*rp=Players::Instance().Get(id);CPed*ped=id==Net().MyId()?me:rp?rp->ped:nullptr;
            if(ped){snprintf(b,sizeof(b),"{\"event\":\"player_melee\",\"seq\":%llu,\"player\":%u,\"style\":%u,\"allowed_moves\":%u}\n",(unsigned long long)seq,id,ped->FightingStyle(),ped->AllowedAttackMoves());Write(b);}
            snprintf(b,sizeof(b),"{\"event\":\"activity\",\"seq\":%llu,\"player\":%u,\"school\":%d,\"population_available\":%d}\n",(unsigned long long)seq,id,PlayerTraining(id),PopulationAvailable(id));Write(b);
        }
        snprintf(b,sizeof(b),"{\"event\":\"generation_budget\",\"seq\":%llu,\"excluded_remote_cars\":%d}\n",(unsigned long long)seq,PopulationBudgetExcluded());Write(b);
        ownedAudit.clear();for(auto&e:entries)if(e.alive&&e.owner==Net().MyId())ownedAudit.push_back(e);
        auditCursor=ownedAudit.empty()?0:auditCursor%ownedAudit.size();
        int asked=0;
        for(auto&[source,records]:remote)for(auto&[key,e]:records){
            if(last-e.received>10000)continue;
            auto it=local.find(key);bool inRange=AuditSet(e.kind)||((e.area==me->AreaCode()||e.area==13)&&(e.pos-me->GetPosition()).Length2D()<160);
            if(it==local.end()){
                if(inRange){Warn("missing_record",e,source);Request(e,source,asked);}
                continue;
            }
            auto&mine=it->second;
            if(mine.owner!=source){Warn("owner_mismatch",e,source);continue;}
            if(!inRange)continue;
            if(!mine.alive){Warn("waiting_native_instance",e,source);if(e.kind>1)Request(e,source,asked);}
            else if(mine.model!=e.model)Warn("model_mismatch",e,source);
            else if(!e.kind&&mine.relation!=e.relation)Warn("driver_mismatch",e,source);
            else if(!e.kind&&mine.look!=e.look)Warn("appearance_mismatch",e,source);
            else if(e.kind>1&&(mine.look!=e.look||mine.relation!=e.relation)){Warn("content_mismatch",e,source);Request(e,source,asked);}
        }
        for(auto it=observations.begin();it!=observations.end();)it=it->second.second==last?std::next(it):observations.erase(it);
        for(auto&[source,records]:remote)for(auto it=records.begin();it!=records.end();)it=last-it->second.received>10000?records.erase(it):std::next(it);
        for(auto*map:{&requests,&warnings})for(auto i=map->begin();i!=map->end();)i=last-i->second>60000?map->erase(i):std::next(i);
        if(file)fflush(file);
    }
    void Request(const Entry&e,uint8_t source,int&asked){
        uint32_t key=((uint32_t)source<<24)|((uint32_t)e.kind<<16)|e.id;
        auto it=observations.find(key);
        if(asked>=4||it==observations.end()||e.received-it->second.first<3000||last-requests[key]<5000)return;
        requests[key]=last;Writer w(proto::G_ENTITY_REQUEST);w.U8(source);w.U8(e.kind);w.U16(e.id);
        Net().Send(proto::CH_RELIABLE,w);++asked;
        LOGI("Sync repair requested: kind=%u id=%04X owner=%u",e.kind,e.id,source);
    }
    static Diagnostics&Instance(){static Diagnostics d;return d;}
};
struct Reg{Reg(){core::RegisterModule(&Diagnostics::Instance());}}reg;
}
}
