#pragma once
#include "shared/lib/sync/AnimationState.h"
#include "shared/lib/sync/Quat.h"
namespace coop {
// The owner executes the AI task. Replicas receive its native pose layers and
// physical binding; no task pointers, native callbacks or extra AI are copied.
struct PedInteraction {
    static constexpr int LayerCount=6;
    uint32_t revision=0;
    uint8_t leader=255;
    uint8_t depth=0,partnerKind=0;uint16_t chain[12]{},partnerId=0;
    int8_t sequenceStep=-1;uint16_t sequenceRepeat=0;
    game::CVector partnerOffset;float partnerHeading=0;
    bool couple=false;uint8_t handArm=255;int16_t handBone=0;game::CVector handOffset;float handSpeed=.25f;int32_t handBlend=250;
    uint16_t task=0xFFFF,leaf=0xFFFF;
    uint8_t parentKind=0;uint16_t parentId=0;
    game::CVector offset,euler;Quat rotation;
    bool turret=false;uint16_t turretAngle=0;float turretRange=0,turretMode=0;
    AnimationState layers[LayerCount];float weights[LayerCount]{1,1,1,1,1,1};
    void Write(Writer&w)const{
        w.U8(leader);w.U32(revision);w.U8(depth);for(unsigned i=0;i<depth&&i<12;++i)w.U16(chain[i]);w.I8(sequenceStep);w.U16(sequenceRepeat);
        w.U8(partnerKind);if(partnerKind){if(partnerKind==1)w.Entity(ENTITY_PED,partnerId);else w.U16(partnerId);w.Vec({partnerOffset.x,partnerOffset.y,partnerOffset.z});w.F32(partnerHeading);}
        w.Bool(couple);w.U8(handArm);if(handArm<2){w.I16(handBone);w.Vec({handOffset.x,handOffset.y,handOffset.z});w.F32(handSpeed);w.I32(handBlend);}
        w.U16(task);w.U16(leaf);w.U8(parentKind);
        if(parentKind){if(parentKind==1)w.Entity(ENTITY_VEHICLE,parentId);else if(parentKind==2)w.Entity(ENTITY_PED,parentId);else if(parentKind==4)w.Entity(ENTITY_OBJECT,parentId);else w.U16(parentId);w.Vec({offset.x,offset.y,offset.z});w.Vec({euler.x,euler.y,euler.z});rotation.Write(w);w.Bool(turret);if(turret){w.U16(turretAngle);w.F32(turretRange);w.F32(turretMode);}}
        for(int i=0;i<LayerCount;++i){layers[i].Write(w);if(layers[i].Present())w.Unit8(weights[i]);}
    }
    bool Read(Reader&r){
        leader=r.U8();if(leader>=3&&leader!=255)return false;revision=r.U32();depth=r.U8();if(depth>12)return false;for(unsigned i=0;i<depth;++i){chain[i]=r.U16();if(chain[i]>=4096)return false;}sequenceStep=r.I8();sequenceRepeat=r.U16();
        partnerKind=r.U8();if(partnerKind>2)return false;if(partnerKind){partnerId=r.U16();auto p=r.Vec();partnerOffset={p.x,p.y,p.z};partnerHeading=r.F32();if(partnerOffset.Length()>30||std::fabs(partnerHeading)>7||(partnerKind==1&&!partnerId)||(partnerKind==2&&partnerId>=3))return false;}if(sequenceStep < -1 || sequenceStep >7)return false;
        couple=r.Bool();handArm=r.U8();if(handArm!=255&&handArm>1)return false;
        if(handArm<2){handBone=r.I16();auto p=r.Vec();handOffset={p.x,p.y,p.z};handSpeed=r.F32();handBlend=r.I32();if(handBone < -1||handBone>255||!std::isfinite(handOffset.x)||!std::isfinite(handOffset.y)||!std::isfinite(handOffset.z)||handOffset.Length()>3||!std::isfinite(handSpeed)||handSpeed<0||handSpeed>10||handBlend<0||handBlend>10000)return false;}
        task=r.U16();leaf=r.U16();parentKind=r.U8();
        if(parentKind){parentId=r.U16();auto p=r.Vec(),e=r.Vec();offset={p.x,p.y,p.z};euler={e.x,e.y,e.z};rotation.Read(r);turret=r.Bool();if(turret){turretAngle=r.U16();turretRange=r.F32();turretMode=r.F32();}}
        for(int i=0;i<LayerCount;++i){if(!layers[i].Read(r))return false;if(layers[i].Present())weights[i]=r.Unit8();}
        return r.Ok()&&(task==0xFFFF||task<4096)&&(leaf==0xFFFF||leaf<4096)&&parentKind<=4&&std::isfinite(turretRange)&&std::isfinite(turretMode)&&std::fabs(turretRange)<100&&std::fabs(turretMode)<100&&offset.Length()<100&&euler.Length()<100&&(!parentKind||parentKind==3||parentId);
    }
};
}
