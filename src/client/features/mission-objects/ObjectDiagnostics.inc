// Bounded diagnostics for native object synchronization.
enum ObjectDiagnosticReason:uint16_t {
    OD_OK=0,OD_MALFORMED,OD_SENDER,OD_NOT_READY,OD_MODEL,OD_ORIGIN,
    OD_CONFLICT,OD_OLD_REVISION,OD_PENDING,OD_MODEL_WAIT,OD_POOL_WAIT,
    OD_NATIVE_FAILED,OD_NATIVE_MISSING,OD_CONTEXT,OD_DEPENDENCY,
    OD_REPLACED,OD_CANCELLED,OD_MISSING,OD_UNKNOWN_REMOVE,OD_REPAIRED,
    OD_ALREADY_MISSING,OD_RETAINED,OD_DELETE_FAILED,OD_ADOPTED,OD_BROKEN,
    OD_OBSERVATION_BUSY,OD_OBSERVATION_BUDGET
};
flight::Context IncomingObjectDiagnostic(uint16_t id,uint8_t msg){
    if(!flight::Enabled())return {};
    auto c=Net().DiagnosticContext();if(c.msg!=msg)return {};
    c.entity=id;c.generation=Net().ExpectedEntityGeneration(ENTITY_OBJECT,id);return c;
}
flight::Context LocalObjectDiagnostic(uint16_t id,uint8_t msg){
    if(!flight::Enabled())return {};
    flight::Context c{};c.entity=id;c.generation=Net().EntityGeneration(ENTITY_OBJECT,id);
    c.msg=msg;c.source=Net().MyId();c.incarnation=Net().MyIncarnation();c.world=Net().WorldEpoch();return c;
}
flight::Context ObjectCleanupDiagnostic(const NetObject&n,uint8_t msg){
    if(!flight::Enabled())return {};
    flight::Context c{};c.entity=n.netId;c.generation=n.diagnosticGeneration;c.msg=msg;
    c.source=n.diagnosticLocalPeer;c.incarnation=n.diagnosticLocalIncarnation;c.world=n.diagnosticWorld;return c;
}
bool ObjectDiagnosticSourceCurrent(const NetObject&n,const flight::Context&c){
    if(!c.msg||!c.generation||n.diagnosticWorld!=Net().WorldEpoch()||
       c.entity!=n.netId||n.diagnosticGeneration!=c.generation)return false;
    if(c.source==Net().MyId())return c.incarnation==Net().MyIncarnation()&&c.world==Net().WorldEpoch();
    auto*p=Net().Peer(c.source);return p&&p->incarnation==c.incarnation&&Net().PeerWorldEpoch(c.source)==c.world;
}
bool ObjectDiagnosticCurrent(const NetObject&n,const flight::Context&c){return ObjectDiagnosticSourceCurrent(n,c)&&Net().EntityGeneration(ENTITY_OBJECT,n.netId)==c.generation;}
bool ObjectDiagnosticBody(const NetObject&n){return n.handle>=0&&Alive(n)&&GetObject(n.handle)==n.obj;}
flight::Event ObjectDiagnosticEvent(flight::Context c,uint16_t stage,uint16_t reason,const NetObject*n=nullptr,bool inspectNative=false){
    flight::Event e{};e.context=c;e.stage=stage;e.reason=reason;e.localPeer=Net().MyId();
    flight::AddUnsigned(e,flight::Metric::EntityKind,ENTITY_OBJECT);
    if(n){
        e.owner=n->diagnosticOwner;e.detailKnown|=flight::OwnerKnown;
        flight::AddSigned(e,flight::Metric::RequestedModel,n->model);
        flight::AddUnsigned(e,flight::Metric::ActionRevision,n->motion.revision);
        if(inspectNative&&ObjectDiagnosticBody(*n)){
            e.nativeHandle=(uint32_t)n->handle;
            flight::AddSigned(e,flight::Metric::ObservedModel,n->obj->ModelIndex());
        }
    }
    return e;
}
void RecordObjectDiagnostic(flight::Context c,uint16_t stage,uint16_t reason,const NetObject*n=nullptr,bool inspectNative=false){
    if(!c.msg||!flight::Enabled())return;
    (void)flight::Record(ObjectDiagnosticEvent(c,stage,reason,n,inspectNative));
}
void RetireObjectAttachmentDiagnostic(NetObject&n,uint16_t reason){
    if(n.diagnosticAttachmentPending&&n.diagnosticAttachment.msg&&flight::Enabled()){
        flight::Event e{};e.context=n.diagnosticAttachment;e.stage=664;e.reason=reason;e.localPeer=Net().MyId();
        e.owner=n.diagnosticOwner;e.detailKnown|=flight::OwnerKnown;
        flight::AddUnsigned(e,flight::Metric::EntityKind,ENTITY_OBJECT);
        flight::AddUnsigned(e,flight::Metric::ReferenceKind,3);
        flight::AddUnsigned(e,flight::Metric::ActionRevision,n.diagnosticAttachmentRevision);
        flight::AddUnsigned(e,flight::Metric::ParentKind,n.diagnosticParentKind);
        flight::AddUnsigned(e,flight::Metric::ParentId,n.diagnosticParentId);
        if(n.diagnosticParentGeneration)flight::AddUnsigned(e,flight::Metric::ParentGeneration,n.diagnosticParentGeneration);
        (void)flight::Record(e);
    }
    n.diagnosticAttachmentPending=false;
}
void RetireObjectDiagnostics(NetObject&n,uint16_t reason){
    auto retire=[&](flight::Context c,uint8_t scope){if(!c.msg||!flight::Enabled())return;auto e=ObjectDiagnosticEvent(c,664,reason,&n,false);flight::AddUnsigned(e,flight::Metric::ReferenceKind,scope);(void)flight::Record(e);};
    if(n.diagnosticBirthPending)retire(n.diagnosticBirth,1);
    if(n.diagnosticStatePending)retire(n.diagnosticState,2);
    RetireObjectAttachmentDiagnostic(n,reason);
    n.diagnosticBirthPending=n.diagnosticStatePending=n.diagnosticAttachmentPending=false;
}
void AcceptObjectDiagnostic(NetObject&n,flight::Context c,bool birth){
    if(birth&&n.diagnosticBirthPending)RecordObjectDiagnostic(n.diagnosticBirth,664,OD_REPLACED,&n);
    if(n.diagnosticStatePending)RecordObjectDiagnostic(n.diagnosticState,664,OD_REPLACED,&n);
    if(birth){n.diagnosticBirth=c;n.diagnosticBirthPending=c.msg!=0;n.diagnosticRecovery=false;n.diagnosticBirthLocal=false;}
    n.diagnosticState=c;n.diagnosticStatePending=c.msg!=0;
    n.diagnosticWorld=Net().WorldEpoch();n.diagnosticGeneration=c.generation?c.generation:Net().ExpectedEntityGeneration(ENTITY_OBJECT,n.netId);n.diagnosticOwner=Net().HostId();
    n.diagnosticLocalIncarnation=Net().MyIncarnation();n.diagnosticLocalPeer=Net().MyId();
    n.diagnosticWait=0;uint64_t parentGeneration=0;
    if(n.motion.parentKind==5)parentGeneration=n.motion.train.generation;
    else if(n.motion.parentKind&&n.motion.parentKind!=3){
        uint8_t kind=n.motion.parentKind==1?ENTITY_VEHICLE:n.motion.parentKind==2?ENTITY_PED:ENTITY_OBJECT;
        parentGeneration=Net().ExpectedEntityGeneration(kind,n.motion.parentId);
    }
    RecordObjectDiagnostic(c,663,OD_PENDING,&n);
    bool attachmentChanged=!n.diagnosticAttachmentKnown||n.diagnosticParentKind!=n.motion.parentKind||
        n.diagnosticParentId!=n.motion.parentId||n.diagnosticParentGeneration!=parentGeneration||n.diagnosticAttachmentRevision!=n.motion.revision;
    if(attachmentChanged){
        RetireObjectAttachmentDiagnostic(n,OD_REPLACED);
        n.diagnosticAttachment=c;n.diagnosticAttachmentPending=c.msg!=0;n.diagnosticAttachmentReason=65535;
        n.diagnosticParentKind=n.motion.parentKind;n.diagnosticParentId=n.motion.parentId;
        n.diagnosticParentGeneration=parentGeneration;n.diagnosticAttachmentRevision=n.motion.revision;n.diagnosticAttachmentKnown=true;
    }
    if(attachmentChanged&&c.msg&&flight::Enabled()){
        flight::Event e{};e.context=c;e.stage=665;e.localPeer=Net().MyId();
        flight::AddUnsigned(e,flight::Metric::EntityKind,ENTITY_OBJECT);
        flight::AddUnsigned(e,flight::Metric::ActionRevision,n.motion.revision);
        flight::AddUnsigned(e,flight::Metric::ParentKind,n.motion.parentKind);
        flight::AddUnsigned(e,flight::Metric::ParentId,n.motion.parentId);
        if(n.diagnosticParentGeneration)flight::AddUnsigned(e,flight::Metric::ParentGeneration,n.diagnosticParentGeneration);
        (void)flight::Record(e);
    }
}
void ObjectDiagnosticWait(NetObject&n,uint16_t reason){
    if(n.diagnosticWait==reason)return;
    n.diagnosticWait=reason;
    auto c=n.diagnosticBirthPending?n.diagnosticBirth:n.diagnosticState;
    RecordObjectDiagnostic(c,664,reason,&n);
}
void RecordObjectBirth(NetObject&n){
    if(!n.diagnosticBirthPending||!flight::Enabled())return;
    auto c=n.diagnosticBirth;
    if(!ObjectDiagnosticCurrent(n,c)){RecordObjectDiagnostic(c,664,OD_CONTEXT,&n);n.diagnosticBirthPending=false;return;}
    auto admission=flight::AdmitNativeObservation(c,661,Net().MyId(),true);
    if(admission!=flight::NativeAdmission::Admitted){
        if(admission==flight::NativeAdmission::Busy||admission==flight::NativeAdmission::Budget)ObjectDiagnosticWait(n,admission==flight::NativeAdmission::Busy?OD_OBSERVATION_BUSY:OD_OBSERVATION_BUDGET);
        return;
    }
    if(!ObjectDiagnosticBody(n)){ObjectDiagnosticWait(n,OD_NATIVE_MISSING);return;}
    auto e=ObjectDiagnosticEvent(c,661,n.diagnosticBirthLocal?OD_RETAINED:n.diagnosticRecovery?OD_REPAIRED:n.adopted?OD_ADOPTED:OD_OK,&n,true);
    flight::AddSigned(e,flight::Metric::RequestedArea,n.area);
    flight::AddSigned(e,flight::Metric::ObservedArea,n.obj->AreaCode());
    (void)flight::Record(e);n.diagnosticBirthPending=false;n.diagnosticWait=0;
}
void RecordTrackedObjectBirth(NetObject&n){
    n.diagnosticWorld=Net().WorldEpoch();n.diagnosticGeneration=Net().EntityGeneration(ENTITY_OBJECT,n.netId);
    n.diagnosticLocalIncarnation=Net().MyIncarnation();n.diagnosticLocalPeer=Net().MyId();n.diagnosticOwner=Net().MyId();
    n.diagnosticBirth=LocalObjectDiagnostic(n.netId,proto::G_OBJ_CREATE);n.diagnosticBirthPending=n.diagnosticBirth.msg!=0;n.diagnosticBirthLocal=true;
    RecordObjectBirth(n); // Existing native script object, not our CREATE_OBJECT.
}
void RecordObjectState(NetObject&n){
    if(!n.diagnosticStatePending||!flight::Enabled())return;
    auto c=n.diagnosticState;
    if(!ObjectDiagnosticCurrent(n,c)){RecordObjectDiagnostic(c,664,OD_CONTEXT,&n);n.diagnosticStatePending=false;return;}
    auto admission=flight::AdmitNativeObservation(c,660,Net().MyId());
    if(admission!=flight::NativeAdmission::Admitted){
        if(admission==flight::NativeAdmission::Busy||admission==flight::NativeAdmission::Budget)ObjectDiagnosticWait(n,admission==flight::NativeAdmission::Busy?OD_OBSERVATION_BUSY:OD_OBSERVATION_BUDGET);
        n.diagnosticStatePending=false;return;
    }
    if(!ObjectDiagnosticBody(n)){ObjectDiagnosticWait(n,OD_NATIVE_MISSING);return;}
    flight::Event e{};e.context=c;e.stage=660;e.localPeer=Net().MyId();e.nativeHandle=(uint32_t)n.handle;
    e.owner=n.diagnosticOwner;e.detailKnown|=flight::OwnerKnown;
    flight::AddUnsigned(e,flight::Metric::EntityKind,ENTITY_OBJECT);
    flight::AddUnsigned(e,flight::Metric::ActionRevision,n.motion.revision);
    flight::AddFloat(e,flight::Metric::RequestedX,n.pos.x);flight::AddFloat(e,flight::Metric::RequestedY,n.pos.y);flight::AddFloat(e,flight::Metric::RequestedZ,n.pos.z);
    auto pos=n.obj->GetPosition();flight::AddFloat(e,flight::Metric::ObservedX,pos.x);flight::AddFloat(e,flight::Metric::ObservedY,pos.y);flight::AddFloat(e,flight::Metric::ObservedZ,pos.z);
    (void)flight::Record(e);n.diagnosticStatePending=false;
}
void RecordObjectAttachment(NetObject&n,CPhysical*expected){
    if(!n.diagnosticAttachmentPending||!flight::Enabled())return;
    auto c=n.diagnosticAttachment;
    if(n.motion.parentKind!=n.diagnosticParentKind||n.motion.parentId!=n.diagnosticParentId||
       n.motion.revision!=n.diagnosticAttachmentRevision||
       (n.motion.parentKind==5&&n.motion.train.generation!=n.diagnosticParentGeneration)){
        RetireObjectAttachmentDiagnostic(n,OD_REPLACED);return;
    }
    if(!ObjectDiagnosticCurrent(n,c)){RetireObjectAttachmentDiagnostic(n,OD_CONTEXT);return;}
    if(n.diagnosticParentKind&&n.diagnosticParentKind!=3&&n.diagnosticParentGeneration){
        uint8_t kind=n.diagnosticParentKind==1?ENTITY_VEHICLE:n.diagnosticParentKind==2?ENTITY_PED:n.diagnosticParentKind==5?ENTITY_TRAIN:ENTITY_OBJECT;
        auto generation=Net().EntityGeneration(kind,n.diagnosticParentId,n.diagnosticParentKind==5?n.motion.train.owner:255);
        if(generation&&generation!=n.diagnosticParentGeneration){RetireObjectAttachmentDiagnostic(n,OD_CONTEXT);return;}
    }
    if(n.diagnosticAttachmentReason==OD_DEPENDENCY&&(!expected||NowMs()<n.diagnosticAttachmentRetryAt))return;
    auto admission=flight::AdmitNativeObservation(c,666,Net().MyId(),true);
    if(admission!=flight::NativeAdmission::Admitted){
        if(admission==flight::NativeAdmission::Busy||admission==flight::NativeAdmission::Budget)ObjectDiagnosticWait(n,admission==flight::NativeAdmission::Busy?OD_OBSERVATION_BUSY:OD_OBSERVATION_BUDGET);
        return;
    }
    if(!ObjectDiagnosticBody(n)){ObjectDiagnosticWait(n,OD_NATIVE_MISSING);return;}
    auto*actual=n.obj->AttachedTo();bool matched=n.motion.parentKind?expected&&actual==expected:actual==nullptr;
    uint64_t observedGeneration=0;
    if(matched&&actual&&n.motion.parentKind!=3){
        uint8_t kind=n.motion.parentKind==1?ENTITY_VEHICLE:n.motion.parentKind==2?ENTITY_PED:n.motion.parentKind==5?ENTITY_TRAIN:ENTITY_OBJECT;
        observedGeneration=Net().EntityGeneration(kind,n.motion.parentId,n.motion.parentKind==5?n.motion.train.owner:255);
        if(!observedGeneration||observedGeneration!=n.diagnosticParentGeneration)matched=false;
    }
    uint16_t reason=matched?OD_OK:OD_DEPENDENCY;
    if(!matched&&n.diagnosticAttachmentReason==reason)return;
    n.diagnosticAttachmentReason=reason;n.diagnosticAttachmentRetryAt=NowMs()+100;
    flight::Event e{};e.context=c;e.stage=666;e.reason=reason;e.localPeer=Net().MyId();e.nativeHandle=(uint32_t)n.handle;
    flight::AddUnsigned(e,flight::Metric::EntityKind,ENTITY_OBJECT);
    flight::AddUnsigned(e,flight::Metric::ActionRevision,n.motion.revision);
    flight::AddUnsigned(e,flight::Metric::AttachmentFlags,(actual?1u:0u)|(expected?2u:0u)|(matched?4u:0u));
    flight::AddSigned(e,flight::Metric::NativeResult,matched?1:0);
    if(!actual){flight::AddUnsigned(e,flight::Metric::ParentKind,0);flight::AddUnsigned(e,flight::Metric::ParentId,0);}
    else if(matched){
        flight::AddUnsigned(e,flight::Metric::ParentKind,n.motion.parentKind);
        flight::AddUnsigned(e,flight::Metric::ParentId,n.motion.parentId);
        if(observedGeneration)flight::AddUnsigned(e,flight::Metric::ParentGeneration,observedGeneration);
    }
    (void)flight::Record(e);
    if(matched)n.diagnosticAttachmentPending=false;
}
struct ObjectRemovalDiagnostic {
    flight::Context context{};int handle=-1;CObject*expected=nullptr;bool alive=false,adopted=false;
    uint16_t model=0,unavailableReason=OD_CONTEXT;uint8_t owner=255;flight::NativeAdmission admission=flight::NativeAdmission::Disabled;
    ObjectRemovalDiagnostic(const NetObject&n,flight::Context c):context(c),handle(n.handle),expected(n.obj),adopted(n.adopted),model(n.model),owner(n.diagnosticOwner){
        if(c.msg&&flight::Enabled()&&ObjectDiagnosticSourceCurrent(n,c)){
            admission=flight::AdmitNativeObservation(c,662,Net().MyId(),true);unavailableReason=admission==flight::NativeAdmission::Busy?OD_OBSERVATION_BUSY:OD_OBSERVATION_BUDGET;
            if(admission==flight::NativeAdmission::Admitted)alive=ObjectDiagnosticBody(n);
        }
    }
    void Complete(bool requested)const{
        if(!context.msg||!flight::Enabled())return;
        flight::Event e{};e.context=context;e.stage=662;e.localPeer=Net().MyId();e.owner=owner;e.detailKnown|=flight::OwnerKnown;
        flight::AddUnsigned(e,flight::Metric::EntityKind,ENTITY_OBJECT);flight::AddSigned(e,flight::Metric::RequestedModel,model);
        if(handle>=0)e.nativeHandle=(uint32_t)handle;
        if(admission!=flight::NativeAdmission::Admitted){e.reason=unavailableReason;(void)flight::Record(e);return;}
        if(!alive){e.reason=OD_ALREADY_MISSING;(void)flight::Record(e);return;}
        auto*after=GetObject(handle);bool present=after&&after==expected&&IsObjectValid(after)&&GetObjectRef(after)==handle;
        e.reason=!requested?(present?OD_RETAINED:OD_ALREADY_MISSING):present?(adopted?OD_ADOPTED:OD_DELETE_FAILED):OD_OK;
        flight::AddSigned(e,flight::Metric::NativeResult,present?1:0);
        if(present){flight::AddSigned(e,flight::Metric::ObservedModel,after->ModelIndex());flight::AddUnsigned(e,flight::Metric::NativeFlags,ObjectFlags(after));}
        (void)flight::Record(e);
    }
};
