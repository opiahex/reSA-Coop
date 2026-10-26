#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <vector>
#include "common/api/protocol/Protocol.h"
#include "shared/api/story-loadout/StoryLoadout.h"
#include "shared/api/story-coop/StoryCoop.h"
#include "shared/api/story-stage/StoryStage.h"
#include "shared/api/story-scripts/StoryScripts.h"
#include "shared/api/net-session/Session.h"
#include "shared/game/Script.h"
#include "shared/lib/core/Core.h"
#include "entities/session/Flow.h"
#include "entities/mission/Mission.h"
#include "entities/profile/Profile.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
namespace coop { namespace {
using namespace game;
using game::script::Cmd;
enum : uint8_t { GIVE=1,TAKE=2,FINAL=3 };
struct Loan {
    uint64_t run=0;
    int handle=-1,slot=-1,weapon=0,original=0,originalAmmo=0,originalActive=0;
    int personal=0,borrowed=0,lastAmmo=0;
    bool nativeOnly=false,selectedByStory=false;
};
std::map<int,Loan> loans;
struct WeaponAmmo {int type=0,ammo=0;};
struct NativeInventory {
    int handle=-1,weapon=-1,slot=-1;
    uint64_t run=0;
    bool pendingLoan=false,attached=false;
    Loan loan;
    std::array<WeaponAmmo,NUM_WEAPON_SLOTS> before;
};
std::map<uint32_t,NativeInventory> nativeInventory;
uint32_t nativeToken=0;
unsigned nativeDepth=0;
struct InventoryStrip {
    uint32_t world=0,phase=0,sequence=0,incarnation=0,localWorld=0;
    uint64_t run=0;
    uint8_t host=255,target=255;
};
InventoryStrip pendingStrip,lastStrip;
struct DeferredWeapon {uint8_t operation=0;int weapon=0,ammo=0;bool inHand=false;};
std::vector<DeferredWeapon> stripWeapons;
CPlayerPed* Local(){return core::IsPlayerReady()?FindPlayerPed(0):nullptr;}
enum : uint8_t { EQUIPMENT_PARACHUTE=1,EQUIPMENT_VISION=2 };
struct EquipmentState {
    uint64_t run=0;
    uint32_t world=0,localWorld=0,phase=0,sequence=0,incarnation=0;
    uint8_t kind=0,value=0,target=255,host=255;
    int8_t area=0;
    CVector position;
};
EquipmentState missionVision,pendingParachute;
uint64_t equipmentRun=0,visionSent=0,visionReceived=0;
uint32_t equipmentSequence=0,chuteSeen=0,visionDeclined=0;
bool visionBorrowed=false,applyingVision=false;
uint8_t savedVision=0,appliedVision=0;
uint8_t NativeVision(){return Ref<uint8_t>(0xC402B9)?2:Ref<uint8_t>(0xC402B8)?1:0;}
void SetVision(uint8_t value){
    applyingVision=true;
    Call<0x701120>(false);Call<0x701140>(false);
    if(value==1)Call<0x701120>(true);else if(value==2)Call<0x701140>(true);
    applyingVision=false;
}
void RestoreVision(){
    if(visionBorrowed&&NativeVision()==appliedVision)SetVision(savedVision);
    visionBorrowed=false;
}
void ClearSpecialEquipment(bool restore){
    if(restore)RestoreVision();else visionBorrowed=false;
    missionVision=pendingParachute={};equipmentRun=visionSent=visionReceived=0;
    equipmentSequence=chuteSeen=visionDeclined=0;
}
void SendEquipment(EquipmentState state,uint8_t target=255){
    if(!Net().IsHost()||!state.run||state.run!=StoryRunId())return;
    for(const auto&[id,peer]:Net().Peers()){
        if((target!=255&&target!=id)||!peer.incarnation||!CooperativeInventoryParticipant(id))continue;
        Writer w(proto::G_PLAYER_EXTRAS);w.U8(3);w.U32((uint32_t)NowMs());
        w.U8(state.kind);w.U32(Net().WorldEpoch());w.U64(state.run);w.U32(state.phase);w.U32(state.sequence);
        w.U8(id);w.U32(peer.incarnation);w.U8(state.value);w.I8(state.area);
        w.Vec({state.position.x,state.position.y,state.position.z});Net().Send(proto::CH_RELIABLE,w);
    }
}
bool EquipmentContext(const EquipmentState&state){
    return Net().Online()&&!Net().IsHost()&&state.host==Net().HostId()&&state.target==Net().MyId()&&
        state.world&&state.world==Net().PeerWorldEpoch(state.host)&&state.localWorld==Net().WorldEpoch()&&
        state.run&&state.run==StoryRunId()&&state.incarnation&&state.incarnation==Net().MyIncarnation();
}
void UpdateSpecialEquipment(){
    auto*me=Local();const auto run=StoryRunId();
    if(equipmentRun&&equipmentRun!=run)ClearSpecialEquipment(me!=nullptr);
    if(Net().IsHost()){
        if(!missionVision.run||!me)return;
        if(missionVision.phase!=CooperativeStoryPhase()){
            missionVision.phase=CooperativeStoryPhase();
        }
        if(NowMs()-visionSent>=1000){SendEquipment(missionVision);visionSent=NowMs();}
        return;
    }
    if(pendingParachute.run){
        if(!EquipmentContext(pendingParachute)||pendingParachute.phase<CooperativeStoryPhase())pendingParachute={};
        else if(pendingParachute.phase==CooperativeStoryPhase()&&me&&me->Health()>0&&flow::InHostWorld()&&CooperativeStoryInventoryReady()){
            // Native MANSIO1 selects a parachute even when CJ already owns it
            // and therefore skips GIVE. Each guest has an independent slot.
            int slot=-1;Cmd(0x0782).I(46).Out(&slot).Run();
            if(slot>=0&&slot<NUM_WEAPON_SLOTS&&(me->Weapons()[slot].type!=46||me->Weapons()[slot].ammoTotal<=0)){
                const int selected=me->ActiveWeapon().type,selectedSlot=me->ActiveWeaponSlot();
                ApplyStoryMissionWeapon(GIVE,46,1,false);
                // Native GIVE selects46 even though this event only exposes
                // equipment. Keep the guest's own still-present selection.
                const bool retained=selectedSlot>=0&&selectedSlot<NUM_WEAPON_SLOTS&&me->Weapons()[selectedSlot].type==selected;
                // CPed's native setter restores both active/chosen slots now;
                // player01B9 alone only changes the next requested slot.
                CallMethod<0x5E61F0>(me,retained?selectedSlot:0);
                LOGI("Story parachute available selection=%d restored=%d run=%llu",selected,retained?selected:0,(unsigned long long)pendingParachute.run);
            }
            pendingParachute={};
        }
    }
    const bool applies=me&&me->Health()>0&&EquipmentContext(missionVision)&&missionVision.value&&
        missionVision.phase==CooperativeStoryPhase()&&NowMs()-visionReceived<3000&&flow::InHostWorld()&&
        !LocalStoryPresentationBlocked()&&me->AreaCode()==missionVision.area&&
        (me->GetPosition()-missionVision.position).Length()<160&&visionDeclined!=missionVision.sequence;
    if(!applies){RestoreVision();return;}
    if(visionBorrowed&&NativeVision()!=appliedVision){
        visionBorrowed=false;visionDeclined=missionVision.sequence;return;
    }
    if(!visionBorrowed){
        // Personal goggles remain a local choice. Only borrow an idle view;
        // native toggles and later source OFF release this individual loan.
        if(NativeVision()){visionDeclined=missionVision.sequence;return;}
        savedVision=NativeVision();appliedVision=missionVision.value;visionBorrowed=true;SetVision(appliedVision);
    }
}
int ClampAmmo(int ammo){return std::clamp(ammo,0,99999);}
bool Current(const Loan&loan,CPed*me){return me&&me->Health()>0&&GetPedRef(me)==loan.handle&&loan.slot>=0&&loan.slot<NUM_WEAPON_SLOTS&&me->Weapons()[loan.slot].type==loan.weapon;}
void Sample(Loan&loan,CPed*me){
    if(nativeDepth||!Current(loan,me))return;
    int count=ClampAmmo(me->Weapons()[loan.slot].ammoTotal),delta=count-loan.lastAmmo;
    // Native pickups acquired during the episode belong to this player.
    // Consume the mission supply before touching the player's own ammunition.
    if(delta>0)loan.personal=ClampAmmo(loan.personal+delta);
    else if(delta<0){int used=-delta,borrowed=std::min(loan.borrowed,used);loan.borrowed-=borrowed;loan.personal=std::max(0,loan.personal-(used-borrowed));}
    loan.lastAmmo=count;
}
void Give(CPed*me,int weapon,int ammo){
    int model=-1;Cmd(0x0781).I(weapon).Out(&model).Run();
    if(model>0)LoadModelNow(model);
    Cmd(0x01B2).I(GetPedRef(me)).I(weapon).I(std::max(1,ClampAmmo(ammo))).Run();
    Cmd(0x017B).I(GetPedRef(me)).I(weapon).I(ClampAmmo(ammo)).Run();
}
void Withdraw(const Loan&loan,CPed*me){
    // A different weapon picked up by the player wins over this old loan.
    if(!Current(loan,me))return;
    const bool selected=me->ActiveWeapon().type==loan.weapon;
    Cmd(0x0555).I(loan.handle).I(loan.weapon).Run();
    int weapon=loan.original,ammo=loan.original==loan.weapon?loan.personal:loan.originalAmmo;
    // If the borrowed weapon occupied an empty slot, retain ammunition later
    // picked up for that weapon instead of deleting the player's pickup.
    if(!weapon&&loan.personal>0){weapon=loan.weapon;ammo=loan.personal;}
    if(weapon>0){Give(me,weapon,ammo);if(selected)Cmd(0x01B9).I(loan.handle).I(weapon).Run();}
}
void SampleAll(){if(nativeDepth)return;auto*me=Local();for(auto i=loans.begin();i!=loans.end();){if(!Current(i->second,me))i=loans.erase(i);else{Sample(i->second,me);++i;}}}
uint32_t BeginNativeInventory(int weapon){
    auto*me=Local();if(!me||me->Health()<=0||Net().IsHost())return 0;
    if(weapon>=0&&(!StoryRunId()||!Net().Online()||!flow::InHostWorld()))return 0;
    SampleAll();NativeInventory state;state.handle=GetPedRef(me);state.run=StoryRunId();state.weapon=weapon;state.attached=me->AttachedTo()!=nullptr;
    for(int slot=0;slot<NUM_WEAPON_SLOTS;++slot)state.before[slot]={me->Weapons()[slot].type,ClampAmmo(me->Weapons()[slot].ammoTotal)};
    if(weapon>0&&weapon<=46){
        Cmd(0x0782).I(weapon).Out(&state.slot).Run();
        if(state.slot>=0&&state.slot<NUM_WEAPON_SLOTS){
            auto found=loans.find(state.slot);
            if(found==loans.end()||found->second.weapon!=weapon){
                int original=state.before[state.slot].type,ammo=state.before[state.slot].ammo;
                PersonalStoryWeapon(state.slot,original,ammo);
                auto&loan=state.loan;loan.run=state.run;loan.handle=state.handle;loan.slot=state.slot;loan.weapon=weapon;
                loan.original=original;loan.originalAmmo=ammo;loan.personal=original==weapon?ammo:0;loan.nativeOnly=true;
                loan.originalActive=PersonalStoryWeaponSelection();
                state.pendingLoan=true;
            }
        }
    }
    do{++nativeToken;}while(!nativeToken||nativeInventory.count(nativeToken));
    nativeInventory.emplace(nativeToken,std::move(state));++nativeDepth;return nativeToken;
}
void RestoreNativeInventory(const NativeInventory&state,CPed*me){
    const int selected=me->ActiveWeapon().type;
    for(int slot=0;slot<NUM_WEAPON_SLOTS;++slot){
        const auto&saved=state.before[slot];const auto&now=me->Weapons()[slot];
        if(now.type!=saved.type){
            if(now.type>0&&now.type<=46)Cmd(0x0555).I(state.handle).I(now.type).Run();
            if(saved.type>0&&saved.type<=46)Give(me,saved.type,saved.ammo);
        }else if(saved.type>0&&saved.type<=46&&ClampAmmo(now.ammoTotal)!=saved.ammo)
            Cmd(0x017B).I(state.handle).I(saved.type).I(saved.ammo).Run();
    }
    if(selected>=0&&selected<=46)Cmd(0x01B9).I(state.handle).I(selected).Run();
}
void EndNativeInventory(uint32_t token){
    auto found=nativeInventory.find(token);if(found==nativeInventory.end())return;
    auto state=std::move(found->second);nativeInventory.erase(found);
    auto*me=Local();
    if(me&&me->Health()>0&&GetPedRef(me)==state.handle&&StoryRunId()==state.run){
        if(state.weapon>=0){
            if(me->AttachedTo()&&state.slot>=0&&state.slot<NUM_WEAPON_SLOTS&&me->Weapons()[state.slot].type==state.weapon){
                if(state.pendingLoan)loans[state.slot]=state.loan;
                auto loan=loans.find(state.slot);
                if(loan!=loans.end()&&loan->second.weapon==state.weapon){
                    auto&value=loan->second;value.lastAmmo=ClampAmmo(me->Weapons()[state.slot].ammoTotal);
                    value.borrowed=std::max(0,value.lastAmmo-value.personal);
                    value.selectedByStory=true;
                    LOGD("Story native attachment inventory tracked type=%d personal=%d borrowed=%d",value.weapon,value.personal,value.borrowed);
                }
            }
        }else{
            // Detach zeros the current weapon and restores the native saved
            // active weapon's old count. Preserve the real inventory sampled
            // just before that call, including personal pickups and spent ammo.
            RestoreNativeInventory(state,me);
            if(state.attached&&!me->AttachedTo())for(auto i=loans.begin();i!=loans.end();){
                if(i->second.nativeOnly){auto loan=i->second;i=loans.erase(i);Withdraw(loan,me);}
                else ++i;
            }
        }
    }
    if(nativeDepth)--nativeDepth;
}
const char* StripContextFailure(const InventoryStrip&strip,bool accepted){
    if(!Net().Online()||Net().IsHost())return "session";
    if(strip.host!=Net().HostId())return "host";
    if(strip.target!=Net().MyId())return "target";
    // Each client advances its own world counter when loading. The packet's
    // world belongs to its sender; transport has already accepted that epoch.
    if(!strip.world||strip.world!=Net().PeerWorldEpoch(strip.host))return "sender-world";
    if(accepted&&strip.localWorld!=Net().WorldEpoch())return "receiver-world";
    if(!strip.run||strip.run!=StoryRunId())return "run";
    // Once accepted in order, a permanent loss still has to apply after a
    // later phase if the personal profile was not ready at receipt time.
    if(!strip.phase||(!accepted&&strip.phase!=CooperativeStoryPhase()))return "phase";
    if(!strip.sequence)return "sequence";
    if(!strip.incarnation||strip.incarnation!=Net().MyIncarnation())return "connection";
    return nullptr;
}
void LogStripContext(const char*action,const InventoryStrip&strip,const char*reason){
    LOGD("Story inventory strip %s reason=%s from=%u host=%u target=%u local=%u world=%u senderWorld=%u receiverWorld=%u acceptedWorld=%u run=%llu currentRun=%llu phase=%u currentPhase=%u sequence=%u connection=%u localConnection=%u",
        action,reason,strip.host,Net().HostId(),strip.target,Net().MyId(),strip.world,Net().PeerWorldEpoch(strip.host),
        Net().WorldEpoch(),strip.localWorld,(unsigned long long)strip.run,(unsigned long long)StoryRunId(),
        strip.phase,CooperativeStoryPhase(),strip.sequence,strip.incarnation,Net().MyIncarnation());
}
bool FlushInventoryStrip(){
    if(!pendingStrip.sequence)return true;
    if(const char*reason=StripContextFailure(pendingStrip,true)){
        LogStripContext("expired",pendingStrip,reason);
        pendingStrip={};stripWeapons.clear();return true;
    }
    auto*me=Local();
    // The profile is applied over the host world before this permanent loss;
    // otherwise its old inventory could put the stripped weapons back.
    if(!me||!flow::InHostWorld()||!CooperativeStoryInventoryReady())return false;
    if(me->Health()<=0){pendingStrip={};stripWeapons.clear();return true;}
    auto strip=pendingStrip;pendingStrip={};auto weapons=std::move(stripWeapons);stripWeapons.clear();
    // Returning a loan or a still-unwinding native attachment scope must not
    // resurrect inventory which the screenplay has explicitly destroyed.
    loans.clear();nativeInventory.clear();nativeDepth=0;
    Cmd(0x048F).I(GetPedRef(me)).Run();
    lastStrip=strip;
    SaveProfileNow(true);
    LOGI("Story inventory stripped world=%u run=%llu phase=%u sequence=%u player=%u connection=%u",
        strip.world,(unsigned long long)strip.run,strip.phase,strip.sequence,strip.target,strip.incarnation);
    for(const auto&weapon:weapons)ApplyStoryMissionWeapon(weapon.operation,weapon.weapon,weapon.ammo,weapon.inHand);
    return true;
}
class Loadout:public Module {
    const char*Name()const override{return "StoryLoadout";}
    void OnPostProcess()override{
        UpdateSpecialEquipment();
        if(!FlushInventoryStrip())return;
        if(loans.empty())return;
        SampleAll();
        for(auto&[slot,loan]:loans)if(loan.run!=StoryRunId()){RestoreStoryMissionWeapons();break;}
    }
    void OnPeerResync(uint8_t id)override{
        if(!Net().IsHost())return;
        if(missionVision.run)SendEquipment(missionVision,id);
    }
    void OnGameShutdown()override{ClearSpecialEquipment(false);loans.clear();nativeInventory.clear();nativeDepth=0;pendingStrip={};stripWeapons.clear();}
    void OnSessionEnd()override{ClearSpecialEquipment(core::IsPlayerReady());pendingStrip=lastStrip={};stripWeapons.clear();RestoreStoryMissionWeapons();}
} feature;
struct Register{Register(){core::RegisterModule(&feature);}}registration;
}
void ObserveStoryEquipmentCommand(void*script,int opcode,const int32_t*parameters){
    auto*me=Local();
    if(!parameters||!Net().Online()||!Net().IsHost()||!me||!StoryRunId()||
       !IsSharedStoryScript(script)||LocalStoryPresentationBlocked())return;
    if(equipmentRun!=StoryRunId())ClearSpecialEquipment(false);
    equipmentRun=StoryRunId();
    if(opcode==0x01B9&&parameters[0]==GetPedRef(me)&&parameters[1]==46){
        EquipmentState state;state.run=equipmentRun;state.phase=CooperativeStoryPhase();state.sequence=++equipmentSequence;
        state.kind=EQUIPMENT_PARACHUTE;state.value=1;state.area=me->AreaCode();state.position=me->GetPosition();
        SendEquipment(state);
        LOGI("Story parachute availability run=%llu phase=%u sequence=%u",(unsigned long long)state.run,state.phase,state.sequence);
    }else if(opcode==0x08B1||opcode==0x08B2){
        const uint8_t kind=opcode==0x08B1?1:2;
        const uint8_t value=parameters[0]?kind:missionVision.value==kind?0:missionVision.value;
        RestoreVision();missionVision={};missionVision.run=equipmentRun;missionVision.kind=EQUIPMENT_VISION;
        missionVision.phase=CooperativeStoryPhase();missionVision.sequence=++equipmentSequence;
        missionVision.value=value;missionVision.area=me->AreaCode();missionVision.position=me->GetPosition();
        SendEquipment(missionVision);visionSent=NowMs();
        LOGI("Story vision command opcode=%04X run=%llu phase=%u mode=%u",opcode,(unsigned long long)equipmentRun,missionVision.phase,missionVision.value);
    }
}
void HandleStoryEquipmentEvent(uint8_t from,Reader&r){
    EquipmentState state;state.kind=r.U8();state.world=r.U32();state.run=r.U64();state.phase=r.U32();state.sequence=r.U32();
    state.target=r.U8();state.incarnation=r.U32();state.value=r.U8();state.area=r.I8();auto p=r.Vec();
    state.position={p.x,p.y,p.z};state.host=from;state.localWorld=Net().WorldEpoch();
    if(!r.Ok()||!state.phase||state.phase!=CooperativeStoryPhase()||!state.sequence||state.kind<EQUIPMENT_PARACHUTE||state.kind>EQUIPMENT_VISION||
       state.value>2||(state.kind==EQUIPMENT_PARACHUTE&&state.value!=1)||state.area<0||state.area>18||
       !std::isfinite(p.x)||!std::isfinite(p.y)||!std::isfinite(p.z)||state.position.Length()>25000||!EquipmentContext(state))return;
    if(equipmentRun!=state.run)ClearSpecialEquipment(core::IsPlayerReady());
    equipmentRun=state.run;
    if(state.kind==EQUIPMENT_PARACHUTE){
        if(chuteSeen&&(int32_t)(state.sequence-chuteSeen)<=0)return;
        chuteSeen=state.sequence;pendingParachute=state;
    }else{
        if(missionVision.sequence&&(int32_t)(state.sequence-missionVision.sequence)<0)return;
        if(visionBorrowed&&state.value!=missionVision.value)RestoreVision();
        missionVision=state;visionReceived=NowMs();
    }
}
void ObserveLocalStoryVision(){
    if(applyingVision||Net().IsHost()||PlayerInFocus()!=0)return;
    visionBorrowed=false;visionDeclined=missionVision.sequence;
}
void ApplyStoryMissionWeapon(uint8_t operation,int weapon,int ammo,bool inHand){
    if(operation<GIVE||operation>FINAL||weapon<=0||weapon>46)return;
    auto*me=Local();if(!me||me->Health()<=0||Net().IsHost()||!flow::InHostWorld())return;
    if(!FlushInventoryStrip()){
        if(stripWeapons.size()<64)stripWeapons.push_back({operation,weapon,ammo,inHand});
        else LOGW("Story inventory strip deferred weapon limit reached");
        return;
    }
    SampleAll();
    int slot=-1;Cmd(0x0782).I(weapon).Out(&slot).Run();if(slot<0||slot>=NUM_WEAPON_SLOTS)return;
    auto found=loans.find(slot);
    if(operation==TAKE||operation==FINAL){
        if(found==loans.end()||found->second.weapon!=weapon)return;
        auto loan=found->second;loans.erase(found);
        if(operation==TAKE||ammo<0){Withdraw(loan,me);LOGI("Story weapon loan returned type=%d run=%llu",weapon,(unsigned long long)loan.run);return;}
        // The host still owns this weapon after native cleanup: it is a kept
        // story award. Its final supply bounds only the borrowed portion;
        // personal ammunition and pickups are not overwritten by host totals.
        if(Current(loan,me)){
            int count=ClampAmmo(loan.personal+std::min(loan.borrowed,ClampAmmo(ammo)));
            Cmd(0x017B).I(loan.handle).I(weapon).I(count).Run();
            LOGI("Story weapon award settled type=%d ammo=%d run=%llu",weapon,count,(unsigned long long)loan.run);
        }
        return;
    }
    const int originalActive=PersonalStoryWeaponSelection();
    if(found!=loans.end()&&found->second.weapon!=weapon){auto loan=found->second;loans.erase(found);Withdraw(loan,me);found=loans.end();}
    if(found==loans.end()){
        const auto&old=me->Weapons()[slot];Loan loan;loan.run=StoryRunId();loan.handle=GetPedRef(me);loan.slot=slot;loan.weapon=weapon;loan.original=old.type;loan.originalAmmo=ClampAmmo(old.ammoTotal);loan.personal=old.type==weapon?loan.originalAmmo:0;loan.originalActive=originalActive;
        found=loans.emplace(slot,loan).first;
    }
    auto&loan=found->second;loan.nativeOnly=false;
    int count=std::max(loan.personal,ClampAmmo(ammo));
    Give(me,weapon,count);loan.borrowed=std::max(0,count-loan.personal);loan.lastAmmo=count;
    if(inHand){loan.selectedByStory=true;Cmd(0x01B9).I(loan.handle).I(weapon).Run();}
}
void ApplyStoryMissionStrip(uint8_t from,uint32_t world,uint64_t run,uint32_t phase,
                            uint32_t sequence,uint8_t target,uint32_t targetIncarnation){
    InventoryStrip strip;strip.world=world;strip.phase=phase;strip.sequence=sequence;strip.incarnation=targetIncarnation;
    strip.localWorld=Net().WorldEpoch();strip.run=run;strip.host=from;strip.target=target;
    if(const char*reason=StripContextFailure(strip,false)){
        LogStripContext("rejected",strip,reason);
        return;
    }
    auto seen=[&](const InventoryStrip&prior){return prior.sequence&&prior.host==from&&prior.world==world&&prior.run==run&&prior.incarnation==targetIncarnation&&
        (int32_t)(sequence-prior.sequence)<=0;};
    if(seen(lastStrip)||seen(pendingStrip)){
        LOGD("Story inventory strip replay rejected run=%llu phase=%u sequence=%u",(unsigned long long)run,phase,sequence);
        return;
    }
    pendingStrip=strip;stripWeapons.clear();
    if(!FlushInventoryStrip())LOGD("Story inventory strip queued run=%llu phase=%u sequence=%u ready=%d inHostWorld=%d",
        (unsigned long long)run,phase,sequence,CooperativeStoryInventoryReady(),flow::InHostWorld());
}
void RestoreStoryMissionWeapons(){auto*me=Local();for(auto&[slot,loan]:loans){Sample(loan,me);Withdraw(loan,me);}loans.clear();}
void PersonalStoryWeapon(int slot,int&weapon,int&ammo){
    auto found=loans.find(slot);if(found==loans.end())return;
    auto*me=Local();auto&loan=found->second;if(!Current(loan,me))return;Sample(loan,me);
    weapon=loan.original;ammo=loan.original==loan.weapon?loan.personal:loan.originalAmmo;
    if(!weapon&&loan.personal>0){weapon=loan.weapon;ammo=loan.personal;}
}
int PersonalStoryWeaponSelection(){
    auto*me=Local();if(!me)return 0;
    auto found=loans.find(me->ActiveWeaponSlot());
    if(found!=loans.end()&&Current(found->second,me)&&found->second.selectedByStory)
        return found->second.originalActive;
    return me->ActiveWeapon().type;
}
StoryNativeInventoryScope::StoryNativeInventoryScope():token(BeginNativeInventory(-1)){}
StoryNativeInventoryScope::StoryNativeInventoryScope(int weapon):token(BeginNativeInventory(weapon)){}
StoryNativeInventoryScope::~StoryNativeInventoryScope(){if(token)EndNativeInventory(token);}
}
