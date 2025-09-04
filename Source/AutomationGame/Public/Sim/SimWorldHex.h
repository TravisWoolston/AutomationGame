#pragma once

#include "AutomationGameMinimal.h"
#include "Containers/Set.h"
#include "Sim/HexTypes.h"
#include "Sim/Tiles.h"
#include "Sim/ArmTypes.h"
#include "Sim/OrbTypes.h"
#include "Sim/HexCoord.h"              // <-- Added for EHexOrientation
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

	// --- Visual interpolation helpers (read-only) ---
// --- Visual interpolation helpers (read-only) ---
public:
	// ----- Visual interpolation (belt smoothing) -----
	void SetFrameAlpha(float InAlpha) { FrameAlpha = FMath::Clamp(InAlpha, 0.f, 0.9999f); }
	bool GetItemVisualLocation(int32 ItemId, FVector& OutWorld) const
	{
		// Find owning hex
		const FHex* FoundHex = nullptr;
		for (const auto& KV : ItemMap)
		{
			if (KV.Value.Id == ItemId)
			{
				FoundHex = &KV.Key;
				break;
			}
		}
		if (!FoundHex)
		{
			return false;
		}

		const FHex CurrentHex = *FoundHex;
		const FVector A = HexCenterToWorld(CurrentHex);

		// Not on a belt -> static
		const FTile* Tile = TileMap.Find(CurrentHex);
		if (!Tile || Tile->Kind != ETileKind::Belt)
		{
			OutWorld = A;
			return true;
		}

		const float* PLogicPtr = BeltProgress.Find(ItemId);
		float LogicalP = PLogicPtr ? *PLogicPtr : 0.f;

		const bool bBlockedEdge = LogicalP >= BeltBlockedClamp - 0.0001f;
		float VisualP = LogicalP;
		if (!bBlockedEdge)
		{
			VisualP = FMath::Min(BeltBlockedClamp - 0.0001f,
				LogicalP + FrameAlpha * BeltSpeedTilesPerTick);
		}

		const uint8 Dir = (uint8)(Tile->Arg & 0xFF);
		const FHex NextHex = HexUtil::Neighbor(CurrentHex, (EHexDir)(Dir % 6));
		const FVector B = HexCenterToWorld(NextHex);

		OutWorld = FMath::Lerp(A, B, VisualP);
		return true;
	}

	// Incremented every StepSim so render layer can reliably detect a new fixed step
	int32 GetStepCounter() const { return StepCounter; }

	// Optional external config
	void SetBeltSpeed(float TilesPerTick) { BeltSpeedTilesPerTick = FMath::Max(0.f, TilesPerTick); }

private:
	float FrameAlpha = 0.f;
	int32 StepCounter = 0;

	// Cached for world conversion (set these externally if you have a driver; defaults keep old behavior)
public:
	// Call these from your driver each frame / on change (optional)
	void SetWorldTransform(const FVector& InAnchor, float InGridYawDeg, EHexOrientation InOrientation)
	{
		WorldAnchor = InAnchor;
		GridYawDeg = InGridYawDeg;
		GridOrientation = InOrientation;
	}
private:
	FVector WorldAnchor = FVector::ZeroVector;
	float   GridYawDeg = 0.f;
	EHexOrientation GridOrientation = EHexOrientation::PointyTop;

	FVector HexCenterToWorld(const FHex& H) const
	{
		const float Tile = TileSizeUU;
		const EHexOrientation Ori = GridOrientation;
		const float Sqrt3 = 1.7320508075688772f;

		const float q = (float)H.Q;
		const float r = (float)H.R;

		FVector2D Local;
		if (Ori == EHexOrientation::PointyTop)
		{
			// pointy-top axial -> pixel
			Local.X = Tile * (Sqrt3 * q + (Sqrt3 * 0.5f) * r);
			Local.Y = Tile * (1.5f * r);
		}
		else
		{
			// flat-top axial -> pixel
			Local.X = Tile * (1.5f * q);
			Local.Y = Tile * (Sqrt3 * r + (Sqrt3 * 0.5f) * q);
		}

		// Apply grid yaw
		const float Rad = FMath::DegreesToRadians(GridYawDeg);
		const float CosA = FMath::Cos(Rad);
		const float SinA = FMath::Sin(Rad);
		const float RX = Local.X * CosA - Local.Y * SinA;
		const float RY = Local.X * SinA + Local.Y * CosA;

		return WorldAnchor + FVector(RX, RY, 0.f);
	}

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

	// Add near other member declarations
	struct FConveyorState
	{
		float Progress = 0.f;   // [0,1)
		uint8 Dir = 0;          // cached outgoing direction of the belt we are on
	};

	TMap<int32, FConveyorState> Conveyor;     // ItemId -> state

	// --- Continuous belt support ---
	UPROPERTY()
	TMap<int32, float> BeltProgress; // ItemId -> [0,1) progress along outgoing direction (toward forward neighbor)

	// Optional tunables (expose later)
	float BeltSpeedTilesPerTick = 1.0f;     // logical tiles advanced per simulation tick
	float BeltBlockedClamp = 0.98f;    // where to visually freeze when blocked near edge

	// Optional: future group scaffolding
	struct FAttachedGroup
	{
		int32 GroupId = -1;
		TArray<int32> ItemIds;      // member item ids
		TArray<FHex>   RelHexes;    // offsets relative to RootHex (same length as ItemIds)
		FHex RootHex;
		uint8 Dir = 0;
		float Progress = 0.f;
		bool bActive = true;
	};
	TArray<FAttachedGroup> Groups;
	TMap<int32, int32> ItemToGroup; // ItemId -> GroupId

private:
	struct FBond
	{
		int32 ItemA = 0;
		int32 ItemB = 0;
		FHex   Root;      // origin hex where Bond tile is placed
		int8   dQ = 0;    // ItemB.Q - ItemA.Q (axial delta when bonded)
		int8   dR = 0;    // ItemB.R - ItemA.R
		bool   bActive = true;
	};

	TArray<FBond> Bonds;
	TMap<int32, int32> ItemToBond; // ItemId -> Bonds index

	void TryFormOrUpdateBondAt(const FHex& Root, uint8 Dir, const TSet<int32>& HeldIds);
	void CleanupBrokenBonds(const TMap<int32, FHex>& ItemPos, const TSet<int32>& HeldIds);
};
