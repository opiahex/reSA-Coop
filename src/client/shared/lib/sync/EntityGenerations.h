#pragma once
#include <algorithm>
#include <array>
#include <map>
#include <vector>
#include "common/lib/buffer/EntityRef.h"
namespace coop {
// One identity survives handover and native streaming. Reusing a numeric ID
// creates a different identity; a delayed CREATE cannot resurrect a retired one.
class EntityGenerations {
public:
    struct Record {uint64_t generation=0;bool live=false;std::array<uint64_t,8> retired{};std::vector<EntityRef> links;};
    enum Verdict { Accept,Wait,Reject };
    std::map<uint32_t,Record> records;
    uint64_t serial=1,discarded=0;
    void Reset(uint64_t seed=1){records.clear();serial=seed?seed:1;discarded=0;}
    bool Live(uint8_t kind,uint16_t id,uint8_t scope=255)const{auto i=records.find(EntityRef{kind,scope,id}.Key());return i!=records.end()&&i->second.live&&i->second.generation;}
    uint64_t Current(uint8_t kind,uint16_t id,uint8_t scope=255)const{auto i=records.find(EntityRef{kind,scope,id}.Key());return i==records.end()?0:i->second.generation;}
    static void Replace(Record&r,uint64_t generation){
        if(r.generation&&(r.generation>>32)!=(generation>>32)){std::rotate(r.retired.rbegin(),r.retired.rbegin()+1,r.retired.rend());r.retired[0]=r.generation;}
        r.generation=generation;r.live=true;r.links.clear();
    }
    uint64_t Begin(uint8_t kind,uint16_t id,uint8_t scope=255){auto&r=records[EntityRef{kind,scope,id}.Key()];if(++serial==0)++serial;Replace(r,serial);return serial;}
    bool Stamp(std::vector<EntityRef>&refs,uint8_t self){
        for(auto&ref:refs){if(ref.kind>ENTITY_OBJECT&&ref.scope==255)ref.scope=self;
            auto&r=records[ref.Key()];if(!r.generation&&(ref.flags&ENTITY_BIRTH))Begin(ref.kind,ref.id,ref.scope);
            if(ref.flags&ENTITY_OPTIONAL){if(!ref.generation)ref.generation=r.generation;if(!ref.generation)return false;continue;}
            if(!r.generation||!r.live||(ref.generation&&ref.generation!=r.generation))return false;ref.generation=r.generation;}
        return true;
    }
    static bool Origin(uint8_t from,const EntityRef&r,uint8_t host){
        if(r.kind==ENTITY_VEHICLE||r.kind==ENTITY_PED)return (r.id>>12)==from+1;
        if(r.kind==ENTITY_OBJECT)return from==host;
        return r.scope==from;
    }
    Verdict Check(const std::vector<EntityRef>&refs,uint8_t from,uint8_t host)const{
        bool wait=false;
        for(auto&ref:refs){
            if(!ref.kind||ref.kind>ENTITY_TRAIN||!ref.generation||ref.flags>15||(ref.kind<=ENTITY_OBJECT&&ref.scope!=255)||(ref.kind>ENTITY_OBJECT&&ref.scope>=3))return Reject;
            auto i=records.find(ref.Key());
            if(ref.flags&ENTITY_BIRTH){
                if(i==records.end())continue;const auto&r=i->second;
                if(ref.generation==r.generation){if(!r.live)return Reject;continue;}
                if(((ref.generation>>32)==(r.generation>>32)&&ref.generation<r.generation)||std::any_of(r.retired.begin(),r.retired.end(),[&](auto old){return old&&(old>>32)==(ref.generation>>32);})||!Origin(from,ref,host))return Reject;
                continue;
            }
            if(ref.flags&ENTITY_OPTIONAL)continue;
            // A dependency declared by this same packet becomes available atomically.
            bool declared=std::any_of(refs.begin(),refs.end(),[&](auto&other){return other.Key()==ref.Key()&&other.generation==ref.generation&&(other.flags&ENTITY_BIRTH);});
            if(declared)continue;
            if(i==records.end()||!i->second.generation){wait=true;continue;}
            if(!i->second.live||i->second.generation!=ref.generation)return Reject;
        }
        return wait?Wait:Accept;
    }
    template<class Replaced>void Commit(const std::vector<EntityRef>&refs,Replaced replaced){
        for(auto&ref:refs)if(ref.flags&ENTITY_BIRTH){auto&r=records[ref.Key()];if(r.generation!=ref.generation){if(r.generation)replaced(ref.kind,ref.scope,ref.id);Replace(r,ref.generation);}}
        for(auto&ref:refs)if(ref.flags&ENTITY_SNAPSHOT){auto&r=records[ref.Key()];r.links.clear();for(auto&link:refs)if(link.Key()!=ref.Key())r.links.push_back(link);}
    }
    void Complete(const std::vector<EntityRef>&refs){for(auto&ref:refs)if(ref.flags&ENTITY_REMOVE){auto i=records.find(ref.Key());if(i!=records.end()&&i->second.generation==ref.generation){i->second.live=false;i->second.links.clear();}}}
    bool Match(const std::vector<EntityRef>&refs)const{for(auto&ref:refs){auto i=records.find(ref.Key());if(i==records.end()||!i->second.live||i->second.generation!=ref.generation)return false;}return true;}
    bool Link(uint8_t root,uint16_t id,uint8_t target,uint16_t other,uint8_t scope=255)const{
        auto i=records.find(EntityRef{root,scope,id}.Key());if(i==records.end())return false;
        for(auto&ref:i->second.links)if(ref.kind==target&&ref.id==other)return Match({ref});
        return false;
    }
};
}
