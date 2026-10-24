#include "shared/lib/core/Core.h"
namespace coop::core {
void CollectSyncAudit(std::vector<SyncAuditEntry>&out){for(auto*m:SyncAuditModules())m->CollectSyncAudit(out);}
bool RepairSyncEntity(uint8_t kind,uint16_t id){for(auto*m:SyncAuditModules())if(m->RepairSyncEntity(kind,id))return true;return false;}
}
