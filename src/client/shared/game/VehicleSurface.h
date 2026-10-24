#pragma once
#include <cmath>

#include "shared/game/Game.h"

namespace game {
// The native collision layouts and entry points are for SA 1.0 US:
// CColLine (0x20), CColPoint (0x2C), CEntity::GetColModel (0x535300),
// CCollision::ProcessLineOfSight (0x417950). Query the specific vehicle's
// collision mesh: a contact pointer or a bounding box is not a roof.
inline bool VehicleSurfaceBelow(CVehicle* vehicle, const CMatrix& matrix,
                                const CVector& centre, CVector* surface = nullptr) {
    if (!vehicle || !(vehicle->Flags() & 1u)) return false;
    auto* model = CallMethod<0x535300, void*>(vehicle);
    if (!model || !Field<void*>(model, 0x2C)) return false;
    struct Line { CVector start; float startSize; CVector end; float endSize; };
    struct Point { CVector position; float pad; CVector normal; float pad2; uint8_t tail[12]; };
    static_assert(sizeof(Line) == 0x20 && sizeof(Point) == 0x2C);
    Line line{centre + CVector{0, 0, 0.1f}, 0, centre - CVector{0, 0, 1.45f}, 0};
    Point hit{};
    float fraction = 1.0f;
    if (!Call<0x417950, bool>(&line, &matrix, model, &hit, &fraction, false, false)) return false;
    float gap = centre.z - hit.position.z;
    if (!std::isfinite(gap) || gap < 0.55f || gap > 1.4f || hit.normal.z < 0.5f) return false;
    if (surface) *surface = hit.position;
    return true;
}
}  // namespace game
