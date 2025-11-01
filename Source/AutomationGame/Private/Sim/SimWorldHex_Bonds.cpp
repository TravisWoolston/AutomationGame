#include "Sim/SimWorldHex.h"
#include "Actors/BasicArm.h"
#include "AGLogChannels.h"
#include "EngineUtils.h"

// Bond formation and management functions
void USimWorldHex::StepBonds()
{
	if (TileMap.Num() == 0 || ItemMap.Num() == 0)
	{
		if (Bonds.Num() > 0) CleanupBrokenBonds(TSet<int32>());
		return;
	}

	if (bDevDebugBonds)
	{
		UE_LOG(LogAGSim, Log, TEXT("StepBonds: %d tiles, %d items"), TileMap.Num(), ItemMap.Num());
	}

	// Gather held IDs for cleanup (for CleanupBrokenBonds)
	TSet<int32> HeldIds;
	for (const FArmState& A : Arms)
	{
		if (!A.bHolding) continue;
		if (A.Held.bIsCluster)
		{
			for (const FOrbData& Sub : A.Held.Orbs)
				if (Sub.ItemId > 0) HeldIds.Add(Sub.ItemId);
		}
		else if (A.Held.Id > 0)
		{
			HeldIds.Add(A.Held.Id);
		}
	}

	CleanupBrokenBonds(HeldIds);

	// Mapping kept outside the loop so unbond phase can reuse it
	TMap<FHex, int32> VirtualHeldItemAt;   // World hex -> ItemId
	TMap<int32, int32> HeldItemToArmIndex; // ItemId -> ArmIndex

	// NEW: helper that absorbs the entire connected board component into the held cluster
	auto AssimilateBoardComponent = [&](int32 HeldItemId, int32 BoardStartId)
		{
			TArray<int32> Stack;
			TSet<int32> Visited;
			Stack.Add(BoardStartId);

			while (Stack.Num() > 0)
			{
				const int32 Cur = Stack.Pop(EAllowShrinking::No);
				if (Visited.Contains(Cur)) continue;
				Visited.Add(Cur);

				// Find Cur on board
				FHex CurHex; bool bFoundHex = false;
				for (const auto& Pair : ItemMap)
				{
					if (Pair.Value.Id == Cur)
					{
						CurHex = Pair.Key;
						bFoundHex = true;
						break;
					}
				}

				// Only assimilate if it's still on the board
				if (bFoundHex)
				{
					AddBoardOrbToHeldItem(HeldItemId, Cur, CurHex);
				}

				// Traverse active bonds to neighbors (board or held, we only assimilate ones still on board)
				if (const TArray<int32>* BondList = ItemToBonds.Find(Cur))
				{
					for (int32 BondIdx : *BondList)
					{
						if (!Bonds.IsValidIndex(BondIdx)) continue;
						const FBond& B = Bonds[BondIdx];
						if (!B.bActive) continue;

						const int32 Other = (B.ItemA == Cur) ? B.ItemB : B.ItemA;
						if (Other <= 0 || Visited.Contains(Other)) continue;

						// Only chase neighbors that are still on the board (present in ItemMap)
						bool bNeighborOnBoard = false;
						for (const auto& P : ItemMap)
						{
							if (P.Value.Id == Other) { bNeighborOnBoard = true; break; }
						}
						if (bNeighborOnBoard)
						{
							Stack.Add(Other);
						}
					}
				}
			}
		};

	// Iterate bond formation + assimilation until no progress
	const int32 MaxAssimilationPasses = 12;
	for (int32 Pass = 0; Pass < MaxAssimilationPasses; ++Pass)
	{
		bool bAssimilatedThisPass = false;

		// Rebuild virtual occupancy for held items using COMMITTED SIM STATE
		VirtualHeldItemAt.Reset();
		HeldItemToArmIndex.Reset();

		auto Rot60 = [](FHex D, uint8 StepsCCW)
			{
				for (uint8 i = 0; i < (StepsCCW % 6); ++i) { int32 q = D.Q, r = D.R; D.Q = -r; D.R = q + r; }
				return D;
			};

		for (int32 ArmIndex = 0; ArmIndex < Arms.Num(); ++ArmIndex)
		{
			const FArmState& A = Arms[ArmIndex];
			if (!A.bHolding) continue;

			const FHex GripHex = GetArmGripHex_Sim(ArmIndex);
			const uint8 ccw = GetHeldCCW_Steps_Sim(ArmIndex);

			if (A.Held.bIsCluster)
			{
				for (const FOrbData& Sub : A.Held.Orbs)
				{
					if (Sub.ItemId <= 0) continue;
					const FHex Rot = Rot60(FHex(Sub.dQ, Sub.dR), ccw);
					const FHex World(GripHex.Q + Rot.Q, GripHex.R + Rot.R);
					VirtualHeldItemAt.FindOrAdd(World, Sub.ItemId);
					HeldItemToArmIndex.FindOrAdd(Sub.ItemId, ArmIndex);
				}
			}
			else if (A.Held.Id > 0)
			{
				VirtualHeldItemAt.FindOrAdd(GripHex, A.Held.Id);
				HeldItemToArmIndex.FindOrAdd(A.Held.Id, ArmIndex);
			}
		}

		TSet<int32> Newly;
		TArray<TPair<int32, int32>> BondsFormedThisPass;

		struct FM { int32 ReceiverArm = -1; int32 DonorArm = -1; int32 ItemIdA = -1; int32 ItemIdB = -1; };
		TArray<FM> MergesThisPass;

		for (const auto& KV : TileMap)
		{
			const FTile& T = KV.Value;
			if (T.Kind != ETileKind::Bond) continue;

			const uint8 Dir = (uint8)(T.Arg & 0xFF);
			const FHex AHex = KV.Key;
			const FHex BHex = HexUtil::Neighbor(AHex, (EHexDir)(Dir % 6));

			int32 AId = 0, BId = 0;
			bool bAIsHeld = false, bBIsHeld = false;

			if (const FItem* IA = ItemMap.Find(AHex)) { AId = IA->Id; }
			else if (int32* HeldA = VirtualHeldItemAt.Find(AHex)) { AId = *HeldA; bAIsHeld = true; }

			if (const FItem* IB = ItemMap.Find(BHex)) { BId = IB->Id; }
			else if (int32* HeldB = VirtualHeldItemAt.Find(BHex)) { BId = *HeldB; bBIsHeld = true; }

			if (AId <= 0 || BId <= 0 || AId == BId) continue;

			// Dedup existing bond
			bool bDup = false;
			if (const TArray<int32>* AList = ItemToBonds.Find(AId))
			{
				for (int32 BondIdx : *AList)
				{
					if (!Bonds.IsValidIndex(BondIdx)) continue;
					const FBond& B = Bonds[BondIdx];
					if ((B.ItemA == AId && B.ItemB == BId) || (B.ItemA == BId && B.ItemB == AId))
					{
						bDup = true; break;
					}
				}
			}
			if (bDup) continue;

			// Create bond record
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
			BondsFormedThisPass.Add(TPair<int32, int32>(AId, BId));

			// Assimilate immediately when one side is held and the other is board
			if (bAIsHeld && !bBIsHeld)
			{
				AssimilateBoardComponent(AId, BId);
				bAssimilatedThisPass = true;
			}
			else if (bBIsHeld && !bAIsHeld)
			{
				AssimilateBoardComponent(BId, AId);
				bAssimilatedThisPass = true;
			}
			else if (bAIsHeld && bBIsHeld)
			{
				// Held-to-held: collect merge
				int32 ArmHoldingA = -1, ArmHoldingB = -1;
				if (int32* ArmIdx = HeldItemToArmIndex.Find(AId)) ArmHoldingA = *ArmIdx;
				if (int32* ArmIdx = HeldItemToArmIndex.Find(BId)) ArmHoldingB = *ArmIdx;

				if (ArmHoldingA >= 0 && ArmHoldingB >= 0 && ArmHoldingA != ArmHoldingB)
				{
					FM M;
					M.ReceiverArm = FMath::Min(ArmHoldingA, ArmHoldingB);
					M.DonorArm = FMath::Max(ArmHoldingA, ArmHoldingB);
					M.ItemIdA = (M.ReceiverArm == ArmHoldingA) ? AId : BId;
					M.ItemIdB = (M.DonorArm == ArmHoldingA) ? AId : BId;

					bool bAlready = false;
					for (const FM& E : MergesThisPass)
					{
						if (E.ReceiverArm == M.ReceiverArm && E.DonorArm == M.DonorArm) { bAlready = true; break; }
					}
					if (!bAlready) MergesThisPass.Add(M);
				}
			}
		}

		// Process held↔held merges now (they change virtual occupancy for next pass)
		for (const FM& M : MergesThisPass)
		{
			if (M.ReceiverArm >= 0 && M.ReceiverArm < Arms.Num() &&
				M.DonorArm >= 0 && M.DonorArm < Arms.Num())
			{
				FArmState& Receiver = Arms[M.ReceiverArm];
				FArmState& Donor = Arms[M.DonorArm];
				if (Receiver.bHolding && Donor.bHolding)
				{
					ProcessHeldToHeldMerge(M.ReceiverArm, M.DonorArm);
					bAssimilatedThisPass = true;
				}
			}
		}

		if (!bAssimilatedThisPass)
		{
			// Nothing more to assimilate this tick
			break;
		}
	}

	if (bDevDebugBonds)
	{
		UE_LOG(LogAGSim, Log, TEXT("StepBonds: assimilation passes finished"));
	}


	// Internal bonds (held cluster against bond tiles), using committed sim state (unchanged)
	for (int32 ArmIndex = 0; ArmIndex < Arms.Num(); ++ArmIndex)
	{
		const FArmState& A = Arms[ArmIndex];
		if (!A.bHolding || !A.Held.bIsCluster) continue;

		const FHex GripHex = GetArmGripHex_Sim(ArmIndex);
		const uint8 ccw = GetHeldCCW_Steps_Sim(ArmIndex);

		auto Rot60 = [](FHex D, uint8 StepsCCW)
			{
				for (uint8 i = 0; i < (StepsCCW % 6); ++i)
				{
					int32 q = D.Q, r = D.R;
					D.Q = -r; D.R = q + r;
				}
				return D;
			};

		for (const auto& TileKV : TileMap)
		{
			const FTile& T = TileKV.Value;
			if (T.Kind != ETileKind::Bond) continue;

			const uint8 Dir = (uint8)(T.Arg & 0xFF);
			const FHex BondHex = TileKV.Key;
			const FHex BondTargetHex = HexUtil::Neighbor(BondHex, (EHexDir)(Dir % 6));

			for (const FOrbData& SubA : A.Held.Orbs)
			{
				if (SubA.ItemId <= 0) continue;
				const FHex RotA = Rot60(FHex(SubA.dQ, SubA.dR), ccw);
				const FHex WorldA(GripHex.Q + RotA.Q, GripHex.R + RotA.R);
				if (WorldA != BondHex) continue;

				for (const FOrbData& SubB : A.Held.Orbs)
				{
					if (SubB.ItemId <= 0 || SubB.ItemId == SubA.ItemId) continue;

					const FHex RotB = Rot60(FHex(SubB.dQ, SubB.dR), ccw);
					const FHex WorldB(GripHex.Q + RotB.Q, GripHex.R + RotB.R);
					if (WorldB != BondTargetHex) continue;

					// Avoid duplicate
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

					FBond InternalBond;
					InternalBond.ItemA = SubA.ItemId;
					InternalBond.ItemB = SubB.ItemId;
					InternalBond.Root = BondHex;
					InternalBond.dQ = (int8)(BondTargetHex.Q - BondHex.Q);
					InternalBond.dR = (int8)(BondTargetHex.R - BondHex.R);
					InternalBond.bActive = true;

					const int32 NewIdx = Bonds.Add(InternalBond);
					AddItemBondLink(InternalBond.ItemA, NewIdx);
					AddItemBondLink(InternalBond.ItemB, NewIdx);
				}
			}
		}
	}

	// Unbond tiles (board or virtual-held) – use the last-built VirtualHeldItemAt
	for (const auto& KV : TileMap)
	{
		const FTile& T = KV.Value;
		if (T.Kind != ETileKind::Unbond) continue;

		const uint8 Dir = (uint8)(T.Arg & 0xFF);
		const FHex AHex = KV.Key;
		const FHex BHex = HexUtil::Neighbor(AHex, (EHexDir)(Dir % 6));

		int32 AId = 0, BId = 0;
		if (FItem* IA = ItemMap.Find(AHex)) AId = IA->Id;
		else if (int32* HeldA = VirtualHeldItemAt.Find(AHex)) AId = *HeldA;

		if (FItem* IB = ItemMap.Find(BHex)) BId = IB->Id;
		else if (int32* HeldB = VirtualHeldItemAt.Find(BHex)) BId = *HeldB;

		if (AId > 0 && BId > 0)
		{

			BreakBondBetween(AId, BId);
		}
	}

	// Immediately check for held cluster splits after breaking bonds
	// This ensures disconnected orbs are dropped right away
	CheckAndRemoveDisconnectedOrbsFromHeldClusters();

	ArmsReadyForUnbond.Empty();

	// Bookkeeping for next frame
	for (int32 ArmIndex = 0; ArmIndex < Arms.Num(); ++ArmIndex)
	{
		FArmState& S = Arms[ArmIndex];
		S.PrevArmLen = S.ArmLen;
		S.PrevTrackIdx = S.TrackIdx;
	}
}

void USimWorldHex::StepBondsSecondPass()
{
	// Second pass for bond formation - only forms bonds for arms that have pending rotations
	// This allows bonds to form AFTER rotations complete, preventing obstruction

	if (TileMap.Num() == 0 || ItemMap.Num() == 0) return;

	// Only run second bond pass if there are pending rotations
	bool bHasPendingRotations = false;
	for (int32 ArmIndex = 0; ArmIndex < Arms.Num(); ++ArmIndex)
	{
		if (PendingFacingByArm.Contains(ArmIndex))
		{
			bHasPendingRotations = true;
			break;
		}
	}

	if (!bHasPendingRotations) return; // No pending rotations, skip second pass

	if (bDevDebugBonds && GEngine)
	{
		GEngine->AddOnScreenDebugMessage(-1, 1.0f, FColor::Magenta,
			TEXT("StepBondsSecondPass: Forming bonds after rotations complete"));
	}

	// Use the existing bond formation logic but with current positions (after rotation)
	// This is essentially the same as the first pass but runs after rotations are committed

	// Gather held IDs for cleanup
	TSet<int32> HeldIds;
	for (const FArmState& A : Arms)
	{
		if (!A.bHolding) continue;
		if (A.Held.bIsCluster)
		{
			for (const FOrbData& Sub : A.Held.Orbs)
				if (Sub.ItemId > 0) HeldIds.Add(Sub.ItemId);
		}
		else if (A.Held.Id > 0)
		{
			HeldIds.Add(A.Held.Id);
		}
	}

	// Mapping for held items at their current positions (after rotation)
	TMap<FHex, int32> VirtualHeldItemAt;
	TMap<int32, int32> HeldItemToArmIndex;

	for (int32 ArmIndex = 0; ArmIndex < Arms.Num(); ++ArmIndex)
	{
		const FArmState& A = Arms[ArmIndex];
		if (!A.bHolding) continue;

		const FHex GripHex = GetArmGripHex_Sim(ArmIndex);
		const uint8 ccw = GetHeldCCW_Steps_Sim(ArmIndex);

		if (A.Held.bIsCluster)
		{
			for (const FOrbData& Sub : A.Held.Orbs)
			{
				if (Sub.ItemId <= 0) continue;
				const FHex Rot = Rot60CCW(FHex(Sub.dQ, Sub.dR), ccw);
				const FHex World(GripHex.Q + Rot.Q, GripHex.R + Rot.R);
				VirtualHeldItemAt.FindOrAdd(World, Sub.ItemId);
				HeldItemToArmIndex.FindOrAdd(Sub.ItemId, ArmIndex);
			}
		}
		else if (A.Held.Id > 0)
		{
			VirtualHeldItemAt.FindOrAdd(GripHex, A.Held.Id);
			HeldItemToArmIndex.FindOrAdd(A.Held.Id, ArmIndex);
		}
	}

	// Form bonds using current positions (same logic as first pass)
	TSet<int32> Newly;

	for (const auto& KV : TileMap)
	{
		const FTile& T = KV.Value;
		if (T.Kind != ETileKind::Bond) continue;

		const uint8 Dir = (uint8)(T.Arg & 0xFF);
		const FHex AHex = KV.Key;
		const FHex BHex = HexUtil::Neighbor(AHex, (EHexDir)(Dir % 6));

		int32 AId = 0, BId = 0;
		bool bAIsHeld = false, bBIsHeld = false;

		if (const FItem* IA = ItemMap.Find(AHex)) { AId = IA->Id; }
		else if (int32* HeldA = VirtualHeldItemAt.Find(AHex)) { AId = *HeldA; bAIsHeld = true; }

		if (const FItem* IB = ItemMap.Find(BHex)) { BId = IB->Id; }
		else if (int32* HeldB = VirtualHeldItemAt.Find(BHex)) { BId = *HeldB; bBIsHeld = true; }

		if (AId == 0 || BId == 0 || AId == BId) continue;
		if (Newly.Contains(AId) || Newly.Contains(BId)) continue;

		// Check if bond already exists
		bool bAlreadyBonded = false;
		if (const TArray<int32>* AList = ItemToBonds.Find(AId))
		{
			for (int32 BondIdx : *AList)
			{
				if (!Bonds.IsValidIndex(BondIdx)) continue;
				const FBond& Existing = Bonds[BondIdx];
				if ((Existing.ItemA == AId && Existing.ItemB == BId) ||
					(Existing.ItemA == BId && Existing.ItemB == AId))
				{
					bAlreadyBonded = true;
					break;
				}
			}
		}
		if (bAlreadyBonded) continue;

		// Create new bond
		FBond NewB;
		NewB.ItemA = AId;
		NewB.ItemB = BId;
		NewB.Root = AHex;
		NewB.dQ = (int8)(BHex.Q - AHex.Q);
		NewB.dR = (int8)(BHex.R - AHex.R);
		NewB.bActive = true;

		const int32 Index = Bonds.Add(NewB);
		AddItemBondLink(NewB.ItemA, Index);
		AddItemBondLink(NewB.ItemB, Index);
		Newly.Add(NewB.ItemA);
		Newly.Add(NewB.ItemB);

		if (bDevDebugBonds && GEngine)
		{
			GEngine->AddOnScreenDebugMessage(-1, 1.0f, FColor::Magenta,
				FString::Printf(TEXT("SecondPass Bond: %d-%d at (%d,%d)"), AId, BId, AHex.Q, AHex.R));
		}
	}
}

TMap<FHex, int32> USimWorldHex::GetPredictedBondFormations() const
{
	// Opus Magnum-style logic: predict which board orbs will bond to held clusters
	// These positions should be excluded from rotation obstruction checks
	TMap<FHex, int32> PredictedBonds;

	if (TileMap.Num() == 0 || ItemMap.Num() == 0) return PredictedBonds;

	// Build virtual occupancy for held items using COMMITTED SIM STATE
	TMap<FHex, int32> VirtualHeldItemAt;
	TMap<int32, int32> HeldItemToArmIndex;

	auto Rot60 = [](FHex D, uint8 StepsCCW)
		{
			for (uint8 i = 0; i < (StepsCCW % 6); ++i) { int32 q = D.Q, r = D.R; D.Q = -r; D.R = q + r; }
			return D;
		};

	for (int32 ArmIndex = 0; ArmIndex < Arms.Num(); ++ArmIndex)
	{
		const FArmState& A = Arms[ArmIndex];
		if (!A.bHolding) continue;

		const FHex GripHex = GetArmGripHex_Sim(ArmIndex);
		const uint8 ccw = GetHeldCCW_Steps_Sim(ArmIndex);

		if (A.Held.bIsCluster)
		{
			for (const FOrbData& Sub : A.Held.Orbs)
			{
				if (Sub.ItemId <= 0) continue;
				const FHex Rot = Rot60(FHex(Sub.dQ, Sub.dR), ccw);
				const FHex World(GripHex.Q + Rot.Q, GripHex.R + Rot.R);
				VirtualHeldItemAt.FindOrAdd(World, Sub.ItemId);
				HeldItemToArmIndex.FindOrAdd(Sub.ItemId, ArmIndex);
			}
		}
		else if (A.Held.Id > 0)
		{
			VirtualHeldItemAt.FindOrAdd(GripHex, A.Held.Id);
			HeldItemToArmIndex.FindOrAdd(A.Held.Id, ArmIndex);
		}
	}

	// Check all bond tiles to see which will form bonds
	for (const auto& KV : TileMap)
	{
		const FTile& T = KV.Value;
		if (T.Kind != ETileKind::Bond) continue;

		const uint8 Dir = (uint8)(T.Arg & 0xFF);
		const FHex AHex = KV.Key;
		const FHex BHex = HexUtil::Neighbor(AHex, (EHexDir)(Dir % 6));

		int32 AId = 0, BId = 0;
		bool bAIsHeld = false, bBIsHeld = false;

		if (const FItem* IA = ItemMap.Find(AHex)) { AId = IA->Id; }
		else if (int32* HeldA = VirtualHeldItemAt.Find(AHex)) { AId = *HeldA; bAIsHeld = true; }

		if (const FItem* IB = ItemMap.Find(BHex)) { BId = IB->Id; }
		else if (int32* HeldB = VirtualHeldItemAt.Find(BHex)) { BId = *HeldB; bBIsHeld = true; }

		if (AId <= 0 || BId <= 0 || AId == BId) continue;

		// Check if bond already exists
		bool bAlreadyBonded = false;
		if (const TArray<int32>* AList = ItemToBonds.Find(AId))
		{
			for (int32 BondIdx : *AList)
			{
				if (!Bonds.IsValidIndex(BondIdx)) continue;
				const FBond& Existing = Bonds[BondIdx];
				if ((Existing.ItemA == AId && Existing.ItemB == BId) ||
					(Existing.ItemA == BId && Existing.ItemB == AId))
				{
					bAlreadyBonded = true;
					break;
				}
			}
		}
		if (bAlreadyBonded) continue;

		// If one item is on board and the other is held, this is a predicted bond formation
		if ((!bAIsHeld && bBIsHeld) || (bAIsHeld && !bBIsHeld))
		{
			// The board item will bond to the held item, so exclude it from obstruction
			FHex BoardItemHex = bAIsHeld ? BHex : AHex;
			int32 BoardItemId = bAIsHeld ? BId : AId;
			PredictedBonds.Add(BoardItemHex, BoardItemId);

			if (bDevDebugBonds && GEngine)
			{
				GEngine->AddOnScreenDebugMessage(-1, 1.0f, FColor::Cyan,
					FString::Printf(TEXT("Predicted bond: board orb %d at (%d,%d) will bond to held item"),
						BoardItemId, BoardItemHex.Q, BoardItemHex.R));
			}
		}
	}

	return PredictedBonds;
}