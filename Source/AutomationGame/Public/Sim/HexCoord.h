#pragma once
#include "AutomationGameMinimal.h"
#include "HexCoord.generated.h"

UENUM(BlueprintType)
enum class EHexOrientation : uint8
{
    PointyTop UMETA(DisplayName="Pointy-Top (vertices up/down)"),
    FlatTop   UMETA(DisplayName="Flat-Top (edges up/down)")
};

USTRUCT()
struct FHexAxial
{
    GENERATED_BODY()
    int32 Q=0;
    int32 R=0;
    FHexAxial(){}
    FHexAxial(int32 InQ,int32 InR):Q(InQ),R(InR){}
};

namespace HexCoord
{
    // Rotate a 2D vector by degrees (Z-up)
    inline FVector2D Rotate2D(const FVector2D& V, float Deg)
    {
        const double Rad = FMath::DegreesToRadians(Deg);
        const double C = FMath::Cos(Rad);
        const double S = FMath::Sin(Rad);
        return FVector2D(V.X * C - V.Y * S, V.X * S + V.Y * C);
    }

    // Axial -> world XY (local, before yaw) using tileSize as "radius"
    inline FVector2D AxialToXY(const FHexAxial& A, float TileSize, EHexOrientation Ori)
    {
        if (Ori == EHexOrientation::PointyTop)
        {
            // pointy: x = size*sqrt(3)*(q + r/2), y = size*3/2*r
            const float x = TileSize * 1.7320508075688772f * (A.Q + A.R * 0.5f);
            const float y = TileSize * 1.5f * A.R;
            return FVector2D(x, y);
        }
        else
        {
            // flat: x = size*3/2*q, y = size*sqrt(3)*(r + q/2)
            const float x = TileSize * 1.5f * A.Q;
            const float y = TileSize * 1.7320508075688772f * (A.R + A.Q * 0.5f);
            return FVector2D(x, y);
        }
    }

    // world XY -> axial (float) for pointy
    inline FVector2D XYToAxialF_Pointy(const FVector2D& P, float TileSize)
    {
        const float q = (P.X * 0.5773502691896258f - P.Y * 0.3333333333333333f) / TileSize; // 1/sqrt(3), 1/3
        const float r = (P.Y * 0.6666666666666666f) / TileSize;                              // 2/3
        return FVector2D(q, r);
    }
    // world XY -> axial (float) for flat
    inline FVector2D XYToAxialF_Flat(const FVector2D& P, float TileSize)
    {
        const float q = (P.X * 0.6666666666666666f) / TileSize;                              // 2/3
        const float r = (-P.X * 0.3333333333333333f + P.Y * 0.5773502691896258f) / TileSize; // -1/3, 1/sqrt(3)
        return FVector2D(q, r);
    }

    inline FHexAxial CubeRound(float qf, float rf)
    {
        const float sf = -qf - rf;
        int32 qi = FMath::RoundToInt(qf);
        int32 ri = FMath::RoundToInt(rf);
        int32 si = FMath::RoundToInt(sf);

        const float qd = FMath::Abs(qi - qf);
        const float rd = FMath::Abs(ri - rf);
        const float sd = FMath::Abs(si - sf);

        if (qd > rd && qd > sd)
            qi = -ri - si;
        else if (rd > sd)
            ri = -qi - si;
        // else si corrected implicitly

        return FHexAxial(qi, ri);
    }

    inline FHexAxial XYToAxialRound(const FVector2D& P, float TileSize, EHexOrientation Ori)
    {
        FVector2D AF = (Ori == EHexOrientation::PointyTop) ? XYToAxialF_Pointy(P, TileSize)
                                                            : XYToAxialF_Flat(P, TileSize);
        return CubeRound(AF.X, AF.Y);
    }
}
