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

// (add near other file‑scope statics)
static constexpr int32 MAX_ARM_LEN = 6;
static bool bDevDebugArms = true; // set true to see arm/hand debug messages
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
void USimWorldHex::StepArms()
{
	// Apply pending sim values produced by last tick’s Rotate/Extend/Retract
	if (PendingFacingByArm.Num() || PendingArmLenByArm.Num() || PendingGrabFacingByArm.Num())
	{
		for (const auto& KV : PendingFacingByArm)     SimFacingByArm.Add(KV.Key, KV.Value);
		for (const auto& KV : PendingArmLenByArm)     SimArmLenByArm.Add(KV.Key, KV.Value);
		for (const auto& KV : PendingGrabFacingByArm) SimGrabFacingByArm.Add(KV.Key, KV.Value);
		PendingFacingByArm.Empty();
		PendingArmLenByArm.Empty();
		PendingGrabFacingByArm.Empty();
	}

	// Build reverse lookup (item id -> hex) once for this arm step (excludes held items)
	TMap<int32, FHex> ItemIdToHex;
	ItemIdToHex.Reserve(ItemMap.Num());
	for (const auto& KV : ItemMap)
	{
		if (KV.Value.Id != 0)
			ItemIdToHex.Add(KV.Value.Id, KV.Key);
	}

	// Note: We don't reset bRotationCompleted here anymore
	// It stays true until a new rotation starts, allowing grab to check
	// if the previous rotation completed

	// --- Local helpers (can access private FBond type through this) ---
	auto GatherBondedClusterFn = [&](int32 RootId, TSet<int32>& OutIds)
		{
			if (RootId == 0 || OutIds.Contains(RootId)) return;
			TArray<int32> Stack;
			Stack.Add(RootId);
			OutIds.Add(RootId);
			while (Stack.Num())
			{
				int32 Cur = Stack.Pop(EAllowShrinking::No);
				if (const TArray<int32>* BondList = ItemToBonds.Find(Cur))
				{
					for (int32 BondIdx : *BondList)
					{
						if (!Bonds.IsValidIndex(BondIdx)) continue;
						const FBond& B = Bonds[BondIdx];
						const int32 Other = (B.ItemA == Cur) ? B.ItemB : B.ItemA;
						if (Other != 0 && !OutIds.Contains(Other))
						{
							OutIds.Add(Other);
							Stack.Add(Other);
						}
					}
				}
			}
		};

	auto RotateAxial60Fn = [](const FHex& D, int32 StepsCCW)->FHex
		{
			int32 n = ((StepsCCW % 6) + 6) % 6;
			FHex r = D;
			for (int32 i = 0; i < n; ++i)
			{
				const int32 q = r.Q;
				const int32 rr = r.R;
				r.Q = -rr;
				r.R = q + rr;
			}
			return r;
		};

	auto ApproxXYToAxialFn = [&](const FVector2D& Offset)->FHex
		{
			// Small brute force – cluster sizes are tiny.
			float Best = FLT_MAX;
			int32 BestQ = 0, BestR = 0;
			for (int q = -6; q <= 6; ++q)
			{
				for (int r = -6; r <= 6; ++r)
				{
					const FVector2D XY = HexCoord::AxialToXY(FHexAxial(q, r), 1.f, GridOrientation);
					const float DistSq = (XY - Offset).SizeSquared();
					if (DistSq < Best)
					{
						Best = DistSq;
						BestQ = q;
						BestR = r;
					}
				}
			}
			return FHex(BestQ, BestR);
		};
	// ---------------------------------------------------------------

	for (int32 ArmIndex = 0; ArmIndex < Arms.Num(); ArmIndex++)
	{
		FArmState& S = Arms[ArmIndex];
		if (S.Tape.Num() == 0) continue;


		// Allow multiple operations per tick for immediate execution
		int32 OpsExecutedThisTick = 0;
		const int32 MaxOpsPerTick = 3; // Safety limit to prevent infinite loops
		bool bContinueProcessing = true;

		while (bContinueProcessing && OpsExecutedThisTick < MaxOpsPerTick)
		{
			bContinueProcessing = false; // Default to stopping after one op
			OpsExecutedThisTick++;


			const FOp& Op = S.Tape[S.PC];
			bool bResetCommandExpanded = false; // Track if Reset expanded the tape

			// Check if the previous command was a Grab that we need to execute now
			if (S.PC > 0 && !S.bHolding)
			{
				int32 PrevPC = S.PC - 1;
				const FOp& PrevOp = S.Tape[PrevPC];

				if (PrevOp.Cmd == ECmd::Grab)
				{
					//// Execute the deferred grab now
					//if (bDebugLogs && GEngine)
					//{
					//	GEngine->AddOnScreenDebugMessage(-1, 1.0f, FColor::Cyan,
					//		FString::Printf(TEXT("Arm %d: Executing deferred grab from PC=%d"), ArmIndex, PrevPC));
					//}

					// Execute grab logic here (copied from original grab case)
					bool bGrabSucceeded = false;
					FHex Target = S.BaseHex;
					for (int i = 0; i < S.ArmLen; ++i)
						Target = HexUtil::Neighbor(Target, S.Facing);

					FItem* Found = ItemMap.Find(Target);

					// Check if another arm is already trying to grab this cluster
					bool bAnotherArmGrabbing = false;
					if (Found)
					{
						for (const FPendingGrab& PendingGrab : PendingGrabs)
						{
							if (PendingGrab.ArmIndex != ArmIndex &&
								PendingGrab.TargetHex == Target &&
								PendingGrab.ClusterId == Found->Id)
							{
								bAnotherArmGrabbing = true;
								break;
							}
						}
					}

					if (!bAnotherArmGrabbing)
					{
						if (!Found)
						{
							FItem Temp;
							if (TryArmPickItem(Target, Temp))
							{
								S.Held = Temp;
								S.bHolding = true;
								S.PrevHeldOrbCount = 1;
								SkipInputThisFrame.Add(Target);
								bGrabSucceeded = true;
							}
						}
						else
						{
							// Handle cluster grab (simplified for brevity)
							TSet<int32> ClusterIds;
							if (Found->Id != 0 && ItemToBonds.Contains(Found->Id))
								GatherBondedClusterFn(Found->Id, ClusterIds);

							if (ClusterIds.Num() <= 1)
							{
								FItem SingleOut;
								if (TryArmPickItem(Target, SingleOut))
								{
									S.Held = SingleOut;
									S.bHolding = true;
									S.PrevHeldOrbCount = 1;
									SkipInputThisFrame.Add(Target);
									bGrabSucceeded = true;
								}
							}
							// Note: Full cluster grab logic would go here but omitted for brevity
						}
					}
				}
			}

			// Clean up any stale pending grabs for this arm (if it's not trying to grab anymore)
			if (Op.Cmd != ECmd::Grab)
			{
				for (int32 i = PendingGrabs.Num() - 1; i >= 0; i--)
				{
					if (PendingGrabs[i].ArmIndex == ArmIndex)
					{
						PendingGrabs.RemoveAt(i);
					}
				}
			}

			switch (Op.Cmd)
			{
			case ECmd::Wait:
			{
				if (S.WaitCounter == 0)
				{
					S.WaitCounter = (Op.Arg == 0 ? 1 : Op.Arg);
					if (bDebugLogs && GEngine)
					{
						GEngine->AddOnScreenDebugMessage(-1, 1.0f, FColor::Yellow,
							FString::Printf(TEXT("Arm %d: Starting wait for %d ticks"), ArmIndex, S.WaitCounter));
					}
				}
				--S.WaitCounter;
				if (S.WaitCounter > 0)
				{
					// Still waiting - don't advance PC, just continue to next arm
					if (bDebugLogs && GEngine)
					{
						GEngine->AddOnScreenDebugMessage(-1, 0.1f, FColor::Cyan,
							FString::Printf(TEXT("Arm %d: Waiting... %d ticks left"), ArmIndex, S.WaitCounter));
					}
					break; // Exit switch without advancing PC
				}
				S.WaitCounter = 0;
				if (bDebugLogs && GEngine)
				{
					GEngine->AddOnScreenDebugMessage(-1, 1.0f, FColor::Green,
						FString::Printf(TEXT("Arm %d: Wait complete, advancing to next instruction"), ArmIndex));
				}
			} break;

			case ECmd::Grab:
			{
				if (Op.Cmd != ECmd::Grab)
				{
					for (int32 i = PendingGrabs.Num() - 1; i >= 0; i--)
					{
						if (PendingGrabs[i].ArmIndex == ArmIndex)
						{
							PendingGrabs.RemoveAt(i);
						}
					}
				}
				// If already holding, just advance
				if (S.bHolding)
				{
					bContinueProcessing = true; // allow next op this tick
					break;
				}

				// Compute grip/target hex (visual-facing has already been applied to S.Facing here)
				const FHex Target = GetArmGripHex_Sim(ArmIndex);


				FItem* Found = ItemMap.Find(Target);

				// Try single grab first
				if (!Found)
				{
					FItem Temp;
					if (TryArmPickItem(Target, Temp))
					{
						// Don't allow re-grab of items we dropped ourselves this tick
						if (const int32* DroppedByArm = RecentlyDroppedByArm.Find(Temp.Id))
						{
							if (*DroppedByArm == ArmIndex)
							{
								// Put it back and keep waiting on this op
								ItemMap.Add(Target, Temp);
								bContinueProcessing = false;
								break;
							}
						}

						S.Held = Temp;
						S.bHolding = true;
						S.PrevHeldOrbCount = 1;
						SkipInputThisFrame.Add(Target);
						// single item has no held-rotation
						SimGrabFacingByArm.Add(ArmIndex, int8(-1));

						// Advance immediately
						bContinueProcessing = true;
					}
					else
					{
						// No item to pick up this tick; stay on Grab
						bContinueProcessing = false;
					}
					break;
				}

				// We have something at target; decide if it's a bonded cluster
				TSet<int32> ClusterIds;
				if (Found->Id != 0 && ItemToBonds.Contains(Found->Id))
				{
					// Gather bonded ids
					TArray<int32> Stack;
					Stack.Add(Found->Id);
					ClusterIds.Add(Found->Id);

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
								if (OtherId > 0 && !ClusterIds.Contains(OtherId))
								{
									ClusterIds.Add(OtherId);
									Stack.Add(OtherId);
								}
							}
						}
					}
				}

				// If not actually a multi-item cluster, do a single grab fast-path
				if (ClusterIds.Num() <= 1)
				{
					FItem SingleOut;
					if (TryArmPickItem(Target, SingleOut))
					{
						if (const int32* DroppedByArm = RecentlyDroppedByArm.Find(SingleOut.Id))
						{
							if (*DroppedByArm == ArmIndex)
							{
								// Put it back; do not advance PC
								ItemMap.Add(Target, SingleOut);
								bContinueProcessing = false;
								break;
							}
						}

						S.Held = SingleOut;
						S.bHolding = true;
						S.PrevHeldOrbCount = 1;
						SkipInputThisFrame.Add(Target);
						SimGrabFacingByArm.Add(ArmIndex, int8(-1));

						// Advance immediately
						bContinueProcessing = true;
					}
					else
					{
						bContinueProcessing = false;
					}
					break;
				}

				// Multi-item cluster: assemble immediately
				FItem Cluster;
				Cluster.bIsCluster = true;
				Cluster.Id = Found->Id;
				Cluster.GrabFacing = (int8)S.Facing; // seed with current facing (visual already committed to S.Facing)

				bool bAllFound = true;
				for (int32 ItemId : ClusterIds)
				{
					FHex ItemHex;
					bool bItemFound = false;

					for (const auto& KV2 : ItemMap)
					{
						if (KV2.Value.Id == ItemId)
						{
							ItemHex = KV2.Key;
							bItemFound = true;
							break;
						}
					}

					if (!bItemFound)
					{
						bAllFound = false;
						break;
					}

					const FHex D(ItemHex.Q - Target.Q, ItemHex.R - Target.R);

					FOrbData Orb;
					const FItem* It = ItemMap.Find(ItemHex);
					if (It && !It->bIsCluster)
					{
						// Disallow re-grab of items we dropped ourselves this tick
						if (const int32* DroppedByArm = RecentlyDroppedByArm.Find(It->Id))
						{
							if (*DroppedByArm == ArmIndex)
							{
								bAllFound = false; // treat as not ready yet
								break;
							}
						}

						Orb.Type = (uint8)It->Single.Type;
						Orb.Color = (uint8)It->Single.Color;
						Orb.ItemId = It->Id;
						Orb.dQ = (int8)D.Q;
						Orb.dR = (int8)D.R;
						Orb.Offset = FVector2D::ZeroVector;
						Cluster.Orbs.Add(Orb);
					}
				}

				if (bAllFound && Cluster.Orbs.Num() > 0)
				{
					// Remove all cluster members from the board
					for (const FOrbData& Sub : Cluster.Orbs)
					{
						for (auto It = ItemMap.CreateIterator(); It; ++It)
						{
							if (It.Value().Id == Sub.ItemId)
							{
								SkipInputThisFrame.Add(It.Key());
								HeldItemLastHex.Add(Sub.ItemId, It.Key());
								It.RemoveCurrent();
								break;
							}
						}
					}

					S.Held = Cluster;
					S.bHolding = true;
					S.PrevHeldOrbCount = Cluster.Orbs.Num();
					// seed sim-held orientation immediately
					SimGrabFacingByArm.Add(ArmIndex, Cluster.GrabFacing);

					// Advance immediately
					bContinueProcessing = true;
				}
				else
				{
					// Not ready to grab the cluster now; stay on Grab
					bContinueProcessing = false;
				}
			}
			break;

			case ECmd::Drop:
				if (S.bHolding)
				{
					FHex GripTarget = S.BaseHex;
					for (int i = 0; i < S.ArmLen; ++i)
						GripTarget = HexUtil::Neighbor(GripTarget, S.Facing);

					if (!S.Held.bIsCluster)
					{
						// Clear any ghost tracking for this single orb before placing it
						HeldItemLastHex.Remove(S.Held.Id);
						NewlyPlacedItems.Remove(S.Held.Id);
						RecentlyDroppedByArm.Remove(S.Held.Id);

						// Also remove any ghost orb from ItemMap that might be causing collision issues
						for (auto It = ItemMap.CreateIterator(); It; ++It)
						{
							if (It.Value().Id == S.Held.Id)
							{
								It.RemoveCurrent();
								break;
							}
						}

						if (TryArmPlaceItem(GripTarget, S.Held))
						{
							S.bHolding = false;
							S.Held = FItem();
							S.bWaitingForBond = false;
							S.WaitForBondInitialCount = 0;
							S.PrevHeldOrbCount = 0;
							// NEW: clear sim held-rotation immediately (not only pending)
							SimGrabFacingByArm.Add(ArmIndex, int8(-1));
							PendingGrabFacingByArm.Add(ArmIndex, int8(-1));
							SkipOutputThisFrame.Add(GripTarget);

							// Enable immediate execution of next command after successful drop
							bContinueProcessing = true;
						}
						else
						{
							// Drop failed - location occupied
						}
					}
					else
					{
						// Compute rotation delta relative to grab, not absolute facing.
						const uint8 CurFacingSteps = (uint8)S.Facing % 6;
						const int8 StoredGrabFacing = (int8)(S.Held.GrabFacing);
						const uint8 FacingDelta = (StoredGrabFacing >= 0)
							? (uint8)(((int32)CurFacingSteps - (int32)(StoredGrabFacing % 6) + 6) % 6)
							: CurFacingSteps; // fallback for legacy items

						bool bBlocked = false;
						TArray<FHex> TargetHexes;
						TargetHexes.Reserve(S.Held.Orbs.Num());

						for (const FOrbData& Sub : S.Held.Orbs)
						{
							FHex Local;
							if (Sub.dQ != 0 || Sub.dR != 0 || Sub.Offset.IsNearlyZero())
								Local = FHex(Sub.dQ, Sub.dR);
							else
								Local = ApproxXYToAxialFn(Sub.Offset); // legacy fallback

							// FacingDelta was computed as (CurFacing - GrabFacing) in a system where
							// +1 is a clockwise 60° step (see DirToYaw). RotateAxial60Fn expects
							// positive steps = CCW, so we invert the direction.
							const uint8 CCWSteps = (uint8)((6 - FacingDelta) % 6);
							FHex RotLocal = RotateAxial60Fn(Local, CCWSteps);

							FHex WorldHex(GripTarget.Q + RotLocal.Q, GripTarget.R + RotLocal.R);
							if (ItemMap.Contains(WorldHex)) { bBlocked = true; break; }
							TargetHexes.Add(WorldHex);
						}

						if (bBlocked)
						{
							if (S.WaitCounter == 0) S.WaitCounter = 1;
							break;
						}

						// Clear any ghost tracking for this arm before placing real orbs
						// This prevents teleporting behavior from stale HeldItemLastHex entries
						for (const FOrbData& Sub : S.Held.Orbs)
						{
							HeldItemLastHex.Remove(Sub.ItemId);
							NewlyPlacedItems.Remove(Sub.ItemId);
							RecentlyDroppedByArm.Remove(Sub.ItemId);

							// Also remove any ghost orbs from ItemMap that might be causing collision issues
							for (auto It = ItemMap.CreateIterator(); It; ++It)
							{
								if (It.Value().Id == Sub.ItemId)
								{
									It.RemoveCurrent();
									break;
								}
							}
						}

						// Place each sub‑orb back with its preserved ItemId (or assign if missing)
						for (int32 idx = 0; idx < S.Held.Orbs.Num(); ++idx)
						{
							const FOrbData& Sub = S.Held.Orbs[idx];
							FOrb O; O.Type = (EOrbType)Sub.Type; O.Color = (EOrbColor)Sub.Color;

							FItem NewItem = FItem::MakeSingle(O);
							if (Sub.ItemId > 0)
							{
								NewItem.Id = Sub.ItemId;          // reuse original Id
								// Defensive: keep NextItemId ahead
								NextItemId = FMath::Max(NextItemId, NewItem.Id + 1);
							}
							else
							{
								NewItem.Id = NextItemId++;        // legacy fallback
							}

							if (TryPutItem(TargetHexes[idx], NewItem))
							{
								SkipOutputThisFrame.Add(TargetHexes[idx]);
								HeldItemLastHex.Remove(NewItem.Id);
							}
						}

						// Bonds referencing these Ids were never removed; offsets will self‑heal in CleanupBrokenBonds next tick.
						S.bHolding = false;
						S.Held = FItem();
						S.bWaitingForBond = false;
						S.WaitForBondInitialCount = 0;
						S.PrevHeldOrbCount = 0;
						// NEW: clear sim held-rotation immediately (not only pending)
						SimGrabFacingByArm.Add(ArmIndex, int8(-1));
						PendingGrabFacingByArm.Add(ArmIndex, int8(-1));

						// Enable immediate execution of next command after successful drop
						bContinueProcessing = true;
					}

					// Clean up any pending grabs for this arm
					for (int32 i = PendingGrabs.Num() - 1; i >= 0; i--)
					{
						if (PendingGrabs[i].ArmIndex == ArmIndex)
						{
							PendingGrabs.RemoveAt(i);
						}
					}
				}
				break;

			case ECmd::RotateL:
			case ECmd::RotateR:
			{
				// Mark that rotation is starting
				S.bRotationCompleted = false;

				// If we were told to wait (from a previously blocked rotate) count it down and retry next tick.
				if (S.WaitCounter > 0)
				{
					--S.WaitCounter;
					// Do NOT advance PC; retry this same rotate instruction.
					continue;
				}

				EHexDir NewFacing = (Op.Cmd == ECmd::RotateL)
					? (EHexDir)(((uint8)S.Facing + 1) % 6)
					: (EHexDir)(((uint8)S.Facing + 5) % 6);

				// Hand positions before/after rotation
				FHex OldHand = S.BaseHex;
				for (int i = 0; i < S.ArmLen; ++i)
					OldHand = HexUtil::Neighbor(OldHand, S.Facing);

				FHex NewHand = S.BaseHex;
				for (int i = 0; i < S.ArmLen; ++i)
					NewHand = HexUtil::Neighbor(NewHand, NewFacing);

				const int32 dQ = NewHand.Q - OldHand.Q;
				const int32 dR = NewHand.R - OldHand.R;

				// Occupancy: world items
				TSet<FHex> WorldOcc;
				for (const auto& KV : ItemMap)
					WorldOcc.Add(KV.Key);

				// Helper: compute hand hex for a given arm/facing
				auto ComputeGripHex = [](const FArmState& A, EHexDir Facing)->FHex
					{
						FHex Grip = A.BaseHex;
						for (int i = 0; i < A.ArmLen; ++i)
							Grip = HexUtil::Neighbor(Grip, Facing);
						return Grip;
					};

				// Helper: rotate an axial coordinate by StepsCCW 60° steps
				auto Rot60 = [](FHex D, uint8 StepsCCW)
					{
						for (uint8 i = 0; i < (StepsCCW % 6); ++i)
						{
							const int32 q = D.Q;
							const int32 r = D.R;
							D.Q = -r;
							D.R = q + r;
						}
						return D;
					};

				// Build occupancy for other arms' held orbs (and their hands)
				TSet<FHex> OtherHeldOcc;
				for (int32 OtherIdx = 0; OtherIdx < Arms.Num(); ++OtherIdx)
				{
					if (OtherIdx == ArmIndex) continue;
					const FArmState& O = Arms[OtherIdx];
					if (!O.bHolding) continue;

					// Always include their hand hex
					const FHex OHand = ComputeGripHex(O, O.Facing);
					OtherHeldOcc.Add(OHand);

					if (O.Held.bIsCluster)
					{
						// Virtual positions of their sub-orbs at current facing
						const uint8 cur = (uint8)O.Facing % 6;
						const int8 grab = (int8)O.Held.GrabFacing;
						const uint8 facingDelta = (grab >= 0)
							? (uint8)(((int32)cur - (int32)(grab % 6) + 6) % 6)
							: cur;
						const uint8 ccw = (uint8)((6 - facingDelta) % 6);

						for (const FOrbData& Sub : O.Held.Orbs)
						{
							FHex Local(Sub.dQ, Sub.dR);
							FHex RotLocal = Rot60(Local, ccw);
							OtherHeldOcc.Add(FHex(OHand.Q + RotLocal.Q, OHand.R + RotLocal.R));
						}
					}
					else
					{
						// Single held orb occupies the hand
						OtherHeldOcc.Add(OHand);
					}
				}

				// Self held-orb positions at start and end, plus two “mid” phases
				auto BuildLocals = [&](EHexDir Facing, TArray<FHex>& OutLocals)->void
					{
						OutLocals.Reset();
						if (!S.bHolding)
							return;

						const FHex Grip = ComputeGripHex(S, Facing);
						if (!S.Held.bIsCluster)
						{
							OutLocals.Add(Grip);
							return;
						}

						const uint8 cur = (uint8)Facing % 6;
						const int8 grab = (int8)S.Held.GrabFacing;
						const uint8 facingDelta = (grab >= 0)
							? (uint8)(((int32)cur - (int32)(grab % 6) + 6) % 6)
							: cur;
						const uint8 ccw = (uint8)((6 - facingDelta) % 6);

						for (const FOrbData& Sub : S.Held.Orbs)
						{
							FHex Local(Sub.dQ, Sub.dR);
							FHex RotLocal = Rot60(Local, ccw);
							OutLocals.Add(FHex(Grip.Q + RotLocal.Q, Grip.R + RotLocal.R));
						}
					};

				TArray<FHex> StartLocals, EndLocals, MidTranslateLocals, MidRotateLocals;
				BuildLocals(S.Facing, StartLocals);
				BuildLocals(NewFacing, EndLocals);

				// Mid A: translate hand first (Old orientation at New hand)
				if (S.bHolding)
				{
					if (!S.Held.bIsCluster)
					{
						MidTranslateLocals.Add(NewHand);
					}
					else
					{
						// Old orientation at New hand
						const uint8 curOld = (uint8)S.Facing % 6;
						const int8 grab = (int8)S.Held.GrabFacing;
						const uint8 facingDeltaOld = (grab >= 0)
							? (uint8)(((int32)curOld - (int32)(grab % 6) + 6) % 6)
							: curOld;
						const uint8 ccwOld = (uint8)((6 - facingDeltaOld) % 6);
						for (const FOrbData& Sub : S.Held.Orbs)
						{
							FHex Local(Sub.dQ, Sub.dR);
							FHex RotLocal = Rot60(Local, ccwOld);
							MidTranslateLocals.Add(FHex(NewHand.Q + RotLocal.Q, NewHand.R + RotLocal.R));
						}
					}
				}

				// Mid B: rotate first (New orientation at Old hand)
				if (S.bHolding)
				{
					if (!S.Held.bIsCluster)
					{
						MidRotateLocals.Add(OldHand);
					}
					else
					{
						const uint8 curNew = (uint8)NewFacing % 6;
						const int8 grab = (int8)S.Held.GrabFacing;
						const uint8 facingDeltaNew = (grab >= 0)
							? (uint8)(((int32)curNew - (int32)(grab % 6) + 6) % 6)
							: curNew;
						const uint8 ccwNew = (uint8)((6 - facingDeltaNew) % 6);
						for (const FOrbData& Sub : S.Held.Orbs)
						{
							FHex Local(Sub.dQ, Sub.dR);
							FHex RotLocal = Rot60(Local, ccwNew);
							MidRotateLocals.Add(FHex(OldHand.Q + RotLocal.Q, OldHand.R + RotLocal.R));
						}
					}
				}

				auto OverlapsAny = [&](const FHex& H)->bool
					{
						return WorldOcc.Contains(H) || OtherHeldOcc.Contains(H);
					};

				bool bBlocked = false;

				// 1) Destination hand cannot contain foreign item/held orb (covers single held-orb too)
				// Only check this if the arm is holding something
				if (S.bHolding && OverlapsAny(NewHand))
				{
					bBlocked = true;
				}

				// 2) End positions of all sub-orbs
				if (!bBlocked)
				{
					for (const FHex& H : EndLocals)
					{
						if (OverlapsAny(H))
						{
							bBlocked = true;
							break;
						}
					}
				}

				// 3) Mid A: translate first (hand moves; orientation unchanged)
				if (!bBlocked)
				{
					for (const FHex& H : MidTranslateLocals)
						if (OverlapsAny(H)) { bBlocked = true; break; }
				}

				// 4) Mid B: rotate first (orientation changes; hand stays)
				if (!bBlocked)
				{
					for (const FHex& H : MidRotateLocals)
						if (OverlapsAny(H)) { bBlocked = true; break; }
				}

				if (bBlocked)
				{
					// Arm cannot rotate this tick; set a 1‑tick wait and retry SAME instruction.
					if (S.WaitCounter == 0)
					{
						S.WaitCounter = 1;
					}
					// Do NOT advance PC; keep pointing at this rotate op.
					continue;
				}

				// Commit visual/current arm rotation immediately
				S.Facing = NewFacing;

				// Defer simulation-facing to next fixed tick
				PendingFacingByArm.Add(ArmIndex, static_cast<uint8>(S.Facing));
				// Note: we do not touch SimFacingByArm here, it updates next tick
			}
			break;

			case ECmd::Extend:
			{
				const int32 Delta = (Op.Arg == 0 ? 1 : Op.Arg);
				const int32 OldLen = S.ArmLen;
				S.ArmLen = FMath::Clamp<int32>(S.ArmLen + Delta, 1, MAX_ARM_LEN);
				// Defer simulation reach to next fixed tick
				PendingArmLenByArm.Add(ArmIndex, S.ArmLen);

				if (bDevDebugArms && GEngine && S.ArmLen != OldLen)
				{
					FHex Hand = S.BaseHex;
					for (int i = 0; i < S.ArmLen; ++i)
						Hand = HexUtil::Neighbor(Hand, S.Facing);
					GEngine->AddOnScreenDebugMessage(
						-1, 1.5f, FColor::Green,
						FString::Printf(TEXT("[Arm %d] EXTEND (Δ=%d) Len=%d Hand=(%d,%d)"),
							ArmIndex, Delta, S.ArmLen, Hand.Q, Hand.R));
				}
			}
			break;

			case ECmd::Retract:
			{
				const int32 Delta = (Op.Arg == 0 ? 1 : Op.Arg);
				const int32 OldLen = S.ArmLen;
				S.ArmLen = FMath::Clamp<int32>(S.ArmLen - Delta, 1, MAX_ARM_LEN);
				// Defer simulation reach to next fixed tick
				PendingArmLenByArm.Add(ArmIndex, S.ArmLen);

				if (bDevDebugArms && GEngine && S.ArmLen != OldLen)
				{
					FHex Hand = S.BaseHex;
					for (int i = 0; i < S.ArmLen; ++i)
						Hand = HexUtil::Neighbor(Hand, S.Facing);
					GEngine->AddOnScreenDebugMessage(
						-1, 1.5f, FColor::Orange,
						FString::Printf(TEXT("[Arm %d] RETRACT (Δ=%d) Len=%d Hand=(%d,%d)"),
							ArmIndex, Delta, S.ArmLen, Hand.Q, Hand.R));
				}
			}
			break;
			case ECmd::MovePos: S.TrackIdx = FMath::Clamp<int32>(S.TrackIdx + 1, -4, 4); break;
			case ECmd::MoveNeg: S.TrackIdx = FMath::Clamp<int32>(S.TrackIdx - 1, -4, 4); break;
			case ECmd::PivotL:  break;
			case ECmd::PivotR:  break;
			case ECmd::Reset:
			{
				// Reset command implementation: Generate commands to drop and return to home position
				// Calculate movements needed to return to home position
				int32 CurrentReach = S.ArmLen;  // Not const, as we may update it after extend/drop
				const int32 TargetReach = S.HomeArmLen;
				const uint8 CurrentRotation = static_cast<uint8>(S.Facing);
				const uint8 TargetRotation = static_cast<uint8>(S.HomeFacing);


				// Create a temporary program to return to home
				TArray<FOp> ResetOps;

				// Handle drop if holding
				if (S.bHolding)
				{
					ResetOps.Add({ ECmd::Drop, 0 });
				}

				// Add reach adjustments
				const int32 ReachDiff = TargetReach - CurrentReach;
				if (ReachDiff > 0)
				{
					// Need to extend
					for (int32 i = 0; i < ReachDiff; ++i)
					{
						ResetOps.Add({ ECmd::Extend, 1 });
					}
				}
				else if (ReachDiff < 0)
				{
					// Need to retract
					for (int32 i = 0; i < -ReachDiff; ++i)
					{
						ResetOps.Add({ ECmd::Retract, 1 });
					}
				}

				// Add rotation adjustments - find shortest path
				// In hex coordinates, RotateL increases the facing value (0->1->2->3->4->5->0)
				// and RotateR decreases it (0->5->4->3->2->1->0)
				// So to go from current to target:
				// - If we need to increase facing, we use RotateL
				// - If we need to decrease facing, we use RotateR

				if (CurrentRotation != TargetRotation)
				{
					// Calculate steps needed in each direction
					// Steps using RotateL (counter-clockwise, increases facing)
					const int32 StepsCounterClockwise = (TargetRotation - CurrentRotation + 6) % 6;
					// Steps using RotateR (clockwise, decreases facing)
					const int32 StepsClockwise = (CurrentRotation - TargetRotation + 6) % 6;

					// Choose the shorter path
					if (StepsClockwise <= StepsCounterClockwise)
					{
						// Use clockwise rotation (RotateR)
						for (int32 i = 0; i < StepsClockwise; ++i)
						{
							ResetOps.Add({ ECmd::RotateR, 1 });
						}
					}
					else
					{
						// Use counter-clockwise rotation (RotateL)
						for (int32 i = 0; i < StepsCounterClockwise; ++i)
						{
							ResetOps.Add({ ECmd::RotateL, 1 });
						}
					}
				}

				// If we generated any reset operations, insert them into the tape
				if (ResetOps.Num() > 0)
				{
					// Insert the reset operations after the current PC
					S.Tape.Insert(ResetOps, S.PC + 1);
					bResetCommandExpanded = true;

					// Enable immediate execution of the first reset operation
					bContinueProcessing = true;
				}
				else
				{
					// Already at home position
				}
			}
			break;
			case ECmd::Repeat:  break;
			case ECmd::RotateHeldL:
				if (S.bHolding && S.Held.bIsCluster)
				{
					S.bRotationCompleted = false;
					S.Held.GrabFacing = (S.Held.GrabFacing + 5) % 6;
					// Defer simulation-held orientation to next fixed tick
					PendingGrabFacingByArm.Add(ArmIndex, S.Held.GrabFacing);
				}
				break;
			case ECmd::RotateHeldR:
				if (S.bHolding && S.Held.bIsCluster)
				{
					S.bRotationCompleted = false;
					S.Held.GrabFacing = (S.Held.GrabFacing + 1) % 6;
					// Defer simulation-held orientation to next fixed tick
					PendingGrabFacingByArm.Add(ArmIndex, S.Held.GrabFacing);
				}
				break;
			case ECmd::WaitForBond:
				if (S.bHolding)
				{
					const int32 CurrentCountNow = S.Held.bIsCluster ? S.Held.Orbs.Num() : 1;

					// Initialize the wait state on first entry using the CURRENT count
					if (!S.bWaitingForBond)
					{
						S.bWaitingForBond = true;
						S.WaitForBondInitialCount = CurrentCountNow;

						// If a bond already exists (eg. assimilation earlier this tick), proceed immediately
						if (CurrentCountNow > S.WaitForBondInitialCount)
						{
							S.bWaitingForBond = false;
							S.WaitForBondInitialCount = 0;
							bContinueProcessing = true;
							break;
						}
					}

					// Progress check
					if (CurrentCountNow > S.WaitForBondInitialCount)
					{
						S.bWaitingForBond = false;
						S.WaitForBondInitialCount = 0;
						bContinueProcessing = true;
					}
					else
					{
						break; // keep waiting
					}
				}
				break;
			}

			// Handle PC advancement based on command execution result
			// Some commands (like failed grabs or waiting) shouldn't advance PC
			bool bShouldAdvancePC = true;
			switch (Op.Cmd)
			{
			case ECmd::Wait:
				if (S.WaitCounter > 0) bShouldAdvancePC = false;
				break;
			case ECmd::Grab:
				// Check if we're still trying to grab (failed grab)
				// In the grab case above, if grab fails we break without setting bContinueProcessing
				// So if we get here and bContinueProcessing is false, it means grab failed
				if (!bContinueProcessing) bShouldAdvancePC = false;
				break;
			case ECmd::WaitForBond:
				if (S.bWaitingForBond) bShouldAdvancePC = false;
				break;
			default:
				break;
			}

			if (bShouldAdvancePC)
			{
				// Special handling for Reset command: don't advance PC if we just expanded the tape
				// The PC is already pointing to where it should be after insertion
				if (bResetCommandExpanded)
				{
					// PC stays where it is - it's now pointing at the Reset command
					// We need to advance it to the first inserted operation
					S.PC = (S.PC + 1) % S.Tape.Num();
				}
				else
				{
					int32 OldPC = S.PC;
					S.PC = (S.PC + 1) % S.Tape.Num();

					// Check if we wrapped around (looped)
					if (S.PC == 0 && OldPC > 0)
					{
						// We've looped! Check if the tape has been expanded by Reset commands
						// If so, we need to rebuild it from the original program

						// Find the corresponding actor and rebuild the tape
						ABasicArm* ArmActor = nullptr;
						if (UWorld* World = GetWorld())
						{
							for (TActorIterator<ABasicArm> It(World); It; ++It)
							{
								ABasicArm* TestArm = *It;
								if (TestArm && TestArm->SimArmIndex == ArmIndex)
								{
									ArmActor = TestArm;
									break;
								}
							}
						}

						if (ArmActor)
						{
							// Rebuild the tape from the original program
							TArray<FOp> OriginalTape = ArmActor->BuildOpsFromProgram(ArmActor->Program);
							if (S.Tape.Num() != OriginalTape.Num())
							{
								// Restore original tape to remove temporary Reset operations
								S.Tape = OriginalTape;
							}
						}
					}

				}
			}
		} // End of while loop for immediate execution

	}

	// Update previous orb counts for next tick's WaitForBond checks
	for (FArmState& S : Arms)
	{
		S.PrevHeldOrbCount = S.bHolding ? (S.Held.bIsCluster ? S.Held.Orbs.Num() : 1) : 0;
	}
}
void USimWorldHex::StepArmsSecondPass()
{
	// Second pass - only process arms that are trying to grab and don't have anything
	// This allows immediate grabbing of orbs that were just unbonded

	for (int32 ArmIndex = 0; ArmIndex < Arms.Num(); ArmIndex++)
	{
		FArmState& S = Arms[ArmIndex];
		if (S.Tape.Num() == 0) continue;

		// Only process arms that are trying to grab and failed in the first pass
		const FOp& Op = S.Tape[S.PC];
		if (Op.Cmd != ECmd::Grab || S.bHolding) continue;

		// Gate on committed sim state (delayed facing/length)
		const uint8 SimFace = SimFacingByArm.Contains(ArmIndex)
			? SimFacingByArm[ArmIndex]
			: uint8(S.Facing);
		const int32 SimLen = SimArmLenByArm.Contains(ArmIndex)
			? SimArmLenByArm[ArmIndex]
			: S.ArmLen;

		if (SimFace != uint8(S.Facing) || SimLen != S.ArmLen)
		{
			// Still in-flight rotation/extension; wait for next tick
			continue;
		}
		// Calculate target position
		const FHex Target = GetArmGripHex_Sim(ArmIndex);

		// Try to grab again
		FItem* Found = ItemMap.Find(Target);
		if (!Found)
		{
			FItem Temp;
			if (TryArmPickItem(Target, Temp))
			{
				if (const int32* DroppedByArm = RecentlyDroppedByArm.Find(Temp.Id))
				{
					if (*DroppedByArm == ArmIndex) continue;
				}

				S.Held = Temp;
				S.bHolding = true;
				S.PrevHeldOrbCount = 1;
				SimGrabFacingByArm.Add(ArmIndex, int8(-1));
				S.PC = (S.PC + 1) % S.Tape.Num();
			}
		}
		else
		{
			// Check for bonded cluster
			TSet<int32> ClusterIds;
			if (Found->Id != 0 && ItemToBonds.Contains(Found->Id))
			{
				// Gather all bonded items
				TArray<int32> Stack;
				Stack.Add(Found->Id);
				ClusterIds.Add(Found->Id);

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
							if (OtherId > 0 && !ClusterIds.Contains(OtherId))
							{
								ClusterIds.Add(OtherId);
								Stack.Add(OtherId);
							}
						}
					}
				}
			}

			if (ClusterIds.Num() <= 1)
			{
				// Single item
				FItem SingleOut;
				if (TryArmPickItem(Target, SingleOut))
				{
					if (const int32* DroppedByArm = RecentlyDroppedByArm.Find(SingleOut.Id))
					{
						if (*DroppedByArm == ArmIndex)
						{
							continue;
						}
					}

					S.Held = SingleOut;
					S.bHolding = true;
					S.PrevHeldOrbCount = 1;
					// NEW: single item has no held-rotation
					SimGrabFacingByArm.Add(ArmIndex, int8(-1));

					// Advance PC since grab succeeded
					S.PC = (S.PC + 1) % S.Tape.Num();
				}
			}
			else
			{
				// Multi-item cluster - grab the whole thing
				FItem Cluster;
				Cluster.bIsCluster = true;
				Cluster.Id = Found->Id;
				Cluster.GrabFacing = (int8)S.Facing;

				// Gather all cluster members
				bool bAllFound = true;
				for (int32 ItemId : ClusterIds)
				{
					FHex ItemHex;
					bool bItemFound = false;

					for (const auto& KV : ItemMap)
					{
						if (KV.Value.Id == ItemId)
						{
							ItemHex = KV.Key;
							bItemFound = true;
							break;
						}
					}

					if (!bItemFound)
					{
						bAllFound = false;
						break;
					}

					// Calculate relative position
					FHex D(ItemHex.Q - Target.Q, ItemHex.R - Target.R);

					FOrbData Orb;
					const FItem* It = ItemMap.Find(ItemHex);
					if (It && !It->bIsCluster)
					{
						// Disallow re-grab of items we dropped ourselves this tick
						if (const int32* DroppedByArm = RecentlyDroppedByArm.Find(It->Id))
						{
							if (*DroppedByArm == ArmIndex)
							{
								bAllFound = false; // treat as not ready yet
								break;
							}
						}

						Orb.Type = (uint8)It->Single.Type;
						Orb.Color = (uint8)It->Single.Color;
						Orb.ItemId = It->Id;
						Orb.dQ = (int8)D.Q;
						Orb.dR = (int8)D.R;
						Orb.Offset = FVector2D::ZeroVector;
						Cluster.Orbs.Add(Orb);
					}
				}

				if (bAllFound && Cluster.Orbs.Num() > 0)
				{
					// Remove all items from board
					for (const FOrbData& Sub : Cluster.Orbs)
					{
						for (auto It = ItemMap.CreateIterator(); It; ++It)
						{
							if (It.Value().Id == Sub.ItemId)
							{
								HeldItemLastHex.Add(Sub.ItemId, It.Key());
								It.RemoveCurrent();
								break;
							}
						}
					}

					S.Held = Cluster;
					S.bHolding = true;
					S.PrevHeldOrbCount = Cluster.Orbs.Num();
					// seed sim-held orientation immediately
					SimGrabFacingByArm.Add(ArmIndex, Cluster.GrabFacing);

					// Advance PC
					S.PC = (S.PC + 1) % S.Tape.Num();
					S.bReadyForGrab = false;
				}
				else
				{
					S.bReadyForGrab = true;
				}
			}
		}
	}

	// Helper to execute one fast op (Drop/Grab/Rotate/Extend/RotateHeld) with proper checks
	auto ExecFastOpOnce = [&](int32 ArmIndex)->bool
		{
			if (ArmIndex < 0 || ArmIndex >= Arms.Num()) return false;
			FArmState& S = Arms[ArmIndex];
			if (S.Tape.Num() == 0) return false;

			// Local helpers
			auto RotateAxial60Fn = [](const FHex& D, uint8 StepsCCW)->FHex
				{
					FHex r = D;
					for (uint8 i = 0; i < (StepsCCW % 6); ++i)
					{
						const int32 q = r.Q, rr = r.R;
						r.Q = -rr; r.R = q + rr;
					}
					return r;
				};
			auto ComputeGripHex = [](const FArmState& A)->FHex
				{
					FHex Grip = A.BaseHex;
					for (int i = 0; i < A.ArmLen; ++i)
						Grip = HexUtil::Neighbor(Grip, A.Facing);
					return Grip;
				};

			const FOp& Op = S.Tape[S.PC];

			switch (Op.Cmd)
			{
			case ECmd::Drop:
			{
				if (!S.bHolding) { S.PC = (S.PC + 1) % S.Tape.Num(); return true; }
				FHex GripTarget = ComputeGripHex(S);

				if (!S.Held.bIsCluster)
				{
					HeldItemLastHex.Remove(S.Held.Id);
					NewlyPlacedItems.Remove(S.Held.Id);
					RecentlyDroppedByArm.Remove(S.Held.Id);

					// Remove any ghost
					for (auto It = ItemMap.CreateIterator(); It; ++It)
					{
						if (It.Value().Id == S.Held.Id) { It.RemoveCurrent(); break; }
					}

					if (TryArmPlaceItem(GripTarget, S.Held))
					{
						S.bHolding = false;
						S.Held = FItem();
						S.bWaitingForBond = false;
						S.WaitForBondInitialCount = 0;
						S.PrevHeldOrbCount = 0;
						SimGrabFacingByArm.Add(ArmIndex, int8(-1));
						PendingGrabFacingByArm.Add(ArmIndex, int8(-1));
						SkipOutputThisFrame.Add(GripTarget);

						S.PC = (S.PC + 1) % S.Tape.Num();
						return true;
					}
					return false;
				}
				else
				{
					const uint8 CurFacingSteps = (uint8)S.Facing % 6;
					const int8 StoredGrabFacing = (int8)(S.Held.GrabFacing);
					const uint8 FacingDelta = (StoredGrabFacing >= 0)
						? (uint8)(((int32)CurFacingSteps - (int32)(StoredGrabFacing % 6) + 6) % 6)
						: CurFacingSteps;

					bool bBlocked = false;
					TArray<FHex> TargetHexes;
					TargetHexes.Reserve(S.Held.Orbs.Num());

					for (const FOrbData& Sub : S.Held.Orbs)
					{
						FHex Local(Sub.dQ, Sub.dR);
						const uint8 CCWSteps = (uint8)((6 - FacingDelta) % 6);
						FHex RotLocal = RotateAxial60Fn(Local, CCWSteps);

						FHex WorldHex(GripTarget.Q + RotLocal.Q, GripTarget.R + RotLocal.R);
						if (ItemMap.Contains(WorldHex)) { bBlocked = true; break; }
						TargetHexes.Add(WorldHex);
					}

					if (bBlocked) return false;

					for (const FOrbData& Sub : S.Held.Orbs)
					{
						HeldItemLastHex.Remove(Sub.ItemId);
						NewlyPlacedItems.Remove(Sub.ItemId);
						RecentlyDroppedByArm.Remove(Sub.ItemId);
						for (auto It = ItemMap.CreateIterator(); It; ++It)
						{
							if (It.Value().Id == Sub.ItemId) { It.RemoveCurrent(); break; }
						}
					}

					for (int32 idx = 0; idx < S.Held.Orbs.Num(); ++idx)
					{
						const FOrbData& Sub = S.Held.Orbs[idx];
						FOrb O; O.Type = (EOrbType)Sub.Type; O.Color = (EOrbColor)Sub.Color;

						FItem NewItem = FItem::MakeSingle(O);
						if (Sub.ItemId > 0) { NewItem.Id = Sub.ItemId; NextItemId = FMath::Max(NextItemId, NewItem.Id + 1); }
						else { NewItem.Id = NextItemId++; }

						if (TryPutItem(TargetHexes[idx], NewItem))
						{
							SkipOutputThisFrame.Add(TargetHexes[idx]);
							HeldItemLastHex.Remove(NewItem.Id);
						}
					}

					S.bHolding = false;
					S.Held = FItem();
					S.bWaitingForBond = false;
					S.WaitForBondInitialCount = 0;
					S.PrevHeldOrbCount = 0;
					SimGrabFacingByArm.Add(ArmIndex, int8(-1));
					PendingGrabFacingByArm.Add(ArmIndex, int8(-1));

					S.PC = (S.PC + 1) % S.Tape.Num();
					return true;
				}
			}

			case ECmd::Grab:
			{
				if (S.bHolding) { S.PC = (S.PC + 1) % S.Tape.Num(); return true; }

				FHex Target = ComputeGripHex(S);
				FItem* Found = ItemMap.Find(Target);

				// Single first
				if (!Found)
				{
					FItem Temp;
					if (TryArmPickItem(Target, Temp))
					{
						if (const int32* DroppedByArm = RecentlyDroppedByArm.Find(Temp.Id)) { if (*DroppedByArm == ArmIndex) return false; }
						S.Held = Temp; S.bHolding = true; S.PrevHeldOrbCount = 1; SimGrabFacingByArm.Add(ArmIndex, int8(-1));
						S.PC = (S.PC + 1) % S.Tape.Num();
						return true;
					}
					return false;
				}

				// Cluster BFS
				TSet<int32> ClusterIds;
				if (Found->Id != 0 && ItemToBonds.Contains(Found->Id))
				{
					TArray<int32> Stack; Stack.Add(Found->Id); ClusterIds.Add(Found->Id);
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
								if (OtherId > 0 && !ClusterIds.Contains(OtherId)) { ClusterIds.Add(OtherId); Stack.Add(OtherId); }
							}
						}
					}
				}

				if (ClusterIds.Num() <= 1)
				{
					FItem SingleOut;
					if (TryArmPickItem(Target, SingleOut))
					{
						if (const int32* DroppedByArm = RecentlyDroppedByArm.Find(SingleOut.Id)) { if (*DroppedByArm == ArmIndex) return false; }
						S.Held = SingleOut; S.bHolding = true; S.PrevHeldOrbCount = 1; SimGrabFacingByArm.Add(ArmIndex, int8(-1));
						S.PC = (S.PC + 1) % S.Tape.Num();
						return true;
					}
					return false;
				}
				else
				{
					FItem Cluster; Cluster.bIsCluster = true; Cluster.Id = Found->Id; Cluster.GrabFacing = (int8)S.Facing;
					bool bAllFound = true;
					for (int32 ItemId : ClusterIds)
					{
						FHex ItemHex; bool bItemFound = false;
						for (const auto& KV : ItemMap) { if (KV.Value.Id == ItemId) { ItemHex = KV.Key; bItemFound = true; break; } }
						if (!bItemFound) { bAllFound = false; break; }
						const FItem* It = ItemMap.Find(ItemHex);
						if (It && !It->bIsCluster)
						{
							if (const int32* DroppedByArm = RecentlyDroppedByArm.Find(It->Id)) { if (*DroppedByArm == ArmIndex) { bAllFound = false; break; } }
							const FHex D(ItemHex.Q - Target.Q, ItemHex.R - Target.R);
							FOrbData Orb; Orb.Type = (uint8)It->Single.Type; Orb.Color = (uint8)It->Single.Color; Orb.ItemId = It->Id; Orb.dQ = (int8)D.Q; Orb.dR = (int8)D.R;
							Cluster.Orbs.Add(Orb);
						}
					}
					if (!bAllFound || Cluster.Orbs.Num() == 0) return false;

					for (const FOrbData& Sub : Cluster.Orbs)
					{
						for (auto It = ItemMap.CreateIterator(); It; ++It)
						{
							if (It.Value().Id == Sub.ItemId) { HeldItemLastHex.Add(Sub.ItemId, It.Key()); It.RemoveCurrent(); break; }
						}
					}
					S.Held = Cluster; S.bHolding = true; S.PrevHeldOrbCount = Cluster.Orbs.Num(); SimGrabFacingByArm.Add(ArmIndex, Cluster.GrabFacing);
					S.PC = (S.PC + 1) % S.Tape.Num();
					return true;
				}
			}

			case ECmd::RotateHeldL:
				if (S.bHolding && S.Held.bIsCluster)
				{
					S.bRotationCompleted = false;
					S.Held.GrabFacing = (S.Held.GrabFacing + 5) % 6;
					PendingGrabFacingByArm.Add(ArmIndex, S.Held.GrabFacing);
				}
				S.PC = (S.PC + 1) % S.Tape.Num();
				return true;

			case ECmd::RotateHeldR:
				if (S.bHolding && S.Held.bIsCluster)
				{
					S.bRotationCompleted = false;
					S.Held.GrabFacing = (S.Held.GrabFacing + 1) % 6;
					PendingGrabFacingByArm.Add(ArmIndex, S.Held.GrabFacing);
				}
				S.PC = (S.PC + 1) % S.Tape.Num();
				return true;

			case ECmd::Extend:
			{
				const int32 Delta = (Op.Arg == 0 ? 1 : Op.Arg);
				const int32 OldLen = S.ArmLen;
				S.ArmLen = FMath::Clamp<int32>(S.ArmLen + Delta, 1, MAX_ARM_LEN);
				PendingArmLenByArm.Add(ArmIndex, S.ArmLen);
				if (bDevDebugArms && GEngine && S.ArmLen != OldLen)
				{
					FHex Hand = ComputeGripHex(S);
					GEngine->AddOnScreenDebugMessage(-1, 1.0f, FColor::Green,
						FString::Printf(TEXT("[Arm %d] EXTEND (Δ=%d) Len=%d Hand=(%d,%d)"), ArmIndex, Delta, S.ArmLen, Hand.Q, Hand.R));
				}
				S.PC = (S.PC + 1) % S.Tape.Num();
				return true;
			}

			case ECmd::Retract:
			{
				const int32 Delta = (Op.Arg == 0 ? 1 : Op.Arg);
				const int32 OldLen = S.ArmLen;
				S.ArmLen = FMath::Clamp<int32>(S.ArmLen - Delta, 1, MAX_ARM_LEN);
				PendingArmLenByArm.Add(ArmIndex, S.ArmLen);
				if (bDevDebugArms && GEngine && S.ArmLen != OldLen)
				{
					FHex Hand = ComputeGripHex(S);
					GEngine->AddOnScreenDebugMessage(-1, 1.0f, FColor::Orange,
						FString::Printf(TEXT("[Arm %d] RETRACT (Δ=%d) Len=%d Hand=(%d,%d)"), ArmIndex, Delta, S.ArmLen, Hand.Q, Hand.R));
				}
				S.PC = (S.PC + 1) % S.Tape.Num();
				return true;
			}

			case ECmd::RotateL:
			case ECmd::RotateR:
			{
				// Collision-aware rotate (same checks as StepArms)
				S.bRotationCompleted = false;

				// If previously blocked and waiting, consume the wait and retry next tick
				if (S.WaitCounter > 0)
				{
					--S.WaitCounter;
					return false; // don't advance PC here
				}

				EHexDir NewFacing = (Op.Cmd == ECmd::RotateL)
					? (EHexDir)(((uint8)S.Facing + 1) % 6)
					: (EHexDir)(((uint8)S.Facing + 5) % 6);

				// Hand positions before/after
				FHex OldHand = ComputeGripHex(S);
				EHexDir OldFacing = S.Facing;
				S.Facing = NewFacing;
				FHex NewHand = ComputeGripHex(S);
				S.Facing = OldFacing; // restore for now

				// Occupancy: world items
				TSet<FHex> WorldOcc;
				for (const auto& KV : ItemMap)
					WorldOcc.Add(KV.Key);

				// Other arms held occupancy (hands + virtual orbs)
				TSet<FHex> OtherHeldOcc;
				for (int32 OtherIdx = 0; OtherIdx < Arms.Num(); ++OtherIdx)
				{
					if (OtherIdx == ArmIndex) continue;
					const FArmState& O = Arms[OtherIdx];
					if (!O.bHolding) continue;

					const FHex OHand = ComputeGripHex(O);
					OtherHeldOcc.Add(OHand);

					if (O.Held.bIsCluster)
					{
						const uint8 cur = (uint8)O.Facing % 6;
						const int8 grab = (int8)O.Held.GrabFacing;
						const uint8 facingDelta = (grab >= 0)
							? (uint8)(((int32)cur - (int32)(grab % 6) + 6) % 6)
							: cur;
						const uint8 ccw = (uint8)((6 - facingDelta) % 6);
						for (const FOrbData& Sub : O.Held.Orbs)
						{
							FHex Local(Sub.dQ, Sub.dR);
							FHex RotLocal = RotateAxial60Fn(Local, ccw);
							OtherHeldOcc.Add(FHex(OHand.Q + RotLocal.Q, OHand.R + RotLocal.R));
						}
					}
					else
					{
						OtherHeldOcc.Add(OHand);
					}
				}

				// Build self locals for start and end, plus mid phases
				auto BuildLocals = [&](EHexDir Facing, TArray<FHex>& OutLocals)->void
					{
						OutLocals.Reset();
						if (!S.bHolding) return;

						// Grip at this facing
						const EHexDir Saved = S.Facing;
						S.Facing = Facing;
						FHex Grip = ComputeGripHex(S);
						S.Facing = Saved;

						if (!S.Held.bIsCluster)
						{
							OutLocals.Add(Grip);
							return;
						}

						const uint8 cur = (uint8)Facing % 6;
						const int8 grab = (int8)S.Held.GrabFacing;
						const uint8 facingDelta = (grab >= 0)
							? (uint8)(((int32)cur - (int32)(grab % 6) + 6) % 6)
							: cur;
						const uint8 ccw = (uint8)((6 - facingDelta) % 6);

						for (const FOrbData& Sub : S.Held.Orbs)
						{
							FHex Local(Sub.dQ, Sub.dR);
							FHex RotLocal = RotateAxial60Fn(Local, ccw);
							OutLocals.Add(FHex(Grip.Q + RotLocal.Q, Grip.R + RotLocal.R));
						}
					};

				auto OverlapsAny = [&](const FHex& H)->bool
					{
						return WorldOcc.Contains(H) || OtherHeldOcc.Contains(H);
					};

				TArray<FHex> StartLocals, EndLocals, MidTranslateLocals, MidRotateLocals;
				BuildLocals(S.Facing, StartLocals);
				BuildLocals(NewFacing, EndLocals);

				// Mid A: translate first (old orientation at new hand)
				if (S.bHolding)
				{
					if (!S.Held.bIsCluster) { MidTranslateLocals.Add(NewHand); }
					else
					{
						const uint8 curOld = (uint8)S.Facing % 6;
						const int8 grab = (int8)S.Held.GrabFacing;
						const uint8 facingDeltaOld = (grab >= 0)
							? (uint8)(((int32)curOld - (int32)(grab % 6) + 6) % 6)
							: curOld;
						const uint8 ccwOld = (uint8)((6 - facingDeltaOld) % 6);
						for (const FOrbData& Sub : S.Held.Orbs)
						{
							FHex Local(Sub.dQ, Sub.dR);
							FHex RotLocal = RotateAxial60Fn(Local, ccwOld);
							MidTranslateLocals.Add(FHex(NewHand.Q + RotLocal.Q, NewHand.R + RotLocal.R));
						}
					}
				}

				// Mid B: rotate first (new orientation at old hand)
				if (S.bHolding)
				{
					if (!S.Held.bIsCluster) { MidRotateLocals.Add(OldHand); }
					else
					{
						const uint8 curNew = (uint8)NewFacing % 6;
						const int8 grab = (int8)S.Held.GrabFacing;
						const uint8 facingDeltaNew = (grab >= 0)
							? (uint8)(((int32)curNew - (int32)(grab % 6) + 6) % 6)
							: curNew;
						const uint8 ccwNew = (uint8)((6 - facingDeltaNew) % 6);
						for (const FOrbData& Sub : S.Held.Orbs)
						{
							FHex Local(Sub.dQ, Sub.dR);
							FHex RotLocal = RotateAxial60Fn(Local, ccwNew);
							MidRotateLocals.Add(FHex(OldHand.Q + RotLocal.Q, OldHand.R + RotLocal.R));
						}
					}
				}

				bool bBlocked = false;

				// 1) Destination hand cannot contain foreign item/held orb if holding
				if (S.bHolding && OverlapsAny(NewHand)) bBlocked = true;

				// 2) End locals
				if (!bBlocked)
				{
					for (const FHex& H : EndLocals) { if (OverlapsAny(H)) { bBlocked = true; break; } }
				}

				// 3) Mid translate
				if (!bBlocked)
				{
					for (const FHex& H : MidTranslateLocals) { if (OverlapsAny(H)) { bBlocked = true; break; } }
				}

				// 4) Mid rotate
				if (!bBlocked)
				{
					for (const FHex& H : MidRotateLocals) { if (OverlapsAny(H)) { bBlocked = true; break; } }
				}

				if (bBlocked)
				{
					if (S.WaitCounter == 0) S.WaitCounter = 1;
					return false; // keep PC (retry next tick)
				}

				// Commit visual/current rotation immediately; sim-facing updates next tick
				S.Facing = NewFacing;
				PendingFacingByArm.Add(ArmIndex, static_cast<uint8>(S.Facing));
				S.PC = (S.PC + 1) % S.Tape.Num();
				return true;
			}

			default:
				return false;
			}
		};

	// NEW: finalize WaitForBond in the same tick (after StepBonds ran) and execute next op(s) immediately
	for (int32 ArmIndex = 0; ArmIndex < Arms.Num(); ArmIndex++)
	{
		FArmState& S = Arms[ArmIndex];
		if (S.Tape.Num() == 0) continue;

		if (S.PC < 0 || S.PC >= S.Tape.Num()) continue;
		const FOp& Op = S.Tape[S.PC];
		if (Op.Cmd != ECmd::WaitForBond) continue;
		if (!S.bHolding) continue;

		const int32 CurrentCount = S.Held.bIsCluster ? S.Held.Orbs.Num() : 1;

		// If bonds increased count this tick, clear wait and advance now
		if (S.bWaitingForBond && CurrentCount > S.WaitForBondInitialCount)
		{
			S.bWaitingForBond = false;
			S.WaitForBondInitialCount = 0;

			// Advance past WaitForBond
			S.PC = (S.PC + 1) % S.Tape.Num();

			// Execute up to 3 fast ops immediately (Rotate/Extend/RotateHeld/Grab/Drop)
			int executed = 0;
			const int kMaxFast = 3;
			while (executed < kMaxFast)
			{
				const int32 PrevPC = S.PC;
				if (!ExecFastOpOnce(ArmIndex)) break;
				++executed;
				// Safety: stop if PC didn't advance
				if (S.PC == PrevPC) break;
			}
		}
	}
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

