#pragma once
#include "AutomationGameMinimal.h"
#include "HexTypes.generated.h"

UENUM(BlueprintType)
enum class EHexDir : uint8 { D0 = 0, D1, D2, D3, D4, D5 };

/** Axial hex coordinate (q, r). */
USTRUCT(BlueprintType)
struct AUTOMATIONGAME_API FHex
{
	GENERATED_BODY();

	UPROPERTY(EditAnywhere, BlueprintReadWrite) int32 Q = 0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) int32 R = 0;

	FHex() {}
	FHex(int32 InQ, int32 InR) : Q(InQ), R(InR) {}

	bool operator==(const FHex& Other) const { return Q == Other.Q && R == Other.R; }
	bool operator!=(const FHex& Other) const { return !(*this == Other); }
};

FORCEINLINE uint32 GetTypeHash(const FHex& H)
{
	return HashCombine(::GetTypeHash(H.Q), ::GetTypeHash(H.R));
}

namespace HexUtil
{
	// Axial direction vectors (pointy-top convention)
	static const FHex Dir[6] = { FHex(1,0), FHex(1,-1), FHex(0,-1), FHex(-1,0), FHex(-1,1), FHex(0,1) };

	FORCEINLINE FHex Neighbor(const FHex& H, EHexDir D)
	{
		const FHex& V = Dir[(uint8)D % 6];
		return FHex(H.Q + V.Q, H.R + V.R);
	}

	// World conversion (top-down pointy hexes). TileSize is center-to-center spacing.
	FORCEINLINE FVector ToWorld(const FHex& H, float TileSize)
	{
		const float x = TileSize * (FMath::Sqrt(3.f) * (float)H.Q + FMath::Sqrt(3.f) / 2.f * (float)H.R);
		const float y = TileSize * (3.f / 2.f * (float)H.R);
		return FVector(x, y, 0.f);
	}
}
