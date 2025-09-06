#pragma once

#include "AutomationGameMinimal.h"
#include "Containers/Set.h"
#include "Sim/HexTypes.h"
#include "Sim/Tiles.h"
#include "Sim/ArmTypes.h"
#include "Sim/OrbTypes.h"
#include "Sim/HexCoord.h"
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
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sim") float FixedStepSec = 0.20f; // 5 Hz

	// ---- Arm <-> Item API ----
	UFUNCTION(BlueprintCallable, Category = "Sim|Arm")
	bool TryArmPickItem(const FHex& From, FItem& OutItem);
	UFUNCTION(BlueprintCallable, Category = "Sim|Arm")
	bool TryArmPlaceItem(const FHex& To, const FItem& InItem);
	UFUNCTION(BlueprintCallable, Category = "Sim|Arm")
	bool PeekItemAt(const FHex& H, FItem& OutItem) const;

	// ---- Orbs overlay ----
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
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Sim") TMap<FHex, FItem> ItemMap;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Sim") TArray<FArmState> Arms;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Sim") int32 OutputCount = 0;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Sim") int32 TileRevision = 0;

	// World lifecycle
	UFUNCTION(BlueprintCallable, Category = "Sim") void Initialize(int32 Radius = 20);
	UFUNCTION(BlueprintCallable, Category = "Sim") void StepSim();

	// Build API
	UFUNCTION(BlueprintCallable, Category = "Build") bool  PlaceTile(const FHex& H, const FTile& T);
	UFUNCTION(BlueprintCallable, Category = "Build") int32 AddArm(const FArmState& Arm);

	// Item helpers
	bool TryPutItem(const FHex& H, const FItem& I);
	bool TryTakeItem(const FHex& H, FItem& Out);

public:
	// ----- Visual interpolation (belt smoothing) -----
	void SetFrameAlpha(float InAlpha) { FrameAlpha = FMath::Clamp(InAlpha, 0.f, 0.9999f); }
	bool GetItemVisualLocation(int32 ItemId, FVector& OutWorld) const;

	int32 GetStepCounter() const { return StepCounter; }
	void SetBeltSpeed(float TilesPerTick) { BeltSpeedTilesPerTick = FMath::Max(0.f, TilesPerTick); }

	// External world transform (optional)
	void SetWorldTransform(const FVector& InAnchor, float InGridYawDeg, EHexOrientation InOrientation)
	{
		WorldAnchor = InAnchor;
		GridYawDeg = InGridYawDeg;
		GridOrientation = InOrientation;
	}

private:
	float FrameAlpha = 0.f;
	int32 StepCounter = 0;

	// Cached world transform
	FVector WorldAnchor = FVector::ZeroVector;
	float   GridYawDeg = 0.f;
	EHexOrientation GridOrientation = EHexOrientation::PointyTop;

	FVector HexCenterToWorld(const FHex& H) const;

private:
	// One-frame skip sets
	TSet<FHex> SkipInputThisFrame;
	TSet<FHex> SkipOutputThisFrame;

	// UI overlay
	UPROPERTY() TMap<FHex, uint8> OrbAt;

	// Item Id generator
	int32 NextItemId = 1;

	// Core step pieces
	void StepArms();
	void StepBelts();
	void StepGlyphs();
	void StepBonds();

	static uint8 KindFromItem(const FItem& /*Item*/) { return (uint8)EOrbType::BaseA; }

	// Belt progress
	UPROPERTY()
	TMap<int32, float> BeltProgress; // ItemId -> progress [0,1)

	float BeltSpeedTilesPerTick = 1.0f;
	float BeltBlockedClamp = 0.98f;

private:
	struct FBond
	{
		int32 ItemA = 0;
		int32 ItemB = 0;
		FHex   Root;
		int8   dQ = 0;
		int8   dR = 0;
		bool   bActive = true;
	};

	TArray<FBond> Bonds;

	// Multi-bond support: each item can appear in many bonds.
	TMap<int32, TArray<int32>> ItemToBonds; // ItemId -> list of bond indices

	void TryFormBondAt(const FHex& Root, uint8 Dir, const TSet<int32>& HeldIds, TSet<int32>& NewlyBondedItems);
	void CleanupBrokenBonds(const TSet<int32>& HeldIds);
	void RemoveBondIndex(int32 Index);

	// Helpers for multi-bond bookkeeping
	void AddItemBondLink(int32 ItemId, int32 BondIndex);
	void RemoveItemBondLink(int32 ItemId, int32 BondIndex);

	UPROPERTY(EditAnywhere, Category = "Debug")
	bool bDevDebugBonds = true;

	// Held items last board position (for future bond persistence if needed)
	TMap<int32, FHex> HeldItemLastHex;

private:
	// ---- Bond control (glyphs will call these later) ----
	UFUNCTION(BlueprintCallable, Category="Sim|Bond")
	bool BreakBondBetween(int32 ItemA, int32 ItemB);

	UFUNCTION(BlueprintCallable, Category="Sim|Bond")
	int32 BreakAllBondsForItem(int32 ItemId);

	bool IsItemBonded(int32 ItemId) const
	{
		if (const TArray<int32>* L = ItemToBonds.Find(ItemId))
			return L->Num() > 0;
		return false;
	}
};
