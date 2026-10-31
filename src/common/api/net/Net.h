#pragma once
#include <enet/enet.h>

#include <cstdint>
#include <vector>

#include "common/api/protocol/Protocol.h"
#include "common/lib/buffer/Buffer.h"
#ifdef COOP_SERVER
#include "common/lib/diagnostics/PassengerTrace.h"
#endif

namespace coop::net {

// Reference-counted enet_initialize/enet_deinitialize.
bool Init();
void Shutdown();

// ENet's datagrams of up to 1392 bytes (1420 with IP and UDP) do not pass every line (a VPN, a mobile network, a router
// that drops IP fragments): a reliable packet that never gets through ends the connection 10-20 s later. The two sides
// agree on the smaller MTU of the two (the connect handshake), so either one setting this is enough.
constexpr enet_uint32 kMtu = 1200;

// Packet flags for a channel: reliable channels get ENET_PACKET_FLAG_RELIABLE.
uint32_t FlagsForChannel(uint8_t channel);

inline ENetPacket* MakePacket(const void* data, size_t size, uint8_t channel) {
    return enet_packet_create(data, size, FlagsForChannel(channel));
}

inline void Send(ENetPeer* peer, uint8_t channel, const Writer& w) {
#ifdef COOP_SERVER
    // Service tracing deliberately contains no payload fingerprint: these
    // messages can carry credentials, profiles, save data and player text.
    flight::Context diagnostic{};
    if(w.Size()&&!proto::IsRelayed(w.Data()[0])){
        diagnostic=passenger_trace::Packet(w.Data(),w.Size(),channel,false,proto::kServerId);
        passenger_trace::Emit(diagnostic,passenger_trace::Producer,passenger_trace::Ok,proto::kServerId);
        passenger_trace::Describe(diagnostic,w.Data(),w.Size(),channel,false,proto::kServerId);
    }
    if(!peer){passenger_trace::Emit(diagnostic,passenger_trace::TransportSend,passenger_trace::NotConnected,proto::kServerId);return;}
    ENetPacket* p=MakePacket(w.Data(),w.Size(),channel);
    if(!p){passenger_trace::Emit(diagnostic,passenger_trace::TransportSend,passenger_trace::AllocationFailed,proto::kServerId);return;}
    int sent=enet_peer_send(peer,channel,p);
    passenger_trace::Emit(diagnostic,passenger_trace::TransportSend,sent?passenger_trace::SendFailed:passenger_trace::Ok,proto::kServerId);
    if(sent!=0)enet_packet_destroy(p);
#else
    if (!peer) return;
    ENetPacket* p = MakePacket(w.Data(), w.Size(), channel);
    if (p && enet_peer_send(peer, channel, p) != 0) enet_packet_destroy(p);  // a peer going away takes nothing
#endif
}

}  // namespace coop::net
