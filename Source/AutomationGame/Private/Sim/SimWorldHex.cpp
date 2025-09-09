#include "Sim/SimWorldHex.h"
#include "Engine/Engine.h"
#include "Sim/HexCoord.h"

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

			FHex OldHand = S.BaseHex;
			for (int i = 0; i < S.ArmLen; ++i)
				OldHand = HexUtil::Neighbor(OldHand, S.Facing);

			FHex NewHand = S.BaseHex;
			for (int i = 0; i < S.ArmLen; ++i)
				NewHand = HexUtil::Neighbor(NewHand, NewFacing);

			const int32 dQ = NewHand.Q - OldHand.Q;
			const int32 dR = NewHand.R - OldHand.R;

			bool bBlocked = false;

			// (A) Collision / cluster checks only if holding
			if (S.bHolding)
			{
				TSet<int32> ClusterIds;
				if (S.Held.Id != 0 && ItemToBonds.Contains(S.Held.Id))
				{
					TArray<int32> Stack;
					Stack.Add(S.Held.Id);
					ClusterIds.Add(S.Held.Id);
					while (Stack.Num())
					{
						int32 CurId = Stack.Pop(EAllowShrinking::No);
						if (const TArray<int32>* BondList = ItemToBonds.Find(CurId))
						{
							for (int32 BondIdx : *BondList)
							{
								if (!Bonds.IsValidIndex(BondIdx)) continue;
								const FBond& B = Bonds[BondIdx];
								const int32 Other = (B.ItemA == CurId) ? B.ItemB : B.ItemA;
								if (!ClusterIds.Contains(Other))
								{
									ClusterIds.Add(Other);
									Stack.Add(Other);
								}
							}
						}
					}
				}
				if (ClusterIds.Num() == 0 && S.Held.Id != 0)
					ClusterIds.Add(S.Held.Id);

				// 1) Destination hand cannot contain foreign item
				if (const FItem* Occ = ItemMap.Find(NewHand))
				{
					if (!ClusterIds.Contains(Occ->Id))
						bBlocked = true;
				}

				// 2) Cluster rigid translation test
				if (!bBlocked && ClusterIds.Num() > 1)
				{
					for (int32 ItemId : ClusterIds)
					{
						if (ItemId == S.Held.Id) continue;
						const FHex* CurHex = ItemIdToHex.Find(ItemId);
						if (!CurHex) continue;
						FHex Dest(CurHex->Q + dQ, CurHex->R + dR);
						if (const FItem* Occ = ItemMap.Find(Dest))
						{
							if (!ClusterIds.Contains(Occ->Id))
							{
								bBlocked = true;
								break;
							}
						}
					}
				}
			}

			// (B) Inter-arm overlap (even if not holding)
			if (!bBlocked)
			{
				for (int32 OtherIdx = 0; OtherIdx < Arms.Num(); ++OtherIdx)
				{
					if (OtherIdx == ArmIndex) continue;
					const FArmState& Other = Arms[OtherIdx];
					if (!Other.bHolding) continue;

					FHex OtherHand = Other.BaseHex;
					for (int i = 0; i < Other.ArmLen; ++i)
						OtherHand = HexUtil::Neighbor(OtherHand, Other.Facing);

					if (NewHand == OtherHand)
					{
						bBlocked = true;
						break;
					}

					// If translating bonded cluster, ensure no cluster member lands on another arm's hand
					if (S.bHolding && S.Held.Id != 0 && !bBlocked)
					{
						for (auto& KV : ItemMap)
						{
							const FItem& It = KV.Value;
							if (It.Id == 0 || It.Id == S.Held.Id) continue;
							if (IsItemBonded(It.Id))
							{
								const FHex CurHex = KV.Key;
								FHex Dest(CurHex.Q + dQ, CurHex.R + dR);
								if (Dest == OtherHand)
								{
									bBlocked = true;
									break;
								}
							}
						}
						if (bBlocked) break;
					}
				}
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

		case ECmd::Extend:
		{
			// Op.Arg == 0 -> default step of 1
			const int32 Delta = (Op.Arg == 0 ? 1 : Op.Arg);
			const int32 OldLen = S.ArmLen;
			S.ArmLen = FMath::Clamp<int32>(S.ArmLen + Delta, 1, MAX_ARM_LEN);

			if (bDevDebugArms && GEngine && S.ArmLen != OldLen)
			{
				FHex Hand = S.BaseHex;
				for (int i = 0; i < S.ArmLen; ++i)
					Hand = HexUtil::Neighbor(Hand, S.Facing);
				GEngine->AddOnScreenDebugMessage(
					-1, 1.5f, FColor::Cyan,
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

	// Bond formation (world or held)
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
		if (FItem* IA = ItemMap.Find(AHex)) AId = IA->Id;
		else if (int32* HeldA = VirtualHeldItemAt.Find(AHex)) AId = *HeldA;

		if (FItem* IB = ItemMap.Find(BHex)) BId = IB->Id;
		else if (int32* HeldB = VirtualHeldItemAt.Find(BHex)) BId = *HeldB;

		if (AId <= 0 || BId <= 0 || AId == BId) continue;

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
		const int32 NewIdx = Bonds.Add(NewB);
		AddItemBondLink(NewB.ItemA, NewIdx);
		AddItemBondLink(NewB.ItemB, NewIdx);
		Newly.Add(NewB.ItemA);
		Newly.Add(NewB.ItemB);

		if (bDevDebugBonds && GEngine)
		{
			GEngine->AddOnScreenDebugMessage(-1, 0.6f, FColor::Purple,
				FString::Printf(TEXT("Bond formed (live/held ok): %d-%d"), AId, BId));
		}
	}
}

void USimWorldHex::StepBelts()
{
	if (TileMap.Num() == 0 || ItemMap.Num() == 0) return;

	const float Speed = FMath::Max(0.f, BeltSpeedTilesPerTick);
	const float BlockClamp = BeltBlockedClamp;

	// --- NEW: gather all hand hexes of arms that are currently holding an item (treated as occupied) ---
	TSet<FHex> HeldHandHexes;
	for (const FArmState& A : Arms)
	{
		if (!A.bHolding) continue;
		FHex Hand = A.BaseHex;
		for (int i = 0; i < A.ArmLen; ++i)
			Hand = HexUtil::Neighbor(Hand, A.Facing);
		HeldHandHexes.Add(Hand);
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

	/* ---------- Snapshot occupancy (excluding held items which we add via HeldHandHexes) ---------- */
	TSet<FHex> Occupied;
	Occupied.Reserve(ItemMap.Num() + HeldHandHexes.Num());
	for (const auto& KV : ItemMap) Occupied.Add(KV.Key);
	for (const FHex& HH : HeldHandHexes) Occupied.Add(HH); // NEW

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

			// NEW: block if destination overlaps a held hand hex
			if (HeldHandHexes.Contains(Dest)) { bBlocked = true; break; }

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
			(HeldHandHexes.Contains(To)) ||                                // NEW
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
			// Re-check against held hand hex (in case arm grabbed during same tick earlier logic)
			if (HeldHandHexes.Contains(M.To)) continue;

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

	/* ---------- Pull pass (treat held hand hexes as blocked targets) ---------- */
	TSet<FHex> LocalOccupied;
	for (const auto& KV : ItemMap) LocalOccupied.Add(KV.Key);
	for (const FHex& HH : HeldHandHexes) LocalOccupied.Add(HH); // NEW

	TSet<int32> PulledThisPass;

	auto TryTranslateClusterPull = [&](int32 SeedItemId, const FHex& DestHex, uint8, TSet<int32>& OutMovedIds) -> bool
		{
			// If destination is a held hand hex, block
			if (HeldHandHexes.Contains(DestHex)) return false; // NEW

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

				if (HeldHandHexes.Contains(ToHex)) return false; // NEW
				if (LocalOccupied.Contains(ToHex))
				{
					if (const FItem* Occ = ItemMap.Find(ToHex))
						if (!ClusterItemIds.Contains(Occ->Id))
							return false;
						else
							// Occupied by held hand hex or something synthetic
							return false;
				}
			}

			TMap<FHex, FItem> Snapshot = ItemMap;
			for (int32 Id : ClusterItemIds)
			{
				const FHex* FromHex = ItemHex.Find(Id);
				if (!FromHex) continue;
				FHex ToHex(FromHex->Q + dQ, FromHex->R + dR);
				if (HeldHandHexes.Contains(ToHex)) continue; // safety

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

		// NEW: blocked if a held hand is here
		if (HeldHandHexes.Contains(BeltHex)) continue;

		if (ItemMap.Contains(BeltHex)) continue;

		const uint8 Dir = (uint8)(KV.Value.Arg & 0xFF);
		const auto Opp = [](EHexDir D) { return (EHexDir)(((uint8)D + 3) % 6); };
		const FHex Behind = HexUtil::Neighbor(BeltHex, Opp((EHexDir)(Dir % 6)));

		if (FItem* BehindItem = ItemMap.Find(Behind))
		{
			if (ItemsMovedForward.Contains(BehindItem->Id)) continue;
			if (PulledThisPass.Contains(BehindItem->Id)) continue;

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
						(ForwardTile && ForwardTile->Kind == ETileKind::Belt && !ItemMap.Contains(BehindForward) && !HeldHandHexes.Contains(BehindForward));
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
		for (const FHex& HH : HeldHandHexes) OccupiedNow.Add(HH); // NEW

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

				if (HeldHandHexes.Contains(To)) // NEW: blocked by held hand
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
				if (HeldHandHexes.Contains(M.To)) continue; // safety
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




