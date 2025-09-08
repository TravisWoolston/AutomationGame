#include "Sim/SimWorldHex.h"
#include "Engine/Engine.h"
#include "Sim/HexCoord.h"

// Near top (after includes) add if not already in header:
static constexpr float DEFAULT_BELT_SPEED_TILES_PER_SECOND = 1.0f;
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
				FHex Target = S.BaseHex;
				for (int i = 0; i < S.ArmLen; ++i)
					Target = HexUtil::Neighbor(Target, S.Facing);
				FItem Temp;
				if (TryArmPickItem(Target, Temp))
				{
					S.Held = Temp;
					S.bHolding = true;
					SkipInputThisFrame.Add(Target);
				}
			}
			break;

		case ECmd::Drop:
			if (S.bHolding)
			{
				FHex Target = S.BaseHex;
				for (int i = 0; i < S.ArmLen; ++i)
					Target = HexUtil::Neighbor(Target, S.Facing);
				if (TryArmPlaceItem(Target, S.Held))
				{
					S.bHolding = false;
					S.Held = FItem();
					SkipOutputThisFrame.Add(Target);
				}
			}
			break;

		case ECmd::RotateL:
		case ECmd::RotateR:
		{
			if (S.bHolding)
			{
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

				// Build bonded cluster (connected component) containing held item
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

				// 1) New hand destination cannot contain a foreign (non-cluster) item
				if (const FItem* Occ = ItemMap.Find(NewHand))
				{
					if (!ClusterIds.Contains(Occ->Id))
						bBlocked = true;
				}

				// 2) Translate every on-board cluster member by (dQ,dR), ensure no foreign collisions
				if (!bBlocked && ClusterIds.Num() > 1)
				{
					for (int32 ItemId : ClusterIds)
					{
						if (ItemId == S.Held.Id) continue; // held item not currently on board
						const FHex* CurHex = ItemIdToHex.Find(ItemId);
						if (!CurHex) continue; // could have been picked up earlier this tick

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

				if (bBlocked)
				{
					S.WaitCounter = 1;
					goto AdvancePC;
				}
			}

			// Commit rotation
			if (Op.Cmd == ECmd::RotateL)
				S.Facing = (EHexDir)(((uint8)S.Facing + 1) % 6);
			else
				S.Facing = (EHexDir)(((uint8)S.Facing + 5) % 6);
		}
		break;

		case ECmd::Extend:  S.ArmLen = FMath::Clamp<int32>(S.ArmLen + 1, 1, 3); break;
		case ECmd::Retract: S.ArmLen = FMath::Clamp<int32>(S.ArmLen - 1, 1, 3); break;
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
		if (A.bHolding && A.Held.Id != 0)
			HeldIds.Add(A.Held.Id);

	CleanupBrokenBonds(HeldIds);

	TSet<int32> Newly;
	for (const auto& KV : TileMap)
	{
		const FTile& T = KV.Value;
		if (T.Kind != ETileKind::Bond) continue;
		const uint8 Dir = (uint8)(T.Arg & 0xFF);
		TryFormBondAt(KV.Key, Dir, HeldIds, Newly);
	}
}

void USimWorldHex::StepBelts()
{
	if (TileMap.Num() == 0 || ItemMap.Num() == 0) return;

	const float Speed = FMath::Max(0.f, BeltSpeedTilesPerTick);
	const float BlockClamp = BeltBlockedClamp;

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

	/* ---------- Snapshot occupancy ---------- */
	TSet<FHex> Occupied;
	Occupied.Reserve(ItemMap.Num());
	for (const auto& KV : ItemMap) Occupied.Add(KV.Key);

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
	const float Dt = FMath::Max(0.f, FixedStepSec);          // real seconds per logical step
	const float TilesPerSec = (BeltSpeedTilesPerSecond > 0.f) ? BeltSpeedTilesPerSecond
		: (BeltSpeedTilesPerTick / FMath::Max(KINDA_SMALL_NUMBER, Dt));
	const float ProgressPerStep = TilesPerSec * Dt;

	for (FBeltItemRef& Ref : BeltItems)
	{
		float& P = BeltProgress[Ref.ItemId];
		P += ProgressPerStep;   // time–scaled progress
	}

	/* ---------- Build bond connectivity (clusters) ---------- */
	// Build adjacency list: ItemId -> neighbors (bonded items)
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

	// Quick item -> hex map
	TMap<int32, FHex> ItemHex;
	ItemHex.Reserve(ItemMap.Num());
	for (const auto& KV : ItemMap)
		if (KV.Value.Id != 0)
			ItemHex.Add(KV.Value.Id, KV.Key);

	// Find connected components among bonded items
	TSet<int32> Visited;
	struct FCluster
	{
		TArray<int32> Items;
		TSet<int32>   ItemSet;
	};
	TArray<FCluster> Clusters;

	for (const auto& KV : Adj)
	{
		const int32 Start = KV.Key;
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
			{
				for (int32 N : *Neigh)
				{
					if (!Visited.Contains(N))
					{
						Visited.Add(N);
						Stack.Add(N);
					}
				}
			}
		}

		// Only treat as cluster if >1 item (single bonded list with length 1 meaningless)
		if (C.Items.Num() > 1)
		{
			// Deterministic order for stability
			C.Items.Sort();
			Clusters.Add(MoveTemp(C));
		}
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

			// Already in cluster? check occupancy
			if (Occupied.Contains(Dest) && !C.ItemSet.Contains([&]()
				{
					// To know if occupied by cluster, find which item sits there
					const FItem* Same = ItemMap.Find(Dest);
					return Same ? Same->Id : -1;
				}()))
			{
				// If another item outside cluster occupies destination -> blocked
				if (const FItem* Other = ItemMap.Find(Dest))
				{
					if (!C.ItemSet.Contains(Other->Id))
					{
						bBlocked = true;
						break;
					}
				}
			}
			// Also if ItemMap has item not in cluster
			if (const FItem* IM = ItemMap.Find(Dest))
			{
				if (!C.ItemSet.Contains(IM->Id))
				{
					bBlocked = true;
					break;
				}
			}
		}

		if (bBlocked)
		{
			for (FController& Ctrl : Controllers)
			{
				if (float* P = BeltProgress.Find(Ctrl.ItemId))
					*P = FMath::Min(*P, BlockClamp);
			}
			continue;
		}

		// Reserve destination occupancy (remove old, add new) & record moves
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

		// Consume controller progress (subtract 1 for controllers with P>=1)
		for (FController& Ctrl : Controllers)
		{
			if (float* P = BeltProgress.Find(Ctrl.ItemId))
			{
				*P -= 1.f;
				if (*P < 0.f) *P = 0.f;
			}
		}
	}

	/* ---------- Single-item (unbonded) forward scheduling ---------- */
	struct FForwardTry { int32 ItemId; FHex From; FHex To; };
	TArray<FForwardTry> Singles;
	Singles.Reserve(BeltItems.Num());

	for (const FBeltItemRef& Ref : BeltItems)
	{
		if (ItemsMovedForward.Contains(Ref.ItemId)) continue; // already moved by cluster
		if (IsItemBonded(Ref.ItemId)) continue;               // bonded but cluster couldn't move -> skip forward
		float& P = BeltProgress[Ref.ItemId];
		if (P < 1.f) {
			continue;
		}

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

		const bool bDestOccupied = ItemMap.Contains(To);
		const bool bDestBlockedNow =
			(bDestOccupied && !WillVacateThisTick(To));

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

	/* ---------- Apply moves (clusters first then singles) ---------- */
	if (ClusterMoves.Num() || Singles.Num())
	{
		// Snapshot for safe lookups
		TMap<FHex, FItem> Original = ItemMap;

		// Apply cluster moves
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
				// NEW: prevent spawning under a cluster piece moved onto an input this tick
				if (TD->Kind == ETileKind::Input)
					SkipInputThisFrame.Add(M.To);
			}
			else
			{
				BeltProgress.Remove(M.ItemId);
			}
		}

		// Apply single moves
		for (const FForwardTry& M : Singles)
		{
			const FItem* Found = Original.Find(M.From);
			if (!Found || Found->Id != M.ItemId) continue;
			if (ItemMap.Contains(M.To)) continue; // became blocked indirectly

			ItemMap.Remove(M.From);
			OrbAt.Remove(M.From);
			ItemMap.Add(M.To, *Found);
			OrbAt.Add(M.To, KindFromItem(*Found));

			if (const FTile* TD = TileMap.Find(M.To))
			{
				if (TD->Kind != ETileKind::Belt)
					BeltProgress.Remove(M.ItemId);
				else
				{
					// keep residual (already subtracted 1)
				}
				if (TD->Kind == ETileKind::Input)
					SkipInputThisFrame.Add(M.To);
			}
			else
			{
				BeltProgress.Remove(M.ItemId);
			}
		}
	}

	// Rebuild ItemHex AFTER moves so pull pass sees updated positions (singles were not updated earlier)
	ItemHex.Empty();
	for (const auto& KV : ItemMap)
	{
		if (KV.Value.Id != 0)
			ItemHex.Add(KV.Value.Id, KV.Key);
	}

	/* ---------- Pull pass (empty belt pulls from behind) with cluster awareness ---------- */
	// This pass brings items/clusters onto belts when forward progress < 1.
	TSet<FHex> LocalOccupied; // fresh occupancy after moves
	for (const auto& KV : ItemMap) LocalOccupied.Add(KV.Key);

	// Track which item ids (and their clusters) have already been pulled this pass to prevent zipping
	TSet<int32> PulledThisPass;

	auto TryTranslateClusterPull = [&](int32 SeedItemId, const FHex& DestHex, uint8 /*ForwardDir*/, TSet<int32>& OutMovedIds) -> bool
		{
			// Determine cluster (if any) (reuse bond adjacency 'Adj' built earlier)
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
					{
						for (int32 N : *Neigh)
						{
							if (!ClusterItemIds.Contains(N))
							{
								ClusterItemIds.Add(N);
								Stack.Add(N);
							}
						}
					}
				}
			}
			else
			{
				ClusterItemIds.Add(SeedItemId);
			}

			for (int32 Id : ClusterItemIds)
				if (OutMovedIds.Contains(Id))
					return false;

			const FHex* SeedFrom = ItemHex.Find(SeedItemId);
			if (!SeedFrom) return false;
			const int32 dQ = DestHex.Q - SeedFrom->Q;
			const int32 dR = DestHex.R - SeedFrom->R;

			// Validate all cluster members can translate without foreign collision
			for (int32 Id : ClusterItemIds)
			{
				const FHex* FromHex = ItemHex.Find(Id);   // FIX: was ItemIdToHex
				if (!FromHex) return false;
				FHex ToHex(FromHex->Q + dQ, FromHex->R + dR);

				if (LocalOccupied.Contains(ToHex))
				{
					if (const FItem* Occ = ItemMap.Find(ToHex))
					{
						if (!ClusterItemIds.Contains(Occ->Id))
							return false;
					}
				}
				if (const FItem* Occ = ItemMap.Find(ToHex))
				{
					if (!ClusterItemIds.Contains(Occ->Id))
						return false;
				}
			}

			// Apply translation
			TMap<FHex, FItem> Snapshot = ItemMap;
			for (int32 Id : ClusterItemIds)
			{
				const FHex* FromHex = ItemHex.Find(Id);
				if (!FromHex) continue;
				FHex ToHex(FromHex->Q + dQ, FromHex->R + dR);

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

			for (int32 Id : ClusterItemIds)
				OutMovedIds.Add(Id);

			return true;
		};

	// Iterate belt tiles and attempt single pull per item/cluster
	for (const auto& KV : TileMap)
	{
		if (KV.Value.Kind != ETileKind::Belt) continue;
		const FHex BeltHex = KV.Key;
		if (ItemMap.Contains(BeltHex)) continue; // already occupied

		const uint8 Dir = (uint8)(KV.Value.Arg & 0xFF);
		const auto Opp = [](EHexDir D) { return (EHexDir)(((uint8)D + 3) % 6); };
		const FHex Behind = HexUtil::Neighbor(BeltHex, Opp((EHexDir)(Dir % 6)));

		if (FItem* BehindItem = ItemMap.Find(Behind))
		{
			// Skip if already moved earlier this tick
			if (ItemsMovedForward.Contains(BehindItem->Id)) continue;
			if (PulledThisPass.Contains(BehindItem->Id)) continue;

			bool bAllowPull = true;

			// Direction priority:
			// If the behind tile is a belt whose forward hex is NOT this belt,
			// only allow lateral pull if that forward hex is NOT an empty belt (i.e. cannot advance straight).
			if (const FTile* BehindTile = TileMap.Find(Behind); BehindTile && BehindTile->Kind == ETileKind::Belt)
			{
				const uint8 BehindDir = (uint8)(BehindTile->Arg & 0xFF) % 6;
				const FHex BehindForward = HexUtil::Neighbor(Behind, (EHexDir)BehindDir);
				const bool bFeedsThisBelt = (BehindForward == BeltHex);

				if (!bFeedsThisBelt)
				{
					const FTile* ForwardTile = TileMap.Find(BehindForward);
					const bool bForwardIsEmptyBelt =
						(ForwardTile && ForwardTile->Kind == ETileKind::Belt && !ItemMap.Contains(BehindForward));

					// If the upstream belt can move forward into an empty belt tile, we defer (no side steal).
					if (bForwardIsEmptyBelt)
					{
						bAllowPull = false;
					}
				}
			}

			if (!bAllowPull)
				continue;

			TryTranslateClusterPull(BehindItem->Id, BeltHex, Dir, PulledThisPass);
		}
	}

	/* ---------- Immediate forward pass AFTER pulls (eliminate 1‑tick wait) ---------- */
	{
		TSet<FHex> OccupiedNow;
		for (const auto& KV : ItemMap) OccupiedNow.Add(KV.Key);

		struct FLateMove { int32 ItemId; FHex From; FHex To; };
		TArray<FLateMove> LateMoves;
		LateMoves.Reserve(16);

		for (const auto& KV : ItemMap)
		{
			const FHex& Hex = KV.Key;
			const FItem& It = KV.Value;
			if (It.Id == 0) continue;
			if (ItemsMovedForward.Contains(It.Id)) continue;
			if (PulledThisPass.Contains(It.Id)) { /* allowed to still move */ }

			const FTile* T = TileMap.Find(Hex);
			if (!T || T->Kind != ETileKind::Belt) continue;

			// Skip bonded items (cluster logic already handled earlier; could extend if needed)
			if (IsItemBonded(It.Id)) continue;

			float* Prog = BeltProgress.Find(It.Id);
			if (!Prog) continue;

			// If progress reached a full tile, push now
			if (*Prog >= 1.f)
			{
				const uint8 Dir = (uint8)(T->Arg & 0xFF) % 6;
				const FHex To = HexUtil::Neighbor(Hex, (EHexDir)Dir);
				if (!OccupiedNow.Contains(To) && !ItemMap.Contains(To))
				{
					LateMoves.Add({ It.Id, Hex, To });
					OccupiedNow.Remove(Hex);
					OccupiedNow.Add(To);
				}
				else
				{
					// Blocked: clamp progress so we don't grow unbounded
					*Prog = FMath::Min(*Prog, BeltBlockedClamp);
				}
			}
		}

		if (LateMoves.Num())
		{
			TMap<FHex, FItem> Snap = ItemMap;
			for (const FLateMove& M : LateMoves)
			{
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

	// ---- record all movement this tick for rendering interpolation ----
	ItemsMovedLastStep = ItemsMovedForward;
	for (int32 PulledId : PulledThisPass)
	{
		ItemsMovedLastStep.Add(PulledId);
	}
}

// --- NEW: split glyph handling so inputs spawn before belts ---
// (place near other private helpers)

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



