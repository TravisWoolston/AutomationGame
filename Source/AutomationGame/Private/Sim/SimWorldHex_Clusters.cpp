#include "Sim/SimWorldHex.h"
#include "Actors/BasicArm.h"
#include "EngineUtils.h"

// Cluster management functions
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
			// Check if this cluster contains the held orb
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
			// Single held orb matches
			bContainsHeldOrb = true;
		}
		
		if (!bContainsHeldOrb) continue;
		
		// Find the board orb
		FItem* BoardItem = ItemMap.Find(BoardOrbHex);
		if (!BoardItem || BoardItem->Id != BoardOrbId) continue;
		
		// Calculate the arm's current grip position
		FHex GripHex = Arm.BaseHex;
		for (int i = 0; i < Arm.ArmLen; ++i)
			GripHex = HexUtil::Neighbor(GripHex, Arm.Facing);
		
		if (Arm.Held.bIsCluster)
		{
			// Existing cluster - add the board orb to it
			AddBoardOrbToHeldCluster(HeldOrbId, BoardOrbId, BoardOrbHex);
		}
		else
		{
			// Single held orb - convert to cluster and add the board orb
			// Create a new cluster from the single held orb
			FItem NewCluster;
			NewCluster.bIsCluster = true;
			NewCluster.Id = Arm.Held.Id; // Use the single orb's ID as the cluster ID
			NewCluster.GrabFacing = (int8)Arm.Facing; // Current arm facing as grab facing
			
			// Add the original single orb as the first sub-orb (at center)
			FOrbData OriginalOrb;
			OriginalOrb.ItemId = Arm.Held.Id;
			OriginalOrb.Type = (uint8)Arm.Held.Single.Type;
			OriginalOrb.Color = (uint8)Arm.Held.Single.Color;
			OriginalOrb.dQ = 0; // At center
			OriginalOrb.dR = 0; // At center
			OriginalOrb.Offset = FVector2D::ZeroVector;
			NewCluster.Orbs.Add(OriginalOrb);
			
			// Calculate the board orb's position relative to the grip
			FHex BoardOrbOffset(
				BoardOrbHex.Q - GripHex.Q,
				BoardOrbHex.R - GripHex.R
			);
			
			// Add the board orb as a sub-orb
			FOrbData BoardOrb;
			BoardOrb.ItemId = BoardOrbId;
			BoardOrb.Type = (uint8)BoardItem->Single.Type;
			BoardOrb.Color = (uint8)BoardItem->Single.Color;
			BoardOrb.dQ = (int8)BoardOrbOffset.Q;
			BoardOrb.dR = (int8)BoardOrbOffset.R;
			BoardOrb.Offset = FVector2D::ZeroVector; // Let the arm calculate this from dQ/dR
			NewCluster.Orbs.Add(BoardOrb);
			
			// Update the arm's held item to the new cluster
			Arm.Held = NewCluster;
			
			// Remove the board orb from the board
			ItemMap.Remove(BoardOrbHex);
			
			// Update HeldItemLastHex for both orbs
			HeldItemLastHex.FindOrAdd(Arm.Held.Id, GripHex); // Original orb at grip
			HeldItemLastHex.FindOrAdd(BoardOrbId, BoardOrbHex); // Board orb at its original position
			
			if (bDevDebugBonds && GEngine)
			{
				GEngine->AddOnScreenDebugMessage(-1, 1.0f, FColor::Orange,
					FString::Printf(TEXT("Converted single orb %d to cluster, added board orb %d: dQ=%d dR=%d"), 
						Arm.Held.Id, BoardOrbId, BoardOrb.dQ, BoardOrb.dR));
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

		// Get the arm's visual rotation from the actor to ensure correct positioning
		uint8 VisualFacing = (uint8)Arm.Facing; // Default to simulation facing
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

		// Calculate the arm's grip position using visual facing
		FHex GripHex = Arm.BaseHex;
		for (int i = 0; i < Arm.ArmLen; ++i)
			GripHex = HexUtil::Neighbor(GripHex, (EHexDir)VisualFacing);

		// Calculate rotation for held cluster using visual facing
		const uint8 cur = VisualFacing % 6;
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
					int32 OtherItemId = (Bond.ItemA == Sub.ItemId) ? Bond.ItemB : Bond.ItemA;
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
					// This is the held orb - we should disconnect the cluster, not the held orb
					// Place all remaining orbs in the cluster on the board and keep holding the held orb as a single orb
					
					// First, place all other orbs in the cluster on the board
					for (int32 OtherIndex = Arm.Held.Orbs.Num() - 1; OtherIndex >= 0; OtherIndex--)
					{
						const FOrbData& OtherSub = Arm.Held.Orbs[OtherIndex];
						if (OtherSub.ItemId <= 0) continue;
						
						// Skip the held orb itself (dQ=0, dR=0)
						if (OtherSub.dQ == 0 && OtherSub.dR == 0) continue;
						
						// Calculate the world position of this other orb
						FHex OtherLocal(OtherSub.dQ, OtherSub.dR);
						FHex OtherRot = Rot60(OtherLocal, ccw);
						FHex OtherWorldPos(GripHex.Q + OtherRot.Q, GripHex.R + OtherRot.R);

						// Create a single orb item for the disconnected orb
						FItem DisconnectedOrb;
						DisconnectedOrb.bIsCluster = false;
						DisconnectedOrb.Id = OtherSub.ItemId;
						DisconnectedOrb.Single.Type = (EOrbType)OtherSub.Type;
						DisconnectedOrb.Single.Color = (EOrbColor)OtherSub.Color;

						// Place the orb immediately in the simulation
						ItemMap.Add(OtherWorldPos, DisconnectedOrb);
						
						// Mark this item as newly placed so it waits a tick before being moved by belts
						NewlyPlacedItems.Add(OtherSub.ItemId);
						
						// Mark this orb as recently dropped by this arm to prevent immediate re-grabbing
						RecentlyDroppedByArm.Add(OtherSub.ItemId, ArmIndex);
						
						// Remove from HeldItemLastHex since this orb is no longer held
						HeldItemLastHex.Remove(OtherSub.ItemId);
						
						if (bDebugLogs && bDevDebugBonds && GEngine)
						{
							GEngine->AddOnScreenDebugMessage(-1, 1.0f, FColor::Red,
								FString::Printf(TEXT("Disconnected cluster orb %d removed from held cluster and placed at (%d,%d)"), 
									OtherSub.ItemId, OtherWorldPos.Q, OtherWorldPos.R));
						}
					}
					
					// Convert the held cluster to a single orb (keeping the held orb)
					FOrbData HeldOrb = Sub; // The held orb (dQ=0, dR=0)
					Arm.Held.bIsCluster = false;
					Arm.Held.Orbs.Empty();
					Arm.Held.Single.Type = (EOrbType)HeldOrb.Type;
					Arm.Held.Single.Color = (EOrbColor)HeldOrb.Color;
					Arm.Held.Id = HeldOrb.ItemId;
					Arm.Held.GrabFacing = -1; // Clear grab facing for single orbs
					
					if (bDebugLogs && bDevDebugBonds && GEngine)
					{
						GEngine->AddOnScreenDebugMessage(-1, 1.0f, FColor::Yellow,
							FString::Printf(TEXT("Held orb %d converted to single orb after cluster disconnection"), HeldOrb.ItemId));
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
					
					break; // Exit the orb loop since we've processed the entire cluster
				}
				else
				{
					// This is not the held orb - proceed with normal disconnection
					// Calculate the world position of this orb
					FHex Local(Sub.dQ, Sub.dR);
					FHex Rot = Rot60(Local, ccw);
					FHex WorldPos(GripHex.Q + Rot.Q, GripHex.R + Rot.R);

					// Create a single orb item for the disconnected orb
					FItem DisconnectedOrb;
					DisconnectedOrb.bIsCluster = false;
					DisconnectedOrb.Id = Sub.ItemId;
					DisconnectedOrb.Single.Type = (EOrbType)Sub.Type;
					DisconnectedOrb.Single.Color = (EOrbColor)Sub.Color;

					// Place the orb immediately in the simulation
					ItemMap.Add(WorldPos, DisconnectedOrb);
					
					// Mark this item as newly placed so it waits a tick before being moved by belts
					NewlyPlacedItems.Add(Sub.ItemId);
					
					// Mark this orb as recently dropped by this arm to prevent immediate re-grabbing
					RecentlyDroppedByArm.Add(Sub.ItemId, ArmIndex);

					// Check if this will be the last orb in the cluster after removal
					bool bWillBeLastOrb = (Arm.Held.Orbs.Num() == 1);
					FOrbData RemainingOrb;
					if (bWillBeLastOrb)
					{
						// Get the remaining orb's data before removing the disconnected one
						RemainingOrb = Arm.Held.Orbs[0];
					}

					// Remove the orb from the held cluster
					Arm.Held.Orbs.RemoveAt(SubIndex);

					// Remove from HeldItemLastHex since this orb is no longer held
					HeldItemLastHex.Remove(Sub.ItemId);

					if (bDebugLogs && bDevDebugBonds && GEngine)
					{
						GEngine->AddOnScreenDebugMessage(-1, 1.0f, FColor::Red,
							FString::Printf(TEXT("Disconnected orb %d removed from held cluster and placed at (%d,%d)"), 
								Sub.ItemId, WorldPos.Q, WorldPos.R));
					}

					// If this was the last orb in the cluster, convert to single orb
					if (bWillBeLastOrb)
					{
						// Convert the remaining single orb to a single orb item
						Arm.Held.bIsCluster = false;
						Arm.Held.Orbs.Empty();
						Arm.Held.Single.Type = (EOrbType)RemainingOrb.Type;
						Arm.Held.Single.Color = (EOrbColor)RemainingOrb.Color;
						Arm.Held.Id = RemainingOrb.ItemId;
						Arm.Held.GrabFacing = -1; // Clear grab facing for single orbs
						break; // Exit the loop since we've converted to single orb
					}
					else if (Arm.Held.Orbs.Num() == 0)
					{
						// This should never happen, but just in case
						Arm.bHolding = false;
						Arm.Held = FItem();
						break; // Exit the loop since we're no longer holding anything
					}
				}
			}
		}
	}
}
