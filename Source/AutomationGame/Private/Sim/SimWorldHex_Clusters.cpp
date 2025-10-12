#include "Sim/SimWorldHex.h"
#include "Actors/BasicArm.h"
#include "Rendering/OrbRenderer.h"
#include "EngineUtils.h"
#include "Engine/Engine.h"

// Cluster management functions
void USimWorldHex::AddBoardOrbToHeldCluster(int32 HeldOrbId, int32 BoardOrbId, const FHex& BoardOrbHex)
{
	// Find the arm holding the cluster that contains HeldOrbId
	for (int32 ArmIndex = 0; ArmIndex < Arms.Num(); ++ArmIndex)
	{
		FArmState& Arm = Arms[ArmIndex];
		if (!Arm.bHolding || !Arm.Held.bIsCluster) continue;

		bool bContainsHeldOrb = false;
		for (const FOrbData& Sub : Arm.Held.Orbs)
		{
			if (Sub.ItemId == HeldOrbId) { bContainsHeldOrb = true; break; }
		}
		if (!bContainsHeldOrb) continue;

		// Delegate to the sim-space version using the cluster's Held.Id
		const int32 ClusterId = Arm.Held.Id;
		AddBoardOrbToHeldItem(ClusterId, BoardOrbId, BoardOrbHex);

		if (bDevDebugBonds && GEngine)
		{
			GEngine->AddOnScreenDebugMessage(
				-1, 1.0f, FColor::Orange,
				FString::Printf(TEXT("Board orb %d added to held cluster %d via sim-space attach"), BoardOrbId, ClusterId));
		}
		return; // Done
	}

	// Not found: no arm currently holding the given held orb id
	if (bDevDebugBonds && GEngine)
	{
		GEngine->AddOnScreenDebugMessage(-1, 1.5f, FColor::Red,
			FString::Printf(TEXT("AddBoardOrbToHeldCluster: held orb %d not found in any held cluster"), HeldOrbId));
	}
}

void USimWorldHex::AddBoardOrbToHeldItem(int32 HeldOrbId, int32 BoardOrbId, const FHex& BoardOrbHex)
{
	// Find the arm holding the item containing HeldOrbId
	for (int32 ArmIndex = 0; ArmIndex < Arms.Num(); ArmIndex++)
	{
		FArmState& Arm = Arms[ArmIndex];
		if (!Arm.bHolding) continue;

		// Check if this arm is holding the specified orb
		bool bContainsHeldOrb = false;
		if (Arm.Held.bIsCluster)
		{
			for (const FOrbData& Sub : Arm.Held.Orbs)
			{
				if (Sub.ItemId == HeldOrbId)
				{
					bContainsHeldOrb = true;
					break;
				}
			}
		}
		else if (Arm.Held.Id == HeldOrbId)
		{
			bContainsHeldOrb = true;
		}

		if (!bContainsHeldOrb) continue;

		// Find the board orb
		FItem* BoardItem = ItemMap.Find(BoardOrbHex);
		if (!BoardItem || BoardItem->Id != BoardOrbId) continue;

		// Use SIM-committed state for grip
		const FHex GripHex = GetArmGripHex_Sim(ArmIndex);

		if (Arm.Held.bIsCluster)
		{
			// Existing cluster - add the board orb directly using SIM rotation (avoid recursion)

			// ccw steps mapping local->world; for world->local we rotate CW by same amount
			const uint8 CCWSteps = GetHeldCCW_Steps_Sim(ArmIndex);

			// Convert world offset to cluster-local dQ/dR using SIM frame
			const FHex WorldOffset(BoardOrbHex.Q - GripHex.Q, BoardOrbHex.R - GripHex.R);
			const FHex Local = Rot60CW(WorldOffset, CCWSteps);

			FOrbData BoardOrb;
			BoardOrb.ItemId = BoardOrbId;
			BoardOrb.Type = (uint8)BoardItem->Single.Type;
			BoardOrb.Color = (uint8)BoardItem->Single.Color;
			BoardOrb.dQ = (int8)Local.Q;
			BoardOrb.dR = (int8)Local.R;
			BoardOrb.Offset = FVector2D::ZeroVector;

			// Update caches BEFORE removing from the board to avoid flicker
			HeldItemLastHex.FindOrAdd(Arm.Held.Id, GripHex);
			HeldItemLastHex.FindOrAdd(BoardOrbId, BoardOrbHex);

			// Remove from board and append to cluster
			ItemMap.Remove(BoardOrbHex);
			Arm.Held.Orbs.Add(BoardOrb);

			if (bDevDebugBonds && GEngine)
			{
				GEngine->AddOnScreenDebugMessage(
					-1, 1.0f, FColor::Orange,
					FString::Printf(TEXT("Added board orb %d to held cluster %d (SIM frame): dQ=%d dR=%d"),
						BoardOrbId, Arm.Held.Id, BoardOrb.dQ, BoardOrb.dR));
			}

			// Force visuals to refresh immediately
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

				for (TActorIterator<AActor> ActorItr(World); ActorItr; ++ActorItr)
				{
					if (UOrbRenderer* Renderer = (*ActorItr)->FindComponentByClass<UOrbRenderer>())
					{
						Renderer->RebuildCurrPos();
						break;
					}
				}
			}
		}
		else
		{
			// Single held orb - convert to cluster and add the board orb
			FItem NewCluster;
			NewCluster.bIsCluster = true;
			NewCluster.Id = Arm.Held.Id;

			// GrabFacing must reflect the SIM-committed facing now
			const uint8 SimFace = (SimFacingByArm.Contains(ArmIndex) ? SimFacingByArm[ArmIndex] : (uint8)Arm.Facing);
			NewCluster.GrabFacing = (int8)SimFace;

			// NEW: seed SIM held orientation immediately so SIM conversions this tick are correct
			SimGrabFacingByArm.Add(ArmIndex, (int8)SimFace);

			// Add the original single orb as center
			FOrbData OriginalOrb;
			OriginalOrb.ItemId = Arm.Held.Id;
			OriginalOrb.Type = (uint8)Arm.Held.Single.Type;
			OriginalOrb.Color = (uint8)Arm.Held.Single.Color;
			OriginalOrb.dQ = 0;
			OriginalOrb.dR = 0;
			OriginalOrb.Offset = FVector2D::ZeroVector;
			NewCluster.Orbs.Add(OriginalOrb);

			// Since GrabFacing == SimFacing, local == world offset at attach time
			const FHex BoardOrbOffset(BoardOrbHex.Q - GripHex.Q, BoardOrbHex.R - GripHex.R);

			FOrbData BoardOrb;
			BoardOrb.ItemId = BoardOrbId;
			BoardOrb.Type = (uint8)BoardItem->Single.Type;
			BoardOrb.Color = (uint8)BoardItem->Single.Color;
			BoardOrb.dQ = (int8)BoardOrbOffset.Q;
			BoardOrb.dR = (int8)BoardOrbOffset.R;
			BoardOrb.Offset = FVector2D::ZeroVector;
			NewCluster.Orbs.Add(BoardOrb);

			// Update held to new cluster
			Arm.Held = NewCluster;

			// Update HeldItemLastHex before removing from ItemMap
			HeldItemLastHex.FindOrAdd(Arm.Held.Id, GripHex);
			HeldItemLastHex.FindOrAdd(BoardOrbId, BoardOrbHex);

			// Remove the board orb from the board
			ItemMap.Remove(BoardOrbHex);

			if (bDevDebugBonds && GEngine)
			{
				GEngine->AddOnScreenDebugMessage(-1, 1.0f, FColor::Orange,
					FString::Printf(TEXT("Converted single orb %d to cluster, added board orb %d (SIM frame): dQ=%d dR=%d"),
						Arm.Held.Id, BoardOrbId, BoardOrb.dQ, BoardOrb.dR));
			}

			// Force visuals to refresh immediately
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

				for (TActorIterator<AActor> ActorItr(World); ActorItr; ++ActorItr)
				{
					if (UOrbRenderer* Renderer = (*ActorItr)->FindComponentByClass<UOrbRenderer>())
					{
						Renderer->RebuildCurrPos();
						break;
					}
				}
			}
		}

		break; // Only add to the first matching arm
	}
}

void USimWorldHex::CheckAndRemoveDisconnectedOrbsFromHeldClusters()
{
	// Check each held cluster for disconnected orbs
	for (int32 ArmIndex = 0; ArmIndex < Arms.Num(); ArmIndex++)
	{
		FArmState& Arm = Arms[ArmIndex];
		if (!Arm.bHolding || !Arm.Held.bIsCluster || Arm.Held.Orbs.Num() <= 1) continue;

		// Use SIM-committed state (not visual) to align with bond creation/breaking
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

		// Check each orb in the cluster to see if it's still connected
		for (int32 SubIndex = Arm.Held.Orbs.Num() - 1; SubIndex >= 0; SubIndex--)
		{
			const FOrbData& Sub = Arm.Held.Orbs[SubIndex];
			if (Sub.ItemId <= 0) continue;

			// Check if this orb has any bonds to other orbs in the cluster
			bool bHasBondsInCluster = false;
			if (const TArray<int32>* BondList = ItemToBonds.Find(Sub.ItemId))
			{
				for (int32 BondIdx : *BondList)
				{
					if (!Bonds.IsValidIndex(BondIdx)) continue;
					const FBond& Bond = Bonds[BondIdx];
					if (!Bond.bActive) continue;

					// Check if this bond connects to another orb in the same cluster
					const int32 OtherItemId = (Bond.ItemA == Sub.ItemId) ? Bond.ItemB : Bond.ItemA;
					for (const FOrbData& OtherSub : Arm.Held.Orbs)
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
				// Check if this is the held orb (the one at dQ=0, dR=0)
				if (Sub.dQ == 0 && Sub.dR == 0)
				{
					// Disconnect the cluster: place all other orbs; keep holding center as single
					for (int32 OtherIndex = Arm.Held.Orbs.Num() - 1; OtherIndex >= 0; OtherIndex--)
					{
						const FOrbData& OtherSub = Arm.Held.Orbs[OtherIndex];
						if (OtherSub.ItemId <= 0) continue;
						if (OtherSub.dQ == 0 && OtherSub.dR == 0) continue;

						FHex OtherLocal(OtherSub.dQ, OtherSub.dR);
						FHex OtherRot = Rot60(OtherLocal, ccw);
						FHex OtherWorldPos(GripHex.Q + OtherRot.Q, GripHex.R + OtherRot.R);

						FItem DisconnectedOrb;
						DisconnectedOrb.bIsCluster = false;
						DisconnectedOrb.Id = OtherSub.ItemId;
						DisconnectedOrb.Single.Type = (EOrbType)OtherSub.Type;
						DisconnectedOrb.Single.Color = (EOrbColor)OtherSub.Color;

						ItemMap.Add(OtherWorldPos, DisconnectedOrb);
						NewlyPlacedItems.Add(OtherSub.ItemId);
						RecentlyDroppedByArm.Add(OtherSub.ItemId, ArmIndex);
						HeldItemLastHex.Remove(OtherSub.ItemId);

						if (bDebugLogs && bDevDebugBonds && GEngine)
						{
							GEngine->AddOnScreenDebugMessage(-1, 1.0f, FColor::Red,
								FString::Printf(TEXT("Disconnected cluster orb %d placed at (%d,%d)"),
									OtherSub.ItemId, OtherWorldPos.Q, OtherWorldPos.R));
						}
					}

					// Convert to single, clear sim-held orientation immediately
					const FOrbData HeldOrb = Sub;
					Arm.Held.bIsCluster = false;
					Arm.Held.Orbs.Empty();
					Arm.Held.Single.Type = (EOrbType)HeldOrb.Type;
					Arm.Held.Single.Color = (EOrbColor)HeldOrb.Color;
					Arm.Held.Id = HeldOrb.ItemId;
					Arm.Held.GrabFacing = -1;
					SimGrabFacingByArm.Add(ArmIndex, int8(-1));

					if (bDebugLogs && bDevDebugBonds && GEngine)
					{
						GEngine->AddOnScreenDebugMessage(-1, 1.0f, FColor::Yellow,
							FString::Printf(TEXT("Held orb %d converted to single after cluster disconnection"), HeldOrb.ItemId));
					}

					// Force visual refresh
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

					break; // done with this arm
				}
				else
				{
					// Drop the disconnected non-center orb
					FHex Local(Sub.dQ, Sub.dR);
					FHex Rot = Rot60(Local, ccw);
					FHex WorldPos(GripHex.Q + Rot.Q, GripHex.R + Rot.R);

					FItem DisconnectedOrb;
					DisconnectedOrb.bIsCluster = false;
					DisconnectedOrb.Id = Sub.ItemId;
					DisconnectedOrb.Single.Type = (EOrbType)Sub.Type;
					DisconnectedOrb.Single.Color = (EOrbColor)Sub.Color;

					ItemMap.Add(WorldPos, DisconnectedOrb);
					NewlyPlacedItems.Add(Sub.ItemId);
					RecentlyDroppedByArm.Add(Sub.ItemId, ArmIndex);

					const bool bWillBeLastOrb = (Arm.Held.Orbs.Num() == 1);
					FOrbData RemainingOrb;
					if (bWillBeLastOrb)
					{
						RemainingOrb = Arm.Held.Orbs[0];
					}

					Arm.Held.Orbs.RemoveAt(SubIndex);
					HeldItemLastHex.Remove(Sub.ItemId);

					if (bDebugLogs && bDevDebugBonds && GEngine)
					{
						GEngine->AddOnScreenDebugMessage(-1, 1.0f, FColor::Red,
							FString::Printf(TEXT("Disconnected orb %d placed at (%d,%d)"),
								Sub.ItemId, WorldPos.Q, WorldPos.R));
					}

					if (bWillBeLastOrb)
					{
						Arm.Held.bIsCluster = false;
						Arm.Held.Orbs.Empty();
						Arm.Held.Single.Type = (EOrbType)RemainingOrb.Type;
						Arm.Held.Single.Color = (EOrbColor)RemainingOrb.Color;
						Arm.Held.Id = RemainingOrb.ItemId;
						Arm.Held.GrabFacing = -1;
						SimGrabFacingByArm.Add(ArmIndex, int8(-1));
						break;
					}
					else if (Arm.Held.Orbs.Num() == 0)
					{
						Arm.bHolding = false;
						Arm.Held = FItem();
						SimGrabFacingByArm.Add(ArmIndex, int8(-1));
						break;
					}
				}
			}
		}
	}
}

void USimWorldHex::ProcessHeldToHeldMerge(int32 ReceiverArmIndex, int32 DonorArmIndex)
{
	if (ReceiverArmIndex < 0 || ReceiverArmIndex >= Arms.Num() ||
		DonorArmIndex < 0 || DonorArmIndex >= Arms.Num())
	{
		return;
	}

	FArmState& ReceiverArm = Arms[ReceiverArmIndex];
	FArmState& DonorArm = Arms[DonorArmIndex];

	if (!ReceiverArm.bHolding || !DonorArm.bHolding)
	{
		return;
	}

	// Ensure both are clusters
	if (!ReceiverArm.Held.bIsCluster)
	{
		FItem NewCluster;
		NewCluster.bIsCluster = true;
		NewCluster.Id = ReceiverArm.Held.Id;
		// Use SIM-committed facing
		const uint8 SimFace = (SimFacingByArm.Contains(ReceiverArmIndex) ? SimFacingByArm[ReceiverArmIndex] : (uint8)ReceiverArm.Facing);
		NewCluster.GrabFacing = (int8)SimFace;

		FOrbData SingleOrb;
		SingleOrb.Type = (uint8)ReceiverArm.Held.Single.Type;
		SingleOrb.Color = (uint8)ReceiverArm.Held.Single.Color;
		SingleOrb.ItemId = ReceiverArm.Held.Id;
		SingleOrb.dQ = 0;
		SingleOrb.dR = 0;
		SingleOrb.Offset = FVector2D::ZeroVector;
		NewCluster.Orbs.Add(SingleOrb);

		ReceiverArm.Held = NewCluster;
	}

	if (!DonorArm.Held.bIsCluster)
	{
		FItem NewCluster;
		NewCluster.bIsCluster = true;
		NewCluster.Id = DonorArm.Held.Id;
		const uint8 SimFace = (SimFacingByArm.Contains(DonorArmIndex) ? SimFacingByArm[DonorArmIndex] : (uint8)DonorArm.Facing);
		NewCluster.GrabFacing = (int8)SimFace;

		FOrbData SingleOrb;
		SingleOrb.Type = (uint8)DonorArm.Held.Single.Type;
		SingleOrb.Color = (uint8)DonorArm.Held.Single.Color;
		SingleOrb.ItemId = DonorArm.Held.Id;
		SingleOrb.dQ = 0;
		SingleOrb.dR = 0;
		SingleOrb.Offset = FVector2D::ZeroVector;
		NewCluster.Orbs.Add(SingleOrb);

		DonorArm.Held = NewCluster;
	}

	// Use SIM grip positions
	const FHex ReceiverGrip = GetArmGripHex_Sim(ReceiverArmIndex);
	const FHex DonorGrip = GetArmGripHex_Sim(DonorArmIndex);

	// Offset from receiver's grip to donor's grip in world axial
	const FHex GripOffset(DonorGrip.Q - ReceiverGrip.Q, DonorGrip.R - ReceiverGrip.R);

	// Donor local->world CCW steps (SIM)
	const uint8 DonorCCWSteps = GetHeldCCW_Steps_Sim(DonorArmIndex);

	// Add all orbs from donor to receiver, adjusting their positions
	int32 DonorOrbCount = DonorArm.Held.Orbs.Num();
	for (const FOrbData& DonorOrb : DonorArm.Held.Orbs)
	{
		FOrbData NewOrb = DonorOrb;

		// donor local -> world (relative to donor grip) using SIM rotation
		const FHex DonorLocal(DonorOrb.dQ, DonorOrb.dR);
		const FHex DonorWorldRel = Rot60CCW(DonorLocal, DonorCCWSteps);

		// Re-express in receiver local coordinates (still in the receiver cluster's current SIM frame)
		NewOrb.dQ = GripOffset.Q + DonorWorldRel.Q;
		NewOrb.dR = GripOffset.R + DonorWorldRel.R;

		NewOrb.Offset = HexCoord::AxialToXY(FHexAxial(NewOrb.dQ, NewOrb.dR), 1.f, GridOrientation);

		ReceiverArm.Held.Orbs.Add(NewOrb);
	}

	if (bDebugLogs && bDevDebugBonds && GEngine)
	{
		GEngine->AddOnScreenDebugMessage(-1, 2.0f, FColor::Green,
			FString::Printf(TEXT("ProcessHeldToHeldMerge (SIM): Arm %d absorbed %d orbs from Arm %d"),
				ReceiverArmIndex, DonorOrbCount, DonorArmIndex));
	}

	// Clear donor's held item
	DonorArm.bHolding = false;
	DonorArm.Held = FItem();
	DonorArm.bWaitingForBond = false;
	DonorArm.WaitForBondInitialCount = 0;
	DonorArm.PrevHeldOrbCount = 0;
}
