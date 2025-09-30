#include "Sim/SimWorldHex.h"

// Belt processing functions
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
			// Only initialize belt progress if the item was already on a belt (has existing progress)
			// This prevents newly spawned/disconnected items from immediately moving on belts
			if (BeltProgress.Contains(KV.Value.Id))
			{
				// Item was already on a belt, keep its progress
			}
			else
			{
				// Item is new to a belt, don't initialize progress yet - let it start fresh next frame
			}
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
		if (NewlyPlacedItems.Contains(KV.Value.Id)) continue; // Skip newly placed items
		// Only process items that have existing belt progress (were already on belts)
		BeltItems.Add({ KV.Value.Id, KV.Key });
	}
	const float Dt = FMath::Max(0.f, FixedStepSec);
	const float TilesPerSec = (Speed / FMath::Max(KINDA_SMALL_NUMBER, Dt));
	const float ProgressPerStep = TilesPerSec * Dt;

	// Initialize belt progress for new items on belts
	for (const auto& KV : ItemMap)
	{
		if (KV.Value.Id == 0) continue;
		if (NewlyPlacedItems.Contains(KV.Value.Id)) continue;
		if (BeltProgress.Contains(KV.Value.Id)) continue; // Already has progress
		
		const FTile* T = TileMap.Find(KV.Key);
		if (T && T->Kind == ETileKind::Belt)
		{
			// New item on belt - start with 0 progress
			BeltProgress.Add(KV.Value.Id, 0.f);
		}
	}

	// Advance progress for all belt items
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
				if (TD->Kind == ETileKind::Output)
					OrbsNewlyMovedToOutput.Add(M.To);
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
				if (TD->Kind == ETileKind::Output)
					OrbsNewlyMovedToOutput.Add(M.To);
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
				{
					// Only add belt progress if this item wasn't just placed
					if (!NewlyPlacedItems.Contains(Id))
					{
						BeltProgress.Add(Id, 0.f);
					}
				}
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

				// Check if moved to output tile
				if (const FTile* TD = TileMap.Find(M.To))
				{
					if (TD->Kind == ETileKind::Output)
						OrbsNewlyMovedToOutput.Add(M.To);
				}

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
	
	/* ---------- Initialize belt progress for new items on belts ---------- */
	// After all belt processing is complete, initialize progress for items that are on belts but don't have progress yet
	for (const auto& KV : ItemMap)
	{
		const FTile* T = TileMap.Find(KV.Key);
		if (T && T->Kind == ETileKind::Belt && KV.Value.Id != 0)
		{
			if (!BeltProgress.Contains(KV.Value.Id) && !NewlyPlacedItems.Contains(KV.Value.Id))
			{
				// This is a new item on a belt, initialize its progress to 0 for next frame
				BeltProgress.Add(KV.Value.Id, 0.f);
			}
		}
	}
	
	/* ---------- Clear newly placed items for next frame ---------- */
	NewlyPlacedItems.Empty();
	
	/* ---------- Clear recently dropped by arm tracking for next frame ---------- */
	RecentlyDroppedByArm.Empty();
}
