#pragma once
#include <cstdint>
namespace coop {
enum EntityKind : uint8_t { ENTITY_VEHICLE=1,ENTITY_PED=2,ENTITY_OBJECT=3,ENTITY_PROP=4,ENTITY_PICKUP=5,ENTITY_FIRE=6,ENTITY_TRAIN=7 };
enum EntityRefFlags : uint8_t { ENTITY_USE=0,ENTITY_BIRTH=1,ENTITY_REMOVE=2,ENTITY_SNAPSHOT=4,ENTITY_OPTIONAL=8 };
struct EntityRef {
    uint8_t kind=0,scope=255;uint16_t id=0;uint64_t generation=0;uint8_t flags=0;
    uint32_t Key()const{return (uint32_t(kind)<<24)|(uint32_t(scope)<<16)|id;}
};
}
