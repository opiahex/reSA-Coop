// reSA-Coop Diagnostic System
// Copyright (c) 2026 opiahex
// Original source: https://github.com/opiahex/reSA-Coop
// SPDX-License-Identifier: LicenseRef-reSA-Coop-Diagnostics-Attribution-1.0
// See DIAGNOSTICS-LICENSE.txt, DIAGNOSTICS-NOTICE.md and DIAGNOSTICS-COMPONENTS.md.

#pragma once
#include "common/lib/diagnostics/FlightRecorder.h"
#include "common/api/protocol/Protocol.h"
#include <atomic>
#include <cmath>
#include <cstring>

namespace coop::passenger_trace {
enum Stage : uint16_t { Producer=1,Framed,OutQueued,TransportSend,ServerReceive,Relay,TransportReceive,Receiver,Fence,Decode,Accepted,NativeApply,
 Metadata=512,DispatchBegin,DispatchEnd,ServiceDispatchBegin,ServiceDispatchEnd };
enum Reason : uint16_t { Ok=0,Malformed,ReferencesUnavailable,NotConnected,QueueLimit,AllocationFailed,SendFailed,NotJoined,NotInGame,WrongConnection,WrongWorld,Expired,GenerationRejected,DependencyWait,SessionChanged,NoHandler,CommonDecode,InvalidSeat,Self,OldMotion,OldTime,SeatHeld,NativeMissing,Replaced,PeerLeft,SessionEnded,Resync,StateExpired,
 HandlerReturned,ReaderMalformed,ServiceMetadataOnly,FingerprintBounded,NoRecipients,UnsupportedService,AdditionalReference,StateTransitionCacheBusy,StateTransitionCacheEvicted };
inline thread_local flight::Event publication{};
struct Publication {
 flight::Event previous;
 Publication(float hp,float max,float armour,uint32_t epoch):previous(publication){publication={};publication.health=hp;publication.maxHealth=max;publication.armour=armour;publication.motionEpoch=epoch;}
 ~Publication(){publication=previous;}
};
inline uint32_t U32(const uint8_t*p){return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);}
inline uint64_t U64(const uint8_t*p){return U32(p)|(uint64_t(U32(p+4))<<32);}
inline uint8_t Channel(uint8_t c){return c==proto::CH_STATE_PLAYER1||c==proto::CH_STATE_PLAYER2?uint8_t(proto::CH_STATE):c;}
constexpr size_t kHashBytes=4096;
// Local bounded capture policy only. Prefix observations do not authorize a
// packet, establish a native death/task outcome or identify a unique event.
struct PedTransitionEntry {
 uint64_t generation=0,firstFingerprint=0,earliestFingerprint=0,used=0;
 uint32_t recording=0,incarnation=0,world=0,latestClock=0,aliveClock=0,firstClock=0,earliestClock=0;
 uint16_t entity=0;uint8_t source=255,msg=0;
 bool occupied=false,latestDead=false,aliveBoundary=false,deathEpisode=false;
};
struct PedTransitionCache {std::atomic_flag guard=ATOMIC_FLAG_INIT;std::array<PedTransitionEntry,512>entries{};uint64_t serial=0;};
inline PedTransitionCache pedTransitionCache{};
inline size_t PedTransitionBucket(const flight::Context&c,uint32_t recording){
 uint64_t hash=c.generation^(uint64_t(c.world)<<32)^c.incarnation^(uint64_t(c.entity)<<16)^(uint64_t(c.source)<<8)^c.msg^(uint64_t(recording)*0x9e3779b97f4a7c15ull);
 hash^=hash>>33;hash*=0xff51afd7ed558ccdull;hash^=hash>>33;return (size_t(hash)&127u)*4;
}
inline bool SamePedTransitionOrigin(const PedTransitionEntry&e,const flight::Context&c,uint32_t recording){
 return e.occupied&&e.recording==recording&&e.generation==c.generation&&e.incarnation==c.incarnation&&e.world==c.world&&e.entity==c.entity&&e.source==c.source&&e.msg==c.msg;
}
inline void PedTransitionHint(flight::Context&c,bool dead){
 const auto recording=flight::ActiveRecordingToken();
 if(!recording){if(dead&&flight::Enabled())c.diagnosticFlags|=flight::StateCacheBusy;return;}
 auto&cache=pedTransitionCache;
 if(cache.guard.test_and_set(std::memory_order_acquire)){c.diagnosticFlags|=flight::StateCacheBusy;return;}
 // Start/Stop can race a packet already being decoded. Never commit a prior
 // recording's observation into the new token, or spin waiting for a producer.
 if(flight::ActiveRecordingToken()!=recording){cache.guard.clear(std::memory_order_release);if(dead&&flight::Enabled())c.diagnosticFlags|=flight::StateCacheBusy;return;}
 PedTransitionEntry*entry=nullptr,*replace=nullptr;const auto bucket=PedTransitionBucket(c,recording);
 for(size_t i=bucket;i<bucket+4;++i){auto&e=cache.entries[i];if(SamePedTransitionOrigin(e,c,recording)){entry=&e;break;}
  if(!replace||((!e.occupied||e.recording!=recording)&&replace->occupied&&replace->recording==recording)||(e.occupied&&replace->occupied&&e.recording==recording&&replace->recording==recording&&e.used<replace->used))replace=&e;
 }
 bool critical=false;
 if(!entry){
  entry=replace;if(entry->occupied&&entry->recording==recording)c.diagnosticFlags|=flight::StateCacheEvicted;
  *entry={};entry->generation=c.generation;entry->recording=recording;entry->incarnation=c.incarnation;entry->world=c.world;entry->entity=c.entity;entry->source=c.source;entry->msg=c.msg;entry->occupied=true;entry->latestClock=c.wireTime;entry->latestDead=dead;
  if(dead){entry->deathEpisode=true;entry->firstClock=entry->earliestClock=c.wireTime;entry->firstFingerprint=entry->earliestFingerprint=c.fingerprint;critical=true;}
  else{entry->aliveBoundary=true;entry->aliveClock=c.wireTime;}
 }else{
  const auto delta=int32_t(c.wireTime-entry->latestClock);
  // Retain the first observed death and the earliest retained refinement. This
  // is a bounded episode hint, not an unbounded ledger of critical packets.
  if(dead&&entry->deathEpisode&&((c.wireTime==entry->firstClock&&c.fingerprint==entry->firstFingerprint)||(c.wireTime==entry->earliestClock&&c.fingerprint==entry->earliestFingerprint)))critical=true;
  if(delta>0){
   if(dead&&!entry->latestDead){entry->deathEpisode=true;entry->firstClock=entry->earliestClock=c.wireTime;entry->firstFingerprint=entry->earliestFingerprint=c.fingerprint;critical=true;}
   else if(!dead){entry->aliveBoundary=true;entry->aliveClock=c.wireTime;entry->deathEpisode=false;}
   entry->latestClock=c.wireTime;entry->latestDead=dead;
  }else if(delta<0&&dead&&entry->latestDead&&entry->deathEpisode&&(!entry->aliveBoundary||int32_t(c.wireTime-entry->aliveClock)>0)&&int32_t(c.wireTime-entry->earliestClock)<0){
   entry->earliestClock=c.wireTime;entry->earliestFingerprint=c.fingerprint;critical=true;
  }
 }
 entry->used=++cache.serial;cache.guard.clear(std::memory_order_release);
 if(critical)c.diagnosticFlags|=flight::CriticalState;
}
inline void DecodePedTransitionHint(flight::Context&c,const uint8_t*p,size_t n,size_t off,size_t payload,uint8_t count){
 if(c.msg!=proto::G_PED_SYNC&&c.msg!=proto::G_PED_ACTION)return;
 // Ped ID + fixed prefix through area: health at27, flags at36. Exact offsets
 // are tested against production PedSyncState::Write, not a second wire format.
 if(payload>n||n-payload<42)return;
 const auto id=uint16_t(p[payload])|(uint16_t(p[payload+1])<<8);uint64_t generation=0;bool ambiguous=false;
 for(size_t i=off+5;i<off+5+size_t(count)*13;i+=13)if(p[i]==ENTITY_PED&&p[i+1]==255&&(uint16_t(p[i+2])|(uint16_t(p[i+3])<<8))==id){const auto value=U64(p+i+4);if(!value||(generation&&generation!=value))ambiguous=true;generation=value;}
 if(!id||!generation||ambiguous||c.source>=3||!c.incarnation||!c.world)return;
 float position[3]{},hp=0,armour=0;std::memcpy(position,p+payload+6,12);std::memcpy(&hp,p+payload+27,4);std::memcpy(&armour,p+payload+31,4);
 const auto flags=p[payload+36];const auto vehicle=uint16_t(p[payload+38])|(uint16_t(p[payload+39])<<8);const auto seat=int8_t(p[payload+40]);
 if(!std::isfinite(position[0])||!std::isfinite(position[1])||!std::isfinite(position[2])||!std::isfinite(hp)||hp<0||hp>1000000||!std::isfinite(armour)||armour<0||armour>1000000||p[payload+26]>7||(p[payload+37]&~31u)||seat< -1||seat>=8||((flags&4)&&!vehicle))return;
 c.entity=id;c.generation=generation;c.wireTime=U32(p+payload+2);PedTransitionHint(c,(flags&1)||hp<=0);
}
// Same bounded canonical bytes at client upload, relay and client download.
// Neither a complete nor a bounded fingerprint is a unique event identity.
// Services (credentials/profile/chat/world data) and voice have no content hash.
inline flight::Context Packet(const uint8_t*p,size_t n,uint8_t channel,bool incoming,uint8_t source=255,uint32_t incarnation=0){
 flight::Context c{};if(!flight::Enabled()||!p||!n)return c;
 c.msg=p[0];c.channel=Channel(channel);c.source=source;c.incarnation=incarnation;
 if(!proto::IsRelayed(p[0]))return c;
 size_t off=incoming?6:1;if(incoming&&n>=6){c.source=p[1];c.incarnation=U32(p+2);}
 if(channel!=proto::CH_VOICE&&p[0]!=proto::G_VOICE){uint64_t h=14695981039346656037ull;h=(h^p[0])*1099511628211ull;
  size_t end=off<n?off+((n-off)<kHashBytes?(n-off):kHashBytes):off;
  for(size_t i=off;i<end;++i)h=(h^p[i])*1099511628211ull;c.fingerprint=h;}
 if(n<off+5)return c;c.world=U32(p+off);uint8_t count=p[off+4];size_t refsEnd=off+5+size_t(count)*13;
 size_t payload=refsEnd+proto::kEntityAuthorityBytes;
 if(count>128||n<payload)return c;
 if(p[0]!=proto::G_PLAYER_PASSENGER){
  if(count){c.entity=uint16_t(p[off+7])|(uint16_t(p[off+8])<<8);c.generation=U64(p+off+9);}
  // These two layouts start with ped ID + PedSyncState::time. Decode only the
  // bounded numeric clock; it is the source state clock, not arrival time.
  if((p[0]==proto::G_PED_SYNC||p[0]==proto::G_PED_ACTION)&&n>=payload+6)c.wireTime=U32(p+payload+2);
  DecodePedTransitionHint(c,p,n,off,payload,count);
  return c;
 }
 if(n>=payload+7){c.entity=uint16_t(p[payload])|(uint16_t(p[payload+1])<<8);c.wireTime=U32(p+payload+3);
  for(size_t i=off+5;i<refsEnd;i+=13)if(p[i]==1&&(uint16_t(p[i+2])|(uint16_t(p[i+3])<<8))==c.entity){c.generation=U64(p+i+4);break;}}
 return c;
}
inline void Emit(flight::Context c,Stage stage,Reason reason=Ok,uint8_t local=255){if(c.msg||(stage==Metadata&&reason==Malformed)){flight::Event e{};e.context=c;e.stage=stage;e.reason=reason;e.localPeer=local;flight::Record(e);}}
// Explicit scope markers accompany metadata-only or truncated observations.
// Additional reference records retain the packet equivalence key and contain
// copied network IDs/generations only, never native pointers or payload bytes.
inline void Describe(flight::Context c,const uint8_t*p,size_t n,uint8_t channel,bool incoming,uint8_t local=255){
 if(!flight::Enabled())return;
 if(!p||!n){Emit(c,Metadata,Malformed,local);return;}
 if(!p[0]){Emit(c,Metadata,Malformed,local);return;}
 if(c.diagnosticFlags&flight::StateCacheBusy)Emit(c,Metadata,StateTransitionCacheBusy,local);
 if(c.diagnosticFlags&flight::StateCacheEvicted)Emit(c,Metadata,StateTransitionCacheEvicted,local);
 if(!proto::IsRelayed(p[0])||channel==proto::CH_VOICE||p[0]==proto::G_VOICE){Emit(c,Metadata,ServiceMetadataOnly,local);return;}
 size_t off=incoming?6:1;
 if(n<off+5){Emit(c,Metadata,Malformed,local);return;}
 uint8_t count=p[off+4];size_t refsEnd=off+5+size_t(count)*13;
 size_t payload=refsEnd+proto::kEntityAuthorityBytes;
 if(count>128||n<payload){Emit(c,Metadata,Malformed,local);return;}
 if(n-off>kHashBytes)Emit(c,Metadata,FingerprintBounded,local);
 for(size_t i=off+5;i<refsEnd;i+=13){
  auto ref=c;ref.entity=uint16_t(p[i+2])|(uint16_t(p[i+3])<<8);ref.generation=U64(p+i+4);
  if(ref.entity!=c.entity||ref.generation!=c.generation)Emit(ref,Metadata,AdditionalReference,local);
 }
}
}
