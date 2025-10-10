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

	// DEBUG: Log when StepBonds is called
	if (bDevDebugBonds)
	{
		UE_LOG(LogAGSim, Log, TEXT("StepBonds: %d tiles, %d items"), TileMap.Num(), ItemMap.Num());
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

	// Check for disconnected orbs in held clusters and remove them immediately
	// This handles cases where arms aren't rotating or aren't over unbond tiles
	CheckAndRemoveDisconnectedOrbsFromHeldClusters();

	// Build virtual occupancy for held orbs (both single and cluster)
	TMap<FHex, int32> VirtualHeldItemAt;
	for (int32 ArmIndex = 0; ArmIndex < Arms.Num(); ArmIndex++)
	{
		const FArmState& A = Arms[ArmIndex];
		if (!A.bHolding) continue;

		FHex GripHex = A.BaseHex;
		for (int i = 0; i < A.ArmLen; ++i)
			GripHex = HexUtil::Neighbor(GripHex, A.Facing);

		if (A.Held.bIsCluster)
		{
			// Handle cluster orbs
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
		else
		{
			// Handle single held orb
			if (A.Held.Id > 0)
			{
				VirtualHeldItemAt.FindOrAdd(GripHex, A.Held.Id);
			}
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

	// Only form bonds when arms are not actively rotating to prevent premature bond formation
	if (bAnyArmMoving)
	{
		// Check if any arms are still actively rotating (StepAlpha < 0.95)
		bool bAnyArmActivelyRotating = false;
		if (UWorld* World = GetWorld())
		{
			for (TActorIterator<ABasicArm> ActorItr(World); ActorItr; ++ActorItr)
			{
				ABasicArm* ArmActor = *ActorItr;
				if (ArmActor && ArmActor->SimArmIndex >= 0 && ArmActor->SimArmIndex < Arms.Num())
				{
					if (ArmActor->IsAnimating() && ArmActor->GetStepAlpha() < 0.95f)
					{
						bAnyArmActivelyRotating = true;
						break;
					}
				}
			}
		}

		// If any arm is actively rotating, skip bond formation
		if (bAnyArmActivelyRotating)
		{
			if (bDevDebugBonds)
			{
				UE_LOG(LogAGSim, Log, TEXT("Skipping bond formation - %d arm(s) actively rotating"), MovingArmCount);
			}
			return;
		}
		else if (bDevDebugBonds)
		{
			UE_LOG(LogAGSim, Log, TEXT("Allowing bond formation - arms nearly finished rotating"));
		}
	}

	// Bond formation (world or virtual held)
	TSet<int32> Newly;
	int32 BondTileCount = 0;
	for (const auto& KV : TileMap)
	{
		const FTile& T = KV.Value;
		if (T.Kind != ETileKind::Bond) continue;
		BondTileCount++;
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

		// DEBUG: Log bond detection (commented out for production)
		// UE_LOG(LogAGSim, Warning, TEXT("Bond detected: AId=%d (held=%s) BId=%d (held=%s) at AHex=(%d,%d) BHex=(%d,%d)"), 
		//	AId, bAIsHeld ? TEXT("yes") : TEXT("no"), BId, bBIsHeld ? TEXT("yes") : TEXT("no"), 
		//	AHex.Q, AHex.R, BHex.Q, BHex.R);

		// Debug logging

		// Debug: Log what we're trying to bond
		if (bDevDebugBonds)
		{
			FString ArmInfoA = bAIsHeld ? TEXT("(Held)") : TEXT("(Board)");
			FString ArmInfoB = bBIsHeld ? TEXT("(Held)") : TEXT("(Board)");
			UE_LOG(LogAGSim, Log, TEXT("Attempting bond: %d%s-%d%s"), AId, *ArmInfoA, BId, *ArmInfoB);
		}

		// Allow bonds between items held by different arms or between held and board items
		if (Newly.Contains(AId) || Newly.Contains(BId)) { /* allow multiple bonds same tick but avoid duplication */ }

		// Prevent duplicate bond between same two items (order-agnostic)
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
			FString ArmInfoA = bAIsHeld ? TEXT("(Held)") : TEXT("(Board)");
			FString ArmInfoB = bBIsHeld ? TEXT("(Held)") : TEXT("(Board)");
			GEngine->AddOnScreenDebugMessage(-1, 0.6f, FColor::Purple,
				FString::Printf(TEXT("Bond formed: %d%s-%d%s"), AId, *ArmInfoA, BId, *ArmInfoB));
		}

		// Handle held-to-board bonds: add board orb to held item (cluster or single)
		// Use async approach - delay until arm finishes visual rotation
		if (bAIsHeld && !bBIsHeld)
		{
			// Find which arm is holding A
			int32 HoldingArmIndex = -1;
			// DEBUG: Log arm lookup (commented out for production)
			// UE_LOG(LogAGSim, Warning, TEXT("Looking for arm holding AId=%d"), AId);

			for (int32 ArmIndex = 0; ArmIndex < Arms.Num(); ArmIndex++)
			{
				const FArmState& Arm = Arms[ArmIndex];
				// UE_LOG(LogAGSim, Warning, TEXT("  Arm %d: bHolding=%s, Held.Id=%d"), 
				//	ArmIndex, Arm.bHolding ? TEXT("yes") : TEXT("no"), Arm.Held.Id);

				if (Arm.bHolding)
				{
					// Check if this arm is holding the item directly
					if (Arm.Held.Id == AId)
					{
						HoldingArmIndex = ArmIndex;
						// UE_LOG(LogAGSim, Warning, TEXT("  Found holding arm: %d (direct match)"), ArmIndex);
						break;
					}
					// Check if this arm is holding a cluster that contains the item
					else if (Arm.Held.bIsCluster)
					{
						// Look through the cluster's sub-orbs
						for (const FOrbData& SubOrb : Arm.Held.Orbs)
						{
							if (SubOrb.ItemId == AId)
							{
								HoldingArmIndex = ArmIndex;
								// UE_LOG(LogAGSim, Warning, TEXT("  Found holding arm: %d (cluster sub-orb match)"), ArmIndex);
								break;
							}
						}
						if (HoldingArmIndex >= 0) break;
					}
				}
			}

			if (HoldingArmIndex >= 0)
			{
				// Create pending bond formation - will be processed when arm finishes rotating
				FPendingBondFormation Pending;
				Pending.HeldItemId = AId;
				Pending.BoardItemId = BId;
				Pending.BoardItemHex = BHex;
				Pending.ArmIndex = HoldingArmIndex;
				Pending.TimeRemaining = 1.0f; // Wait 1 tick for visual rotation to complete (tick-based delay)
				PendingBondFormations.Add(Pending);

				if (bDebugLogs && bDevDebugBonds && GEngine)
				{
					GEngine->AddOnScreenDebugMessage(-1, 1.0f, FColor::Cyan,
						FString::Printf(TEXT("Queued bond formation: board orb %d -> held item %d (arm %d)"), BId, AId, HoldingArmIndex));
				}

				// DEBUG: Always log pending formations for debugging (commented out for production)
				// UE_LOG(LogAGSim, Warning, TEXT("Created pending bond formation: board orb %d -> held item %d (arm %d), total pending: %d"), 
				//	BId, AId, HoldingArmIndex, PendingBondFormations.Num());
			}
		}
		else if (bBIsHeld && !bAIsHeld)
		{
			// Find which arm is holding B
			int32 HoldingArmIndex = -1;
			// DEBUG: Log arm lookup (commented out for production)
			// UE_LOG(LogAGSim, Warning, TEXT("Looking for arm holding BId=%d"), BId);

			for (int32 ArmIndex = 0; ArmIndex < Arms.Num(); ArmIndex++)
			{
				const FArmState& Arm = Arms[ArmIndex];
				// UE_LOG(LogAGSim, Warning, TEXT("  Arm %d: bHolding=%s, Held.Id=%d"), 
				//	ArmIndex, Arm.bHolding ? TEXT("yes") : TEXT("no"), Arm.Held.Id);

				if (Arm.bHolding)
				{
					// Check if this arm is holding the item directly
					if (Arm.Held.Id == BId)
					{
						HoldingArmIndex = ArmIndex;
						// UE_LOG(LogAGSim, Warning, TEXT("  Found holding arm: %d (direct match)"), ArmIndex);
						break;
					}
					// Check if this arm is holding a cluster that contains the item
					else if (Arm.Held.bIsCluster)
					{
						// Look through the cluster's sub-orbs
						for (const FOrbData& SubOrb : Arm.Held.Orbs)
						{
							if (SubOrb.ItemId == BId)
							{
								HoldingArmIndex = ArmIndex;
								// UE_LOG(LogAGSim, Warning, TEXT("  Found holding arm: %d (cluster sub-orb match)"), ArmIndex);
								break;
							}
						}
						if (HoldingArmIndex >= 0) break;
					}
				}
			}

			if (HoldingArmIndex >= 0)
			{
				// Check if this board item is part of a dropped cluster
				// Look for other board items that might be part of the same dropped cluster
				TArray<FItem*> ClusterItems;
				TArray<FHex> ClusterHexes;

				// Start with the current board item
				FItem* CurrentItem = ItemMap.Find(AHex);
				if (CurrentItem && CurrentItem->Id == AId)
				{
					ClusterItems.Add(CurrentItem);
					ClusterHexes.Add(AHex);

					// Look for other board items that might be part of the same cluster
					// We'll check adjacent hexes for items that might be part of the dropped cluster
					for (int32 NeighborDir = 0; NeighborDir < 6; NeighborDir++)
					{
						FHex NeighborHex = HexUtil::Neighbor(AHex, (EHexDir)NeighborDir);
						FItem* NeighborItem = ItemMap.Find(NeighborHex);
						if (NeighborItem && NeighborItem->Id > 0)
						{
							// Check if this item was recently dropped (part of a cluster)
							// We can use the RecentlyDroppedByArm map to detect this
							if (RecentlyDroppedByArm.Contains(NeighborItem->Id))
							{
								ClusterItems.Add(NeighborItem);
								ClusterHexes.Add(NeighborHex);
							}
						}
					}
				}

				// Always use the pending bond formation system to ensure WaitForBond works correctly
				// This applies to both single items and clusters
				FPendingBondFormation Pending;
				Pending.HeldItemId = BId;
				Pending.BoardItemId = AId;
				Pending.BoardItemHex = AHex;
				Pending.ArmIndex = HoldingArmIndex;
				Pending.TimeRemaining = 1.0f; // Wait 1 tick for visual rotation to complete (tick-based delay)
				PendingBondFormations.Add(Pending);

				if (bDebugLogs && bDevDebugBonds && GEngine)
				{
					GEngine->AddOnScreenDebugMessage(-1, 1.0f, FColor::Cyan,
						FString::Printf(TEXT("Queued bond formation: board orb %d -> held item %d (arm %d)"), AId, BId, HoldingArmIndex));
				}
			}
		}
		else if (bAIsHeld && bBIsHeld)
		{
			// Both items are held - find which arms are holding them
			int32 ArmHoldingA = -1;
			int32 ArmHoldingB = -1;

			for (int32 ArmIdx = 0; ArmIdx < Arms.Num(); ArmIdx++)
			{
				const FArmState& Arm = Arms[ArmIdx];
				if (!Arm.bHolding) continue;

				if (Arm.Held.bIsCluster)
				{
					for (const FOrbData& Sub : Arm.Held.Orbs)
					{
						if (Sub.ItemId == AId) ArmHoldingA = ArmIdx;
						if (Sub.ItemId == BId) ArmHoldingB = ArmIdx;
					}
				}
				else
				{
					if (Arm.Held.Id == AId) ArmHoldingA = ArmIdx;
					if (Arm.Held.Id == BId) ArmHoldingB = ArmIdx;
				}
			}

			// If both arms found and they're different arms, create a pending merge
			if (ArmHoldingA >= 0 && ArmHoldingB >= 0 && ArmHoldingA != ArmHoldingB)
			{
				// Decide which arm receives and which donates
				// For now, the lower index arm receives (arbitrary but consistent)
				int32 ReceiverArm = FMath::Min(ArmHoldingA, ArmHoldingB);
				int32 DonorArm = FMath::Max(ArmHoldingA, ArmHoldingB);

				// Check if we already have a pending merge for these arms
				bool bAlreadyPending = false;
				for (const FPendingHeldMerge& Existing : PendingHeldMerges)
				{
					if ((Existing.ReceiverArmIndex == ReceiverArm && Existing.DonorArmIndex == DonorArm) ||
						(Existing.ReceiverArmIndex == DonorArm && Existing.DonorArmIndex == ReceiverArm))
					{
						bAlreadyPending = true;
						break;
					}
				}

				if (!bAlreadyPending)
				{
					FPendingHeldMerge PendingMerge;
					PendingMerge.ReceiverArmIndex = ReceiverArm;
					PendingMerge.DonorArmIndex = DonorArm;
					PendingMerge.ItemIdA = (ReceiverArm == ArmHoldingA) ? AId : BId;
					PendingMerge.ItemIdB = (DonorArm == ArmHoldingA) ? AId : BId;
					PendingHeldMerges.Add(PendingMerge);

					if (bDevDebugBonds)
					{
						UE_LOG(LogAGSim, Log, TEXT("Created pending held-to-held merge: Arm %d will receive from Arm %d"),
							ReceiverArm, DonorArm);
					}
				}
			}
		}
	}

	// Debug: Log bond tile processing results
	if (bDevDebugBonds)
	{
		UE_LOG(LogAGSim, Log, TEXT("Processed %d bond tiles"), BondTileCount);
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

	// Unbond processing (world or virtual held)
	for (const auto& KV : TileMap)
	{
		const FTile& T = KV.Value;
		if (T.Kind != ETileKind::Unbond) continue;
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

		// Only process if both items exist
		if (AId <= 0 || BId <= 0) continue;

		// Remove any existing bonds between these items
		BreakBondBetween(AId, BId);
	}

	// Process unbonds for held clusters whose arms have completed rotation
	for (const auto& KV : ArmsReadyForUnbond)
	{
		int32 ArmIndex = KV.Key;
		uint8 VisualFacing = KV.Value;

		if (ArmIndex < 0 || ArmIndex >= Arms.Num()) continue;

		FArmState& A = Arms[ArmIndex];
		if (!A.bHolding || !A.Held.bIsCluster) continue;

		// Skip if this arm was already processed by the old unbond function
		// (which runs before this new function and might have converted cluster to single orb)
		if (A.Held.Orbs.Num() <= 1) continue;

		// Calculate the arm's grip position using visual facing
		FHex GripHex = A.BaseHex;
		for (int i = 0; i < A.ArmLen; ++i)
			GripHex = HexUtil::Neighbor(GripHex, (EHexDir)VisualFacing);

		// Check if the arm is positioned over an unbond tile before processing unbonds
		const FTile* UnbondTile = TileMap.Find(GripHex);
		if (!UnbondTile || UnbondTile->Kind != ETileKind::Unbond)
		{
			// Arm is not positioned over an unbond tile, skip unbond processing
			continue;
		}

		// Calculate rotation for held cluster using visual facing
		const uint8 cur = VisualFacing % 6;
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

		// For held clusters, always disconnect the orb that's not at the grip position
		// (dQ=0, dR=0), regardless of the unbond tile's rotation
		for (int32 SubIndex = A.Held.Orbs.Num() - 1; SubIndex >= 0; SubIndex--)
		{
			const FOrbData& SubA = A.Held.Orbs[SubIndex];
			if (SubA.ItemId <= 0) continue;

			// Skip the grabbed orb (the one at the grip position)
			if (SubA.dQ == 0 && SubA.dR == 0) continue;

			// Check if this orb has any bonds to other orbs in the cluster
			bool bHasBondsInCluster = false;
			if (const TArray<int32>* BondList = ItemToBonds.Find(SubA.ItemId))
			{
				for (int32 BondIdx : *BondList)
				{
					if (!Bonds.IsValidIndex(BondIdx)) continue;
					const FBond& Bond = Bonds[BondIdx];
					if (!Bond.bActive) continue;

					// Check if this bond connects to another orb in the same cluster
					int32 OtherItemId = (Bond.ItemA == SubA.ItemId) ? Bond.ItemB : Bond.ItemA;
					for (const FOrbData& OtherSub : A.Held.Orbs)
					{
						if (OtherSub.ItemId == OtherItemId)
						{
							bHasBondsInCluster = true;
							break;
						}
					}
					if (bHasBondsInCluster) break;
				}
			}

			// If this orb has no bonds to other orbs in the cluster, remove it
			if (!bHasBondsInCluster)
			{
				// Calculate the world position of this orb
				FHex LocalA(SubA.dQ, SubA.dR);
				FHex RotA = Rot60(LocalA, ccw);
				FHex WorldA(GripHex.Q + RotA.Q, GripHex.R + RotA.R);

				// Create a single orb item for the unbonded orb
				FItem UnbondedOrb;
				UnbondedOrb.bIsCluster = false;
				UnbondedOrb.Id = SubA.ItemId;
				UnbondedOrb.Single.Type = (EOrbType)SubA.Type;
				UnbondedOrb.Single.Color = (EOrbColor)SubA.Color;

				// Place the unbonded orb immediately so it can be grabbed right away
				TryPutItem(WorldA, UnbondedOrb);

				// Also create an animated orb for visual effect only
				// Note: The orb is already in ItemMap, so this is just for visuals
				//FAnimatedOrb AnimatedOrb;
				//AnimatedOrb.Item = UnbondedOrb;
				//AnimatedOrb.Hex = WorldA;
				//AnimatedOrb.AnimationTime = 0.f;
				//AnimatedOrb.AnimationDuration = 0.25f; // Quick visual effect
				//AnimatedOrb.bIsInput = false; // This is an unbonded orb, not an input
				//AnimatedOrb.bActive = true;
				//AnimatedOrbs.Add(AnimatedOrb);

				// Remove the orb from the held cluster
				A.Held.Orbs.RemoveAt(SubIndex);

				// Remove from HeldItemLastHex since this orb is no longer held
				HeldItemLastHex.Remove(SubA.ItemId);

				// If this was the last orb in the cluster, convert to single orb
				if (A.Held.Orbs.Num() == 1)
				{
					// Convert the remaining single orb to a single orb item
					const FOrbData& RemainingOrb = A.Held.Orbs[0];
					A.Held.bIsCluster = false;
					A.Held.Orbs.Empty();
					A.Held.Single.Type = (EOrbType)RemainingOrb.Type;
					A.Held.Single.Color = (EOrbColor)RemainingOrb.Color;
					A.Held.Id = RemainingOrb.ItemId;
					A.Held.GrabFacing = -1; // Clear grab facing for single orbs
				}
				else if (A.Held.Orbs.Num() == 0)
				{
					// This should never happen, but just in case
					A.bHolding = false;
					A.Held = FItem();
				}

				if (bDebugLogs && bDevDebugBonds && GEngine)
				{
					GEngine->AddOnScreenDebugMessage(-1, 1.0f, FColor::Red,
						FString::Printf(TEXT("Unbond: Orb %d removed from held cluster and placed at (%d,%d)"),
							SubA.ItemId, WorldA.Q, WorldA.R));
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

				break; // Only remove one orb per arm per tick
			}
		}

		// Special case: Check if the unbond tile was triggered on the grabbed orb itself (dQ=0, dR=0)
		// In this case, we need to place the grabbed orb on the board and continue holding the remaining cluster
		for (int32 SubIndex = A.Held.Orbs.Num() - 1; SubIndex >= 0; SubIndex--)
		{
			const FOrbData& SubA = A.Held.Orbs[SubIndex];
			if (SubA.ItemId <= 0) continue;

			// Only process the grabbed orb (the one at the grip position)
			if (SubA.dQ != 0 || SubA.dR != 0) continue;

			// Check if this grabbed orb has any bonds to other orbs in the cluster
			bool bHasBondsInCluster = false;
			if (const TArray<int32>* BondList = ItemToBonds.Find(SubA.ItemId))
			{
				for (int32 BondIdx : *BondList)
				{
					if (!Bonds.IsValidIndex(BondIdx)) continue;
					const FBond& Bond = Bonds[BondIdx];
					if (!Bond.bActive) continue;

					// Check if this bond connects to another orb in the same cluster
					int32 OtherItemId = (Bond.ItemA == SubA.ItemId) ? Bond.ItemB : Bond.ItemA;
					for (const FOrbData& OtherSub : A.Held.Orbs)
					{
						if (OtherSub.ItemId == OtherItemId)
						{
							bHasBondsInCluster = true;
							break;
						}
					}
					if (bHasBondsInCluster) break;
				}
			}

			// If the grabbed orb has no bonds to other orbs in the cluster, place it on the board
			// and continue holding the remaining cluster
			if (!bHasBondsInCluster && A.Held.Orbs.Num() > 1)
			{
				// Calculate the world position of the grabbed orb (it's at the grip position)
				FHex WorldA = GripHex;

				// Create a single orb item for the grabbed orb
				FItem UnbondedOrb;
				UnbondedOrb.bIsCluster = false;
				UnbondedOrb.Id = SubA.ItemId;
				UnbondedOrb.Single.Type = (EOrbType)SubA.Type;
				UnbondedOrb.Single.Color = (EOrbColor)SubA.Color;

				// Place the unbonded orb immediately so it can be grabbed right away
				TryPutItem(WorldA, UnbondedOrb);

				// Also create an animated orb for visual effect only
				// Note: The orb is already in ItemMap, so this is just for visuals
				//FAnimatedOrb AnimatedOrb;
				//AnimatedOrb.Item = UnbondedOrb;
				//AnimatedOrb.Hex = WorldA;
				//AnimatedOrb.AnimationTime = 0.f;
				//AnimatedOrb.AnimationDuration = 0.25f; // Quick visual effect
				//AnimatedOrb.bIsInput = false; // This is an unbonded orb, not an input
				//AnimatedOrb.bActive = true;
				//AnimatedOrbs.Add(AnimatedOrb);

				// Remove the grabbed orb from the held cluster
				A.Held.Orbs.RemoveAt(SubIndex);

				// Remove from HeldItemLastHex since this orb is no longer held
				HeldItemLastHex.Remove(SubA.ItemId);

				// After removing the grabbed orb, we need to pick a new grabbed orb from the remaining cluster
				if (A.Held.Orbs.Num() > 0)
				{
					// Find the first remaining orb to become the new grabbed orb
					FOrbData& NewGrabbedOrb = A.Held.Orbs[0];

					// Adjust all other orbs' positions relative to the new grabbed orb
					for (FOrbData& Orb : A.Held.Orbs)
					{
						Orb.dQ -= NewGrabbedOrb.dQ;
						Orb.dR -= NewGrabbedOrb.dR;
					}

					// The new grabbed orb should be at (0,0)
					NewGrabbedOrb.dQ = 0;
					NewGrabbedOrb.dR = 0;

					// Update the held item's ID to match the new grabbed orb
					A.Held.Id = NewGrabbedOrb.ItemId;
				}

				if (bDebugLogs && bDevDebugBonds && GEngine)
				{
					GEngine->AddOnScreenDebugMessage(-1, 1.0f, FColor::Red,
						FString::Printf(TEXT("Unbond: Grabbed orb %d placed at (%d,%d). Remaining cluster orbs: %d"),
							SubA.ItemId, WorldA.Q, WorldA.R, A.Held.Orbs.Num()));
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

				break; // Only process one grabbed orb per arm per tick
			}
		}
	}

	// Clear the arms ready for unbond processing
	ArmsReadyForUnbond.Empty();

	// Update previous state for next frame's rotation completion detection
	for (int32 ArmIndex = 0; ArmIndex < Arms.Num(); ArmIndex++)
	{
		FArmState& S = Arms[ArmIndex];
		// S.PrevFacing = S.Facing;
		S.PrevArmLen = S.ArmLen;
		S.PrevTrackIdx = S.TrackIdx;
	}
}