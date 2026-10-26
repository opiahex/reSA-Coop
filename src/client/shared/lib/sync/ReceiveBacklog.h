#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "common/api/protocol/Protocol.h"

namespace coop {

inline uint64_t ReceivePacketLifetime(uint8_t channel) {
    if (channel == proto::CH_VOICE) return 300;
    if (channel == proto::CH_STATE || channel == proto::CH_STATE_PLAYER1 || channel == proto::CH_STATE_PLAYER2)
        return 750;
    return 0;
}

// During a loading pause, discard expired movement/voice already in the queue.
// Reliable events and world/profile transfers keep their original order. Dropping
// new arrivals instead kept the oldest poses and discarded the recovery frames.
template <class Packet>
size_t ExpireReceiveBacklog(std::vector<Packet>& packets, uint64_t now, uint64_t& bytes) {
    const auto before = packets.size();
    packets.erase(std::remove_if(packets.begin(), packets.end(), [&](const Packet& packet) {
        const auto lifetime = ReceivePacketLifetime(packet.channel);
        if (!lifetime || !packet.received || now < packet.received || now - packet.received <= lifetime) return false;
        const auto size = packet.data.size();
        bytes = size > bytes ? 0 : bytes - size;
        return true;
    }), packets.end());
    return before - packets.size();
}

}  // namespace coop
