#pragma once
#include <cstdint>
namespace coop {
struct StoryActorQuery {uint8_t inputs=0,outputs=0;bool vehicle=false;};
// Read-only continuation commands. Never rewrite GET_PLAYER_CHAR, global CJ
// aliases, controls, rewards, health setters or arbitrary tasks.
inline StoryActorQuery StoryContinuationQuery(int opcode){
    switch(opcode){
    case 0x00D9:case 0x03C0:case 0x0811:return {1,1,true};
    case 0x00A0:return {1,3,false};
    case 0x04C4:return {4,3,false};
    case 0x0172:case 0x0470:case 0x0226:case 0x04DD:case 0x06AC:return {1,1,false};
    case 0x041A:return {2,1,false};
    default:return {};
    }
}
inline bool FreshStoryContinuation(uint64_t now,uint64_t action,uint64_t received){
    return action&&now>=action&&now>=received&&now-received<1500;
}
// A guest may leave a supplied ride. Only a new assignment can seat them again.
inline bool SameStoryRole(uint8_t aMode,uint16_t aVehicle,uint64_t aGeneration,uint16_t aObject,
                          uint8_t bMode,uint16_t bVehicle,uint64_t bGeneration,uint16_t bObject){
    return aMode==bMode&&aVehicle==bVehicle&&aGeneration==bGeneration&&aObject==bObject;
}
}
