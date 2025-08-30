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
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sim") float TileSizeUU = 120.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sim") float FixedStepSec = 0.20f; // 5 Hz for clarity

	// ---- Arm <-> Item API (Blueprint-safe names) ----
	/** Arm tries to pick an item at hex. If empty but the tile is an Input, synthesize one and succeed. */
	UFUNCTION(BlueprintCallable, Category = "Sim|Arm")
	bool TryArmPickItem(const FHex& From, FItem& OutItem);

	/** Arm tries to place an item at hex. If that hex is an Output, the item is consumed immediately and counted. */
	UFUNCTION(BlueprintCallable, Category = "Sim|Arm")
	bool TryArmPlaceItem(const FHex& To, const FItem& InItem);

	/** Read-only peek for render/UI. */
	UFUNCTION(BlueprintCallable, Category = "Sim|Arm")
	bool PeekItemAt(const FHex& H, FItem& OutItem) const;

	// ---- Orbs overlay (tile presence used by UI preview) ----
	UFUNCTION(BlueprintCallable, Category = "Sim|Orbs")
	bool TryPlaceOrbAt(const FHex& Hex, uint8 Kind);
	UFUNCTION(BlueprintCallable, Category = "Sim|Orbs")
	bool TryPickupOrbAt(const FHex& Hex, uint8& OutKind);
	UFUNCTION(BlueprintCallable, Category = "Sim|Arm")
	bool HasOrbAt(const FHex& H) const;
	UFUNCTION(BlueprintPure, Category = "Sim|Orbs")
	bool GetOrbKindAt(const FHex& Hex, uint8& OutKind) const;

	// Maps
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Sim") TMap<FHex, FTile> TileMap;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Sim") TMap<FHex, FItem> ItemMap; // at most one per tile for MVP
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Sim") TArray<FArmState> Arms;

	// Debug counters
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Sim") int32 OutputCount = 0;

	// Revision counter (increments whenever tiles change) so renderers can rebuild.
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Sim") int32 TileRevision = 0;

	// World lifecycle
	UFUNCTION(BlueprintCallable, Category = "Sim") void Initialize(int32 Radius = 20);
	UFUNCTION(BlueprintCallable, Category = "Sim") void StepSim(); // one fixed step

	// Build API
	UFUNCTION(BlueprintCallable, Category = "Build") bool  PlaceTile(const FHex& H, const FTile& T);
	UFUNCTION(BlueprintCallable, Category = "Build") int32 AddArm(const FArmState& Arm);

	// Low-level Item helpers (not exposed to BP to avoid overload collisions)
	bool TryPutItem(const FHex& H, const FItem& I);
	bool TryTakeItem(const FHex& H, FItem& Out);

private:
	// One-frame grace for StepSim() so Input/Output don’t instantly fire after arm actions
	TSet<FHex> SkipInputThisFrame;
	TSet<FHex> SkipOutputThisFrame;

	// Simple presence map for orb-kind previews (UI overlays)
	UPROPERTY() TMap<FHex, uint8> OrbAt;

	// Identity generator for items
	int32 NextItemId = 1;

	// Internal sim pieces
	void StepArms();
	void StepBelts();   // move items along directional belts
	void StepGlyphs();

	// Map an item to an orb preview kind (until you add multi-orb support)
	static uint8 KindFromItem(const FItem& /*Item*/) { return (uint8)EOrbType::BaseA; }
};
