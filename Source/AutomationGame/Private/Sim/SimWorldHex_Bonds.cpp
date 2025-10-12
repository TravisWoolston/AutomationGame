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

	CleanupBrokenBonds(HeldIds);
	//CheckAndRemoveDisconnectedOrbsFromHeldClusters();

	// Build virtual occupancy for held items using COMMITTED SIM STATE
	TMap<FHex, int32> VirtualHeldItemAt;   // World hex -> ItemId
	TMap<int32, int32> HeldItemToArmIndex; // ItemId -> ArmIndex

	auto Rot60 = [](FHex D, uint8 StepsCCW)
		{
			for (uint8 i = 0; i < (StepsCCW % 6); ++i) { int32 q = D.Q, r = D.R; D.Q = -r; D.R = q + r; }
			return D;
		};

	for (int32 ArmIndex = 0; ArmIndex < Arms.Num(); ++ArmIndex)
	{
		const FArmState& A = Arms[ArmIndex];
		if (!A.bHolding) continue;

		const int32 SimLen = GetSimArmLen(ArmIndex, A);
		uint8 SimFace = GetSimFacing(ArmIndex, A);
		const int8  SimGrab = GetSimGrabFacing(ArmIndex, A);

		// If this arm finished rotation visually, use that facing for this tick.
		if (const uint8* ReadyFace = ArmsReadyForUnbond.Find(ArmIndex))
		{
			SimFace = (*ReadyFace) % 6;
		}

		FHex GripHex = A.BaseHex;
		for (int i = 0; i < SimLen; ++i)
			GripHex = HexUtil::Neighbor(GripHex, (EHexDir)(SimFace % 6));

		if (A.Held.bIsCluster)
		{
			const uint8 cur = (uint8)(SimFace % 6);
			const uint8 facingDelta = (SimGrab >= 0)
				? (uint8)(((int32)cur - (int32)(SimGrab % 6) + 6) % 6)
				: cur;
			const uint8 ccw = (uint8)((6 - facingDelta) % 6);

			for (const FOrbData& Sub : A.Held.Orbs)
			{
				if (Sub.ItemId <= 0) continue;
				FHex Local(Sub.dQ, Sub.dR);
				FHex Rot = Rot60(Local, ccw);
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

	// Form bonds (board or virtual-held) deterministically with sim state
	TSet<int32> Newly;
	int32 BondTileCount = 0;

	// Collect merges to process once per tick (avoid duplicates)
	struct FM { int32 ReceiverArm = -1; int32 DonorArm = -1; int32 ItemIdA = -1; int32 ItemIdB = -1; };
	TArray<FM> MergesThisTick;

	for (const auto& KV : TileMap)
	{
		const FTile& T = KV.Value;
		if (T.Kind != ETileKind::Bond) continue;
		BondTileCount++;

		const uint8 Dir = (uint8)(T.Arg & 0xFF);
		const FHex AHex = KV.Key;
		const FHex BHex = HexUtil::Neighbor(AHex, (EHexDir)(Dir % 6));

		int32 AId = 0, BId = 0;
		bool bAIsHeld = false, bBIsHeld = false;

		if (FItem* IA = ItemMap.Find(AHex)) { AId = IA->Id; }
		else if (int32* HeldA = VirtualHeldItemAt.Find(AHex)) { AId = *HeldA; bAIsHeld = true; }

		if (FItem* IB = ItemMap.Find(BHex)) { BId = IB->Id; }
		else if (int32* HeldB = VirtualHeldItemAt.Find(BHex)) { BId = *HeldB; bBIsHeld = true; }

		if (AId <= 0 || BId <= 0 || AId == BId) continue;

		// Dedup
		bool bDup = false;
		if (const TArray<int32>* AList = ItemToBonds.Find(AId))
		{
			for (int32 BondIdx : *AList)
			{
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

		// Held<->Board: assimilate immediately
		if (bAIsHeld && !bBIsHeld)
		{
			AddBoardOrbToHeldItem(AId, BId, BHex);
		}
		else if (bBIsHeld && !bAIsHeld)
		{
			AddBoardOrbToHeldItem(BId, AId, AHex);
		}
		else if (bAIsHeld && bBIsHeld)
		{
			// Held-to-held merge (deferred per pair)
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
				for (const FM& E : MergesThisTick)
				{
					if (E.ReceiverArm == M.ReceiverArm && E.DonorArm == M.DonorArm) { bAlready = true; break; }
				}
				if (!bAlready) MergesThisTick.Add(M);
			}
		}
	}

	// Process merges
	for (const FM& M : MergesThisTick)
	{
		if (M.ReceiverArm >= 0 && M.ReceiverArm < Arms.Num() &&
			M.DonorArm >= 0 && M.DonorArm < Arms.Num())
		{
			FArmState& Receiver = Arms[M.ReceiverArm];
			FArmState& Donor = Arms[M.DonorArm];
			if (Receiver.bHolding && Donor.bHolding)
			{
				ProcessHeldToHeldMerge(M.ReceiverArm, M.DonorArm);
			}
		}
	}

	if (bDevDebugBonds)
	{
		UE_LOG(LogAGSim, Log, TEXT("Processed %d bond tiles"), BondTileCount);
	}

	// Internal bonds (held cluster against bond tiles), using sim state
	for (int32 ArmIndex = 0; ArmIndex < Arms.Num(); ++ArmIndex)
	{
		const FArmState& A = Arms[ArmIndex];
		if (!A.bHolding || !A.Held.bIsCluster) continue;

		// Recompute grip and ccw using "ready" facing if available
		int32 SimLen = GetSimArmLen(ArmIndex, A);
		uint8 SimFace = GetSimFacing(ArmIndex, A);
		int8  SimGrab = GetSimGrabFacing(ArmIndex, A);

		if (const uint8* ReadyFace = ArmsReadyForUnbond.Find(ArmIndex))
		{
			SimFace = (*ReadyFace) % 6;
		}

		FHex GripHex = A.BaseHex;
		for (int i = 0; i < SimLen; ++i)
			GripHex = HexUtil::Neighbor(GripHex, (EHexDir)(SimFace % 6));

		const uint8 cur = (uint8)(SimFace % 6);
		const uint8 facingDelta = (SimGrab >= 0)
			? (uint8)(((int32)cur - (int32)(SimGrab % 6) + 6) % 6)
			: cur;
		const uint8 ccw = (uint8)((6 - facingDelta) % 6);

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

	// Unbond tiles (board or virtual-held)
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

		if (AId > 0 && BId > 0) BreakBondBetween(AId, BId);
	}

	ArmsReadyForUnbond.Empty();

	// Bookkeeping for next frame
	for (int32 ArmIndex = 0; ArmIndex < Arms.Num(); ++ArmIndex)
	{
		FArmState& S = Arms[ArmIndex];
		S.PrevArmLen = S.ArmLen;
		S.PrevTrackIdx = S.TrackIdx;
	}
}