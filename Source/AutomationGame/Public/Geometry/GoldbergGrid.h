#pragma once

#include "CoreMinimal.h"
#include "Sim/HexTypes.h"
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
	/** Build the Goldberg grid from icosahedron subdivision. */
	void Build(int32 Frequency, float Radius, const FVector& Origin);
	
	/** Build the hex-to-cell mapping after Build(). Assigns Q,R based on angular position. */
	void BuildHexMapping();
	
	/** Get all cells. */
	const TArray<FGoldbergCell>& GetCells() const { return Cells; }
	
	/** Get subdivision frequency. */
	int32 GetFrequency() const { return CachedFrequency; }
	
	/** Get sphere radius. */
	float GetRadius() const { return CachedRadius; }
	
	/** Get sphere origin. */
	FVector GetOrigin() const { return CachedOrigin; }
	
	/** Get cell index for a flat hex, returns INDEX_NONE if invalid/pentagon. */
	int32 GetCellIndexForHex(const FHex& Hex) const;
	
	/** Get hex coordinates for a cell index. Returns FHex(0,0) if invalid. */
	FHex GetHexForCellIndex(int32 CellIndex) const;
	
	/** Check if a hex position is valid (exists and is not a pentagon). */
	bool IsValidHex(const FHex& Hex) const;
	
	/** Get cell world rotation (surface normal as up). */
	FRotator GetCellRotation(int32 CellIndex) const;
	
	/** Get total valid (non-pentagon) cell count. */
	int32 GetValidCellCount() const;

private:
	// Generated cells
	UPROPERTY() TArray<FGoldbergCell> Cells;
	
	// Maps flat FHex coordinates to Goldberg cell indices
	UPROPERTY() TMap<FHex, int32> HexToCellMap;
	
	// Reverse mapping for lookups
	UPROPERTY() TMap<int32, FHex> CellToHexMap;
	
	int32 CachedFrequency = 0;
	float CachedRadius = 0.f;
	FVector CachedOrigin = FVector::ZeroVector;
	int32 CachedValidCellCount = 0;
};
