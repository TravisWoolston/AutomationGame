#include "Sim/SimWorldHex.h"
#include "Engine/Engine.h"
#include "Sim/HexCoord.h"
#include "Actors/BasicArm.h"
#include "Actors/OrbSimDriver.h"
#include "EngineUtils.h"
#include "DrawDebugHelpers.h"
#include "AGLogChannels.h"

// Near top (after includes) add if not already in header:
static constexpr float DEFAULT_BELT_SPEED_TILES_PER_SECOND = 0.0f;
// Helper used only inside StepBonds to index held cluster sub‑orbs.
struct FHeldOrbRef
{
	int32 ArmIndex = -1;
	int32 SubIdx = -1;
};
// Prefer per‑second speed; if <=0 we fall back to legacy BeltSpeedTilesPerTick.
float BeltSpeedTilesPerSecond = DEFAULT_BELT_SPEED_TILES_PER_SECOND;
void USimWorldHex::Initialize(int32 /*Radius*/)
{
	UE_LOG(LogTemp, Warning, TEXT("SimWorldHex::Initialize() clearing %d arms from simulation"), Arms.Num());
	TileMap.Empty();
	ItemMap.Empty();
	Arms.Empty();
	OutputCount = 0;
	TileRevision = 0;
	NextItemId = 1;
	OrbAt.Empty();
	BeltProgress.Empty();
	Bonds.Empty();
	ItemToBonds.Empty();
	SimFacingByArm.Empty();
	SimArmLenByArm.Empty();
	SimGrabFacingByArm.Empty();
	PendingFacingByArm.Empty();
	PendingArmLenByArm.Empty();
	PendingGrabFacingByArm.Empty();
}




// ---------- NEW: Add missing definitions ----------

int32 USimWorldHex::AddArm(const FArmState& Arm)
{
	int32 NewIndex = Arms.Add(Arm);
	// Clamp / sanitize a few runtime fields (defensive)
	FArmState& S = Arms[NewIndex];
	S.PC = S.PC % FMath::Max(1, S.Tape.Num());
	S.WaitCounter = 0;
	// Ensure facing in 0..5
	S.Facing = (EHexDir)((uint8)S.Facing % 6);
	S.HomeFacing = (EHexDir)((uint8)S.HomeFacing % 6);
	S.ArmLen = FMath::Clamp<int32>(S.ArmLen <= 0 ? 1 : S.ArmLen, 1, 6);
	S.HomeArmLen = FMath::Clamp<int32>(S.HomeArmLen <= 0 ? 1 : S.HomeArmLen, 1, 6);
	// Initialize rotation completed to true (no rotation in progress)
	S.bRotationCompleted = true;

	// Seed sim state to current values
	SimFacingByArm.Add(NewIndex, static_cast<uint8>(S.Facing));
	SimArmLenByArm.Add(NewIndex, S.ArmLen);
	SimGrabFacingByArm.Add(NewIndex, int8(-1)); // no held cluster yet

	UE_LOG(LogTemp, Warning, TEXT("SimWorldHex::AddArm - Added arm at (%d,%d) facing=%d assigned SimIndex=%d (total arms: %d)"),
		Arm.BaseHex.Q, Arm.BaseHex.R, (int32)Arm.Facing, NewIndex, Arms.Num());

	return NewIndex;
}

bool USimWorldHex::TryPutItem(const FHex& H, const FItem& I)
{
	if (ItemMap.Contains(H))
	{
		return false; // already occupied
	}
	FItem Copy = I;
	// Assign an Id if caller forgot (StepGlyphs sets it, but be safe)
	if (Copy.Id == 0)
	{
		Copy.Id = NextItemId++;
	}

	// Items placed by arms should start with bIsReady = false to prevent immediate belt pulling
	// This gives items at least one tick to settle before being moved by belts
	Copy.bIsReady = false;

	// Special case: Check if placing on a bond/unbond tile area
	const FTile* Tile = TileMap.Find(H);
	bool bIsOnBondArea = false;

	if (Tile && (Tile->Kind == ETileKind::Bond || Tile->Kind == ETileKind::Unbond))
	{
		// Placing directly on a primary bond/unbond tile
		bIsOnBondArea = true;
	}
	else
	{
		// Check if placing on a partner area of a bond/unbond glyph
		for (const auto& TileKV : TileMap)
		{
			const FTile& CheckTile = TileKV.Value;
			if (CheckTile.Kind != ETileKind::Bond && CheckTile.Kind != ETileKind::Unbond) continue;

			// Calculate this glyph's partner hex
			const uint8 CheckGlyphDir = (uint8)(CheckTile.Arg & 0xFF);
			const FHex CheckPartnerHex = HexUtil::Neighbor(TileKV.Key, (EHexDir)(CheckGlyphDir % 6));

			// If we're placing on this glyph's partner hex
			if (CheckPartnerHex == H)
			{
				bIsOnBondArea = true;
				break;
			}
		}
	}

	// Note: Items placed on bond/unbond areas (primary or partner) keep bIsReady = false
	// Items placed elsewhere also start with bIsReady = false to prevent immediate pulling
	// The bIsReady flag will be set to true at the end of the current simulation tick

	ItemMap.Add(H, Copy);
	OrbAt.Add(H, KindFromItem(Copy));

	// Add to NewlyPlacedItems to prevent belt processing this tick
	NewlyPlacedItems.Add(Copy.Id);

	return true;
}

bool USimWorldHex::TryTakeItem(const FHex& H, FItem& Out)
{
	if (FItem* Found = ItemMap.Find(H))
	{
		Out = *Found;
		ItemMap.Remove(H);
		OrbAt.Remove(H);
		// Belt progress bookkeeping
		if (Out.Id != 0)
		{
			BeltProgress.Remove(Out.Id);
		}
		return true;
	}
	return false;
}

// --------------------------------------------------

bool USimWorldHex::PlaceTile(const FHex& H, const FTile& T)
{
	if (T.Kind == ETileKind::Bond)
	{
		const FHex Partner = HexUtil::Neighbor(H, (EHexDir)(T.Arg % 6));
		if (TileMap.Contains(Partner) &&
			TileMap[Partner].Kind != ETileKind::Empty &&
			TileMap[Partner].Kind != ETileKind::Bond)
		{
			UE_LOG(LogTemp, Warning, TEXT("Bond placement blocked: partner hex occupied."));
			return false;
		}
	}
	if (T.Kind == ETileKind::Unbond)
	{
		const FHex Partner = HexUtil::Neighbor(H, (EHexDir)(T.Arg % 6));
		if (TileMap.Contains(Partner) &&
			TileMap[Partner].Kind != ETileKind::Empty &&
			TileMap[Partner].Kind != ETileKind::Unbond)
		{
			UE_LOG(LogTemp, Warning, TEXT("Unbond placement blocked: partner hex occupied."));
			return false;
		}
	}
	if (T.Kind == ETileKind::Empty)
	{
		const bool bHad = TileMap.Remove(H) > 0;
		if (bHad)
		{
			// NOTE: We NO LONGER remove bonds whose Root == H.
			// Bonds now persist until explicitly broken by a glyph.
			ItemMap.Remove(H);
			OrbAt.Remove(H);
			++TileRevision;
		}
		return true;
	}

	TileMap.Add(H, T);
	++TileRevision;
	return true;
}

// --- Bond-breaking public API (called by future glyph logic) ---
bool USimWorldHex::BreakBondBetween(int32 ItemA, int32 ItemB)
{
	for (int32 i = 0; i < Bonds.Num(); ++i)
	{
		const FBond& B = Bonds[i];
		if ((B.ItemA == ItemA && B.ItemB == ItemB) ||
			(B.ItemA == ItemB && B.ItemB == ItemA))
		{
			RemoveBondIndex(i);
			return true;
		}
	}
	return false;
}

int32 USimWorldHex::BreakAllBondsForItem(int32 ItemId)
{
	int32 Removed = 0;
	// Gather first to avoid index churn while removing
	TArray<int32> ToRemove;
	for (int32 i = 0; i < Bonds.Num(); ++i)
	{
		const FBond& B = Bonds[i];
		if (B.ItemA == ItemId || B.ItemB == ItemId)
			ToRemove.Add(i);
	}
	// Remove in reverse order
	for (int32 idx = ToRemove.Num() - 1; idx >= 0; --idx)
	{
		RemoveBondIndex(ToRemove[idx]);
		++Removed;
	}
	return Removed;
}


// --- CleanupBrokenBonds: only remove if an item vanished; otherwise update offsets ---
void USimWorldHex::CleanupBrokenBonds(const TSet<int32>& HeldIds)
{
	if (Bonds.Num() == 0) return;

	// Reconstruct item positions, including synthetic positions for held items (so bonds persist while held)
	TMap<int32, FHex> ItemPos;
	ItemPos.Reserve(ItemMap.Num() + HeldIds.Num());

	for (const auto& KV : ItemMap)
	{
		if (KV.Value.Id != 0)
			ItemPos.Add(KV.Value.Id, KV.Key);
	}

	for (int32 HeldId : HeldIds)
	{
		if (const FHex* Last = HeldItemLastHex.Find(HeldId))
		{
			// Only add if not already present (should not be, since held items are removed from ItemMap)
			ItemPos.FindOrAdd(HeldId, *Last);
		}
	}

	for (int32 i = Bonds.Num() - 1; i >= 0; --i)
	{
		FBond& B = Bonds[i];
		const FHex* AHex = ItemPos.Find(B.ItemA);
		const FHex* BHex = ItemPos.Find(B.ItemB);

		// If either truly gone (not on board and no last-held position), remove
		if (!AHex || !BHex)
		{
			RemoveBondIndex(i);
			continue;
		}

		// Update stored relative offset if it changed (items may have translated / rotated logically)
		const int32 dq = BHex->Q - AHex->Q;
		const int32 dr = BHex->R - AHex->R;
		if (dq != B.dQ || dr != B.dR)
		{
			B.dQ = (int8)dq;
			B.dR = (int8)dr;
			B.Root = *AHex; // keep root near first item for debug
			if (bDevDebugBonds && GEngine)
			{
				GEngine->AddOnScreenDebugMessage(-1, 0.3f, FColor::Silver,
					FString::Printf(TEXT("Bond offset updated (held ok): %d-%d dq=%d dr=%d"),
						B.ItemA, B.ItemB, dq, dr));
			}
		}

		// Update HeldItemLastHex for held items to their current virtual positions
		// This ensures the virtual positions stay current as the arm moves/rotates
		if (HeldIds.Contains(B.ItemA))
		{
			HeldItemLastHex.FindOrAdd(B.ItemA, *AHex);
		}
		if (HeldIds.Contains(B.ItemB))
		{
			HeldItemLastHex.FindOrAdd(B.ItemB, *BHex);
		}
	}
}

void USimWorldHex::MoveBondedClusterOnBelt(int32 SeedItemId, const FHex& FromHex, const FHex& ToHex, const FTile& BeltTile, TMap<int32, FHex>& ItemHex)
{
	// Recursively move all bonded items when one item in a cluster moves on a belt
	// This ensures the entire cluster moves as a cohesive unit

	// Check if the seed item has already been moved this tick (prevents infinite loops)
	FItem* SeedItem = ItemMap.Find(FromHex);
	if (!SeedItem || SeedItem->Id != SeedItemId)
	{
		return; // Item doesn't exist at expected location
	}

	// If the item was already moved this tick, check if it's at the target location
	if (SeedItem->LocationUpdatedThisTick)
	{
		// Verify if the item is already at the destination - if so, we're done
		if (FItem* DestItem = ItemMap.Find(ToHex))
		{
			if (DestItem->Id == SeedItemId)
			{
				return; // Already moved to destination successfully
			}
		}
		return; // Already moved but not to our destination, let other movement handle it
	}

	// Calculate the movement vector
	const int32 dQ = ToHex.Q - FromHex.Q;
	const int32 dR = ToHex.R - FromHex.R;

	// First, gather all bonded items that need to be moved as part of this cluster
	TSet<int32> ClusterItemIds;
	TArray<int32> Stack;
	Stack.Add(SeedItemId);
	ClusterItemIds.Add(SeedItemId);

	// Use BFS to find all connected items through bonds
	while (Stack.Num() > 0)
	{
		int32 CurrentId = Stack.Pop();
		if (const TArray<int32>* BondList = ItemToBonds.Find(CurrentId))
		{
			for (int32 BondIdx : *BondList)
			{
				if (!Bonds.IsValidIndex(BondIdx)) continue;
				const FBond& Bond = Bonds[BondIdx];
				if (!Bond.bActive) continue;

				int32 OtherId = (Bond.ItemA == CurrentId) ? Bond.ItemB : Bond.ItemA;
				if (OtherId > 0 && !ClusterItemIds.Contains(OtherId))
				{
					// Check if this item is on the board and on a belt
					const FHex* OtherHex = ItemHex.Find(OtherId);
					if (OtherHex)
					{
						const FTile* OtherTile = TileMap.Find(*OtherHex);
						if (OtherTile && OtherTile->Kind == ETileKind::Belt)
						{
							FItem* OtherItem = ItemMap.Find(*OtherHex);
							if (OtherItem && OtherItem->Id == OtherId && !OtherItem->LocationUpdatedThisTick)
							{
								ClusterItemIds.Add(OtherId);
								Stack.Add(OtherId);
							}
						}
					}
				}
			}
		}
	}

	// Check if all destination hexes are available
	bool bCanMoveCluster = true;
	TArray<TPair<int32, FHex>> MovePairs; // ItemId -> destination hex

	for (int32 ItemId : ClusterItemIds)
	{
		const FHex* CurrentHex = ItemHex.Find(ItemId);
		if (!CurrentHex)
		{
			bCanMoveCluster = false;
			break;
		}

		FHex DestHex(CurrentHex->Q + dQ, CurrentHex->R + dR);

		// Check if destination is blocked by non-cluster items
		if (ItemMap.Contains(DestHex))
		{
			FItem* DestItem = ItemMap.Find(DestHex);
			if (DestItem && !ClusterItemIds.Contains(DestItem->Id))
			{
				bCanMoveCluster = false;
				break;
			}
		}

		MovePairs.Add(TPair<int32, FHex>(ItemId, DestHex));
	}

	if (!bCanMoveCluster)
	{
		// Cannot move the entire cluster, abort
		return;
	}

	// Now move all items in the cluster atomically
	TMap<FHex, FItem> TempStorage; // Store items temporarily to avoid conflicts

	// First pass: remove all items from their current positions
	for (const TPair<int32, FHex>& MovePair : MovePairs)
	{
		int32 ItemId = MovePair.Key;
		const FHex* CurrentHex = ItemHex.Find(ItemId);
		if (!CurrentHex) continue;

		FItem* Item = ItemMap.Find(*CurrentHex);
		if (!Item || Item->Id != ItemId) continue;

		// Store item temporarily
		FItem MovedItem = *Item;
		MovedItem.LocationUpdatedThisTick = true;
		TempStorage.Add(MovePair.Value, MovedItem);

		// Remove from current position
		ItemMap.Remove(*CurrentHex);
		OrbAt.Remove(*CurrentHex);
	}

	// Second pass: place all items at their new positions
	for (const TPair<FHex, FItem>& TempPair : TempStorage)
	{
		const FHex& DestHex = TempPair.Key;
		const FItem& Item = TempPair.Value;

		// Place item at destination
		ItemMap.Add(DestHex, Item);
		OrbAt.Add(DestHex, KindFromItem(Item));

		// Update ItemHex lookup
		ItemHex[Item.Id] = DestHex;

		// Update belt progress
		if (float* Progress = BeltProgress.Find(Item.Id))
		{
			*Progress -= 1.f;
			if (*Progress < 0.f) *Progress = 0.f;
		}
	}
}

void USimWorldHex::MoveBondedItemRecursive(int32 ItemId, const FHex& FromHex, int32 dQ, int32 dR, TMap<int32, FHex>& ItemHex)
{
	// This function is now deprecated in favor of the atomic cluster movement in MoveBondedClusterOnBelt
	// Keep it for compatibility but make it do nothing to avoid conflicts
	return;
}

void USimWorldHex::StepSim()
{
	PrevItemHex.Empty();
	for (const auto& KV : ItemMap)
		if (KV.Value.Id != 0)
			PrevItemHex.Add(KV.Value.Id, KV.Key);
	ItemsMovedLastStep.Reset();

	// Reset LocationUpdatedThisTick flag for all items at the start of each tick
	for (auto& KV : ItemMap)
	{
		KV.Value.LocationUpdatedThisTick = false;
	}

	++StepCounter;
	SkipInputThisFrame.Reset();
	SkipOutputThisFrame.Reset();
	OrbsNewlyMovedToOutput.Reset();

	// SpawnInputs();          // pass 1 (empty at start)
	StepArms();
	StepBelts();
	SpawnInputsAfterBelts(); // NEW pass 2 (inputs just vacated by belt moves)
	StepBonds();
	CheckAndRemoveDisconnectedOrbsFromHeldClusters();
	// Second pass for arms - allows immediate grab after unbonding
	StepArmsSecondPass();

	ProcessOutputs();

	// Debug visualization - draw bond cylinders when DebugSpheres is enabled
	DrawBondDebugLines();
}
void USimWorldHex::StepGlyphs()
{
	// Legacy entry point if other code still calls it:
	ProcessOutputs();
}




// SpawnInputs() moved to SimWorldHex_InputOutput.cpp

// SpawnInputsAfterBelts() moved to SimWorldHex_InputOutput.cpp

// DoesItemMatchOutputType() moved to SimWorldHex_InputOutput.cpp

// ProcessOutputs() moved to SimWorldHex_InputOutput.cpp

// TryArmPickItem() and TryArmPlaceItem() moved to SimWorldHex_InputOutput.cpp

// PeekItemAt(), TryPlaceOrbAt(), TryPickupOrbAt(), HasOrbAt(), GetOrbKindAt() moved to SimWorldHex_InputOutput.cpp

void USimWorldHex::AddItemBondLink(int32 ItemId, int32 BondIndex)
{
	TArray<int32>& List = ItemToBonds.FindOrAdd(ItemId);
	List.Add(BondIndex);
}

void USimWorldHex::RemoveItemBondLink(int32 ItemId, int32 BondIndex)
{
	if (TArray<int32>* List = ItemToBonds.Find(ItemId))
	{
		List->Remove(BondIndex);
		if (List->Num() == 0)
			ItemToBonds.Remove(ItemId);
	}
}

void USimWorldHex::TryFormBondAt(const FHex& Root, uint8 Dir, const TSet<int32>& HeldIds, TSet<int32>& Newly)
{
	const FHex AHex = Root;
	const FHex BHex = HexUtil::Neighbor(Root, (EHexDir)(Dir % 6));

	FItem* ItemA = ItemMap.Find(AHex);
	FItem* ItemB = ItemMap.Find(BHex);
	if (!ItemA || !ItemB) return;
	if (ItemA->Id == 0 || ItemB->Id == 0) return;
	if (ItemA->Id == ItemB->Id) return;

	if (HeldIds.Contains(ItemA->Id) || HeldIds.Contains(ItemB->Id)) return;
	if (Newly.Contains(ItemA->Id) || Newly.Contains(ItemB->Id)) { /* allow multiple bonds same tick but avoid duplication */ }

	// Prevent duplicate bond between same two items (order-agnostic)
	auto AlreadyBondedPair = [&](int32 AId, int32 BId)
		{
			if (const TArray<int32>* AList = ItemToBonds.Find(AId))
			{
				for (int32 BondIdx : *AList)
				{
					const FBond& Existing = Bonds[BondIdx];
					if ((Existing.ItemA == AId && Existing.ItemB == BId) ||
						(Existing.ItemA == BId && Existing.ItemB == AId))
						return true;
				}
			}
			return false;
		};
	if (AlreadyBondedPair(ItemA->Id, ItemB->Id)) return;

	FBond NewB;
	NewB.ItemA = ItemA->Id;
	NewB.ItemB = ItemB->Id;
	NewB.Root = Root;
	NewB.dQ = (int8)(BHex.Q - AHex.Q);
	NewB.dR = (int8)(BHex.R - AHex.R);
	NewB.bActive = true;

	const int32 Index = Bonds.Add(NewB);
	AddItemBondLink(NewB.ItemA, Index);
	AddItemBondLink(NewB.ItemB, Index);
	Newly.Add(NewB.ItemA);
	Newly.Add(NewB.ItemB);

	if (GEngine && bDevDebugBonds)
	{
		GEngine->AddOnScreenDebugMessage(-1, 0.75f, FColor::Green,
			FString::Printf(TEXT("Bond formed: %d-%d (%d)"), NewB.ItemA, NewB.ItemB, Index));
	}
}


void USimWorldHex::RemoveBondIndex(int32 Index)
{
	if (!Bonds.IsValidIndex(Index)) return;
	const int32 LastIndex = Bonds.Num() - 1;
	const FBond Dead = Bonds[Index];

	// Remove links for dead bond
	RemoveItemBondLink(Dead.ItemA, Index);
	RemoveItemBondLink(Dead.ItemB, Index);

	// Swap removal if not last
	if (Index != LastIndex)
	{
		const FBond Moved = Bonds[LastIndex];
		Bonds.RemoveAtSwap(Index);

		// Fix moved bond indices in its items' lists
		auto FixList = [&](int32 ItemId)
			{
				if (TArray<int32>* L = ItemToBonds.Find(ItemId))
				{
					for (int32& Val : *L)
					{
						if (Val == LastIndex)
						{
							Val = Index;
							break;
						}
					}
				}
			};
		FixList(Moved.ItemA);
		FixList(Moved.ItemB);
	}
	else
	{
		Bonds.RemoveAt(Index);
	}

	if (GEngine && bDevDebugBonds)
	{
		GEngine->AddOnScreenDebugMessage(-1, 0.75f, FColor::Red,
			FString::Printf(TEXT("Bond removed: %d-%d"), Dead.ItemA, Dead.ItemB));
	}
}

FVector USimWorldHex::HexCenterToWorld(const FHex& H) const
{
	const float Tile = TileSizeUU;
	const EHexOrientation Ori = GridOrientation;
	const float Sqrt3 = 1.7320508075688772f;
	const float q = (float)H.Q;
	const float r = (float)H.R;

	FVector2D Local;
	if (Ori == EHexOrientation::PointyTop)
	{
		Local.X = Tile * (Sqrt3 * q + (Sqrt3 * 0.5f) * r);
		Local.Y = Tile * (1.5f * r);
	}
	else
	{
		Local.X = Tile * (1.5f * q);
		Local.Y = Tile * (Sqrt3 * r + (Sqrt3 * 0.5f) * q);
	}

	const float Rad = FMath::DegreesToRadians(GridYawDeg);
	const float CosA = FMath::Cos(Rad);
	const float SinA = FMath::Sin(Rad);
	const float RX = Local.X * CosA - Local.Y * SinA;
	const float RY = Local.X * SinA + Local.Y * CosA;
	return WorldAnchor + FVector(RX, RY, 0.f);
}

bool USimWorldHex::GetItemVisualLocation(int32 ItemId, FVector& OutWorld) const
{
	const FHex* CurHexPtr = nullptr;
	for (const auto& KV : ItemMap)
	{
		if (KV.Value.Id == ItemId)
		{
			CurHexPtr = &KV.Key;
			break;
		}
	}
	if (!CurHexPtr) return false;

	const FHex CurHex = *CurHexPtr;
	const FVector CurCenter = HexCenterToWorld(CurHex);

	// If this item advanced exactly one belt hex this logical step, interpolate previous->current
	if (ItemsMovedLastStep.Contains(ItemId))
	{
		if (const FHex* PrevHex = PrevItemHex.Find(ItemId))
		{
			if (*PrevHex != CurHex)
			{
				const FVector PrevCenter = HexCenterToWorld(*PrevHex);
				float Alpha = FMath::Clamp(FrameAlpha, 0.f, 1.f);
				OutWorld = FMath::Lerp(PrevCenter, CurCenter, Alpha);
				return true;
			}
		}
	}

	// Standard belt in‑tile interpolation (current center -> next center)
	const FTile* Tile = TileMap.Find(CurHex);
	if (!Tile || Tile->Kind != ETileKind::Belt)
	{
		OutWorld = CurCenter;
		return true;
	}

	// Compute next hex now so we can check runtime blocking
	const uint8 Dir = (uint8)(Tile->Arg & 0xFF);
	const FHex NextHex = HexUtil::Neighbor(CurHex, (EHexDir)(Dir % 6));
	const FVector NextCenter = HexCenterToWorld(NextHex);

	// Determine whether the destination is blocked by a board item or any held orb/hand
	auto IsDestBlocked = [&](const FHex& Dest)->bool
		{
			// Board occupancy
			if (ItemMap.Contains(Dest))
				return true;

			// Held occupancy (hands and sub‑orbs)
			for (int32 ArmIdx = 0; ArmIdx < Arms.Num(); ++ArmIdx)
			{
				const FArmState& A = Arms[ArmIdx];
				if (!A.bHolding) continue;

				// Hand hex at committed sim reach/facing
				const FHex Grip = GetArmGripHex_Sim(ArmIdx);

				// Single held orb occupies the grip hex
				if (!A.Held.bIsCluster)
				{
					if (Grip == Dest) return true;
					continue;
				}

				// Cluster: each sub‑orb at rotated offsets
				const uint8 ccw = GetHeldCCW_Steps_Sim(ArmIdx);
				for (const FOrbData& Sub : A.Held.Orbs)
				{
					FHex Local(Sub.dQ, Sub.dR);
					FHex Rot = Rot60CCW(Local, ccw);
					FHex WorldHex(Grip.Q + Rot.Q, Grip.R + Rot.R);
					if (WorldHex == Dest) return true;
				}
			}
			return false;
		};

	const bool bDestBlocked = IsDestBlocked(NextHex);

	// Use belt progress for smooth visual interpolation
	const float* PLogicPtr = BeltProgress.Find(ItemId);
	float LogicalP = PLogicPtr ? *PLogicPtr : 0.f;
	const bool bBlockedEdge = LogicalP >= BeltBlockedClamp - 0.0001f;

	float VisualP = LogicalP;
	if (!bBlockedEdge)
	{
		const float Dt = FMath::Max(0.f, FixedStepSec);
		const float TilesPerSec = BeltSpeedTilesPerTick / FMath::Max(KINDA_SMALL_NUMBER, Dt);
		VisualP = LogicalP + FrameAlpha * TilesPerSec * Dt;

		// Never render beyond the belt's blocked edge
		VisualP = FMath::Min(VisualP, BeltBlockedClamp - 0.0001f);
	}

	// If destination is currently blocked, enforce blocked-edge clamping as an extra safety
	if (bDestBlocked)
	{
		VisualP = FMath::Min(VisualP, BeltBlockedClamp - 0.0001f);
	}

	// Clamp to [0,1) for interpolation across THIS belt segment
	VisualP = FMath::Clamp(VisualP, 0.f, 0.9999f);

	OutWorld = FMath::Lerp(CurCenter, NextCenter, VisualP);
	return true;
}
// AddBoardOrbToHeldCluster() and AddBoardOrbToHeldItem() moved to SimWorldHex_Clusters.cpp
// CheckAndRemoveDisconnectedOrbsFromHeldClusters() moved to SimWorldHex_Clusters.cpp
// CheckAndRemoveDisconnectedOrbsFromHeldClusters() moved to SimWorldHex_Clusters.cpp
void USimWorldHex::DrawBondDebugLines()
{
	// Draw bond cylinders when DebugSpheres is enabled
	if (!bDebugSpheres || Bonds.Num() == 0)
	{
		return;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	// Get driver for coordinate conversion
	AOrbSimDriver* Driver = nullptr;
	for (TActorIterator<AOrbSimDriver> It(World); It; ++It)
	{
		Driver = *It;
		break;
	}

	if (!Driver)
	{
		return;
	}

	const AActor* Anchor = Driver->GetGridAnchor();
	const FVector AnchorLoc = Anchor ? Anchor->GetActorLocation() : FVector::ZeroVector;
	const float Tile = Driver->GetTileSize();
	const EHexOrientation DriverGridOrientation = Driver->GetGridOrientation();
	const float GridYaw = Driver->GetGridYaw();

	// Helper lambda to convert hex to world position
	auto HexToWorld = [&](const FHex& H) -> FVector
		{
			const FVector2D LocalXY = HexCoord::AxialToXY(FHexAxial(H.Q, H.R), Tile, DriverGridOrientation);
			const FVector2D RotXY = HexCoord::Rotate2D(LocalXY, GridYaw);
			return AnchorLoc + FVector(RotXY.X, RotXY.Y, 0.f);
		};

	// Draw debug lines for all active bonds
	for (const FBond& Bond : Bonds)
	{
		if (!Bond.bActive)
		{
			continue;
		}

		// Find the actual positions of the bonded items
		FVector PosA = FVector::ZeroVector;
		FVector PosB = FVector::ZeroVector;
		bool bFoundA = false, bFoundB = false;

		// Check if items are on the board
		for (const auto& ItemKV : ItemMap)
		{
			if (ItemKV.Value.Id == Bond.ItemA)
			{
				PosA = HexToWorld(ItemKV.Key);
				bFoundA = true;
			}
			if (ItemKV.Value.Id == Bond.ItemB)
			{
				PosB = HexToWorld(ItemKV.Key);
				bFoundB = true;
			}
		}

		// Check if items are held by arms (for held clusters)
		if (!bFoundA || !bFoundB)
		{
			for (int32 ArmIndex = 0; ArmIndex < Arms.Num(); ++ArmIndex)
			{
				const FArmState& Arm = Arms[ArmIndex];
				if (!Arm.bHolding) continue;

				const FHex GripHex = GetArmGripHex_Sim(ArmIndex);
				const uint8 ccw = GetHeldCCW_Steps_Sim(ArmIndex);

				auto Rot60 = [](FHex D, uint8 StepsCCW)
					{
						for (uint8 i = 0; i < (StepsCCW % 6); ++i)
						{
							int32 q = D.Q;
							int32 r = D.R;
							D.Q = -r;
							D.R = q + r;
						}
						return D;
					};

				if (Arm.Held.bIsCluster)
				{
					for (const FOrbData& Sub : Arm.Held.Orbs)
					{
						if ((Sub.ItemId == Bond.ItemA && !bFoundA) || (Sub.ItemId == Bond.ItemB && !bFoundB))
						{
							FHex Local(Sub.dQ, Sub.dR);
							FHex Rot = Rot60(Local, ccw);
							FHex WorldHex(GripHex.Q + Rot.Q, GripHex.R + Rot.R);
							const FVector Pos = HexToWorld(WorldHex);
							if (Sub.ItemId == Bond.ItemA) { PosA = Pos; bFoundA = true; }
							else { PosB = Pos; bFoundB = true; }
						}
					}
				}
				else
				{
					if (Arm.Held.Id == Bond.ItemA && !bFoundA) { PosA = HexToWorld(GripHex); bFoundA = true; }
					if (Arm.Held.Id == Bond.ItemB && !bFoundB) { PosB = HexToWorld(GripHex); bFoundB = true; }
				}
			}
		}

		// Only draw if both positions were found
		if (!bFoundA || !bFoundB)
		{
			continue;
		}

		// Add some height offset to make bonds visible above the ground
		PosA.Z += 15.f;
		PosB.Z += 15.f;

		// Calculate bond direction and distance
		FVector BondDirection = (PosB - PosA).GetSafeNormal();
		float BondLength = FVector::Dist(PosA, PosB);
		FVector BondMidpoint = (PosA + PosB) * 0.5f;

		// Draw cylinder to represent the bond
		if (BondLength > 0.f)
		{
			// Calculate rotation to align cylinder with bond direction
			FVector Up = FVector::UpVector;
			FVector Right = FVector::CrossProduct(BondDirection, Up).GetSafeNormal();
			if (Right.IsNearlyZero())
			{
				Right = FVector::RightVector;
			}
			FVector Forward = FVector::CrossProduct(Right, BondDirection).GetSafeNormal();

			// Create rotation matrix from the three axes
			FMatrix RotationMatrix = FMatrix(Forward, Right, BondDirection, FVector::ZeroVector);
			FRotator BondRotation = FQuat(RotationMatrix).Rotator();

			// Draw the cylinder (persistent for 1 second to avoid flickering)
			DrawDebugCylinder(World, PosA, PosB, 2.f, 8, FColor::Cyan, false, 1.0f, 0, 1.f);

			// Draw small spheres at bond endpoints (persistent for 1 second)
			DrawDebugSphere(World, PosA, 4.f, 8, FColor::Cyan, false, 1.0f, 0, 1.f);
			DrawDebugSphere(World, PosB, 4.f, 8, FColor::Cyan, false, 1.0f, 0, 1.f);

			// Draw bond ID text at midpoint (persistent for 1 second)
			FVector TextPosition = BondMidpoint;
			TextPosition.Z += 10.f;
			DrawDebugString(World, TextPosition,
				FString::Printf(TEXT("Bond: %d-%d"), Bond.ItemA, Bond.ItemB),
				nullptr, FColor::Cyan, 1.0f, true, 0.8f);
		}
	}
}


FHex USimWorldHex::GetArmGripHex_Sim(int32 ArmIndex) const
{
	if (ArmIndex < 0 || ArmIndex >= Arms.Num()) return FHex(0, 0);
	const FArmState& A = Arms[ArmIndex];
	const uint8 SimFace = (SimFacingByArm.Contains(ArmIndex) ? SimFacingByArm[ArmIndex] : uint8(A.Facing));
	const int32 SimLen = (SimArmLenByArm.Contains(ArmIndex) ? SimArmLenByArm[ArmIndex] : A.ArmLen);
	FHex Grip = A.BaseHex;
	for (int i = 0; i < SimLen; ++i)
		Grip = HexUtil::Neighbor(Grip, (EHexDir)(SimFace % 6));
	return Grip;
}

uint8 USimWorldHex::GetHeldCCW_Steps_Sim(int32 ArmIndex) const
{
	if (ArmIndex < 0 || ArmIndex >= Arms.Num()) return 0;
	const FArmState& A = Arms[ArmIndex];
	const uint8 cur = (SimFacingByArm.Contains(ArmIndex) ? SimFacingByArm[ArmIndex] : uint8(A.Facing)) % 6;

	int8 simGrab = int8(-1);
	if (const int8* P = SimGrabFacingByArm.Find(ArmIndex))
	{
		simGrab = *P;
	}
	// If sim grab is invalid, fall back to the current held grab (if any)
	const int8 grab = (simGrab >= 0) ? simGrab : (A.Held.bIsCluster ? A.Held.GrabFacing : int8(-1));

	const uint8 facingDelta = (grab >= 0)
		? (uint8)(((int32)cur - (int32)(grab % 6) + 6) % 6)
		: cur;

	return (uint8)((6 - facingDelta) % 6); // CCW steps to convert local->world
}

FHex USimWorldHex::Rot60CCW(FHex D, uint8 StepsCCW)
{
	for (uint8 i = 0; i < (StepsCCW % 6); ++i)
	{
		const int32 q = D.Q;
		const int32 r = D.R;
		D.Q = -r;
		D.R = q + r;
	}
	return D;
}

FHex USimWorldHex::Rot60CW(FHex D, uint8 StepsCW)
{
	return Rot60CCW(D, (uint8)((6 - (StepsCW % 6)) % 6));
}

void USimWorldHex::ProcessPendingBondFormations(float DeltaTime)
{
	// DEBUG: Log total pending formations (commented out for production)
	// if (PendingBondFormations.Num() > 0)
	// {
	//	UE_LOG(LogAGSim, Warning, TEXT("Processing %d pending bond formations"), PendingBondFormations.Num());
	// }

	for (int32 i = PendingBondFormations.Num() - 1; i >= 0; i--)
	{
		FPendingBondFormation& Pending = PendingBondFormations[i];

		// Check if the arm is still valid and holding the item
		if (Pending.ArmIndex < 0 || Pending.ArmIndex >= Arms.Num())
		{
			PendingBondFormations.RemoveAt(i);
			continue;
		}

		const FArmState& Arm = Arms[Pending.ArmIndex];
		if (!Arm.bHolding)
		{
			// Arm is no longer holding anything, cancel the pending formation
			PendingBondFormations.RemoveAt(i);
			continue;
		}

		// Check if the board item still exists
		const FItem* BoardItem = ItemMap.Find(Pending.BoardItemHex);
		if (!BoardItem || BoardItem->Id != Pending.BoardItemId)
		{
			// Board item no longer exists, cancel the pending formation
			PendingBondFormations.RemoveAt(i);
			continue;
		}

		// Decrease time remaining
		Pending.TimeRemaining -= DeltaTime;

		if (Pending.TimeRemaining <= 0.f)
		{
			// Time to form the bond - use the current held item ID
			if (bDebugLogs && bDevDebugBonds && GEngine)
			{
				GEngine->AddOnScreenDebugMessage(-1, 1.0f, FColor::Green,
					FString::Printf(TEXT("Processing pending bond: board orb %d -> held item %d"), Pending.BoardItemId, Arm.Held.Id));
			}

			AddBoardOrbToHeldItem(Arm.Held.Id, Pending.BoardItemId, Pending.BoardItemHex);
			PendingBondFormations.RemoveAt(i);
		}
	}
}


void USimWorldHex::OnArmRotationComplete(int32 /*ArmIndex*/)
{
	// No-op for bonding/merges with sim-delayed facing.
	// Bonds and held-held merges are resolved in StepBonds using committed sim state.
}

void USimWorldHex::OnArmRotationFullyComplete(int32 ArmIndex)
{
	// Visual event only: record state for UX/other subsystems if desired.
	// Do not commit sim-facing here to avoid coupling sim to render frame timing.
	if (ArmIndex >= 0 && ArmIndex < Arms.Num())
	{
		FArmState& S = Arms[ArmIndex];
		S.bRotationCompleted = true;
	}
	
}

void USimWorldHex::CancelPendingBondFormationsForArm(int32 ArmIndex)
{
	// Cancel any pending bond formations for this arm
	for (int32 i = PendingBondFormations.Num() - 1; i >= 0; i--)
	{
		FPendingBondFormation& Pending = PendingBondFormations[i];

		if (Pending.ArmIndex == ArmIndex)
		{
			if (bDebugLogs && bDevDebugBonds && GEngine)
			{
				GEngine->AddOnScreenDebugMessage(-1, 1.0f, FColor::Yellow,
					FString::Printf(TEXT("Canceling pending bond formation for arm %d (arm dropped item)"), ArmIndex));
			}

			PendingBondFormations.RemoveAt(i);
		}
	}
}

