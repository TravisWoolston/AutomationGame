#pragma once
#include "AutomationGameMinimal.h"
#include "Sim/HexTypes.h"

namespace HexPick
{
    // Convert world X,Y to axial hex assuming pointy-top layout with centers spaced by TileSizeUU.
    static inline FHex FromWorldXY(const FVector& World, float TileSizeUU)
    {
        const double x = World.X / TileSizeUU;
        const double y = World.Y / TileSizeUU;
        // cube coords for pointy
        const double qf = (2.0/3.0) * x;
        const double rf = (-1.0/3.0) * x + (1.7320508075688772/3.0) * y;
        const int32 q = FMath::RoundToInt(qf);
        const int32 r = FMath::RoundToInt(rf);
        return FHex(q, r);
    }
}
