#pragma once
#include <cstdint>
#include "shared/game/Game.h"

namespace coop {
// Stable network identity, never a native pool pointer/handle. Set records use
// id 1 and a content digest; providers validate all repair requests themselves.
enum SyncKind : uint8_t { AUDIT_VEHICLE, AUDIT_PED, AUDIT_OBJECT, AUDIT_PICKUPS, AUDIT_MARKERS, AUDIT_FIRE, AUDIT_MISSION };
struct SyncAuditEntry {
    uint8_t kind=0, owner=255;
    uint16_t id=0, model=0;
    int8_t area=0;
    game::CVector pos;
    uint32_t relation=0, look=0;
    bool alive=false;
    uint64_t received=0; // diagnostics receive clock; never serialized
};
inline uint32_t AuditHash(uint32_t h, uint32_t v) { return (h^v)*16777619u; }
inline bool AuditSet(uint8_t kind) { return kind==AUDIT_PICKUPS||kind==AUDIT_MARKERS||kind==AUDIT_MISSION; }
}
