#include "Actors/BasicArm.h"
#include "Engine/Engine.h"
#include "EngineUtils.h"
#include "Rendering/OrbRenderer.h"
#include "Sim/SimWorldHex.h"

// Cluster management functions
void USimWorldHex::AddBoardOrbToHeldCluster(int32 HeldOrbId, int32 BoardOrbId,
                                            const FHex &BoardOrbHex) {
  // Find the arm holding the cluster that contains HeldOrbId
  for (int32 ArmIndex = 0; ArmIndex < Arms.Num(); ++ArmIndex) {
    FArmState &Arm = Arms[ArmIndex];
    if (!Arm.bHolding || !Arm.Held.bIsCluster)
      continue;

    bool bContainsHeldOrb = false;
    for (const FOrbData &Sub : Arm.Held.Orbs) {
      if (Sub.ItemId == HeldOrbId) {
        bContainsHeldOrb = true;
        break;
      }
    }
    if (!bContainsHeldOrb)
      continue;

    // Delegate to the sim-space version using the cluster's Held.Id
    const int32 ClusterId = Arm.Held.Id;
    AddBoardOrbToHeldItem(ClusterId, BoardOrbId, BoardOrbHex);

    if (bDevDebugBonds && GEngine) {
      GEngine->AddOnScreenDebugMessage(
          -1, 1.0f, FColor::Orange,
          FString::Printf(
              TEXT(
                  "Board orb %d added to held cluster %d via sim-space attach"),
              BoardOrbId, ClusterId));
    }
    return; // Done
  }

  // Not found: no arm currently holding the given held orb id
  if (bDevDebugBonds && GEngine) {
    GEngine->AddOnScreenDebugMessage(
        -1, 1.5f, FColor::Red,
        FString::Printf(TEXT("AddBoardOrbToHeldCluster: held orb %d not found "
                             "in any held cluster"),
                        HeldOrbId));
  }
}

void USimWorldHex::AddBoardOrbToHeldItem(int32 HeldOrbId, int32 BoardOrbId,
                                         const FHex &BoardOrbHex) {
  // Find the arm holding the item containing HeldOrbId
  for (int32 ArmIndex = 0; ArmIndex < Arms.Num(); ArmIndex++) {
    FArmState &Arm = Arms[ArmIndex];
    if (!Arm.bHolding)
      continue;

    // Check if this arm is holding the specified orb
    bool bContainsHeldOrb = false;
    if (Arm.Held.bIsCluster) {
      for (const FOrbData &Sub : Arm.Held.Orbs) {
        if (Sub.ItemId == HeldOrbId) {
          bContainsHeldOrb = true;
          break;
        }
      }
    } else if (Arm.Held.Id == HeldOrbId) {
      bContainsHeldOrb = true;
    }

    if (!bContainsHeldOrb)
      continue;

    // Find the board orb
    FItem *BoardItem = ItemMap.Find(BoardOrbHex);
    if (!BoardItem || BoardItem->Id != BoardOrbId)
      continue;

    // Use SIM-committed state for grip
    const FHex GripHex = GetArmGripHex_Sim(ArmIndex);

    if (Arm.Held.bIsCluster) {
      // Existing cluster - add the board orb directly using SIM rotation (avoid
      // recursion)

      // ccw steps mapping local->world; for world->local we rotate CW by same
      // amount
      const uint8 CCWSteps = GetHeldCCW_Steps_Sim(ArmIndex);

      // Convert world offset to cluster-local dQ/dR using SIM frame
      const FHex WorldOffset(BoardOrbHex.Q - GripHex.Q,
                             BoardOrbHex.R - GripHex.R);
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

      if (bDevDebugBonds && GEngine) {
        GEngine->AddOnScreenDebugMessage(
            -1, 1.0f, FColor::Orange,
            FString::Printf(TEXT("Added board orb %d to held cluster %d (SIM "
                                 "frame): dQ=%d dR=%d"),
                            BoardOrbId, Arm.Held.Id, BoardOrb.dQ, BoardOrb.dR));
      }

      // DON'T force visual refresh immediately - let the normal tick cycle
      // handle it This preserves interpolation during RotateHeld commands when
      // bonds form The arm's Tick() will detect the cluster change and update
      // visuals appropriately
    } else {
      // Single held orb - convert to cluster and add the board orb
      FItem NewCluster;
      NewCluster.bIsCluster = true;
      NewCluster.Id = Arm.Held.Id;

      // GrabFacing must reflect the SIM-committed facing now
      const uint8 SimFace =
          (SimFacingByArm.Contains(ArmIndex) ? SimFacingByArm[ArmIndex]
                                             : (uint8)Arm.Facing);
      NewCluster.GrabFacing = (int8)SimFace;

      // NEW: defer SIM held orientation to next tick (consistent with
      // RotateHeld commands) This preserves interpolation when bonds form
      // during RotateHeld commands
      PendingGrabFacingByArm.Add(ArmIndex, (int8)SimFace);

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
      const FHex BoardOrbOffset(BoardOrbHex.Q - GripHex.Q,
                                BoardOrbHex.R - GripHex.R);

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

      if (bDevDebugBonds && GEngine) {
        GEngine->AddOnScreenDebugMessage(
            -1, 1.0f, FColor::Orange,
            FString::Printf(TEXT("Converted single orb %d to cluster, added "
                                 "board orb %d (SIM frame): dQ=%d dR=%d"),
                            Arm.Held.Id, BoardOrbId, BoardOrb.dQ, BoardOrb.dR));
      }

      // DON'T force visual refresh immediately - let the normal tick cycle
      // handle it This preserves interpolation during RotateHeld commands when
      // bonds form The arm's Tick() will detect the cluster change and update
      // visuals appropriately
    }

    break; // Only add to the first matching arm
  }
}

void USimWorldHex::CheckAndRemoveDisconnectedOrbsFromHeldClusters() {
  // Check each held cluster for disconnected orbs using connected component
  // analysis
  for (int32 ArmIndex = 0; ArmIndex < Arms.Num(); ArmIndex++) {
    FArmState &Arm = Arms[ArmIndex];
    if (!Arm.bHolding || !Arm.Held.bIsCluster || Arm.Held.Orbs.Num() <= 1)
      continue;

    // Find the held orb (the one at dQ=0, dR=0) - this is our root for
    // connectivity Store its data early to preserve Type/Color even if all
    // bonds are removed
    int32 HeldOrbItemId = -1;
    const FOrbData *HeldOrbDataPtr = nullptr;
    for (const FOrbData &Sub : Arm.Held.Orbs) {
      if (Sub.ItemId > 0 && Sub.dQ == 0 && Sub.dR == 0) {
        HeldOrbItemId = Sub.ItemId;
        HeldOrbDataPtr = &Sub; // Store pointer to preserve Type/Color
        break;
      }
    }

    if (HeldOrbItemId <= 0 || HeldOrbDataPtr == nullptr)
      continue; // No held orb found, skip

    // Make a copy of the held orb data to preserve it even if we modify the
    // array
    const FOrbData HeldOrbDataBackup = *HeldOrbDataPtr;

    // Build a map of ItemId -> OrbData for quick lookup
    TMap<int32, const FOrbData *> OrbDataByItemId;
    for (const FOrbData &Sub : Arm.Held.Orbs) {
      if (Sub.ItemId > 0) {
        OrbDataByItemId.Add(Sub.ItemId, &Sub);
      }
    }

    // Use BFS to find all orbs connected to the held orb via bonds
    TSet<int32> ConnectedItemIds;
    TQueue<int32> Queue;
    Queue.Enqueue(HeldOrbItemId);
    ConnectedItemIds.Add(HeldOrbItemId);

    while (!Queue.IsEmpty()) {
      int32 CurrentItemId;
      Queue.Dequeue(CurrentItemId);

      // Find all bonds for this item
      if (const TArray<int32> *BondList = ItemToBonds.Find(CurrentItemId)) {
        for (int32 BondIdx : *BondList) {
          if (!Bonds.IsValidIndex(BondIdx))
            continue;
          const FBond &Bond = Bonds[BondIdx];
          if (!Bond.bActive)
            continue;

          // Get the other item in this bond
          const int32 OtherItemId =
              (Bond.ItemA == CurrentItemId) ? Bond.ItemB : Bond.ItemA;

          // Only consider bonds to other orbs in the same held cluster
          if (!OrbDataByItemId.Contains(OtherItemId))
            continue;

          // If we haven't visited this orb yet, add it to the connected set
          if (!ConnectedItemIds.Contains(OtherItemId)) {
            ConnectedItemIds.Add(OtherItemId);
            Queue.Enqueue(OtherItemId);
          }
        }
      }
    }

    // Use SIM-committed state (not visual) to align with bond creation/breaking
    const FHex GripHex = GetArmGripHex_Sim(ArmIndex);
    const uint8 ccw = GetHeldCCW_Steps_Sim(ArmIndex);

    auto Rot60 = [](FHex D, uint8 StepsCCW) {
      for (uint8 i = 0; i < (StepsCCW % 6); ++i) {
        int32 q = D.Q;
        int32 r = D.R;
        D.Q = -r;
        D.R = q + r;
      }
      return D;
    };

    // Find all orbs that are NOT connected to the held orb
    TArray<FOrbData> OrbsToDrop;
    TArray<int32> OrbsToRemove;

    for (int32 SubIndex = Arm.Held.Orbs.Num() - 1; SubIndex >= 0; SubIndex--) {
      const FOrbData &Sub = Arm.Held.Orbs[SubIndex];
      if (Sub.ItemId <= 0)
        continue;

      // If this orb is not in the connected set, it needs to be dropped
      if (!ConnectedItemIds.Contains(Sub.ItemId)) {
        OrbsToDrop.Add(Sub);
        OrbsToRemove.Add(SubIndex);
      }
    }

    // If we found disconnected orbs, drop them
    if (OrbsToDrop.Num() > 0) {
      if (bDebugLogs && bDevDebugBonds && GEngine) {
        GEngine->AddOnScreenDebugMessage(
            -1, 1.0f, FColor::Orange,
            FString::Printf(
                TEXT("Arm %d: Dropping %d disconnected orbs from held cluster"),
                ArmIndex, OrbsToDrop.Num()));
      }

      // Drop each disconnected orb
      for (const FOrbData &Sub : OrbsToDrop) {
        FHex Local(Sub.dQ, Sub.dR);
        FHex Rot = Rot60(Local, ccw);
        FHex WorldPos(GripHex.Q + Rot.Q, GripHex.R + Rot.R, GripHex.Level);

        FItem DisconnectedOrb;
        DisconnectedOrb.bIsCluster = false;
        DisconnectedOrb.Id = Sub.ItemId;
        DisconnectedOrb.Single.Type = (EOrbType)Sub.Type;
        DisconnectedOrb.Single.Color = (EOrbColor)Sub.Color;

        ItemMap.Add(WorldPos, DisconnectedOrb);
        NewlyPlacedItems.Add(Sub.ItemId);
        RecentlyDroppedByArm.Add(Sub.ItemId, ArmIndex);
        HeldItemLastHex.Remove(Sub.ItemId);

        if (bDebugLogs && bDevDebugBonds && GEngine) {
          GEngine->AddOnScreenDebugMessage(
              -1, 1.0f, FColor::Red,
              FString::Printf(TEXT("Disconnected orb %d placed at (%d,%d)"),
                              Sub.ItemId, WorldPos.Q, WorldPos.R));
        }
      }

      // Remove dropped orbs from the held cluster (in reverse order to maintain
      // indices)
      OrbsToRemove.Sort([](int32 A, int32 B) { return A > B; });
      for (int32 SubIndex : OrbsToRemove) {
        Arm.Held.Orbs.RemoveAt(SubIndex);
      }

      // If we removed all orbs except the held orb, convert to single
      // Always use the backed-up held orb data to ensure Type/Color are
      // preserved This is critical when all bonds are removed - the backup has
      // the original data
      if (Arm.Held.Orbs.Num() == 1) {
        // Always use the backup - it was captured before any array
        // modifications
        const FOrbData &HeldOrbData = HeldOrbDataBackup;

        if (HeldOrbData.ItemId > 0) {
          Arm.Held.bIsCluster = false;
          Arm.Held.Orbs.Empty();
          Arm.Held.Single.Type = (EOrbType)HeldOrbData.Type;
          Arm.Held.Single.Color = (EOrbColor)HeldOrbData.Color;
          Arm.Held.Id = HeldOrbData.ItemId;
          Arm.Held.GrabFacing = -1;
          SimGrabFacingByArm.Add(ArmIndex, int8(-1));

          if (bDebugLogs && bDevDebugBonds && GEngine) {
            GEngine->AddOnScreenDebugMessage(
                -1, 1.0f, FColor::Yellow,
                FString::Printf(TEXT("Held orb %d (Type=%d) converted to "
                                     "single after cluster disconnection"),
                                HeldOrbData.ItemId, (int32)HeldOrbData.Type));
          }
        } else {
          // This shouldn't happen, but handle it gracefully
          if (bDebugLogs && bDevDebugBonds && GEngine) {
            GEngine->AddOnScreenDebugMessage(
                -1, 1.0f, FColor::Red,
                TEXT("ERROR: Could not find valid held orb data when "
                     "converting cluster to single!"));
          }
          Arm.bHolding = false;
          Arm.Held = FItem();
          SimGrabFacingByArm.Add(ArmIndex, int8(-1));
        }
      } else if (Arm.Held.Orbs.Num() == 0) {
        // This shouldn't happen (held orb should always remain), but handle it
        // anyway
        Arm.bHolding = false;
        Arm.Held = FItem();
        SimGrabFacingByArm.Add(ArmIndex, int8(-1));
      }
    }
  }
}

void USimWorldHex::ProcessHeldToHeldMerge(int32 ReceiverArmIndex,
                                          int32 DonorArmIndex) {
  if (ReceiverArmIndex < 0 || ReceiverArmIndex >= Arms.Num() ||
      DonorArmIndex < 0 || DonorArmIndex >= Arms.Num()) {
    return;
  }

  FArmState &ReceiverArm = Arms[ReceiverArmIndex];
  FArmState &DonorArm = Arms[DonorArmIndex];

  if (!ReceiverArm.bHolding || !DonorArm.bHolding) {
    return;
  }

  // Ensure both are clusters
  if (!ReceiverArm.Held.bIsCluster) {
    FItem NewCluster;
    NewCluster.bIsCluster = true;
    NewCluster.Id = ReceiverArm.Held.Id;
    // Use SIM-committed facing
    const uint8 SimFace = (SimFacingByArm.Contains(ReceiverArmIndex)
                               ? SimFacingByArm[ReceiverArmIndex]
                               : (uint8)ReceiverArm.Facing);
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

  if (!DonorArm.Held.bIsCluster) {
    FItem NewCluster;
    NewCluster.bIsCluster = true;
    NewCluster.Id = DonorArm.Held.Id;
    const uint8 SimFace =
        (SimFacingByArm.Contains(DonorArmIndex) ? SimFacingByArm[DonorArmIndex]
                                                : (uint8)DonorArm.Facing);
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
  const FHex GripOffset(DonorGrip.Q - ReceiverGrip.Q,
                        DonorGrip.R - ReceiverGrip.R);

  // Donor local->world CCW steps (SIM)
  const uint8 DonorCCWSteps = GetHeldCCW_Steps_Sim(DonorArmIndex);

  // Add all orbs from donor to receiver, adjusting their positions
  int32 DonorOrbCount = DonorArm.Held.Orbs.Num();
  for (const FOrbData &DonorOrb : DonorArm.Held.Orbs) {
    FOrbData NewOrb = DonorOrb;

    // donor local -> world (relative to donor grip) using SIM rotation
    const FHex DonorLocal(DonorOrb.dQ, DonorOrb.dR);
    const FHex DonorWorldRel = Rot60CCW(DonorLocal, DonorCCWSteps);

    // Re-express in receiver local coordinates (still in the receiver cluster's
    // current SIM frame)
    NewOrb.dQ = GripOffset.Q + DonorWorldRel.Q;
    NewOrb.dR = GripOffset.R + DonorWorldRel.R;

    NewOrb.Offset = HexCoord::AxialToXY(FHexAxial(NewOrb.dQ, NewOrb.dR), 1.f,
                                        GridOrientation);

    ReceiverArm.Held.Orbs.Add(NewOrb);
  }

  if (bDebugLogs && bDevDebugBonds && GEngine) {
    GEngine->AddOnScreenDebugMessage(
        -1, 2.0f, FColor::Green,
        FString::Printf(TEXT("ProcessHeldToHeldMerge (SIM): Arm %d absorbed %d "
                             "orbs from Arm %d"),
                        ReceiverArmIndex, DonorOrbCount, DonorArmIndex));
  }

  // Clear donor's held item
  DonorArm.bHolding = false;
  DonorArm.Held = FItem();
  DonorArm.bWaitingForBond = false;
  DonorArm.WaitForBondInitialCount = 0;
  DonorArm.PrevHeldOrbCount = 0;
}

TSet<int32> USimWorldHex::GetFullClusterParentIds(int32 StartItemId) const {
  TSet<int32> BondedParentIds;
  if (StartItemId <= 0)
    return BondedParentIds;

  TArray<int32> Stack;
  Stack.Add(StartItemId);
  BondedParentIds.Add(StartItemId);

  while (Stack.Num() > 0) {
    int32 CurrentParentId = Stack.Pop();

    // 1. Identify all Orb IDs belonging to this parent item
    TArray<int32> AllOrbsInItem;
    AllOrbsInItem.Add(CurrentParentId);

    // Find the item to check for its sub-orbs
    const FItem *ItemPtr = nullptr;
    for (const auto &KV : ItemMap) {
      if (KV.Value.Id == CurrentParentId) {
        ItemPtr = &KV.Value;
        break;
      }
    }

    if (ItemPtr && ItemPtr->bIsCluster) {
      for (const auto &Sub : ItemPtr->Orbs) {
        AllOrbsInItem.Add(Sub.ItemId);
      }
    }

    // 2. Check all bonds from all orbs in this item
    for (int32 OrbId : AllOrbsInItem) {
      if (const TArray<int32> *BondList = ItemToBonds.Find(OrbId)) {
        for (int32 BondIdx : *BondList) {
          if (!Bonds.IsValidIndex(BondIdx))
            continue;
          const FBond &Bond = Bonds[BondIdx];
          if (!Bond.bActive)
            continue;

          int32 OtherOrbId = (Bond.ItemA == OrbId) ? Bond.ItemB : Bond.ItemA;
          if (OtherOrbId > 0) {
            // Map back to parent item ID
            int32 OtherParentId =
                (OtherOrbId > 1000000) ? (OtherOrbId >> 8) : OtherOrbId;
            if (!BondedParentIds.Contains(OtherParentId)) {
              BondedParentIds.Add(OtherParentId);
              Stack.Add(OtherParentId);
            }
          }
        }
      }
    }
  }

  return BondedParentIds;
}
