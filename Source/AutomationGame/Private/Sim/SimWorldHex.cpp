#include "Sim/SimWorldHex.h"
#include "Engine/Engine.h"
#include "Sim/HexCoord.h"
#include "Actors/BasicArm.h"
#include "Actors/OrbSimDriver.h"
#include "EngineUtils.h"
#include "DrawDebugHelpers.h"
#include "AGLogChannels.h"

// Near top (after includes) add if not already in header:
static constexpr float DEFAULT_BELT_SPEED_TILES_PER_SECOND = 1.0f;
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
	ItemMap.Add(H, Copy);
	OrbAt.Add(H, KindFromItem(Copy));
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

void USimWorldHex::StepSim()
{
	PrevItemHex.Empty();
	for (const auto& KV : ItemMap)
		if (KV.Value.Id != 0)
			PrevItemHex.Add(KV.Value.Id, KV.Key);
	ItemsMovedLastStep.Reset();

	++StepCounter;
	SkipInputThisFrame.Reset();
	SkipOutputThisFrame.Reset();
	OrbsNewlyMovedToOutput.Reset();

	SpawnInputs();          // pass 1 (empty at start)
	StepArms();
	StepBelts();
	SpawnInputsAfterBelts(); // NEW pass 2 (inputs just vacated by belt moves)
	StepBonds();
	ProcessOutputs();
	
	// Debug visualization
	DrawBondDebugLines();
}
void USimWorldHex::StepGlyphs()
{
	// Legacy entry point if other code still calls it:
	ProcessOutputs();
}
void USimWorldHex::StepArms()
{
	// Build reverse lookup (item id -> hex) once for this arm step (excludes held items)
	TMap<int32, FHex> ItemIdToHex;
	ItemIdToHex.Reserve(ItemMap.Num());
	for (const auto& KV : ItemMap)
	{
		if (KV.Value.Id != 0)
			ItemIdToHex.Add(KV.Value.Id, KV.Key);
	}

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
		const FOp& Op = S.Tape[S.PC];
		
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
				continue;
			}
			S.WaitCounter = 0;
			if (bDebugLogs && GEngine)
			{
				GEngine->AddOnScreenDebugMessage(-1, 1.0f, FColor::Green,
					FString::Printf(TEXT("Arm %d: Wait complete, advancing to next instruction"), ArmIndex));
			}
		} break;

		case ECmd::Grab:
			if (!S.bHolding)
			{
				bool bGrabSucceeded = false;            // NEW

				FHex Target = S.BaseHex;
				for (int i = 0; i < S.ArmLen; ++i)
					Target = HexUtil::Neighbor(Target, S.Facing);

				FItem* Found = ItemMap.Find(Target);
				
				// Check if another arm is already trying to grab this cluster
				bool bAnotherArmGrabbing = false;
				if (Found)
				{
					// Check if any other arm is pending a grab of this cluster
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
				
				// If another arm is grabbing this cluster, wait
				if (bAnotherArmGrabbing)
				{
					if (bDebugLogs && GEngine)
					{
						GEngine->AddOnScreenDebugMessage(-1, 1.0f, FColor::Orange,
							FString::Printf(TEXT("Arm %d: Another arm is grabbing cluster at (%d,%d), waiting"), ArmIndex, Target.Q, Target.R));
					}
					continue; // Wait for next tick
				}
				if (!Found)
				{
					FItem Temp;
					if (TryArmPickItem(Target, Temp))
					{
						// Check if this orb was recently dropped by this same arm
						if (const int32* DroppedByArm = RecentlyDroppedByArm.Find(Temp.Id))
						{
							if (*DroppedByArm == ArmIndex)
							{
								// This arm just dropped this orb, don't pick it up immediately
								continue;
							}
						}
						
						S.Held = Temp;
						S.bHolding = true;
						SkipInputThisFrame.Add(Target);
						bGrabSucceeded = true;        // NEW
					}
				}
				else
				{
					TSet<int32> ClusterIds;
					if (Found->Id != 0 && ItemToBonds.Contains(Found->Id))
						GatherBondedClusterFn(Found->Id, ClusterIds);

					if (ClusterIds.Num() <= 1)
					{
						// Clear old HeldItemLastHex entries for this arm's previous item
						if (S.bHolding)
						{
							if (S.Held.bIsCluster)
							{
								for (const FOrbData& OldSub : S.Held.Orbs)
								{
									if (OldSub.ItemId > 0)
									{
										HeldItemLastHex.Remove(OldSub.ItemId);
									}
								}
							}
							else if (S.Held.Id > 0)
							{
								HeldItemLastHex.Remove(S.Held.Id);
							}
						}
						
						FItem SingleOut;
						if (TryArmPickItem(Target, SingleOut))
						{
							// Check if this orb was recently dropped by this same arm
							if (const int32* DroppedByArm = RecentlyDroppedByArm.Find(SingleOut.Id))
							{
								if (*DroppedByArm == ArmIndex)
								{
									// This arm just dropped this orb, don't pick it up immediately
									continue;
								}
							}
							
							S.Held = SingleOut;
							S.bHolding = true;
							SkipInputThisFrame.Add(Target);
							bGrabSucceeded = true;    // NEW
						}
					}
					else
					{
						// Clear old HeldItemLastHex entries for this arm's previous cluster
						// This prevents bond formation based on old cluster positions
						if (S.bHolding && S.Held.bIsCluster)
						{
							for (const FOrbData& OldSub : S.Held.Orbs)
							{
								if (OldSub.ItemId > 0)
								{
									HeldItemLastHex.Remove(OldSub.ItemId);
								}
							}
						}
						
						FItem Cluster;
						Cluster.bIsCluster = true;
						Cluster.Id = Found->Id;                 // Arbitrary representative Id (not reused for sub‑orbs)
						Cluster.GrabFacing = (int8)S.Facing;

						// Gather cluster members (Id -> local offsets)
						TArray<int32> ClusterOrder;
						ClusterOrder.Reserve(ClusterIds.Num());
						for (int32 ItemId : ClusterIds)
						{
							const FHex* HexPtr = nullptr;
							for (const auto& KV : ItemMap)
								if (KV.Value.Id == ItemId) { HexPtr = &KV.Key; break; }
							if (!HexPtr) continue;

							FHex D(HexPtr->Q - Target.Q, HexPtr->R - Target.R);
							const FVector2D Off2D = HexCoord::AxialToXY(FHexAxial(D.Q, D.R), 1.f, GridOrientation);

							FOrbData Orb;
							if (const FItem* It = ItemMap.Find(*HexPtr))
							{
								if (!It->bIsCluster)
								{
									Orb.Type = (uint8)It->Single.Type;
									Orb.Color = (uint8)It->Single.Color;
								}
								Orb.ItemId = It->Id;         // preserve original Id
							}
							Orb.Offset = Off2D;
							Orb.dQ = (int8)D.Q;
							Orb.dR = (int8)D.R;
							Cluster.Orbs.Add(Orb);
							ClusterOrder.Add(Orb.ItemId);
						}

						// Preserve last hex positions for ALL items so CleanupBrokenBonds keeps offsets/bonds alive.
						for (int32 ItemId : ClusterIds)
						{
							for (const auto& KV : ItemMap)
								if (KV.Value.Id == ItemId)
									HeldItemLastHex.FindOrAdd(ItemId) = KV.Key;
						}

						// Remove only the items themselves from the board (keep Bonds unchanged!)
						for (int32 ItemId : ClusterIds)
						{
							FHex RemoveHex; bool bFoundHex = false;
							for (const auto& KV : ItemMap)
								if (KV.Value.Id == ItemId) { RemoveHex = KV.Key; bFoundHex = true; break; }
							if (bFoundHex)
							{
								FItem Dummy;
								TryTakeItem(RemoveHex, Dummy);
							}
						}

						if (Cluster.Orbs.Num() > 0)
						{
							S.Held = Cluster;
							S.bHolding = true;
							SkipInputThisFrame.Add(Target);
							bGrabSucceeded = true;
						}
					}
				}

				// If grab failed (nothing there), wait for next tick (retry)
				if (!bGrabSucceeded)
				{
					continue; // Wait for next tick to retry grab
				}
				
				// If grab succeeded, register pending grab and let normal flow advance PC
				if (bGrabSucceeded && Found)
				{
					// Register this arm as grabbing this cluster
					FPendingGrab PendingGrab;
					PendingGrab.ArmIndex = ArmIndex;
					PendingGrab.TargetHex = Target;
					PendingGrab.ClusterId = Found->Id;
					PendingGrabs.Add(PendingGrab);
					
					if (bDebugLogs && GEngine)
					{
						GEngine->AddOnScreenDebugMessage(-1, 1.0f, FColor::Green,
							FString::Printf(TEXT("Arm %d: Grab succeeded, registered pending grab for cluster %d"), ArmIndex, Found->Id));
					}
				}
				else if (bGrabSucceeded)
				{
					if (bDebugLogs && GEngine)
					{
						GEngine->AddOnScreenDebugMessage(-1, 1.0f, FColor::Green,
							FString::Printf(TEXT("Arm %d: Grab succeeded, will advance to next instruction"), ArmIndex));
					}
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
						SkipOutputThisFrame.Add(GripTarget);
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
			if (OverlapsAny(NewHand))
			{
				bBlocked = true;
			}

			// 2) End positions of all sub-orbs
			if (!bBlocked)
			{
				for (const FHex& H : EndLocals)
					if (OverlapsAny(H)) { bBlocked = true; break; }
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

			// Commit rotation
			S.Facing = NewFacing;
		}
		break;

		case ECmd::Retract:
		{
			const int32 Delta = (Op.Arg == 0 ? 1 : Op.Arg);
			const int32 OldLen = S.ArmLen;
			S.ArmLen = FMath::Clamp<int32>(S.ArmLen - Delta, 1, MAX_ARM_LEN);

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
			S.Facing = S.HomeFacing; S.ArmLen = S.HomeArmLen; S.TrackIdx = S.HomeTrackIdx; break;
		case ECmd::Repeat:  break;
		}

		S.PC = (S.PC + 1) % S.Tape.Num();
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
				// FrameAlpha (0..1) provided externally each render frame
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

	const float* PLogicPtr = BeltProgress.Find(ItemId);
	float LogicalP = PLogicPtr ? *PLogicPtr : 0.f;
	const bool bBlockedEdge = LogicalP >= BeltBlockedClamp - 0.0001f;

	float VisualP = LogicalP;
	if (!bBlockedEdge)
	{
		const float Dt = FMath::Max(0.f, FixedStepSec);
		const float TilesPerSec = (BeltSpeedTilesPerSecond > 0.f) ? BeltSpeedTilesPerSecond
			: (BeltSpeedTilesPerTick / FMath::Max(KINDA_SMALL_NUMBER, Dt));
		// Interpolate using elapsed real time within this step
		VisualP = FMath::Min(BeltBlockedClamp - 0.0001f,
			LogicalP + FrameAlpha * TilesPerSec * Dt);
	}

	// Clamp to [0,1) for interpolation across THIS belt segment
	VisualP = FMath::Clamp(VisualP, 0.f, 0.9999f);

	const uint8 Dir = (uint8)(Tile->Arg & 0xFF);
	const FHex NextHex = HexUtil::Neighbor(CurHex, (EHexDir)(Dir % 6));
	const FVector NextCenter = HexCenterToWorld(NextHex);

	OutWorld = FMath::Lerp(CurCenter, NextCenter, VisualP);
	
	return true;
}

// AddBoardOrbToHeldCluster() and AddBoardOrbToHeldItem() moved to SimWorldHex_Clusters.cpp

// CheckAndRemoveDisconnectedOrbsFromHeldClusters() moved to SimWorldHex_Clusters.cpp
// CheckAndRemoveDisconnectedOrbsFromHeldClusters() moved to SimWorldHex_Clusters.cpp



void USimWorldHex::DrawBondDebugLines()
{
	if (!bDebugLines || !bDebugBonds || Bonds.Num() == 0)
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
		FVector PosA, PosB;
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
			for (const FArmState& Arm : Arms)
			{
				if (!Arm.bHolding) continue;
				
				if (Arm.Held.bIsCluster)
				{
					for (const FOrbData& Sub : Arm.Held.Orbs)
					{
						if (Sub.ItemId == Bond.ItemA && !bFoundA)
						{
							// Calculate held orb position
							FHex GripHex = Arm.BaseHex;
							for (int i = 0; i < Arm.ArmLen; ++i)
								GripHex = HexUtil::Neighbor(GripHex, Arm.Facing);
							
							const uint8 cur = (uint8)Arm.Facing % 6;
							const int8 grab = (int8)Arm.Held.GrabFacing;
							const uint8 facingDelta = (grab >= 0)
								? (uint8)(((int32)cur - (int32)(grab % 6) + 6) % 6)
								: cur;
							const uint8 ccw = (uint8)((6 - facingDelta) % 6);
							
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
							
							FHex Local(Sub.dQ, Sub.dR);
							FHex Rot = Rot60(Local, ccw);
							FHex WorldHex(GripHex.Q + Rot.Q, GripHex.R + Rot.R);
							PosA = HexToWorld(WorldHex);
							bFoundA = true;
						}
						if (Sub.ItemId == Bond.ItemB && !bFoundB)
						{
							// Calculate held orb position (same logic as above)
							FHex GripHex = Arm.BaseHex;
							for (int i = 0; i < Arm.ArmLen; ++i)
								GripHex = HexUtil::Neighbor(GripHex, Arm.Facing);
							
							const uint8 cur = (uint8)Arm.Facing % 6;
							const int8 grab = (int8)Arm.Held.GrabFacing;
							const uint8 facingDelta = (grab >= 0)
								? (uint8)(((int32)cur - (int32)(grab % 6) + 6) % 6)
								: cur;
							const uint8 ccw = (uint8)((6 - facingDelta) % 6);
							
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
							
							FHex Local(Sub.dQ, Sub.dR);
							FHex Rot = Rot60(Local, ccw);
							FHex WorldHex(GripHex.Q + Rot.Q, GripHex.R + Rot.R);
							PosB = HexToWorld(WorldHex);
							bFoundB = true;
						}
					}
				}
				else if (Arm.Held.Id == Bond.ItemA && !bFoundA)
				{
					// Single held item
					FHex GripHex = Arm.BaseHex;
					for (int i = 0; i < Arm.ArmLen; ++i)
						GripHex = HexUtil::Neighbor(GripHex, Arm.Facing);
					PosA = HexToWorld(GripHex);
					bFoundA = true;
				}
				else if (Arm.Held.Id == Bond.ItemB && !bFoundB)
				{
					// Single held item
					FHex GripHex = Arm.BaseHex;
					for (int i = 0; i < Arm.ArmLen; ++i)
						GripHex = HexUtil::Neighbor(GripHex, Arm.Facing);
					PosB = HexToWorld(GripHex);
					bFoundB = true;
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
		
		// Draw the bond line (persistent for 1 second to avoid flickering)
		DrawDebugLine(World, PosA, PosB, FColor::Cyan, false, 1.0f, 0, 2.f);
		
		// Draw small spheres at bond endpoints (persistent for 1 second)
		DrawDebugSphere(World, PosA, 4.f, 8, FColor::Cyan, false, 1.0f, 0, 1.f);
		DrawDebugSphere(World, PosB, 4.f, 8, FColor::Cyan, false, 1.0f, 0, 1.f);
		
		// Draw bond ID text at midpoint (persistent for 1 second)
		FVector Midpoint = (PosA + PosB) * 0.5f;
		Midpoint.Z += 10.f;
		DrawDebugString(World, Midpoint, 
			FString::Printf(TEXT("Bond: %d-%d"), Bond.ItemA, Bond.ItemB), 
			nullptr, FColor::Cyan, 1.0f, true, 0.8f);
	}
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


void USimWorldHex::OnArmRotationComplete(int32 ArmIndex)
{

	// Process any pending bond formations for this arm immediately
	for (int32 i = PendingBondFormations.Num() - 1; i >= 0; i--)
	{
		FPendingBondFormation& Pending = PendingBondFormations[i];
		
		if (Pending.ArmIndex == ArmIndex)
		{
			// Check if the arm is still valid and holding something
			if (ArmIndex >= 0 && ArmIndex < Arms.Num())
			{
				const FArmState& Arm = Arms[ArmIndex];
				if (Arm.bHolding)
				{
					// Check if the board item still exists
					const FItem* BoardItem = ItemMap.Find(Pending.BoardItemHex);
					if (BoardItem && BoardItem->Id == Pending.BoardItemId)
					{
						// Form the bond immediately - use the current held item ID
						if (bDebugLogs && bDevDebugBonds && GEngine)
						{
							GEngine->AddOnScreenDebugMessage(-1, 1.0f, FColor::Green,
								FString::Printf(TEXT("Arm rotation complete - forming bond: board orb %d -> held item %d"), Pending.BoardItemId, Arm.Held.Id));
						}
						
						AddBoardOrbToHeldItem(Arm.Held.Id, Pending.BoardItemId, Pending.BoardItemHex);
					}
				}
			}
			
			PendingBondFormations.RemoveAt(i);
		}
	}
	
	// Note: Unbond processing is now handled in OnArmRotationFullyComplete()
	// This function only handles bond formation at 95% rotation completion
}

void USimWorldHex::OnArmRotationFullyComplete(int32 ArmIndex)
{
	// Mark this arm as ready for unbond processing with its current facing direction
	// The actual unbond processing will happen during the next simulation step
	// This function is idempotent - it can be called multiple times safely
	if (ArmIndex >= 0 && ArmIndex < Arms.Num())
	{
		// Check if we already have this arm marked for unbond processing
		// This prevents unnecessary work if called multiple times
		if (ArmsReadyForUnbond.Contains(ArmIndex))
		{
			return; // Already marked, no need to do it again
		}
		
		// Get the arm's visual facing direction when rotation fully completes
		uint8 VisualFacing = (uint8)Arms[ArmIndex].Facing; // Default to simulation facing
		if (UWorld* World = GetWorld())
		{
			for (TActorIterator<ABasicArm> ActorItr(World); ActorItr; ++ActorItr)
			{
				ABasicArm* ArmActor = *ActorItr;
				if (ArmActor && ArmActor->SimArmIndex == ArmIndex)
				{
					// Get the visual facing from the arm actor
					VisualFacing = ArmActor->FacingDir;
					break;
				}
			}
		}
		
		// Store the arm index and the facing direction when rotation fully completed
		ArmsReadyForUnbond.Add(ArmIndex, VisualFacing);
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

