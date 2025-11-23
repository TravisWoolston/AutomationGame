#pragma once
#include "CoreMinimal.h"
#include "Sim/HexTypes.h"
#include "Sim/HexCoord.h"
#include "Sim/Tiles.h"
#include "Actors/BasicArm.h"  // FArmInstr
#include "BlueprintTile.generated.h"

USTRUCT(BlueprintType)
struct AUTOMATIONGAME_API FBlueprintTile
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FHex LocalHex;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	ETileKind TileType;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	uint8 TileDirection;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	bool bIsArm = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	uint8 ArmFacing = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	int32 ArmReach = 1;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = 0, ClampMax = 5))
	uint8 StartingRotation = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = 1))
	int32 StartingReach = 1;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	TArray<FArmInstr> ArmProgram;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	bool bArmLoopProgram = true;

	FBlueprintTile()
		: LocalHex(0, 0), TileType(ETileKind::Input), TileDirection(0), bIsArm(false), ArmFacing(0), ArmReach(1), StartingRotation(0), StartingReach(1), bArmLoopProgram(true) {}

	FBlueprintTile(FHex InLocalHex, ETileKind InTileType, uint8 InDirection)
		: LocalHex(InLocalHex), TileType(InTileType), TileDirection(InDirection), bIsArm(false), ArmFacing(0), ArmReach(1), StartingRotation(0), StartingReach(1), bArmLoopProgram(true) {}

	FBlueprintTile(FHex InLocalHex, uint8 InArmFacing, int32 InArmReach, const TArray<FArmInstr>& InArmProgram, bool bInLoopProgram)
		: LocalHex(InLocalHex), TileType(ETileKind::Empty), TileDirection(0), bIsArm(true), ArmFacing(InArmFacing), ArmReach(InArmReach), StartingRotation(InArmFacing), StartingReach(InArmReach), ArmProgram(InArmProgram), bArmLoopProgram(bInLoopProgram) {}

	FBlueprintTile(FHex InLocalHex, uint8 InArmFacing, int32 InArmReach, uint8 InStartingRotation, int32 InStartingReach, const TArray<FArmInstr>& InArmProgram, bool bInLoopProgram)
		: LocalHex(InLocalHex), TileType(ETileKind::Empty), TileDirection(0), bIsArm(true), ArmFacing(InArmFacing), ArmReach(InArmReach), StartingRotation(InStartingRotation), StartingReach(InStartingReach), ArmProgram(InArmProgram), bArmLoopProgram(bInLoopProgram) {}
};

