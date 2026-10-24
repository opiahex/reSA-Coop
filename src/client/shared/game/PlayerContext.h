#pragma once
#include "shared/game/Game.h"

namespace game {

// Borrow one native slot for a remote player. Its embedded data and the ped's pointer must agree for the
// entire engine call; commit both together before restoring the previous player (including nested calls).
class PlayerSlotContext {
public:
    PlayerSlotContext(CPlayerInfo& slot, CPlayerInfo& remote) : m_slot(slot), m_remote(remote), m_saved(slot) {
        m_slot = m_remote;
        if (m_slot.ped) m_slot.ped->PlayerData() = m_slot.PlayerData();
    }
    ~PlayerSlotContext() {
        m_remote = m_slot;
        if (m_remote.ped) m_remote.ped->PlayerData() = m_remote.PlayerData();
        m_slot = m_saved;
    }
    PlayerSlotContext(const PlayerSlotContext&) = delete;
    PlayerSlotContext& operator=(const PlayerSlotContext&) = delete;

private:
    CPlayerInfo& m_slot;
    CPlayerInfo& m_remote;
    CPlayerInfo m_saved;
};

}  // namespace game
