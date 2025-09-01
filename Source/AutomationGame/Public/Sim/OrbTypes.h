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
	/** Kind/category of orb (resource type). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite) EOrbType  Type = EOrbType::None;
	/** Display/processing color. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite) EOrbColor Color = EOrbColor::White;
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	uint8 Kind = 0;
	/** Orientation on hex grid in 60° steps (0..5). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = 0, ClampMax = 5))
	uint8 Rot60 = 0;
};

USTRUCT(BlueprintType)
struct FOrbBond
{
	GENERATED_BODY()
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	int32 OrbA = 0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	int32 OrbB = 0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	uint8 BondType = 0; // e.g. single, double, etc.
};

USTRUCT(BlueprintType)
struct FOrbData
{
	GENERATED_BODY()
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	int32 Id = 0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	uint8 Type = 0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	uint8 Color = 0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FVector2D Offset; // Position relative to cluster origin
};

/** Item on a tile: either a single orb or a bonded cluster. */
USTRUCT(BlueprintType)
struct AUTOMATIONGAME_API FItem
{
	GENERATED_BODY();
	/** Unique identity for render interpolation across steps. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly) int32 Id = 0;

	/** If false, use Single; if true, use Orbs+Bonds. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite) bool bIsCluster = false;

	/** Single-orb payload when not a cluster. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite) FOrb Single;

	/** Cluster payload (indices correspond to Bonds). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite) TArray<FOrbData> Orbs; // One or more orbs

	/** Bonds tying orbs in the cluster. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite) TArray<FOrbBond> Bonds; // Bonds between orbs

	/** Helper to construct a single-orb item. */
	static FItem MakeSingle(const FOrb& O) { FItem I; I.bIsCluster = false; I.Single = O; return I; }
};