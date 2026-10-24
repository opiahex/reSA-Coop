#pragma once
#include <cstdint>
#include <array>
#include "shared/game/Types.h"

namespace coop {
// Read-only policy for entity discovery, published by the population feature.
struct PopulationShare {
    bool active=false;
    game::CVector centre;
    int8_t area=0;
    uint8_t source=255;
    bool Contains(const game::CVector& position,int8_t entityArea)const {
        return active&&entityArea==area&&(position-centre).Length2D()<300;
    }
};
inline PopulationShare& NearbyPopulationShare(){static PopulationShare share;return share;}
inline bool SharesPopulationAt(const game::CVector& position,int8_t area){return NearbyPopulationShare().Contains(position,area);}
struct PopulationViewer { uint8_t id=255;int8_t area=0;bool active=false;game::CVector position;bool canGenerate=true; };
inline std::array<PopulationViewer,3>& PopulationViewers(){static std::array<PopulationViewer,3> viewers;return viewers;}
inline uint8_t& PopulationHost(){static uint8_t host=0;return host;}
inline std::array<uint8_t,3>& PopulationSources(){static std::array<uint8_t,3> s{255,255,255};return s;}
inline std::array<uint8_t,3> ElectPopulationSources(const std::array<PopulationViewer,3>&viewers,uint8_t host,
    const std::array<uint8_t,3>&previous,float enter=60.f,float leave=90.f){
    std::array<uint8_t,3> result{255,255,255};
    std::array<PopulationViewer,3> indexed{};
    for(const auto&v:viewers)if(v.id<indexed.size())indexed[v.id]=v;
    // A suppressed viewer cannot become the next viewer's generator. Greedy
    // coverage also avoids transitively suppressing a player beyond the real
    // source's spawn radius (host -- guest1 -- guest2 in a line).
    uint8_t order[4]={host,0,1,2};
    for(auto source:order){
        if(source>=viewers.size()||result[source]!=255)continue;
        const auto&s=indexed[source];if(!s.active||!s.canGenerate)continue;
        result[source]=source;
        for(size_t i=0;i<indexed.size();++i){const auto&v=indexed[i];
            if(result[i]!=255||!v.active||v.area!=s.area)continue;
            float radius=previous[i]==source?leave:enter;
            if((v.position-s.position).Length2D()<radius)result[i]=source;
        }
    }
    return result;
}
inline int& PopulationBudgetExcluded(){static int count=0;return count;}
// One recipient for the whole nearby group; a few centimetres of movement or a
// camera rotation must not send the same ambient entity back and forth.
inline uint8_t PopulationOwnerAt(const game::CVector& at,int8_t area,float radius=80.0f){
    uint8_t best=255;
    for(const auto& v:PopulationViewers())if(v.active&&v.canGenerate&&v.id<3&&PopulationSources()[v.id]==v.id&&v.area==area&&(v.position-at).Length2D()<radius)
        if(best==255||v.id==PopulationHost()||(best!=PopulationHost()&&v.id<best))best=v.id;
    return best;
}
inline bool PopulationNeededAt(const game::CVector& at,int8_t area,float radius){
    for(const auto&v:PopulationViewers())if(v.active&&v.area==area&&(v.position-at).Length2D()<radius)return true;
    return false;
}
}
