#include "Sim/SimWorldHex.h"
#include "Engine/Engine.h"
#include "Sim/HexCoord.h"
#include "Actors/BasicArm.h"
#include "Actors/OrbSimDriver.h"
#include "EngineUtils.h"
#include "DrawDebugHelpers.h"

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
static bool bDevDebugArms = false; // set true to see arm/hand debug messages
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

		switch (Op.Cmd)
		{
		case ECmd::Wait:
		{
			if (S.WaitCounter == 0) S.WaitCounter = (Op.Arg == 0 ? 1 : Op.Arg);
			--S.WaitCounter;
			if (S.WaitCounter > 0) goto AdvancePC;
			S.WaitCounter = 0;
		} break;

		case ECmd::Grab:
			if (!S.bHolding)
			{
				bool bGrabSucceeded = false;            // NEW

				FHex Target = S.BaseHex;
				for (int i = 0; i < S.ArmLen; ++i)
					Target = HexUtil::Neighbor(Target, S.Facing);

				FItem* Found = ItemMap.Find(Target);
				if (!Found)
				{
					FItem Temp;
					if (TryArmPickItem(Target, Temp))
					{
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

				// NEW: If grab failed (nothing there), do NOT advance PC; retry next tick.
				if (!bGrabSucceeded)
					continue;
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

	AdvancePC:
		S.PC = (S.PC + 1) % S.Tape.Num();
	}
}

void USimWorldHex::StepBonds()
{
	if (TileMap.Num() == 0 || ItemMap.Num() == 0)
	{
		if (Bonds.Num() > 0) CleanupBrokenBonds(TSet<int32>());
		return;
	}

	TSet<int32> HeldIds;
	for (const FArmState& A : Arms)
	{
		if (!A.bHolding) continue;

		if (A.Held.bIsCluster)
		{
			for (const FOrbData& Sub : A.Held.Orbs)
				if (Sub.ItemId > 0)
					HeldIds.Add(Sub.ItemId);
		}
		else if (A.Held.Id != 0)
		{
			HeldIds.Add(A.Held.Id);
		}
	}

	CleanupBrokenBonds(HeldIds);

	// Build virtual occupancy for held orbs (cluster only)
	TMap<FHex, int32> VirtualHeldItemAt;
	for (const FArmState& A : Arms)
	{
		if (!A.bHolding || !A.Held.bIsCluster) continue;

		FHex GripHex = A.BaseHex;
		for (int i = 0; i < A.ArmLen; ++i)
			GripHex = HexUtil::Neighbor(GripHex, A.Facing);

		const uint8 cur = (uint8)A.Facing % 6;
		const int8 grab = (int8)A.Held.GrabFacing;
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

		for (const FOrbData& Sub : A.Held.Orbs)
		{
			if (Sub.ItemId <= 0) continue;
			FHex Local(Sub.dQ, Sub.dR);
			FHex Rot = Rot60(Local, ccw);
			FHex World(GripHex.Q + Rot.Q, GripHex.R + Rot.R);
			VirtualHeldItemAt.FindOrAdd(World, Sub.ItemId);
		}
	}

	// Check if any arms are currently moving or rotating
	bool bAnyArmMoving = false;
	int32 MovingArmCount = 0;
	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<ABasicArm> ActorItr(World); ActorItr; ++ActorItr)
		{
			ABasicArm* ArmActor = *ActorItr;
			if (ArmActor && ArmActor->SimArmIndex >= 0 && ArmActor->SimArmIndex < Arms.Num())
			{
				// Check if the arm is currently animating (rotating, moving, etc.)
				if (ArmActor->IsAnimating())
				{
					bAnyArmMoving = true;
					MovingArmCount++;
					if (bDebugLogs && bDevDebugBonds && GEngine)
					{
						GEngine->AddOnScreenDebugMessage(-1, 0.5f, FColor::Yellow,
							FString::Printf(TEXT("Arm %d is animating (StepAlpha=%.3f)"), 
								ArmActor->SimArmIndex, ArmActor->GetStepAlpha()));
					}
				}
			}
		}
	}
	
	// Only form bonds when no arms are moving to prevent premature bond formation
	// But be more lenient - only skip if arms are significantly animating
	if (bAnyArmMoving)
	{
		// Check if any arms are just finishing their animation (StepAlpha > 0.9)
		bool bAllArmsNearlyFinished = true;
		if (UWorld* World = GetWorld())
		{
			for (TActorIterator<ABasicArm> ActorItr(World); ActorItr; ++ActorItr)
			{
				ABasicArm* ArmActor = *ActorItr;
				if (ArmActor && ArmActor->SimArmIndex >= 0 && ArmActor->SimArmIndex < Arms.Num())
				{
					if (ArmActor->IsAnimating() && ArmActor->GetStepAlpha() < 0.9f)
					{
						bAllArmsNearlyFinished = false;
						break;
					}
				}
			}
		}
		
		// If arms are nearly finished animating, allow bond formation
		if (!bAllArmsNearlyFinished)
		{
			if (bDebugLogs && bDevDebugBonds && GEngine)
			{
				GEngine->AddOnScreenDebugMessage(-1, 0.5f, FColor::Yellow,
					FString::Printf(TEXT("Skipping bond formation - %d arm(s) moving"), MovingArmCount));
			}
			return;
		}
		else if (bDebugLogs && bDevDebugBonds && GEngine)
		{
			GEngine->AddOnScreenDebugMessage(-1, 0.5f, FColor::Green,
				TEXT("Allowing bond formation - arms nearly finished"));
		}
	}

	// Bond formation (world or virtual held)
	TSet<int32> Newly;
	for (const auto& KV : TileMap)
	{
		const FTile& T = KV.Value;
		if (T.Kind != ETileKind::Bond) continue;
		const uint8 Dir = (uint8)(T.Arg & 0xFF);
		const FHex AHex = KV.Key;
		const FHex BHex = HexUtil::Neighbor(AHex, (EHexDir)(Dir % 6));

		// Resolve item ids at endpoints (world or virtual held)
		int32 AId = 0, BId = 0;
		bool bAIsHeld = false, bBIsHeld = false;
		
		if (FItem* IA = ItemMap.Find(AHex)) 
		{
			AId = IA->Id;
			bAIsHeld = false; // On board
		}
		else if (int32* HeldA = VirtualHeldItemAt.Find(AHex)) 
		{
			AId = *HeldA;
			bAIsHeld = true; // Held
		}

		if (FItem* IB = ItemMap.Find(BHex)) 
		{
			BId = IB->Id;
			bBIsHeld = false; // On board
		}
		else if (int32* HeldB = VirtualHeldItemAt.Find(BHex)) 
		{
			BId = *HeldB;
			bBIsHeld = true; // Held
		}
		
		// Debug: Log when we expect to find held orbs but don't
		if (bDevDebugBonds && GEngine)
		{
			// Check if there should be held orbs at these positions
			bool bExpectedHeldAtA = false, bExpectedHeldAtB = false;
			for (const FArmState& A : Arms)
			{
				if (!A.bHolding || !A.Held.bIsCluster) continue;
				
				FHex GripHex = A.BaseHex;
				for (int i = 0; i < A.ArmLen; ++i)
					GripHex = HexUtil::Neighbor(GripHex, A.Facing);
				
				const uint8 cur = (uint8)A.Facing % 6;
				const int8 grab = (int8)A.Held.GrabFacing;
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
				
				for (const FOrbData& Sub : A.Held.Orbs)
				{
					if (Sub.ItemId <= 0) continue;
					FHex Local(Sub.dQ, Sub.dR);
					FHex Rot = Rot60(Local, ccw);
					FHex World(GripHex.Q + Rot.Q, GripHex.R + Rot.R);
					
					if (World == AHex) bExpectedHeldAtA = true;
					if (World == BHex) bExpectedHeldAtB = true;
				}
			}
			
			if (bExpectedHeldAtA && !bAIsHeld)
			{
				GEngine->AddOnScreenDebugMessage(-1, 1.0f, FColor::Red,
					FString::Printf(TEXT("MISSING: Expected held orb at A(%d,%d) but not found in VirtualHeldItemAt"), AHex.Q, AHex.R));
			}
			if (bExpectedHeldAtB && !bBIsHeld)
			{
				GEngine->AddOnScreenDebugMessage(-1, 1.0f, FColor::Red,
					FString::Printf(TEXT("MISSING: Expected held orb at B(%d,%d) but not found in VirtualHeldItemAt"), BHex.Q, BHex.R));
			}
		}

		if (AId <= 0 || BId <= 0 || AId == BId) continue;
		
		// Allow bonds between held and board items, but handle them specially
		// Don't form bonds between two held items from different arms
		if (bAIsHeld && bBIsHeld) 
		{
			if (bDebugLogs && bDevDebugBonds && GEngine)
			{
				GEngine->AddOnScreenDebugMessage(-1, 0.5f, FColor::Orange,
					FString::Printf(TEXT("Skipping bond formation - both items %d and %d are held"), AId, BId));
			}
			continue;
		}

		// Prevent duplicates
		bool bDup = false;
		if (const TArray<int32>* AList = ItemToBonds.Find(AId))
		{
			for (int32 BondIdx : *AList)
			{
				const FBond& B = Bonds[BondIdx];
				if ((B.ItemA == AId && B.ItemB == BId) ||
					(B.ItemA == BId && B.ItemB == AId))
				{
					bDup = true; break;
				}
			}
		}
		if (bDup) continue;

		// Form bond even if one/both held; we store their last hex / virtual hex. CleanupBrokenBonds
		// keeps offsets coherent as positions change when dropped.
		FBond NewB;
		NewB.ItemA = AId;
		NewB.ItemB = BId;
		NewB.Root = AHex;
		NewB.dQ = (int8)(BHex.Q - AHex.Q);
		NewB.dR = (int8)(BHex.R - AHex.R);
		NewB.bActive = true;
		
		// Form bond between items
		const int32 NewIdx = Bonds.Add(NewB);
		AddItemBondLink(NewB.ItemA, NewIdx);
		AddItemBondLink(NewB.ItemB, NewIdx);
		Newly.Add(NewB.ItemA);
		Newly.Add(NewB.ItemB);

		if (bDebugLogs && bDevDebugBonds && GEngine)
		{
			GEngine->AddOnScreenDebugMessage(-1, 0.6f, FColor::Purple,
				FString::Printf(TEXT("Bond formed: %d-%d"), AId, BId));
		}

		// Handle held-to-board bonds: add board orb to held cluster with correct positioning
		if (bAIsHeld && !bBIsHeld)
		{
			// A is held, B is on board - add B to the held cluster
			if (bDebugLogs && bDevDebugBonds && GEngine)
			{
				GEngine->AddOnScreenDebugMessage(-1, 1.0f, FColor::Orange,
					FString::Printf(TEXT("Adding board orb %d to held cluster (held orb %d)"), BId, AId));
			}
			AddBoardOrbToHeldCluster(AId, BId, BHex);
		}
		else if (bBIsHeld && !bAIsHeld)
		{
			// B is held, A is on board - add A to the held cluster
			if (bDebugLogs && bDevDebugBonds && GEngine)
			{
				GEngine->AddOnScreenDebugMessage(-1, 1.0f, FColor::Orange,
					FString::Printf(TEXT("Adding board orb %d to held cluster (held orb %d)"), AId, BId));
			}
			AddBoardOrbToHeldCluster(BId, AId, AHex);
		}
	}

	// Opus Magnum style: Form bonds between sub-orbs within held clusters
	for (const FArmState& A : Arms)
	{
		if (!A.bHolding || !A.Held.bIsCluster) continue;

		// Check if any bond tiles are adjacent to this held cluster
		FHex GripHex = A.BaseHex;
		for (int i = 0; i < A.ArmLen; ++i)
			GripHex = HexUtil::Neighbor(GripHex, A.Facing);

		const uint8 cur = (uint8)A.Facing % 6;
		const int8 grab = (int8)A.Held.GrabFacing;
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

		// Check each bond tile to see if it's adjacent to any sub-orbs in this cluster
		for (const auto& KV : TileMap)
		{
			const FTile& T = KV.Value;
			if (T.Kind != ETileKind::Bond) continue;
			const uint8 Dir = (uint8)(T.Arg & 0xFF);
			const FHex BondHex = KV.Key;
			const FHex BondTargetHex = HexUtil::Neighbor(BondHex, (EHexDir)(Dir % 6));

			// Check if this bond tile is adjacent to any sub-orbs in the held cluster
			for (const FOrbData& SubA : A.Held.Orbs)
			{
				if (SubA.ItemId <= 0) continue;
				
				FHex LocalA(SubA.dQ, SubA.dR);
				FHex RotA = Rot60(LocalA, ccw);
				FHex WorldA(GripHex.Q + RotA.Q, GripHex.R + RotA.R);

				// Check if this sub-orb is at the bond tile position
				if (WorldA == BondHex)
				{
					// Look for another sub-orb at the bond target position
					for (const FOrbData& SubB : A.Held.Orbs)
					{
						if (SubB.ItemId <= 0 || SubB.ItemId == SubA.ItemId) continue;
						
						FHex LocalB(SubB.dQ, SubB.dR);
						FHex RotB = Rot60(LocalB, ccw);
						FHex WorldB(GripHex.Q + RotB.Q, GripHex.R + RotB.R);

						if (WorldB == BondTargetHex)
						{
							// Form internal cluster bond
							FBond InternalBond;
							InternalBond.ItemA = SubA.ItemId;
							InternalBond.ItemB = SubB.ItemId;
							InternalBond.Root = BondHex;
							InternalBond.dQ = (int8)(BondTargetHex.Q - BondHex.Q);
							InternalBond.dR = (int8)(BondTargetHex.R - BondHex.R);
							InternalBond.bActive = true;

							// Check for duplicates
							bool bDup = false;
							if (const TArray<int32>* AList = ItemToBonds.Find(SubA.ItemId))
							{
								for (int32 BondIdx : *AList)
								{
									const FBond& B = Bonds[BondIdx];
									if ((B.ItemA == SubA.ItemId && B.ItemB == SubB.ItemId) ||
										(B.ItemA == SubB.ItemId && B.ItemB == SubA.ItemId))
									{
										bDup = true; break;
									}
								}
							}
							if (bDup) continue;

							const int32 NewIdx = Bonds.Add(InternalBond);
							AddItemBondLink(InternalBond.ItemA, NewIdx);
							AddItemBondLink(InternalBond.ItemB, NewIdx);
							Newly.Add(InternalBond.ItemA);
							Newly.Add(InternalBond.ItemB);

							if (bDevDebugBonds && GEngine)
							{
								GEngine->AddOnScreenDebugMessage(-1, 0.6f, FColor::Cyan,
									FString::Printf(TEXT("Internal cluster bond formed: %d-%d"), SubA.ItemId, SubB.ItemId));
							}
						}
					}
				}
			}
		}
	}
}

void USimWorldHex::StepBelts()
{
	if (TileMap.Num() == 0 || ItemMap.Num() == 0) return;

	const float Speed = FMath::Max(0.f, BeltSpeedTilesPerTick);
	const float BlockClamp = BeltBlockedClamp;

	// --- NEW: gather all hexes occupied by held orbs (hand hex + cluster sub-orbs) ---
	TSet<FHex> HeldOrbHexes;
	for (const FArmState& A : Arms)
	{
		if (!A.bHolding) continue;
		
		FHex GripHex = A.BaseHex;
		for (int i = 0; i < A.ArmLen; ++i)
			GripHex = HexUtil::Neighbor(GripHex, A.Facing);
		
		if (!A.Held.bIsCluster)
		{
			// Single held orb occupies the hand hex
			HeldOrbHexes.Add(GripHex);
		}
		else
		{
			// Held cluster: calculate positions of all sub-orbs
			const uint8 cur = (uint8)A.Facing % 6;
			const int8 grab = (int8)A.Held.GrabFacing;
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
			
			for (const FOrbData& Sub : A.Held.Orbs)
			{
				FHex Local(Sub.dQ, Sub.dR);
				FHex Rot = Rot60(Local, ccw);
				FHex WorldPos(GripHex.Q + Rot.Q, GripHex.R + Rot.R);
				HeldOrbHexes.Add(WorldPos);
			}
		}
	}

	/* ---------- Maintain BeltProgress for items currently on belts ---------- */
	TSet<int32> LiveIds;
	for (const auto& KV : ItemMap)
	{
		const FTile* T = TileMap.Find(KV.Key);
		if (T && T->Kind == ETileKind::Belt && KV.Value.Id != 0)
		{
			LiveIds.Add(KV.Value.Id);
			BeltProgress.FindOrAdd(KV.Value.Id, 0.f);
		}
	}
	for (auto It = BeltProgress.CreateIterator(); It; ++It)
		if (!LiveIds.Contains(It.Key()))
			It.RemoveCurrent();

	/* ---------- Snapshot occupancy (excluding held items which we add via HeldOrbHexes) ---------- */
	TSet<FHex> Occupied;
	Occupied.Reserve(ItemMap.Num() + HeldOrbHexes.Num());
	for (const auto& KV : ItemMap) Occupied.Add(KV.Key);
	for (const FHex& HH : HeldOrbHexes) Occupied.Add(HH); // NEW

	/* ---------- Collect belt items & advance raw progress ---------- */
	struct FBeltItemRef { int32 ItemId; FHex Hex; };
	TArray<FBeltItemRef> BeltItems;
	BeltItems.Reserve(BeltProgress.Num());
	for (const auto& KV : ItemMap)
	{
		if (KV.Value.Id == 0) continue;
		if (!BeltProgress.Contains(KV.Value.Id)) continue;
		BeltItems.Add({ KV.Value.Id, KV.Key });
	}
	const float Dt = FMath::Max(0.f, FixedStepSec);
	const float TilesPerSec = (BeltSpeedTilesPerSecond > 0.f) ? BeltSpeedTilesPerSecond
		: (BeltSpeedTilesPerTick / FMath::Max(KINDA_SMALL_NUMBER, Dt));
	const float ProgressPerStep = TilesPerSec * Dt;

	for (FBeltItemRef& Ref : BeltItems)
	{
		float& P = BeltProgress[Ref.ItemId];
		P += ProgressPerStep;
	}

	/* ---------- Build bond connectivity (clusters) ---------- */
	TMap<int32, TArray<int32>> Adj;
	Adj.Reserve(Bonds.Num() * 2 + 8);
	for (const FBond& B : Bonds)
	{
		if (B.ItemA == 0 || B.ItemB == 0) continue;
		TArray<int32>& LA = Adj.FindOrAdd(B.ItemA);
		TArray<int32>& LB = Adj.FindOrAdd(B.ItemB);
		LA.Add(B.ItemB);
		LB.Add(B.ItemA);
	}

	TMap<int32, FHex> ItemHex;
	ItemHex.Reserve(ItemMap.Num());
	for (const auto& KV : ItemMap)
		if (KV.Value.Id != 0)
			ItemHex.Add(KV.Value.Id, KV.Key);

	TSet<int32> Visited;
	struct FCluster { TArray<int32> Items; TSet<int32> ItemSet; };
	TArray<FCluster> Clusters;

	for (const auto& KV : Adj)
	{
		int32 Start = KV.Key;
		if (Visited.Contains(Start)) continue;

		FCluster C;
		TArray<int32> Stack;
		Stack.Add(Start);
		Visited.Add(Start);

		while (Stack.Num())
		{
			int32 Cur = Stack.Pop(EAllowShrinking::No);
			C.Items.Add(Cur);
			C.ItemSet.Add(Cur);
			if (const TArray<int32>* Neigh = Adj.Find(Cur))
				for (int32 N : *Neigh)
					if (!Visited.Contains(N)) { Visited.Add(N); Stack.Add(N); }
		}
		if (C.Items.Num() > 1) { C.Items.Sort(); Clusters.Add(MoveTemp(C)); }
	}

	// Helper lambdas
	auto IsBelt = [&](const FHex& H) -> bool
		{
			if (const FTile* T = TileMap.Find(H))
				return T->Kind == ETileKind::Belt;
			return false;
		};
	auto BeltDir = [&](const FHex& H) -> uint8
		{
			if (const FTile* T = TileMap.Find(H))
				return (uint8)(T->Arg & 0xFF);
			return 0;
		};

	// Record moves: item -> new hex
	struct FMove { int32 ItemId; FHex From; FHex To; };
	TArray<FMove> ClusterMoves;
	TSet<int32>   ItemsMovedForward; // to block double scheduling later

	/* ---------- Attempt cluster rigid translation ---------- */
	// Sort clusters deterministically by smallest (Q,R) among their items
	Clusters.Sort([&](const FCluster& A, const FCluster& B)
		{
			auto MinHex = [&](const FCluster& C)
				{
					FHex Min(INT_MAX, INT_MAX);
					for (int32 Id : C.Items)
					{
						if (const FHex* H = ItemHex.Find(Id))
						{
							if (H->Q < Min.Q || (H->Q == Min.Q && H->R < Min.R))
								Min = *H;
						}
					}
					return Min;
				};
			FHex HA = MinHex(A);
			FHex HB = MinHex(B);
			if (HA.Q != HB.Q) return HA.Q < HB.Q;
			return HA.R < HB.R;
		});

	for (FCluster& C : Clusters)
	{
		// Gather controllers (cluster items currently on belts with progress >=1)
		struct FController { int32 ItemId; uint8 Dir; FHex Hex; FHex ForwardHex; int32 dQ; int32 dR; };
		TArray<FController> Controllers;

		for (int32 Id : C.Items)
		{
			const FHex* HexPtr = ItemHex.Find(Id);
			if (!HexPtr) continue;
			if (!IsBelt(*HexPtr)) continue;
			if (float* Prog = BeltProgress.Find(Id); Prog && *Prog >= 1.f)
			{
				uint8 Dir = BeltDir(*HexPtr) % 6;
				FHex Fwd = HexUtil::Neighbor(*HexPtr, (EHexDir)Dir);
				FController Ctrl;
				Ctrl.ItemId = Id;
				Ctrl.Dir = Dir;
				Ctrl.Hex = *HexPtr;
				Ctrl.ForwardHex = Fwd;
				Ctrl.dQ = Fwd.Q - HexPtr->Q;
				Ctrl.dR = Fwd.R - HexPtr->R;
				Controllers.Add(Ctrl);
			}
		}

		if (Controllers.Num() == 0)
			continue; // no impetus this tick

		// Ensure all controllers agree on translation delta (dQ,dR)
		int32 dQ = Controllers[0].dQ;
		int32 dR = Controllers[0].dR;
		bool bConsistent = true;
		for (int32 i = 1; i < Controllers.Num(); ++i)
		{
			if (Controllers[i].dQ != dQ || Controllers[i].dR != dR)
			{
				bConsistent = false;
				break;
			}
		}
		if (!bConsistent)
		{
			// Block: clamp controller progress so they don't overflow forever
			for (FController& Ctrl : Controllers)
			{
				if (float* P = BeltProgress.Find(Ctrl.ItemId))
					*P = FMath::Min(*P, BlockClamp);
			}
			continue;
		}

		// Validate that every destination hex is either inside cluster or empty
		bool bBlocked = false;
		for (int32 Id : C.Items)
		{
			const FHex* H = ItemHex.Find(Id);
			if (!H) { bBlocked = true; break; }
			FHex Dest(H->Q + dQ, H->R + dR);

			// NEW: block if destination overlaps a held orb hex
			if (HeldOrbHexes.Contains(Dest)) { bBlocked = true; break; }

			if (Occupied.Contains(Dest))
			{
				const FItem* Other = ItemMap.Find(Dest);
				if (!Other || !C.ItemSet.Contains(Other->Id))
				{
					// Could also be a held hand hex (already checked)
					if (!Other) { bBlocked = true; break; }
					if (!C.ItemSet.Contains(Other->Id)) { bBlocked = true; break; }
				}
			}
		}

		if (bBlocked)
		{
			for (FController& Ctrl : Controllers)
				if (float* P = BeltProgress.Find(Ctrl.ItemId))
					*P = FMath::Min(*P, BlockClamp);
			continue;
		}

		for (int32 Id : C.Items)
		{
			const FHex* H = ItemHex.Find(Id);
			if (!H) continue;
			FHex Dest(H->Q + dQ, H->R + dR);
			Occupied.Remove(*H);
			Occupied.Add(Dest);
			ClusterMoves.Add({ Id, *H, Dest });
			ItemHex[Id] = Dest;
			ItemsMovedForward.Add(Id);
		}
		for (FController& Ctrl : Controllers)
			if (float* P = BeltProgress.Find(Ctrl.ItemId))
			{
				*P -= 1.f;
				if (*P < 0.f) *P = 0.f;
			}
	}

	/* ---------- Single-item forward scheduling ---------- */
	struct FForwardTry { int32 ItemId; FHex From; FHex To; };
	TArray<FForwardTry> Singles;
	Singles.Reserve(BeltItems.Num());

	for (const FBeltItemRef& Ref : BeltItems)
	{
		if (ItemsMovedForward.Contains(Ref.ItemId)) continue;
		if (IsItemBonded(Ref.ItemId)) continue;
		float& P = BeltProgress[Ref.ItemId];
		if (P < 1.f) continue;

		const FTile* TFrom = TileMap.Find(Ref.Hex);
		if (!TFrom || TFrom->Kind != ETileKind::Belt)
		{
			P = 0.f;
			continue;
		}

		const uint8 Dir = (uint8)(TFrom->Arg & 0xFF);
		const FHex To = HexUtil::Neighbor(Ref.Hex, (EHexDir)(Dir % 6));

		// NEW: allow simultaneous chain advancement.
		auto WillVacateThisTick = [&](const FHex& Dest) -> bool
			{
				const FItem* DestItem = ItemMap.Find(Dest);
				if (!DestItem) return false;
				if (DestItem->Id == Ref.ItemId) return true;
				if (ItemsMovedForward.Contains(DestItem->Id)) return false; // already scheduled elsewhere
				if (IsItemBonded(DestItem->Id)) return false;               // clusters handled earlier
				if (float* DP = BeltProgress.Find(DestItem->Id))
				{
					return (*DP >= 1.f); // it will also move forward this tick
				}
				return false;
			};

		bool bDestBlockedNow =
			(HeldOrbHexes.Contains(To)) ||                                // NEW
			(ItemMap.Contains(To) && !WillVacateThisTick(To));

		if (bDestBlockedNow)
		{
			P = FMath::Min(P, BlockClamp);
			continue;
		}

		// Treat destination as reserved; even if occupied it is assumed to vacate
		Singles.Add({ Ref.ItemId, Ref.Hex, To });
		Occupied.Remove(Ref.Hex);
		Occupied.Add(To);
		P -= 1.f;
		ItemsMovedForward.Add(Ref.ItemId);
	}

	/* ---------- Apply moves ---------- */
	if (ClusterMoves.Num() || Singles.Num())
	{
		TMap<FHex, FItem> Original = ItemMap;

		for (const FMove& M : ClusterMoves)
		{
			const FItem* Found = Original.Find(M.From);
			if (!Found || Found->Id != M.ItemId) continue;

			ItemMap.Remove(M.From);
			OrbAt.Remove(M.From);
			ItemMap.Add(M.To, *Found);
			OrbAt.Add(M.To, KindFromItem(*Found));

			if (const FTile* TD = TileMap.Find(M.To))
			{
				if (TD->Kind != ETileKind::Belt)
					BeltProgress.Remove(M.ItemId);
				if (TD->Kind == ETileKind::Input)
					SkipInputThisFrame.Add(M.To);
			}
			else
			{
				BeltProgress.Remove(M.ItemId);
			}
		}

		for (const FForwardTry& M : Singles)
		{
			// Re-check against held orb hex (in case arm grabbed during same tick earlier logic)
			if (HeldOrbHexes.Contains(M.To)) continue;

			const FItem* Found = Original.Find(M.From);
			if (!Found || Found->Id != M.ItemId) continue;
			if (ItemMap.Contains(M.To)) continue;

			ItemMap.Remove(M.From);
			OrbAt.Remove(M.From);
			ItemMap.Add(M.To, *Found);
			OrbAt.Add(M.To, KindFromItem(*Found));

			if (const FTile* TD = TileMap.Find(M.To))
			{
				if (TD->Kind != ETileKind::Belt)
					BeltProgress.Remove(M.ItemId);
				if (TD->Kind == ETileKind::Input)
					SkipInputThisFrame.Add(M.To);
			}
			else
			{
				BeltProgress.Remove(M.ItemId);
			}
		}
	}

	// Rebuild ItemHex for pull
	ItemHex.Empty();
	for (const auto& KV : ItemMap)
		if (KV.Value.Id != 0)
			ItemHex.Add(KV.Value.Id, KV.Key);

	/* ---------- Pull pass (treat held orb hexes as blocked targets) ---------- */
	TSet<FHex> LocalOccupied;
	for (const auto& KV : ItemMap) LocalOccupied.Add(KV.Key);
	for (const FHex& HH : HeldOrbHexes) LocalOccupied.Add(HH); // NEW

	TSet<int32> PulledThisPass;

	auto TryTranslateClusterPull = [&](int32 SeedItemId, const FHex& DestHex, uint8, TSet<int32>& OutMovedIds) -> bool
		{
			// If destination is a held orb hex, block
			if (HeldOrbHexes.Contains(DestHex)) return false; // NEW

			TSet<int32> ClusterItemIds;
			if (const TArray<int32>* BondIdxList = ItemToBonds.Find(SeedItemId))
			{
				TArray<int32> Stack;
				Stack.Add(SeedItemId);
				ClusterItemIds.Add(SeedItemId);
				while (Stack.Num())
				{
					int32 Cur = Stack.Pop(EAllowShrinking::No);
					if (const TArray<int32>* Neigh = Adj.Find(Cur))
						for (int32 N : *Neigh)
							if (!ClusterItemIds.Contains(N))
							{
								ClusterItemIds.Add(N);
								Stack.Add(N);
							}
				}
			}
			else ClusterItemIds.Add(SeedItemId);

			for (int32 Id : ClusterItemIds)
				if (OutMovedIds.Contains(Id)) return false;

			const FHex* SeedFrom = ItemHex.Find(SeedItemId);
			if (!SeedFrom) return false;
			const int32 dQ = DestHex.Q - SeedFrom->Q;
			const int32 dR = DestHex.R - SeedFrom->R;

			for (int32 Id : ClusterItemIds)
			{
				const FHex* FromHex = ItemHex.Find(Id);
				if (!FromHex) return false;
				FHex ToHex(FromHex->Q + dQ, FromHex->R + dR);

				if (HeldOrbHexes.Contains(ToHex)) return false; // NEW
				if (LocalOccupied.Contains(ToHex))
				{
					// Allow landing on a hex currently occupied by another member of the same cluster;
					// the whole cluster translates atomically. Block only if it's a foreign occupant.
					if (const FItem* Occ = ItemMap.Find(ToHex))
					{
						if (!ClusterItemIds.Contains(Occ->Id))
							return false; // foreign item blocks
					}
					else
					{
						// Occupied by something non-item (e.g. held hand hex); treat as blocked
						return false;
					}
				}
			}

			TMap<FHex, FItem> Snapshot = ItemMap;
			for (int32 Id : ClusterItemIds)
			{
				const FHex* FromHex = ItemHex.Find(Id);
				if (!FromHex) continue;
				FHex ToHex(FromHex->Q + dQ, FromHex->R + dR);
				if (HeldOrbHexes.Contains(ToHex)) continue; // safety

				const FItem* Found = Snapshot.Find(*FromHex);
				if (!Found || Found->Id != Id) continue;

				ItemMap.Remove(*FromHex);
				OrbAt.Remove(*FromHex);
				ItemMap.Add(ToHex, *Found);
				OrbAt.Add(ToHex, KindFromItem(*Found));

				ItemHex[Id] = ToHex;
				LocalOccupied.Remove(*FromHex);
				LocalOccupied.Add(ToHex);

				if (const FTile* TD = TileMap.Find(ToHex); TD && TD->Kind == ETileKind::Belt)
					BeltProgress.Add(Id, 0.f);
				else
					BeltProgress.Remove(Id);
			}
			for (int32 Id : ClusterItemIds) OutMovedIds.Add(Id);
			return true;
		};

	for (const auto& KV : TileMap)
	{
		if (KV.Value.Kind != ETileKind::Belt) continue;
		const FHex BeltHex = KV.Key;

		// NEW: blocked if a held orb is here
		if (HeldOrbHexes.Contains(BeltHex)) continue;

		if (ItemMap.Contains(BeltHex)) continue;

		const uint8 Dir = (uint8)(KV.Value.Arg & 0xFF);
		const auto Opp = [](EHexDir D) { return (EHexDir)(((uint8)D + 3) % 6); };
		const FHex Behind = HexUtil::Neighbor(BeltHex, Opp((EHexDir)(Dir % 6)));

		if (FItem* BehindItem = ItemMap.Find(Behind))
		{
			// Do not pull twice within the same belt step or the same tick
			if (ItemsMovedForward.Contains(BehindItem->Id)) continue;
			if (PulledThisPass.Contains(BehindItem->Id)) continue;

			// NEW: prevent instant pull of freshly spawned/dropped items (not present at this hex last tick)
			const FHex* Prev = PrevItemHex.Find(BehindItem->Id);
			if (!Prev || *Prev != Behind) continue;

			bool bAllowPull = true;
			if (const FTile* BehindTile = TileMap.Find(Behind); BehindTile && BehindTile->Kind == ETileKind::Belt)
			{
				const uint8 BehindDir = (uint8)(BehindTile->Arg & 0xFF) % 6;
				const FHex BehindForward = HexUtil::Neighbor(Behind, (EHexDir)BehindDir);
				const bool bFeedsThisBelt = (BehindForward == BeltHex);
				if (!bFeedsThisBelt)
				{
					const FTile* ForwardTile = TileMap.Find(BehindForward);
					const bool bForwardIsEmptyBelt =
						(ForwardTile && ForwardTile->Kind == ETileKind::Belt && !ItemMap.Contains(BehindForward) && !HeldOrbHexes.Contains(BehindForward));
					if (bForwardIsEmptyBelt)
						bAllowPull = false;
				}
			}
			if (!bAllowPull) continue;

			TryTranslateClusterPull(BehindItem->Id, BeltHex, Dir, PulledThisPass);
		}
	}

	/* ---------- Immediate forward pass AFTER pulls (late moves) ---------- */
	{
		TSet<FHex> OccupiedNow;
		for (const auto& KV : ItemMap) OccupiedNow.Add(KV.Key);
		for (const FHex& HH : HeldOrbHexes) OccupiedNow.Add(HH); // NEW

		struct FLateMove { int32 ItemId; FHex From; FHex To; };
		TArray<FLateMove> LateMoves;

		for (const auto& KV : ItemMap)
		{
			const FHex& Hex = KV.Key;
			const FItem& It = KV.Value;
			if (It.Id == 0) continue;
			if (ItemsMovedForward.Contains(It.Id)) continue;

			const FTile* T = TileMap.Find(Hex);
			if (!T || T->Kind != ETileKind::Belt) continue;
			if (IsItemBonded(It.Id)) continue;

			float* Prog = BeltProgress.Find(It.Id);
			if (!Prog) continue;

			if (*Prog >= 1.f)
			{
				const uint8 Dir = (uint8)(T->Arg & 0xFF) % 6;
				const FHex To = HexUtil::Neighbor(Hex, (EHexDir)Dir);

				if (HeldOrbHexes.Contains(To)) // NEW: blocked by held orb
				{
					*Prog = FMath::Min(*Prog, BeltBlockedClamp);
					continue;
				}

				if (!OccupiedNow.Contains(To) && !ItemMap.Contains(To))
				{
					LateMoves.Add({ It.Id, Hex, To });
					OccupiedNow.Remove(Hex);
					OccupiedNow.Add(To);
				}
				else
				{
					*Prog = FMath::Min(*Prog, BeltBlockedClamp);
				}
			}
		}

		if (LateMoves.Num())
		{
			TMap<FHex, FItem> Snap = ItemMap;
			for (const FLateMove& M : LateMoves)
			{
				if (HeldOrbHexes.Contains(M.To)) continue; // safety
				const FItem* Found = Snap.Find(M.From);
				if (!Found || Found->Id != M.ItemId) continue;
				if (ItemMap.Contains(M.To)) continue;

				ItemMap.Remove(M.From);
				OrbAt.Remove(M.From);
				ItemMap.Add(M.To, *Found);
				OrbAt.Add(M.To, KindFromItem(*Found));

				if (float* P = BeltProgress.Find(M.ItemId))
				{
					*P -= 1.f;
					if (*P < 0.f) *P = 0.f;
				}
				ItemsMovedForward.Add(M.ItemId);
			}
		}
	}

	/* ---------- Record movement for interpolation ---------- */
	ItemsMovedLastStep = ItemsMovedForward;
	for (int32 PulledId : PulledThisPass)
		ItemsMovedLastStep.Add(PulledId);
}

// --- NEW: split glyph handling so inputs spawn before belts ---
void USimWorldHex::SpawnInputs()
{
	if (TileMap.Num() == 0) return;
	for (const auto& KV : TileMap)
	{
		const FHex& H = KV.Key;
		const FTile& T = KV.Value;
		if (T.Kind != ETileKind::Input) continue;
		if (SkipInputThisFrame.Contains(H)) continue;
		if (ItemMap.Contains(H)) continue;           // occupied already

		FOrb O; O.Type = (EOrbType)((uint8)EOrbType::BaseA + (T.Arg % 4));
		FItem NewItem = FItem::MakeSingle(O);
		NewItem.Id = NextItemId++;
		TryPutItem(H, NewItem);
	}
}

// --- NEW helper: spawn inputs after belts so vacated inputs refill same tick ---
void USimWorldHex::SpawnInputsAfterBelts()
{
	if (TileMap.Num() == 0) return;
	for (const auto& KV : TileMap)
	{
		const FHex& H = KV.Key;
		const FTile& T = KV.Value;
		if (T.Kind != ETileKind::Input) continue;
		if (SkipInputThisFrame.Contains(H)) continue;
		if (ItemMap.Contains(H)) continue; // still occupied after belts

		FOrb O; O.Type = (EOrbType)((uint8)EOrbType::BaseA + (T.Arg % 4));
		FItem NewItem = FItem::MakeSingle(O);
		NewItem.Id = NextItemId++;
		TryPutItem(H, NewItem);
		// Mark so a second (unexpected) pass this frame wouldn't double spawn
		SkipInputThisFrame.Add(H);
	}
}

void USimWorldHex::ProcessOutputs()
{
	if (TileMap.Num() == 0) return;
	for (const auto& KV : TileMap)
	{
		const FHex& H = KV.Key;
		const FTile& T = KV.Value;
		if (T.Kind != ETileKind::Output) continue;
		if (SkipOutputThisFrame.Contains(H)) continue;

		FItem Temp;
		if (TryTakeItem(H, Temp))
			++OutputCount;
	}
}

bool USimWorldHex::TryArmPickItem(const FHex& From, FItem& OutItem)
{
	if (TryTakeItem(From, OutItem))
	{
		if (OutItem.Id != 0)
			HeldItemLastHex.Add(OutItem.Id, From);
		return true;
	}
	if (const FTile* T = TileMap.Find(From))
	{
		if (T->Kind == ETileKind::Input)
		{
			FOrb O; O.Type = (EOrbType)((uint8)EOrbType::BaseA + (T->Arg % 4));
			FItem NewItem = FItem::MakeSingle(O);
			NewItem.Id = NextItemId++;
			OutItem = NewItem;
			return true;
		}
	}
	return false;
}

bool USimWorldHex::TryArmPlaceItem(const FHex& To, const FItem& InItem)
{
	if (const FTile* T = TileMap.Find(To))
	{
		if (T->Kind == ETileKind::Output)
		{
			OrbAt.Remove(To);
			++OutputCount;
			HeldItemLastHex.Remove(InItem.Id);
			return true;
		}
	}
	const bool bPlaced = TryPutItem(To, InItem);
	if (bPlaced)
		HeldItemLastHex.Remove(InItem.Id);
	return bPlaced;
}

bool USimWorldHex::PeekItemAt(const FHex& H, FItem& OutItem) const
{
	if (const FItem* Found = ItemMap.Find(H))
	{
		OutItem = *Found;
		return true;
	}
	return false;
}

bool USimWorldHex::TryPlaceOrbAt(const FHex& Hex, uint8 Kind)
{
	if (OrbAt.Contains(Hex)) return false;
	OrbAt.Add(Hex, Kind);
	return true;
}

bool USimWorldHex::TryPickupOrbAt(const FHex& Hex, uint8& OutKind)
{
	if (uint8* K = OrbAt.Find(Hex))
	{
		OutKind = *K;
		OrbAt.Remove(Hex);
		return true;
	}
	return false;
}

bool USimWorldHex::HasOrbAt(const FHex& Hex) const
{
	return OrbAt.Contains(Hex);
}

bool USimWorldHex::GetOrbKindAt(const FHex& Hex, uint8& OutKind) const
{
	if (const uint8* K = OrbAt.Find(Hex))
	{
		OutKind = *K;
		return true;
	}
	return false;
}

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

//void USimWorldHex::CleanupBrokenBonds(const TSet<int32>& /*HeldIds*/)
//{
//	if (Bonds.Num() == 0) return;
//
//	TMap<int32, FHex> ItemPos;
//	for (const auto& KV : ItemMap)
//		if (KV.Value.Id != 0)
//			ItemPos.Add(KV.Value.Id, KV.Key);
//
//	for (int32 i = Bonds.Num() - 1; i >= 0; --i)
//	{
//		const FBond& B = Bonds[i];
//		bool bRemove = false;
//
//		const FHex* AHex = ItemPos.Find(B.ItemA);
//		const FHex* BHex = ItemPos.Find(B.ItemB);
//		if (!AHex || !BHex)
//		{
//			bRemove = true;
//		}
//		else
//		{
//			const int32 dq = BHex->Q - AHex->Q;
//			const int32 dr = BHex->R - AHex->R;
//			if (dq != B.dQ || dr != B.dR)
//				bRemove = true;
//		}
//
//		if (bRemove)
//			RemoveBondIndex(i);
//	}
//}

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

void USimWorldHex::AddBoardOrbToHeldCluster(int32 HeldOrbId, int32 BoardOrbId, const FHex& BoardOrbHex)
{
	// Find the arm holding the cluster containing HeldOrbId
	for (int32 ArmIndex = 0; ArmIndex < Arms.Num(); ArmIndex++)
	{
		FArmState& Arm = Arms[ArmIndex];
		if (!Arm.bHolding || !Arm.Held.bIsCluster) continue;
		
		// Check if this cluster contains the held orb
		bool bContainsHeldOrb = false;
		for (const FOrbData& Sub : Arm.Held.Orbs)
		{
			if (Sub.ItemId == HeldOrbId)
			{
				bContainsHeldOrb = true;
				break;
			}
		}
		
		if (!bContainsHeldOrb) continue;
		
		// Find the board orb
		FItem* BoardItem = ItemMap.Find(BoardOrbHex);
		if (!BoardItem || BoardItem->Id != BoardOrbId) continue;
		
		// Simple direct calculation approach - no temporary state manipulation
		// Calculate the arm's current grip position
		FHex GripHex = Arm.BaseHex;
		for (int i = 0; i < Arm.ArmLen; ++i)
			GripHex = HexUtil::Neighbor(GripHex, Arm.Facing);
		
		// Find the grabbed orb (the one with dQ=0, dR=0) - this is our reference point
		const FOrbData* GrabbedOrb = nullptr;
		for (const FOrbData& Sub : Arm.Held.Orbs)
		{
			if (Sub.dQ == 0 && Sub.dR == 0)
			{
				GrabbedOrb = &Sub;
				break;
			}
		}
		
		if (!GrabbedOrb)
		{
			if (bDevDebugBonds && GEngine)
			{
				GEngine->AddOnScreenDebugMessage(-1, 2.0f, FColor::Red,
					TEXT("ERROR: No grabbed orb found in cluster (dQ=0, dR=0)"));
			}
			continue;
		}
		
		// Calculate the new orb's position relative to the grabbed orb
		// The grabbed orb is at the grip position, so we calculate the offset from there
		FHex NewOrbClusterOffset(
			BoardOrbHex.Q - GripHex.Q,
			BoardOrbHex.R - GripHex.R
		);
		
		// Apply the same rotation that the cluster uses to ensure correct positioning
		const uint8 cur = (uint8)Arm.Facing % 6;
		const int8 grab = (int8)Arm.Held.GrabFacing;
		const uint8 facingDelta = (grab >= 0)
			? (uint8)(((int32)cur - (int32)(grab % 6) + 6) % 6)
			: cur;
		const uint8 ccw = (uint8)((6 - facingDelta) % 6);
		
		// Rotate the offset to match the cluster's current orientation
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
		
		// Apply the inverse rotation to get the local cluster coordinates
		FHex LocalOffset = Rot60(NewOrbClusterOffset, (6 - ccw) % 6);
		
		// Create new FOrbData for the board orb
		FOrbData NewOrb;
		NewOrb.ItemId = BoardOrbId;
		NewOrb.Type = (uint8)BoardItem->Single.Type;
		NewOrb.Color = (uint8)BoardItem->Single.Color;
		NewOrb.dQ = (int8)LocalOffset.Q;
		NewOrb.dR = (int8)LocalOffset.R;
		NewOrb.Offset = FVector2D::ZeroVector; // Let the arm calculate this from dQ/dR
		
		// Add the orb to the cluster
		Arm.Held.Orbs.Add(NewOrb);
		
		// Remove the orb from the board (it's now part of the held cluster)
		ItemMap.Remove(BoardOrbHex);
		
		// Update HeldItemLastHex
		HeldItemLastHex.FindOrAdd(BoardOrbId, BoardOrbHex);
		
		if (bDevDebugBonds && GEngine)
		{
			GEngine->AddOnScreenDebugMessage(-1, 1.0f, FColor::Orange,
				FString::Printf(TEXT("Board orb %d added to held cluster: dQ=%d dR=%d"), 
					BoardOrbId, NewOrb.dQ, NewOrb.dR));
		}
		
		// Force the arm to update its visual immediately
		if (UWorld* World = GetWorld())
		{
			for (TActorIterator<ABasicArm> ActorItr(World); ActorItr; ++ActorItr)
			{
				ABasicArm* ArmActor = *ActorItr;
				if (ArmActor && ArmActor->SimArmIndex == ArmIndex)
				{
					ArmActor->UpdateHeldClusterOnly();
					break;
				}
			}
		}
		
		break; // Only add to the first matching cluster
	}
}

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

void USimWorldHex::CreateTestBonds()
{
	// Create some test bonds for debugging
	// This is a temporary function to test bond visualization
	
	// Clear existing bonds
	Bonds.Empty();
	ItemToBonds.Empty();
	
	// Create test items at specific hexes
	FHex TestHex1(0, 0);
	FHex TestHex2(1, 0);
	FHex TestHex3(0, 1);
	
	// Create test orbs
	FOrb Orb1; Orb1.Type = EOrbType::BaseA; Orb1.Color = EOrbColor::Red;
	FOrb Orb2; Orb2.Type = EOrbType::BaseB; Orb2.Color = EOrbColor::Blue;
	FOrb Orb3; Orb3.Type = EOrbType::BaseA; Orb3.Color = EOrbColor::Green;
	
	FItem Item1 = FItem::MakeSingle(Orb1);
	FItem Item2 = FItem::MakeSingle(Orb2);
	FItem Item3 = FItem::MakeSingle(Orb3);
	
	Item1.Id = NextItemId++;
	Item2.Id = NextItemId++;
	Item3.Id = NextItemId++;
	
	// Place items
	ItemMap.Add(TestHex1, Item1);
	ItemMap.Add(TestHex2, Item2);
	ItemMap.Add(TestHex3, Item3);
	
	// Create bonds between them
	FBond Bond1;
	Bond1.ItemA = Item1.Id;
	Bond1.ItemB = Item2.Id;
	Bond1.Root = TestHex1;
	Bond1.dQ = 1;
	Bond1.dR = 0;
	Bond1.bActive = true;
	
	FBond Bond2;
	Bond2.ItemA = Item1.Id;
	Bond2.ItemB = Item3.Id;
	Bond2.Root = TestHex1;
	Bond2.dQ = 0;
	Bond2.dR = 1;
	Bond2.bActive = true;
	
	// Add bonds
	const int32 Bond1Idx = Bonds.Add(Bond1);
	const int32 Bond2Idx = Bonds.Add(Bond2);
	
	AddItemBondLink(Bond1.ItemA, Bond1Idx);
	AddItemBondLink(Bond1.ItemB, Bond1Idx);
	AddItemBondLink(Bond2.ItemA, Bond2Idx);
	AddItemBondLink(Bond2.ItemB, Bond2Idx);
	
	UE_LOG(LogTemp, Log, TEXT("Created test bonds: %d-%d and %d-%d"), 
		Bond1.ItemA, Bond1.ItemB, Bond2.ItemA, Bond2.ItemB);
}

