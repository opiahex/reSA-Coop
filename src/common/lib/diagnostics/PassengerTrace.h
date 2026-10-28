// reSA-Coop Diagnostic System
// Copyright (c) 2026 opiahex
// Original source: https://github.com/opiahex/reSA-Coop
// SPDX-License-Identifier: LicenseRef-reSA-Coop-Diagnostics-Attribution-1.0
// See DIAGNOSTICS-LICENSE.txt, DIAGNOSTICS-NOTICE.md and DIAGNOSTICS-COMPONENTS.md.

#pragma once
#include "common/lib/diagnostics/FlightRecorder.h"
#include "common/api/protocol/Protocol.h"

namespace coop::passenger_trace {
enum Stage : uint16_t { Producer=1,Framed,OutQueued,TransportSend,ServerReceive,Relay,TransportReceive,Receiver,Fence,Decode,Accepted,NativeApply };
enum Reason : uint16_t { Ok=0,Malformed,ReferencesUnavailable,NotConnected,QueueLimit,AllocationFailed,SendFailed,NotJoined,NotInGame,WrongConnection,WrongWorld,Expired,GenerationRejected,DependencyWait,SessionChanged,NoHandler,CommonDecode,InvalidSeat,Self,OldMotion,OldTime,SeatHeld,NativeMissing,Replaced,PeerLeft,SessionEnded,Resync,StateExpired };
inline thread_local flight::Event publication{};
struct Publication {
 flight::Event previous;
 Publication(float hp,float max,float armour,uint32_t epoch):previous(publication){publication={};publication.health=hp;publication.maxHealth=max;publication.armour=armour;publication.motionEpoch=epoch;}
 ~Publication(){publication=previous;}
};
inline uint32_t U32(const uint8_t*p){return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);}
inline uint64_t U64(const uint8_t*p){return U32(p)|(uint64_t(U32(p+4))<<32);}
inline uint8_t Channel(uint8_t c){return c==proto::CH_STATE_PLAYER1||c==proto::CH_STATE_PLAYER2?uint8_t(proto::CH_STATE):c;}
// Same canonical bytes at client upload, relay and client download. This is a
// publication-equivalence key, NOT a unique ID for indistinguishable retries.
inline flight::Context Packet(const uint8_t*p,size_t n,uint8_t channel,bool incoming,uint8_t source=255,uint32_t incarnation=0){
 flight::Context c{};if(!flight::Enabled()||!n||p[0]!=proto::G_PLAYER_PASSENGER)return c;
 c.msg=p[0];c.channel=Channel(channel);c.source=source;c.incarnation=incarnation;
 size_t off=incoming?6:1;if(incoming&&n>=6){c.source=p[1];c.incarnation=U32(p+2);}
 uint64_t h=14695981039346656037ull;h=(h^p[0])*1099511628211ull;
 for(size_t i=off;i<n;++i)h=(h^p[i])*1099511628211ull;c.fingerprint=h;
 if(n<off+5)return c;c.world=U32(p+off);uint8_t count=p[off+4];size_t payload=off+5+size_t(count)*13;
 if(count>128||n<payload)return c;
 if(n>=payload+7){c.entity=uint16_t(p[payload])|(uint16_t(p[payload+1])<<8);c.wireTime=U32(p+payload+3);
  for(size_t i=off+5;i<payload;i+=13)if(p[i]==1&&(uint16_t(p[i+2])|(uint16_t(p[i+3])<<8))==c.entity){c.generation=U64(p+i+4);break;}}
 return c;
}
inline void Emit(flight::Context c,Stage stage,Reason reason=Ok,uint8_t local=255){if(c.msg){flight::Event e{};e.context=c;e.stage=stage;e.reason=reason;e.localPeer=local;flight::Record(e);}}
}
