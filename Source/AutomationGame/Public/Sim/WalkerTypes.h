#pragma once
#include "AutomationGameMinimal.h"
#include "Sim/HexTypes.h"
#include "WalkerTypes.generated.h"

UENUM(BlueprintType)
enum class EWalkerCmd : uint8
{
	NoOp     UMETA(DisplayName = "NoOp"),
	MoveTo   UMETA(DisplayName = "MoveTo"),
	Reset    UMETA(DisplayName = "Reset to Start (macro)"),
	Wait     UMETA(DisplayName = "Wait (ticks = Arg)")
};

/** One instruction + small integer argument (target hex Q/R, wait ticks, etc.) */
USTRUCT(BlueprintType)
struct AUTOMATIONGAME_API FWalkerInstr
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	EWalkerCmd Cmd = EWalkerCmd::NoOp;

	/** For MoveTo: Q coordinate. For Wait: ticks. Ignored otherwise. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	int32 Arg1 = 0;

	/** For MoveTo: R coordinate. Ignored otherwise. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	int32 Arg2 = 0;

	// Added equality operators so TArray<FWalkerInstr>::operator== works
	bool operator==(const FWalkerInstr& Other) const
	{
		return Cmd == Other.Cmd && Arg1 == Other.Arg1 && Arg2 == Other.Arg2;
	}
	bool operator!=(const FWalkerInstr& Other) const
	{
		return !(*this == Other);
	}
};

USTRUCT(BlueprintType)
struct AUTOMATIONGAME_API FWalkerState
{
	GENERATED_BODY()

	// --- Core state ---
	UPROPERTY(EditAnywhere, BlueprintReadWrite) FHex    BaseHex;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) FHex    HomeHex;  // Starting location for Reset
	UPROPERTY(EditAnywhere, BlueprintReadWrite) FHex    TargetHex;  // Current movement target
	UPROPERTY(EditAnywhere, BlueprintReadWrite) int32   PC = 0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) int32   WaitCounter = 0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) bool    bMoving = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) float   MovementProgress = 0.0f;  // 0.0 to 1.0
	UPROPERTY(EditAnywhere, BlueprintReadWrite) float   TileTraverseRate = 1.0f;  // Tiles per tick

	UPROPERTY(EditAnywhere, BlueprintReadWrite) TArray<FWalkerInstr> Program;

	// Added equality operator so TArray<FWalkerState>::IndexOfByKey works
	bool operator==(const FWalkerState& Other) const
	{
		return BaseHex == Other.BaseHex && 
		       HomeHex == Other.HomeHex && 
		       TargetHex == Other.TargetHex &&
		       PC == Other.PC &&
		       WaitCounter == Other.WaitCounter &&
		       bMoving == Other.bMoving &&
		       FMath::IsNearlyEqual(MovementProgress, Other.MovementProgress) &&
		       FMath::IsNearlyEqual(TileTraverseRate, Other.TileTraverseRate) &&
		       Program == Other.Program;
	}
	bool operator!=(const FWalkerState& Other) const
	{
		return !(*this == Other);
	}
};

