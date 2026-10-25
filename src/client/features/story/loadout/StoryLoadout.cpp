#include <algorithm>
#include <array>
#include <map>
#include "shared/api/story-loadout/StoryLoadout.h"
#include "shared/api/story-coop/StoryCoop.h"
#include "shared/api/net-session/Session.h"
#include "shared/game/Script.h"
#include "shared/lib/core/Core.h"
#include "entities/session/Flow.h"
#include "common/lib/log/Log.h"
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
CPlayerPed* Local(){return core::IsPlayerReady()?FindPlayerPed(0):nullptr;}
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
class Loadout:public Module {
    const char*Name()const override{return "StoryLoadout";}
    void OnPostProcess()override{
        if(loans.empty())return;
        SampleAll();
        for(auto&[slot,loan]:loans)if(loan.run!=StoryRunId()){RestoreStoryMissionWeapons();break;}
    }
    void OnGameShutdown()override{loans.clear();nativeInventory.clear();nativeDepth=0;}
    void OnSessionEnd()override{RestoreStoryMissionWeapons();}
} feature;
struct Register{Register(){core::RegisterModule(&feature);}}registration;
}
void ApplyStoryMissionWeapon(uint8_t operation,int weapon,int ammo,bool inHand){
    if(operation<GIVE||operation>FINAL||weapon<=0||weapon>46)return;
    auto*me=Local();if(!me||me->Health()<=0||Net().IsHost()||!flow::InHostWorld())return;
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
