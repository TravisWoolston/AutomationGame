#pragma once

#include "CoreMinimal.h"
#include "GoldbergGrid.generated.h"

/** Cell on a Goldberg (geodesic) sphere: position + neighbor indices (5 for pentagon, else up to 6). */
USTRUCT()
struct FGoldbergCell
{
	GENERATED_BODY()
	/** Center position (world space). */
	UPROPERTY() FVector Position = FVector::ZeroVector;
	/** Neighbor indices (size 5 or 6). */
	UPROPERTY() TArray<int32> Neighbors;
	/** True if this cell is a pentagon (degree 5). */
	UPROPERTY() bool bPentagon = false;
};

/** Simple runtime-generated Goldberg polyhedron (dual of subdivided icosahedron). */
UCLASS()
class AUTOMATIONGAME_API UGoldbergGrid : public UObject
{
	GENERATED_BODY()
public:
	void Build(int32 Frequency, float Radius, const FVector& Origin);
	const TArray<FGoldbergCell>& GetCells() const { return Cells; }
	int32 GetFrequency() const { return CachedFrequency; }
	float GetRadius() const { return CachedRadius; }
private:
	// Generated cells
	UPROPERTY() TArray<FGoldbergCell> Cells;
	int32 CachedFrequency = 0;
	float CachedRadius = 0.f;
	FVector CachedOrigin = FVector::ZeroVector;
};
