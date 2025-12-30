#include "Sim/SimWorldHex.h"
#include "Sim/HexTypes.h"
#include "Sim/WalkerTypes.h"
#include "Sim/HexCoord.h"
#include "Actors/RequestBeacon.h"

void USimWorldHex::TryWalkerPickUp(FWalkerState& W, FFootState& Foot, int32 WalkerIndex, int32 FootIndex, const TMap<FHex, ARequestBeacon*>& BeaconCache, const TArray<FFootState*>& AllFeet)
{
	if (!ItemMap.Contains(Foot.TargetTile)) return;

	ARequestBeacon* PickUpBeacon = nullptr;
	if (ARequestBeacon* const* FoundBeacon = BeaconCache.Find(Foot.TargetTile))
	{
		if ((*FoundBeacon)->BeaconRole == EBeaconRole::PickUp)
		{
			PickUpBeacon = *FoundBeacon;
		}
	}

	if (!PickUpBeacon || PickUpBeacon->BeaconState != EBeaconState::InProgress) return;

	// Check if another foot is already holding
	for (int32 CheckIdx = 0; CheckIdx < AllFeet.Num(); ++CheckIdx)
	{
		if (CheckIdx != FootIndex && AllFeet[CheckIdx]->bHoldingItem)
		{
			return;
		}
	}

	FItem* Found = ItemMap.Find(Foot.TargetTile);
	if (!Found || Found->Id == 0) return;

	FItem PickedItem;
	bool bPickedSuccessfully = false;

	TSet<int32> ClusterIds;
	if (ItemToBonds.Contains(Found->Id))
	{
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
		if (TryTakeItem(Foot.TargetTile, PickedItem))
		{
			bPickedSuccessfully = true;
		}
	}
	else
	{
		float PickUpBeaconYaw = PickUpBeacon->GetActorRotation().Yaw;
		int32 PickUpRot60 = FMath::RoundToInt(FRotator::NormalizeAxis(PickUpBeaconYaw) / 60.0f);
		if (PickUpRot60 < 0) PickUpRot60 += 6;
		PickUpRot60 = PickUpRot60 % 6;

		FItem Cluster;
		Cluster.bIsCluster = true;
		Cluster.Id = Found->Id;
		Cluster.GrabFacing = (int8)PickUpRot60;

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

			const FItem* It = ItemMap.Find(ItemHex);
			if (It && !It->bIsCluster)
			{
				const FHex Delta(ItemHex.Q - Foot.TargetTile.Q, ItemHex.R - Foot.TargetTile.R);
				FOrbData Orb;
				Orb.Type = (uint8)It->Single.Type;
				Orb.Color = (uint8)It->Single.Color;
				Orb.ItemId = It->Id;
				Orb.dQ = (int32)Delta.Q;
				Orb.dR = (int32)Delta.R;
				Cluster.Orbs.Add(Orb);
			}
		}

		if (bAllFound && Cluster.Orbs.Num() > 0)
		{
			for (const FOrbData& Sub : Cluster.Orbs)
			{
				for (auto It = ItemMap.CreateIterator(); It; ++It)
				{
					if (It.Value().Id == Sub.ItemId)
					{
						It.RemoveCurrent();
						break;
					}
				}
			}
			PickedItem = Cluster;
			bPickedSuccessfully = true;
		}
	}

	if (bPickedSuccessfully)
	{
		// PickUp beacon stays InProgress until delivery - will be set to NotReady when drop-off completes
		Foot.HeldItem = PickedItem;
		Foot.bHoldingItem = true;
		Foot.CurrentTile = Foot.TargetTile;
		Foot.TargetTile = Foot.CurrentTile;
		Foot.StepState = EFootStepState::Planted;
		Foot.StepProgress = 0;
		Foot.HeightOffset = W.StepHeight;
		
		// Track the source pickup beacon hex to keep it InProgress until delivery
		Foot.SourcePickUpHex = FHex(PickUpBeacon->Q, PickUpBeacon->R);
		Foot.bHasSourcePickUp = true;
		
		// Reset timeout counter on successful pickup
		W.TicksSinceLastFootTargetChange = 0;

		if (PickUpBeacon->PairedBeacon)
		{
			FHex DropOffHex(PickUpBeacon->PairedBeacon->Q, PickUpBeacon->PairedBeacon->R);
			Foot.DesignatedDropOffHex = DropOffHex;
			Foot.bHasDesignatedDropOff = true;

			FString HoldingFootQuadrant;
			switch (FootIndex)
			{
			case 0: HoldingFootQuadrant = TEXT("S"); break;
			case 1: HoldingFootQuadrant = TEXT("W"); break;
			case 2: HoldingFootQuadrant = TEXT("E"); break;
			case 3: HoldingFootQuadrant = TEXT("N"); break;
			}

			float OffsetDQ = 0.f, OffsetDR = 0.f;
			if (HoldingFootQuadrant == TEXT("N")) { OffsetDQ = -1.f; OffsetDR = 0.f; }
			else if (HoldingFootQuadrant == TEXT("E")) { OffsetDQ = 0.f; OffsetDR = -1.f; }
			else if (HoldingFootQuadrant == TEXT("S")) { OffsetDQ = 1.f; OffsetDR = 0.f; }
			else if (HoldingFootQuadrant == TEXT("W")) { OffsetDQ = 0.f; OffsetDR = 1.f; }

			float OffsetDistance = (float)W.ReachDistance * 0.7f;
			int32 TargetQ = DropOffHex.Q + FMath::RoundToInt(OffsetDQ * OffsetDistance);
			int32 TargetR = DropOffHex.R + FMath::RoundToInt(OffsetDR * OffsetDistance);
			FHex WalkerTarget(TargetQ, TargetR);

			W.Requests.Empty();
			FWalkerRequest DropReq;
			DropReq.TargetHex = WalkerTarget;
			DropReq.Priority = 2000;
			W.Requests.Add(DropReq);

			// Mark the drop-off beacon as in-progress to prevent others from taking it
			if (PickUpBeacon->PairedBeacon)
			{
				PickUpBeacon->PairedBeacon->SetState(EBeaconState::InProgress);
				UE_LOG(LogTemp, Warning, TEXT("Walker %d: Successfully assigned Drop-Off beacon at (%d,%d)"), WalkerIndex, DropOffHex.Q, DropOffHex.R);
			}
		}
		else
		{
			UE_LOG(LogTemp, Error, TEXT("Walker %d: CRITICAL - Picked up item but beacon at (%d,%d) has no PairedBeacon!"), 
				WalkerIndex, Foot.CurrentTile.Q, Foot.CurrentTile.R);
			// No paired beacon - just complete the current pickup request
			if (W.Requests.Num() > 0) W.Requests.RemoveAt(0);
		}

		W.bProcessingRequest = false;
		W.CurrentTargetHex = W.BaseHex; 
	}
}

bool USimWorldHex::TryWalkerDropOff(FWalkerState& W, FFootState& Foot, int32 WalkerIndex, int32 FootIndex, const TSet<FHex>& HeldOrbHexes, const TSet<FHex>& ArmBaseHexes, const TSet<FHex>& HoldingFootHexes, const TMap<FHex, ARequestBeacon*>& BeaconCache)
{
	bool bIsInRangeOfDropOff = false;
	bool bInCorrectQuadrant = false;
	FHex ActualDropOffHex;

	if (Foot.bHasDesignatedDropOff)
	{
		ActualDropOffHex = Foot.DesignatedDropOffHex;
		int32 dist = HexUtil::Dist(W.BaseHex, ActualDropOffHex);
		bIsInRangeOfDropOff = (dist <= W.ReachDistance);

		const FString DropOffQuad = GetFixedQuadrantFromHex(W.BaseHex, ActualDropOffHex, W.QuadrantRotationOffset);
		FString FootQuad;
		switch (FootIndex)
		{
		case 0: FootQuad = TEXT("S"); break;
		case 1: FootQuad = TEXT("W"); break;
		case 2: FootQuad = TEXT("E"); break;
		case 3: FootQuad = TEXT("N"); break;
		}
		bInCorrectQuadrant = DropOffQuad.Equals(FootQuad, ESearchCase::CaseSensitive);
	}

	const bool bReadyToPlace = bIsInRangeOfDropOff && bInCorrectQuadrant;

	if (bReadyToPlace)
	{
		if (Foot.TargetTile != ActualDropOffHex)
		{
			Foot.TargetTile = ActualDropOffHex;
			Foot.StepState = EFootStepState::Moving;
			Foot.StepProgress = 0;
			Foot.HeightOffset = W.StepHeight;
			UE_LOG(LogTemp, Log, TEXT("Walker %d: Redirecting Foot %d to drop-off tile (%d,%d)"), WalkerIndex, FootIndex, Foot.TargetTile.Q, Foot.TargetTile.R);
			return false;
		}

		ARequestBeacon* DropOffBeacon = nullptr;
		if (ARequestBeacon* const* FoundBeacon = BeaconCache.Find(Foot.DesignatedDropOffHex))
		{
			if ((*FoundBeacon)->BeaconRole == EBeaconRole::DropOff)
			{
				DropOffBeacon = *FoundBeacon;
			}
		}

		if (DropOffBeacon)
		{
			float BeaconYaw = DropOffBeacon->GetActorRotation().Yaw;
			int32 Rot60 = FMath::RoundToInt(FRotator::NormalizeAxis(BeaconYaw) / 60.0f);
			if (Rot60 < 0) Rot60 += 6;
			Rot60 = Rot60 % 6;

			bool bPlacedSuccessfully = false;

			if (!Foot.HeldItem.bIsCluster)
			{
				if (!ItemMap.Contains(Foot.TargetTile))
				{
					Foot.HeldItem.Single.Rot60 = (uint8)Rot60;
					if (TryPutItem(Foot.TargetTile, Foot.HeldItem))
					{
						NewlyPlacedItems.Add(Foot.HeldItem.Id);
						bPlacedSuccessfully = true;
					}
				}
			}
			else
			{
				int32 CurrentRot = (Foot.HeldItem.GrabFacing >= 0) ? ((int32)Foot.HeldItem.GrabFacing % 6) : 0;
				int32 RotDiff = (Rot60 - CurrentRot + 6) % 6;

				TArray<FHex> TargetHexes;
				bool bBlocked = false;
				for (const FOrbData& OrbData : Foot.HeldItem.Orbs)
				{
					FHex Local(OrbData.dQ, OrbData.dR);
					for (int32 r = 0; r < RotDiff; ++r) { int32 OldQ = Local.Q; int32 OldR = Local.R; Local.Q = -OldR; Local.R = OldQ + OldR; }
					FHex WorldHex(Foot.TargetTile.Q + Local.Q, Foot.TargetTile.R + Local.R);
					if (ItemMap.Contains(WorldHex)) { bBlocked = true; break; }
					TargetHexes.Add(WorldHex);
				}

				if (!bBlocked)
				{
					// Foot clearing logic: move any feet blocking cluster placement area
					TArray<FFootState*> WalkerFeet = GetAllFeet(W);
					TArray<int32> BlockingFootIndices;
					
					// Build set of hexes that will be occupied by the cluster
					TSet<FHex> ClusterTargetHexSet;
					for (const FHex& TargetHex : TargetHexes)
					{
						ClusterTargetHexSet.Add(TargetHex);
					}
					ClusterTargetHexSet.Add(Foot.TargetTile); // Include the drop-off anchor
					
					for (const FHex& TargetHex : TargetHexes)
					{
						for (int32 CheckIdx = 0; CheckIdx < 4; ++CheckIdx)
						{
							if (CheckIdx == FootIndex) continue;
							// Check if foot is currently blocking OR will land on a cluster hex
							if (WalkerFeet[CheckIdx]->CurrentTile == TargetHex) 
							{
								BlockingFootIndices.AddUnique(CheckIdx);
							}
							else if (WalkerFeet[CheckIdx]->TargetTile == TargetHex && 
							         WalkerFeet[CheckIdx]->StepState != EFootStepState::Planted)
							{
								// Foot is moving toward a cluster hex - need to redirect
								BlockingFootIndices.AddUnique(CheckIdx);
							}
						}
					}

					if (BlockingFootIndices.Num() > 0)
					{
						// Helper lambda to find a safe target for a foot
						auto FindSafeTarget = [&](int32 FootIdx) -> FHex
						{
							FString FootQuad;
							switch (FootIdx)
							{
							case 0: FootQuad = TEXT("S"); break;
							case 1: FootQuad = TEXT("W"); break;
							case 2: FootQuad = TEXT("E"); break;
							case 3: FootQuad = TEXT("N"); break;
							}
							
							FHex SafeTarget = WalkerFeet[FootIdx]->CurrentTile;
							float BestDist = 0.f;
							for (int32 q = -W.ReachDistance; q <= W.ReachDistance; ++q)
							{
								for (int32 r = -W.ReachDistance; r <= W.ReachDistance; ++r)
								{
									FHex TestHex(W.BaseHex.Q + q, W.BaseHex.R + r);
									int32 dist = HexUtil::Dist(W.BaseHex, TestHex);
									if (dist == 0 || dist > W.ReachDistance || (float)dist < W.MinimumReachDistance) continue;
									if (ClusterTargetHexSet.Contains(TestHex)) continue;
									if (!GetFixedQuadrantFromHex(W.BaseHex, TestHex, W.QuadrantRotationOffset).Equals(FootQuad)) continue;
									if (ItemMap.Contains(TestHex)) continue;
									
									// Prefer tiles farther from the cluster area
									float MinDistToCluster = 999.f;
									for (const FHex& ClusterHex : ClusterTargetHexSet)
									{
										MinDistToCluster = FMath::Min(MinDistToCluster, (float)HexUtil::Dist(TestHex, ClusterHex));
									}
									if (MinDistToCluster > BestDist)
									{
										BestDist = MinDistToCluster;
										SafeTarget = TestHex;
									}
								}
							}
							return SafeTarget;
						};
						
						for (int32 BlockingIdx : BlockingFootIndices)
						{
							FFootState* BlockingFoot = WalkerFeet[BlockingIdx];
							FHex SafeTarget = FindSafeTarget(BlockingIdx);
							
							if (BlockingFoot->StepState == EFootStepState::Planted)
							{
								BlockingFoot->TargetTile = SafeTarget;
								BlockingFoot->StepState = EFootStepState::Raising;
								BlockingFoot->StepProgress = 0;
							}
							else if (ClusterTargetHexSet.Contains(BlockingFoot->TargetTile))
							{
								// Foot is already moving but targeting a cluster hex - redirect it
								BlockingFoot->TargetTile = SafeTarget;
							}
						}
						return false; // Wait for clear
					}

					int32 PlacedCount = 0;
					for (int32 idx = 0; idx < Foot.HeldItem.Orbs.Num(); ++idx)
					{
						const FOrbData& OrbData = Foot.HeldItem.Orbs[idx];
						FOrb O; O.Type = (EOrbType)OrbData.Type; O.Color = (EOrbColor)OrbData.Color; O.Rot60 = 0;
						FItem NewItem = FItem::MakeSingle(O);
						NewItem.Id = (OrbData.ItemId > 0) ? OrbData.ItemId : NextItemId++;
						if (TryPutItem(TargetHexes[idx], NewItem)) { NewlyPlacedItems.Add(NewItem.Id); PlacedCount++; }
					}

					if (PlacedCount == Foot.HeldItem.Orbs.Num())
					{
						bPlacedSuccessfully = true;
						// Rebuild bonds link...
						for (int32 BondIdx = 0; BondIdx < Bonds.Num(); ++BondIdx)
						{
							FBond& Bond = Bonds[BondIdx];
							if (!Bond.bActive) continue;
							for (const FOrbData& OrbData : Foot.HeldItem.Orbs)
							{
								if (OrbData.ItemId > 0 && (Bond.ItemA == OrbData.ItemId || Bond.ItemB == OrbData.ItemId))
								{
									// Ensure link exists
									if (Bond.ItemA == OrbData.ItemId) AddItemBondLink(Bond.ItemA, BondIdx);
									if (Bond.ItemB == OrbData.ItemId) AddItemBondLink(Bond.ItemB, BondIdx);
								}
							}
						}
					}
				}
			}

			if (bPlacedSuccessfully)
			{
				Foot.bHoldingItem = false;
				Foot.bHasDesignatedDropOff = false;
				Foot.bHasSourcePickUp = false;
				Foot.HeldItem = FItem();
				DropOffBeacon->SetState(EBeaconState::NotReady);
				
				// Also set the paired PickUp beacon to NotReady now that delivery is complete
				if (DropOffBeacon->PairedBeacon)
				{
					DropOffBeacon->PairedBeacon->SetState(EBeaconState::NotReady);
				}

				Foot.CurrentTile = Foot.TargetTile;
				Foot.StepState = EFootStepState::Lowering;
				Foot.StepProgress = 0;

				// Complete the current request (body positioning or drop-off)
				if (W.Requests.Num() > 0) W.Requests.RemoveAt(0);
				W.bProcessingRequest = false;
				W.CurrentTargetHex = W.BaseHex;
				return true;
			}
		}
	}
	else if (Foot.bHasDesignatedDropOff && !bReadyToPlace)
	{
		// Calculate the ideal body target to reach the drop-off with this specific foot
		float FootIdealAngleRad = 0.f;
		switch (FootIndex)
		{
		case 3: FootIdealAngleRad = -PI * 0.5f; break; // U/North
		case 2: FootIdealAngleRad = 0.f; break;         // R/East
		case 0: FootIdealAngleRad = PI * 0.5f; break;  // D/South
		case 1: FootIdealAngleRad = PI; break;          // L/West
		}

		// Foot world angle should be the angle to the drop-off hex
		const int32 dq = ActualDropOffHex.Q - W.BaseHex.Q;
		const int32 dr = ActualDropOffHex.R - W.BaseHex.R;
		const float x = 1.7320508f * (float)dq + 0.8660254f * (float)dr;
		const float y = 1.5f * (float)dr;
		const float WorldAngleRad = FMath::Atan2(y, x);

		// The offset vector from Body to Foot at the target placement distance
		float TargetDist = (W.MinimumReachDistance + (float)W.ReachDistance) * 0.5f;
		TargetDist = FMath::Clamp(TargetDist, W.MinimumReachDistance + 0.5f, (float)W.ReachDistance - 0.5f);

		// Offset logic: Body = ActualDropOffHex - (Vector from body to foot)
		// We use current rotation to define the offset because we want to pivot around the body
		const float TotalAngleRad = FootIdealAngleRad + FMath::DegreesToRadians(W.QuadrantRotationOffset);
		const float QuadAX = (0.5773503f * FMath::Cos(TotalAngleRad) - 0.3333333f * FMath::Sin(TotalAngleRad));
		const float QuadAY = (0.6666667f * FMath::Sin(TotalAngleRad));

		FHex BodyTarget(
			ActualDropOffHex.Q - FMath::RoundToInt(QuadAX * TargetDist),
			ActualDropOffHex.R - FMath::RoundToInt(QuadAY * TargetDist)
		);
		
		if (W.CurrentTargetHex != BodyTarget || !W.bProcessingRequest)
		{
			UE_LOG(LogTemp, Log, TEXT("Walker %d: Creating new body stand request for drop-off at (%d,%d)"), WalkerIndex, BodyTarget.Q, BodyTarget.R);
			W.Requests.Empty();
			FWalkerRequest MoveReq; MoveReq.TargetHex = BodyTarget; MoveReq.Priority = 2000;
			W.Requests.Add(MoveReq);
			W.CurrentTargetHex = BodyTarget;
			W.bProcessingRequest = true;
		}
		
		// If we are at the body target but still not ready, it's likely a rotation issue.
		// The walker needs to pivot: other feet must move to rotate the walker until
		// the holding foot's quadrant aligns with the drop-off hex.
		if (W.BaseHex == BodyTarget && !bReadyToPlace)
		{
			UE_LOG(LogTemp, Log, TEXT("Walker %d: At BodyTarget (%d,%d), Foot %d needs pivot (InRange=%d, InQuad=%d)"), 
				WalkerIndex, BodyTarget.Q, BodyTarget.R, FootIndex, bIsInRangeOfDropOff, bInCorrectQuadrant);
		}
		
		// BUGFIX: During pivoting, ALWAYS allow the holding foot's step to complete.
		// This lets the foot land (become Planted) so other feet can move and rotate
		// the walker. Without this, the holding foot blocks in Moving state forever.
		// The foot will re-evaluate its target on the next tick once it's Planted.
		return true;
	}
	return false;
}
