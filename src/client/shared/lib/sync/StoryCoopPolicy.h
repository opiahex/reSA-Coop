#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>
namespace coop {
struct StoryTargetCandidate {uint8_t id=255;float distance=0;bool alive=false,sameArea=false,visible=false;};
// Only the explicit mission-enemy registry calls this policy. Ambient peds
// retain native individual reactions to their own aggressor.
inline uint8_t ChooseStoryTarget(const std::array<StoryTargetCandidate,3>&players,uint8_t current,uint8_t aggressor){
    uint8_t best=255;float score=1e9f;
    for(auto&p:players){if(p.id>=3||!p.alive||!p.sameArea||!p.visible||p.distance>120)continue;
        float value=p.distance*(p.id==current?.75f:1.f)*(p.id==aggressor?.6f:1.f);
        if(value<score||(value==score&&p.id<best)){score=value;best=p.id;}}
    return best;
}
inline bool StoryGunnerSection(bool story,bool scriptDriveBy,bool recordedOrAttached,bool controls,bool cutscene){return story&&scriptDriveBy&&recordedOrAttached&&controls&&!cutscene;}
struct StoryRewardLedger {
    std::vector<uint64_t> paid;
    bool Contains(uint64_t token)const{return std::find(paid.begin(),paid.end(),token)!=paid.end();}
    bool Credit(uint64_t token){if(!token||Contains(token))return false;paid.push_back(token);if(paid.size()>128)paid.erase(paid.begin());return true;}
};
inline bool SharedWeaponCondition(int actual,int requested){return requested==actual||requested==54||(requested==55&&actual>=22&&actual<=38);}
}
