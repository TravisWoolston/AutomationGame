#pragma once
#include "AutomationGameMinimal.h"
#include "OrbTypes.generated.h"

UENUM(BlueprintType)
enum class EOrbType : uint8 {
	None = 0, BaseA, BaseB, BaseC, MetalA, CatalystX
};

UENUM(BlueprintType)
enum class EOrbColor : uint8 {
	None = 0, White, Red, Green, Blue, Yellow, Cyan, Magenta
};

/** Single orb (atom-like piece). */
USTRUCT(BlueprintType)
struct FOrb
{
	GENERATED_BODY();
	UPROPERTY(EditAnywhere, BlueprintReadWrite) EOrbType  Type = EOrbType::None;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) EOrbColor Color = EOrbColor::White;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) uint8 Kind = 0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = 0, ClampMax = 5))
	uint8 Rot60 = 0;
};

USTRUCT(BlueprintType)
struct FOrbBond
{
	GENERATED_BODY()
	UPROPERTY(EditAnywhere, BlueprintReadWrite) int32 OrbA = 0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) int32 OrbB = 0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) uint8 BondType = 0;
};

/** Data for an orb inside a grabbed cluster (includes integer axial delta). */
USTRUCT(BlueprintType)
struct FOrbData
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite) uint8 Type = 0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) uint8 Color = 0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) FVector2D Offset = FVector2D::ZeroVector;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) int32 dQ = 0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) int32 dR = 0;
	// NEW: original item Id for this orb (preserved while held so bonds stay valid)
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly) int32 ItemId = 0;
};

/** Item on a tile: either a single orb or a bonded cluster. */
USTRUCT(BlueprintType)
struct AUTOMATIONGAME_API FItem
{
	GENERATED_BODY();
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly) int32 Id = 0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) bool bIsCluster = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) FOrb Single;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) TArray<FOrbData> Orbs;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) TArray<FOrbBond> Bonds;

	// NEW: facing direction (0..5) of the arm at the moment this cluster was grabbed.
	// Singles ignore this (value stays -1). Used so drop rotates relative, not absolute.
	UPROPERTY()
	int8 GrabFacing = -1;

	// NEW: flag to track if this item has been moved by belts this tick (prevents double-movement)
	UPROPERTY()
	bool LocationUpdatedThisTick = false;

	// NEW: flag to track if this item is ready to be pulled from bond/unbond tiles
	// Set to false when an orb is pushed onto a bond/unbond tile by a belt
	// Set to true when a belt tries to pull but the orb isn't ready (allowing one tick on the tile)
	UPROPERTY()
	bool bIsReady = true;

	static FItem MakeSingle(const FOrb& O) { FItem I; I.bIsCluster = false; I.Single = O; return I; }
};