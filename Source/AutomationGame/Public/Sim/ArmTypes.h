#pragma once
#include "AutomationGameMinimal.h"
#include "Sim/HexTypes.h"
#include "Sim/OrbTypes.h"
#include "ArmTypes.generated.h"

UENUM(BlueprintType)
enum class ECmd : uint8
{
	Wait,
	Grab,
	Drop,
	RotateL,
	RotateR,
	Extend,
	Retract,
	MovePos,
	MoveNeg,
	PivotL,
	PivotR,
	Reset,
	Repeat
};

USTRUCT(BlueprintType)
struct AUTOMATIONGAME_API FOp
{
	GENERATED_BODY()
	UPROPERTY(EditAnywhere, BlueprintReadWrite) ECmd Cmd = ECmd::Wait;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) int32 Arg = 0; // Wait count, Repeat count, etc.
};

USTRUCT(BlueprintType)
struct AUTOMATIONGAME_API FArmState
{
	GENERATED_BODY()

	// --- Core state ---
	UPROPERTY(EditAnywhere, BlueprintReadWrite) FHex    BaseHex;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) EHexDir Facing = (EHexDir)0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) EHexDir HomeFacing = (EHexDir)0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) int32   ArmLen = 1;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) int32   HomeArmLen = 1;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) int32   TrackIdx = 0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) int32   HomeTrackIdx = 0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) int32   PC = 0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) int32   WaitCounter = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite) bool    bHolding = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) FItem   Held;

	UPROPERTY(EditAnywhere, BlueprintReadWrite) TArray<FOp> Tape;

	// --- NEW: visual interpolation snapshot (captured at start of each StepArms) ---
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly) EHexDir PrevFacing = (EHexDir)0;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly) int32   PrevArmLen = 1;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly) int32   PrevTrackIdx = 0;
};
