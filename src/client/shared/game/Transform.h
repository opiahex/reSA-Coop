#pragma once
#include "shared/game/Game.h"
namespace game {
inline float Dot(const CVector&a,const CVector&b){return a.x*b.x+a.y*b.y+a.z*b.z;}
inline CVector TransformPoint(const CMatrix&m,const CVector&p){return m.pos+m.right*p.x+m.forward*p.y+m.up*p.z;}
inline CVector UntransformPoint(const CMatrix&m,const CVector&p){auto d=p-m.pos;return {Dot(d,m.right),Dot(d,m.forward),Dot(d,m.up)};}
inline bool Finite(const CVector&v){return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z);}
}
