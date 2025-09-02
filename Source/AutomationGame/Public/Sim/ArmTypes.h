#pragma once
#include "AutomationGameMinimal.h"
#include "Sim/HexTypes.h"
#include "Sim/OrbTypes.h"
#include "ArmTypes.generated.h"

UENUM(BlueprintType)
enum class ECmd : uint8 {
	Grab, Drop,
	RotateL, RotateR,
	PivotL, PivotR,
	MoveNeg, MovePos,
	Extend, Retract,
	Wait,
	Reset,
	Repeat
};

USTRUCT(BlueprintType)
struct AUTOMATIONGAME_API FOp
{
	GENERATED_BODY();
	UPROPERTY(EditAnywhere, BlueprintReadWrite) ECmd  Cmd = ECmd::Wait;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) uint8 Arg = 0; // Wait count, Repeat count, etc.
};

USTRUCT(BlueprintType)
struct AUTOMATIONGAME_API FArmState
{
	GENERATED_BODY();
	UPROPERTY(EditAnywhere, BlueprintReadWrite) TArray<FOp> Tape;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly) int32  PC = 0;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly) uint8  WaitCounter = 0;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly) bool   bHolding = false;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly) FItem  Held;

	UPROPERTY(EditAnywhere, BlueprintReadWrite) FHex    BaseHex;     // base tile
	UPROPERTY(EditAnywhere, BlueprintReadWrite) EHexDir Facing = EHexDir::D0; // base rotation
	UPROPERTY(EditAnywhere, BlueprintReadWrite) int32 TrackIdx = 0; // on-track index
	UPROPERTY(EditAnywhere, BlueprintReadWrite) int32 ArmLen = 1; // piston length

	// Home pose (for Reset)
	UPROPERTY(EditAnywhere, BlueprintReadWrite) int32 HomeTrackIdx = 0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) EHexDir HomeFacing = EHexDir::D0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) int32 HomeArmLen = 1;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly)
	int32 ArmIndex = -1;
};
