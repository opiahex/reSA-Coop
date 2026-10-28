// reSA-Coop Diagnostic System
// Copyright (c) 2026 opiahex
// Original source: https://github.com/opiahex/reSA-Coop
// SPDX-License-Identifier: LicenseRef-reSA-Coop-Diagnostics-Attribution-1.0
// See DIAGNOSTICS-LICENSE.txt, DIAGNOSTICS-NOTICE.md and DIAGNOSTICS-COMPONENTS.md.

#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdarg>
#include <cstdio>
namespace coop::ui::diagnostic {
constexpr size_t kNativeRows=16,kNativeText=384;
enum class NativeKind:uint8_t { Network,LocalCar,Remote };
struct NativeRow {
    std::array<char,kNativeText> text{};
    uint8_t peer=255;
    uint16_t entity=0;
    bool peerKnown=false,entityKnown=false;
    NativeKind kind=NativeKind::Remote;
};
// Owns only copied values/text; no native pointers, handles or borrowed strings.
struct NativeSnapshot {
    std::array<NativeRow,kNativeRows> rows{};
    size_t count=0;
    bool truncated=false;
    void Add(NativeKind kind,uint8_t peer,bool entityKnown,uint16_t entity,const char*fmt,...){
        if(count==rows.size()){truncated=true;return;}
        auto&r=rows[count++];r={};r.kind=kind;r.peer=peer;r.peerKnown=peer!=255;r.entityKnown=entityKnown;r.entity=entity;
        va_list ap;va_start(ap,fmt);int size=std::vsnprintf(r.text.data(),r.text.size(),fmt,ap);va_end(ap);
        if(size<0){r.text[0]=0;truncated=true;}else if(size>=int(r.text.size()))truncated=true;
    }
};
// Called only by the existing owning-game-thread 250 ms snapshot gate. The
// independent thread check happens before any native/game/session access.
bool CaptureNativeSnapshot(NativeSnapshot&out,uint32_t owningThread);
}
