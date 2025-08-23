#pragma once
#include "AutomationGameMinimal.h"
#include "Engine/DataAsset.h"
#include "Sim/HexTypes.h"
#include "Sim/Tiles.h"
#include "Sim/ArmTypes.h"
#include "OrbBlueprintAssets.generated.h"

/** Pair-like struct so UHT/UPROPERTY can handle it (TPair is not UHT-reflectable). */
USTRUCT(BlueprintType)
struct AUTOMATIONGAME_API FHexTile
{
    GENERATED_BODY();

    UPROPERTY(EditAnywhere, BlueprintReadWrite) FHex  Hex;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) FTile Tile;
};

/** Filter for blueprint ports (what items this port accepts/produces). */
USTRUCT(BlueprintType)
struct AUTOMATIONGAME_API FPortFilter
{
    GENERATED_BODY();
    UPROPERTY(EditAnywhere, BlueprintReadWrite) bool bAnyType = true;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) TArray<EOrbType>  AllowedTypes;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) TArray<EOrbColor> AllowedColors;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) int32 MinCharge = -2;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) int32 MaxCharge =  2;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) uint8 MinSize   = 1;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) uint8 MaxSize   = 8;
};

USTRUCT(BlueprintType)
struct AUTOMATIONGAME_API FBlueprintPort
{
    GENERATED_BODY();
    UPROPERTY(EditAnywhere, BlueprintReadWrite) bool bIsInput = true;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) FHex  LocalHex; // position in local blueprint space
    UPROPERTY(EditAnywhere, BlueprintReadWrite) uint8 LocalDir = 0; // 0..5
    UPROPERTY(EditAnywhere, BlueprintReadWrite) FPortFilter Filter;
};

/** Authoring asset: local hex tiles + arms + glyphs + ports */
UCLASS(BlueprintType)
class AUTOMATIONGAME_API UOrbBlueprintDefinition : public UDataAsset
{
    GENERATED_BODY()
public:
    UPROPERTY(EditAnywhere, BlueprintReadWrite) TArray<FHexTile>  LocalTiles;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) TArray<FArmState> LocalArms;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) TArray<FBlueprintPort> Ports;
};

/** Compiled blueprint: flattened and validated for runtime placement. */
UCLASS(BlueprintType)
class AUTOMATIONGAME_API UOrbBlueprintCompiled : public UObject
{
    GENERATED_BODY()
public:
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly) TArray<FHexTile>  Tiles;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly) TArray<FArmState> Arms;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly) TArray<FBlueprintPort> Ports;
    // Later: baked reservations, pre-expanded tapes, port adjacency caches.
};
