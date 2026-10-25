#include <algorithm>
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
    int handle=-1,slot=-1,weapon=0,original=0,originalAmmo=0;
    int personal=0,borrowed=0,lastAmmo=0;
};
std::map<int,Loan> loans;
CPlayerPed* Local(){return core::IsPlayerReady()?FindPlayerPed(0):nullptr;}
int ClampAmmo(int ammo){return std::clamp(ammo,0,99999);}
bool Current(const Loan&loan,CPed*me){return me&&me->Health()>0&&GetPedRef(me)==loan.handle&&loan.slot>=0&&loan.slot<NUM_WEAPON_SLOTS&&me->Weapons()[loan.slot].type==loan.weapon;}
void Sample(Loan&loan,CPed*me){
    if(!Current(loan,me))return;
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
void SampleAll(){auto*me=Local();for(auto i=loans.begin();i!=loans.end();){if(!Current(i->second,me))i=loans.erase(i);else{Sample(i->second,me);++i;}}}
class Loadout:public Module {
    const char*Name()const override{return "StoryLoadout";}
    void OnPostProcess()override{
        if(loans.empty())return;
        SampleAll();
        for(auto&[slot,loan]:loans)if(loan.run!=StoryRunId()){RestoreStoryMissionWeapons();break;}
    }
    void OnGameShutdown()override{loans.clear();}
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
    if(found!=loans.end()&&found->second.weapon!=weapon){auto loan=found->second;loans.erase(found);Withdraw(loan,me);found=loans.end();}
    if(found==loans.end()){
        const auto&old=me->Weapons()[slot];Loan loan;loan.run=StoryRunId();loan.handle=GetPedRef(me);loan.slot=slot;loan.weapon=weapon;loan.original=old.type;loan.originalAmmo=ClampAmmo(old.ammoTotal);loan.personal=old.type==weapon?loan.originalAmmo:0;
        found=loans.emplace(slot,loan).first;
    }
    auto&loan=found->second;
    int count=std::max(loan.personal,ClampAmmo(ammo));
    Give(me,weapon,count);loan.borrowed=std::max(0,count-loan.personal);loan.lastAmmo=count;
    if(inHand)Cmd(0x01B9).I(loan.handle).I(weapon).Run();
}
void RestoreStoryMissionWeapons(){auto*me=Local();for(auto&[slot,loan]:loans){Sample(loan,me);Withdraw(loan,me);}loans.clear();}
void PersonalStoryWeapon(int slot,int&weapon,int&ammo){
    auto found=loans.find(slot);if(found==loans.end())return;
    auto*me=Local();auto&loan=found->second;if(!Current(loan,me))return;Sample(loan,me);
    weapon=loan.original;ammo=loan.original==loan.weapon?loan.personal:loan.originalAmmo;
    if(!weapon&&loan.personal>0){weapon=loan.weapon;ammo=loan.personal;}
}
}
