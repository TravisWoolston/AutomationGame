#pragma once
#include "AutomationGameMinimal.h"
#include "Containers/Set.h"
#include "Sim/HexTypes.h"
#include "Sim/Tiles.h"
#include "Sim/ArmTypes.h"
#include "Sim/OrbTypes.h"
#include "UObject/Object.h"
#include "SimWorldHex.generated.h"

/** Authoritative deterministic hex simulation world. */
UCLASS(BlueprintType)
class AUTOMATIONGAME_API USimWorldHex : public UObject
{
    GENERATED_BODY()
public:
    // Config
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Sim") float TileSizeUU   = 120.f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Sim") float FixedStepSec = 0.20f; // 5 Hz for clarity

    // Maps
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Sim") TMap<FHex, FTile> TileMap;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Sim") TMap<FHex, FItem> ItemMap; // at most one per tile for MVP
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Sim") TArray<FArmState> Arms;

    // Debug counters
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Sim") int32 OutputCount = 0;

    // Revision counter (increments whenever tiles change) so renderers can rebuild.
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Sim") int32 TileRevision = 0;

    UFUNCTION(BlueprintCallable, Category="Sim") void Initialize(int32 Radius=20);
    UFUNCTION(BlueprintCallable, Category="Sim") void StepSim(); // one fixed step

    // Build API
    UFUNCTION(BlueprintCallable, Category="Build") bool  PlaceTile(const FHex& H, const FTile& T);
    UFUNCTION(BlueprintCallable, Category="Build") int32 AddArm(const FArmState& Arm);

    // Item API
    bool TryPutItem(const FHex& H, const FItem& I);
    bool TryTakeItem(const FHex& H, FItem& Out);

private:
    // One-frame grace so Input doesn't instantly respawn and Output doesn't instantly consume after an Arm action
    TSet<FHex> SkipInputThisFrame;
    TSet<FHex> SkipOutputThisFrame;

    // Identity generator for items
    int32 NextItemId = 1;

    void StepArms();
    void StepBelts();   // move items along directional belts
    void StepGlyphs();
};
