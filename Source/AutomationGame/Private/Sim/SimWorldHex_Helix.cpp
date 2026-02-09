#include "EngineUtils.h"
#include "Framework/GameManager.h"
#include "HelixUtils.h"
#include "Sim/SimWorldHex.h"

// ============================================================
// HELIX OUTPUT SYSTEM IMPLEMENTATION
// ============================================================

void USimWorldHex::RegisterHelixOutputTile(const FHex &Hex) {
  // Check if this tile is already in a group
  if (FindHelixGroupContaining(Hex)) {
    return; // Already registered
  }

  // Helper lambda to find the center-most hex from a list of tiles
  auto FindCenterHex = [this](const TArray<FHex> &Tiles) -> FHex {
    return FindCenterHexRobust(this, Tiles);
  };

  // Find adjacent helix groups to merge with
  TArray<int32> AdjacentGroupIndices;
  for (int32 Dir = 0; Dir < 6; ++Dir) {
    FHex NeighborHex = HexUtil::Neighbor(Hex, (EHexDir)Dir);
    for (int32 GroupIdx = 0; GroupIdx < HelixGroups.Num(); ++GroupIdx) {
      if (HelixGroups[GroupIdx].MemberTiles.Contains(NeighborHex)) {
        AdjacentGroupIndices.AddUnique(GroupIdx);
      }
    }
  }

  if (AdjacentGroupIndices.Num() == 0) {
    // No adjacent groups - create new group
    FHelixOutputGroup NewGroup;
    NewGroup.MemberTiles.Add(Hex);
    NewGroup.CenterHex = Hex;
    NewGroup.CenterWorld = HexCenterToWorld(Hex);
    HelixGroups.Add(NewGroup);
  } else if (AdjacentGroupIndices.Num() == 1) {
    // Add to existing group
    FHelixOutputGroup &Group = HelixGroups[AdjacentGroupIndices[0]];
    Group.MemberTiles.Add(Hex);

    // Recalculate center
    Group.CenterHex = FindCenterHex(Group.MemberTiles);
    Group.CenterWorld = HexCenterToWorld(Group.CenterHex);
  } else {
    // Merge multiple groups
    int32 PrimaryIdx = AdjacentGroupIndices[0];
    FHelixOutputGroup &PrimaryGroup = HelixGroups[PrimaryIdx];
    PrimaryGroup.MemberTiles.Add(Hex);

    // Merge other groups into primary (in reverse order to preserve indices)
    for (int32 i = AdjacentGroupIndices.Num() - 1; i >= 1; --i) {
      int32 MergeIdx = AdjacentGroupIndices[i];
      if (MergeIdx != PrimaryIdx) {
        FHelixOutputGroup &MergeGroup = HelixGroups[MergeIdx];
        PrimaryGroup.MemberTiles.Append(MergeGroup.MemberTiles);
        PrimaryGroup.Rungs.Append(MergeGroup.Rungs);
        PrimaryGroup.TotalClustersProcessed +=
            MergeGroup.TotalClustersProcessed;
        HelixGroups.RemoveAt(MergeIdx);
      }
    }

    // Recalculate center
    PrimaryGroup.CenterHex = FindCenterHex(PrimaryGroup.MemberTiles);
    PrimaryGroup.CenterWorld = HexCenterToWorld(PrimaryGroup.CenterHex);
  }
}

void USimWorldHex::UnregisterHelixOutputTile(const FHex &Hex) {
  for (int32 GroupIdx = HelixGroups.Num() - 1; GroupIdx >= 0; --GroupIdx) {
    FHelixOutputGroup &Group = HelixGroups[GroupIdx];
    int32 TileIdx = Group.MemberTiles.Find(Hex);
    if (TileIdx != INDEX_NONE) {
      Group.MemberTiles.RemoveAt(TileIdx);

      if (Group.MemberTiles.Num() == 0) {
        HelixGroups.RemoveAt(GroupIdx);
      } else {
        // Recalculate center - find center-most hex using robust logic
        Group.CenterHex = FindCenterHexRobust(this, Group.MemberTiles);
        Group.CenterWorld = HexCenterToWorld(Group.CenterHex);
      }
      return;
    }
  }
}

void USimWorldHex::RegisterHelixInputTile(const FHex &Hex,
                                          int32 OutputGroupIndex) {
  if (OutputGroupIndex < 0 || OutputGroupIndex >= HelixGroups.Num()) {
    return;
  }

  FHelixOutputGroup &Group = HelixGroups[OutputGroupIndex];

  // Don't add duplicates
  if (Group.PairedInputTiles.Contains(Hex)) {
    return;
  }

  Group.PairedInputTiles.Add(Hex);
  Group.bHasPairedInput = true;

  // Recalculate input center using robust logic
  Group.PairedInputCenter = FindCenterHexRobust(this, Group.PairedInputTiles);
  Group.PairedInputCenterWorld = HexCenterToWorld(Group.PairedInputCenter);

  Group.PairedInputCenterWorld = HexCenterToWorld(Group.PairedInputCenter);
}

void USimWorldHex::UnregisterHelixInputTile(const FHex &Hex) {
  for (FHelixOutputGroup &Group : HelixGroups) {
    int32 TileIdx = Group.PairedInputTiles.Find(Hex);
    if (TileIdx != INDEX_NONE) {
      // If ANY input tile is removed, we must remove the ENTIRE paired set.
      // This prevents "zombie" input tiles that persist across reloads or
      // edits, and ensures the set is always valid and complete. The user will
      // re-place them as a fresh set.

      // Clear the paired input tiles completely
      Group.PairedInputTiles.Empty();
      Group.bHasPairedInput = false;
      Group.PairedInputCenter = FHex(0, 0, 0);
      Group.PairedInputCenterWorld = FVector::ZeroVector;

      UE_LOG(LogTemp, Warning,
             TEXT("Helix: Input tile removed at (%d,%d), clearing entire "
                  "paired input set for group."),
             Hex.Q, Hex.R);
      return;
    }
  }
}

void USimWorldHex::PairHelixInput(int32 OutputGroupIndex,
                                  const TArray<FHex> &InputTiles,
                                  const FHex &Center) {
  if (OutputGroupIndex < 0 || OutputGroupIndex >= HelixGroups.Num()) {
    return;
  }

  FHelixOutputGroup &Group = HelixGroups[OutputGroupIndex];

  // Clean up any existing claims on these input tiles by other groups
  for (const FHex &InputHex : InputTiles) {
    for (int32 i = 0; i < HelixGroups.Num(); ++i) {
      if (i == OutputGroupIndex)
        continue;

      FHelixOutputGroup &OtherGroup = HelixGroups[i];
      if (OtherGroup.PairedInputTiles.Contains(InputHex)) {
        // Found a conflict. Clear the ENTIRE paired input set for the other
        // group to maintain consistency (as we do in UnregisterHelixInputTile).
        // It's safer to break the old link completely than leave a partial set.
        OtherGroup.PairedInputTiles.Empty();
        OtherGroup.bHasPairedInput = false;
        OtherGroup.PairedInputCenter = FHex(0, 0, 0);
        OtherGroup.PairedInputCenterWorld = FVector::ZeroVector;

        UE_LOG(LogTemp, Warning,
               TEXT("Helix: Re-pairing input tiles. Cleared old paired input "
                    "set for group %d because tile (%d,%d) was reassigned."),
               i, InputHex.Q, InputHex.R);
      }
    }
  }

  Group.PairedInputTiles = InputTiles;
  Group.PairedInputCenter = Center;
  Group.PairedInputCenterWorld = HexCenterToWorld(Center);
  Group.bHasPairedInput = (InputTiles.Num() > 0);
}

bool USimWorldHex::CanPlaceHelixInputAt(const FHex &Hex,
                                        int32 OutputGroupIndex) const {
  if (OutputGroupIndex < 0 || OutputGroupIndex >= HelixGroups.Num()) {
    return false;
  }

  const FHelixOutputGroup &Group = HelixGroups[OutputGroupIndex];

  // The radius is the maximum distance of any member tile from the output
  // center. We add 1 for padding to ensure the spiral animations don't overlap.
  int32 OutputRadius = 0;
  for (const FHex &MemberTile : Group.MemberTiles) {
    OutputRadius =
        FMath::Max(OutputRadius, HexUtil::Dist(Group.CenterHex, MemberTile));
  }

  // Check distance from output center - must be outside the safe radius
  int32 Distance = HexUtil::Dist(Group.CenterHex, Hex);
  if (Distance <= OutputRadius + 1) {
    return false;
  }

  return true;
}

int32 USimWorldHex::GetHelixOutputGroupIndexAt(const FHex &Hex) const {
  for (int32 i = 0; i < HelixGroups.Num(); ++i) {
    if (HelixGroups[i].MemberTiles.Contains(Hex)) {
      return i;
    }
  }
  return -1;
}

USimWorldHex::FHelixOutputGroup *
USimWorldHex::FindHelixGroupContaining(const FHex &Hex) {
  for (FHelixOutputGroup &Group : HelixGroups) {
    if (Group.MemberTiles.Contains(Hex)) {
      return &Group;
    }
  }
  return nullptr;
}

const USimWorldHex::FHelixOutputGroup *
USimWorldHex::FindHelixGroupContaining(const FHex &Hex) const {
  for (const FHelixOutputGroup &Group : HelixGroups) {
    if (Group.MemberTiles.Contains(Hex)) {
      return &Group;
    }
  }
  return nullptr;
}

USimWorldHex::FHelixOutputGroup *
USimWorldHex::FindHelixGroupWithInputContaining(const FHex &Hex) {
  for (FHelixOutputGroup &Group : HelixGroups) {
    if (Group.PairedInputTiles.Contains(Hex)) {
      return &Group;
    }
  }
  return nullptr;
}

const USimWorldHex::FHelixOutputGroup *
USimWorldHex::FindHelixGroupWithInputContaining(const FHex &Hex) const {
  for (const FHelixOutputGroup &Group : HelixGroups) {
    if (Group.PairedInputTiles.Contains(Hex)) {
      return &Group;
    }
  }
  return nullptr;
}

FVector USimWorldHex::GetHelixCenterWorld(int32 GroupIndex) const {
  if (HelixGroups.IsValidIndex(GroupIndex)) {
    return HelixGroups[GroupIndex].CenterWorld;
  }
  return FVector::ZeroVector;
}

bool USimWorldHex::TryProcessHelixCluster(FHelixOutputGroup &Group) {
  if (Group.MemberTiles.Num() == 0) {
    return false;
  }

  // Hardcoded settings as requested: no occupancy/completeness requirements
  const bool bRequiresFull = false;
  const bool bRequiresComplete = false;

  // 1. Build a local map of all orbs on the board for efficient lookup
  struct FOccupantInfo {
    int32 ParentItemId;
    FHex RootHex;
    int32
        SubIndex; // -1 for single, >=0 for cluster sub-orb index in FItem::Orbs
    FOrbData Data;
  };
  TMap<FHex, FOccupantInfo> OrbToParent;
  for (auto &KV : ItemMap) {
    if (KV.Value.bIsCluster) {
      for (int32 i = 0; i < KV.Value.Orbs.Num(); ++i) {
        const FOrbData &Sub = KV.Value.Orbs[i];
        FHex Pos(KV.Key.Q + Sub.dQ, KV.Key.R + Sub.dR, KV.Key.Level);
        OrbToParent.Add(Pos, {KV.Value.Id, KV.Key, i, Sub});
      }
    } else {
      FOrbData SingleData;
      SingleData.Type = (uint8)KV.Value.Single.Type;
      SingleData.Color = (uint8)KV.Value.Single.Color;
      SingleData.dQ = 0;
      SingleData.dR = 0;
      SingleData.ItemId = KV.Value.Id;
      OrbToParent.Add(KV.Key, {KV.Value.Id, KV.Key, -1, SingleData});
    }
  }

  // 2. Identify all orbs directly on the helix mouth tiles
  TArray<FHex> OrbsOnMouth;
  for (const FHex &TileHex : Group.MemberTiles) {
    if (OrbToParent.Contains(TileHex)) {
      const FOccupantInfo &Info = OrbToParent[TileHex];

      // Process immediately when it arrives on the mouth.
      // Easing/animation logic in UpdateHelixGroups will handle the visual
      // transition from the mouth center upward.
      OrbsOnMouth.Add(TileHex);

      OrbsOnMouth.Add(TileHex);
    }
  }

  // No orbs on mouth = nothing to process
  if (OrbsOnMouth.Num() == 0)
    return false;

  // 3. ARRIVAL DETECTION & ANIMATION TRIGGER
  // Check if any of these orbs just moved onto the mouth this tick via belt or
  // ClusterSend
  bool bAnyNewlyArrived = false;
  int32 BatchId = OrbToParent[OrbsOnMouth[0]].ParentItemId;

  for (const FHex &Hex : OrbsOnMouth) {
    int32 Id = OrbToParent[Hex].ParentItemId;
    if (ItemsMovedLastStep.Contains(Id) || OrbsNewlyMovedByBelts.Contains(Id)) {
      bAnyNewlyArrived = true;
      break;
    }
  }

  // Determine if we need to trigger the lift/twist animation for this set.
  // We trigger strictly once per batch arrival.
  bool bNeedsAnimationTrigger = (Group.LastArrivedItemId != BatchId);

  if (bNeedsAnimationTrigger) {
    Group.LastArrivedItemId = BatchId;
    // PRESSURE-BASED EJECTION:
    // If the helix is full, we MUST eject the oldest cluster to make room.
    if (Group.Rungs.Num() >= Group.MaxVisibleRungs) {
      if (!TryEjectHelixCluster(Group, true)) {
        // UE_LOG(LogTemp, Warning,
        //        TEXT("Helix: Entry blocked because ejection path is
        //        obstructed."));
        return false;
      }
    }

    // Prepare for new rung by elevating existing ones
    for (FHelixRung &Rung : Group.Rungs) {
      Rung.TargetZOffset += Group.RungSpacing;
      Rung.RotationDegrees += Group.DegreesPerRung;
    }
    Group.HelixRotationOffset -= Group.DegreesPerRung;
    Group.LastAnimatedTick = StepCounter;
  }

  // 4. Prepare new rung
  FHelixRung NewRung;
  NewRung.SpawnTime = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;
  NewRung.SpawnTick = StepCounter;
  NewRung.ZOffset = 0.f;
  NewRung.TargetZOffset = 0.f; // Start at 0 (on board) for the first step
  NewRung.RotationDegrees = 0.f;

  // 5. Detect entry direction (based on belt pointing into ANY mouth hex)
  uint8 DetectedEntryDir = 0;
  bool bFoundEntryDir = false;
  TSet<FHex> MouthHexSet(Group.MemberTiles);

  for (const FHex &HelixTile : OrbsOnMouth) {
    if (bFoundEntryDir)
      break;
    for (uint8 Dir = 0; Dir < 6; ++Dir) {
      FHex NeighborHex = HexUtil::Neighbor(HelixTile, (EHexDir)Dir);
      if (MouthHexSet.Contains(NeighborHex))
        continue;

      if (const FTile *NeighborTile = TileMap.Find(NeighborHex)) {
        if (NeighborTile->Kind == ETileKind::Belt) {
          uint8 BeltDir = NeighborTile->Arg;
          FHex BeltDest = HexUtil::Neighbor(NeighborHex, (EHexDir)BeltDir);
          if (BeltDest == HelixTile) {
            DetectedEntryDir = BeltDir;
            bFoundEntryDir = true;
            // UE_LOG(
            //     LogTemp, Warning,
            //     TEXT("Helix: Detected entry direction %d from belt at
            //     (%d,%d)"), DetectedEntryDir, NeighborHex.Q, NeighborHex.R);
            break;
          }
        }
      }
    }
  }
  NewRung.EntryDirection = DetectedEntryDir;
  if (!bFoundEntryDir) {
    // UE_LOG(
    //     LogTemp, Warning,
    //     TEXT("Helix: No incoming belt found, defaulting entry direction to
    //     0"));
  }

  // 6. Group board-side removals and populate rung orbs
  TMap<FHex, TSet<int32>> RootToRemovingSubIndices;
  TSet<FHex> SinglesToRemove;
  TSet<int32> AllRemovedOrbIds;

  for (const FHex &Hex : OrbsOnMouth) {
    const FOccupantInfo &Info = OrbToParent[Hex];
    AllRemovedOrbIds.Add(Info.ParentItemId);

    if (Info.SubIndex == -1) {
      SinglesToRemove.Add(Info.RootHex);
    } else {
      RootToRemovingSubIndices.FindOrAdd(Info.RootHex).Add(Info.SubIndex);
    }

    // Add to new rung with helix-local coordinates
    FOrbData RungOrb = Info.Data;
    RungOrb.dQ = Hex.Q - Group.CenterHex.Q;
    RungOrb.dR = Hex.R - Group.CenterHex.R;

    // Populate PreviousBoardHex for smooth entry animation.
    // We need the ACTUAL previous hex of this specific orb.
    if (const FHex *PrevRoot = PrevItemHex.Find(Info.ParentItemId)) {
      if (Info.SubIndex == -1) {
        RungOrb.PreviousBoardHex = *PrevRoot;
      } else {
        // For cluster sub-orbs, the previous hex is the previous root + sub
        // offset
        RungOrb.PreviousBoardHex =
            FHex(PrevRoot->Q + Info.Data.dQ, PrevRoot->R + Info.Data.dR,
                 PrevRoot->Level);
      }
    } else {
      RungOrb.PreviousBoardHex = Hex; // Fallback to current hex
    }

    NewRung.Orbs.Add(RungOrb);
  }

  // 7. Execute Board State Changes (Slicing/Removal)
  for (const FHex &Hex : OrbsOnMouth) {
    OrbAt.Remove(Hex);
  }

  for (const FHex &RootHex : SinglesToRemove) {
    ItemMap.Remove(RootHex);
  }

  for (auto &KV : RootToRemovingSubIndices) {
    FHex RootHex = KV.Key;
    FItem *ItemPtr = ItemMap.Find(RootHex);
    if (!ItemPtr)
      continue;

    // Remove sub-orbs in reverse index order to preserve validity of earlier
    // indices
    TArray<int32> ToRemove = KV.Value.Array();
    ToRemove.Sort([](const int32 &A, const int32 &B) { return A > B; });
    for (int32 Idx : ToRemove) {
      ItemPtr->Orbs.RemoveAt(Idx);
    }

    if (ItemPtr->Orbs.Num() == 0) {
      ItemMap.Remove(RootHex);
    } else {
      // If the root hex itself was removed (determined by checking if any orb
      // is still at RootHex) Note: Offset 0,0 is the definition of the root.
      bool bRootStillExists = false;
      for (const FOrbData &O : ItemPtr->Orbs) {
        if (O.dQ == 0 && O.dR == 0) {
          bRootStillExists = true;
          break;
        }
      }

      if (!bRootStillExists) {
        // Relocate the cluster to the first remaining orb's position
        FItem RevisedItem = *ItemPtr;
        const FOrbData &NewRootOrb = RevisedItem.Orbs[0];
        FHex NewRootHex(RootHex.Q + NewRootOrb.dQ, RootHex.R + NewRootOrb.dR,
                        RootHex.Level);
        int32 dQ_Shift = NewRootOrb.dQ;
        int32 dR_Shift = NewRootOrb.dR;

        for (FOrbData &O : RevisedItem.Orbs) {
          O.dQ -= dQ_Shift;
          O.dR -= dR_Shift;
        }
        ItemMap.Remove(RootHex);
        ItemMap.Add(NewRootHex, RevisedItem);
      }
    }
  }

  // 8. Deactivate board bonds for all consumed orbs
  for (int32 OrbId : AllRemovedOrbIds) {
    if (const TArray<int32> *BondIndices = ItemToBonds.Find(OrbId)) {
      for (int32 BIdx : *BondIndices) {
        if (Bonds.IsValidIndex(BIdx)) {
          Bonds[BIdx].bActive = false;
        }
      }
    }
  }

  // 9. Build internal bonds for the new rung and connections to previous rung
  // Standard internal connectivity (607-616 equivalent)
  for (int32 i = 0; i < NewRung.Orbs.Num(); ++i) {
    FHex PosI(NewRung.Orbs[i].dQ, NewRung.Orbs[i].dR, 0);
    for (int32 j = i + 1; j < NewRung.Orbs.Num(); ++j) {
      FHex PosJ(NewRung.Orbs[j].dQ, NewRung.Orbs[j].dR, 0);
      if (HexUtil::Dist(PosI, PosJ) == 1) {
        NewRung.Bonds.AddUnique(TPair<int32, int32>(i, j));
      }
    }
  }

  // Use previously processed rung for inter-rung bonding
  if (Group.Rungs.Num() > 0) {
    const FHelixRung &PrevRung = Group.Rungs[0];
    for (int32 CurrIdx = 0; CurrIdx < NewRung.Orbs.Num(); ++CurrIdx) {
      FHex CurrHex(NewRung.Orbs[CurrIdx].dQ, NewRung.Orbs[CurrIdx].dR, 0);
      for (int32 PrevIdx = 0; PrevIdx < PrevRung.Orbs.Num(); ++PrevIdx) {
        FHex PrevHex(PrevRung.Orbs[PrevIdx].dQ, PrevRung.Orbs[PrevIdx].dR, 0);
        // Bond if they were adjacent on board or just by "vertical" proximity
        // (DNA strand)
        if (HexUtil::Dist(CurrHex, PrevHex) <= 1) {
          NewRung.BondsToPrevious.AddUnique(
              TPair<int32, int32>(CurrIdx, PrevIdx));
        }
      }
    }
  }

  // Finalize
  Group.Rungs.Insert(NewRung, 0);
  Group.TotalClustersProcessed++;
  Group.LastProcessedTick = StepCounter;
  Group.LastEnteredItemId =
      AllRemovedOrbIds.Num() > 0 ? *AllRemovedOrbIds.CreateConstIterator() : 0;
  Group.LastEnteredItemTick = StepCounter;

  // UE_LOG(LogTemp, Warning,
  //        TEXT("Helix: Successfully entered new rung with %d orbs. Total "
  //             "processed: %lld"),
  //        NewRung.Orbs.Num(), Group.TotalClustersProcessed);

  return true;
}

void USimWorldHex::UpdateHelixGroups(float DeltaTime) {
  float Alpha = GetFrameAlpha();

  for (FHelixOutputGroup &Group : HelixGroups) {
    // Non-continuous: SYNCED with FrameAlpha (0 to 1)
    if (Group.LastAnimatedTick == StepCounter) {
      Group.HelixRotationOffset = -Group.DegreesPerRung * (1.0f - Alpha);
    } else {
      Group.HelixRotationOffset = 0.f;
    }

    // Elevate rungs
    for (int32 i = 0; i < Group.Rungs.Num(); ++i) {
      FHelixRung &Rung = Group.Rungs[i];
      if (Group.LastAnimatedTick == StepCounter) {
        if (Rung.SpawnTick == StepCounter) {
          // New items stay at target Z (0) to slide in horizontally
          Rung.ZOffset = Rung.TargetZOffset;
        } else {
          // Existing items lift to their new target
          Rung.ZOffset =
              Rung.TargetZOffset - Group.RungSpacing * (1.0f - Alpha);
        }
      } else {
        Rung.ZOffset = Rung.TargetZOffset;
      }
    }

    // BondsToBoard management: remove references to items no longer on the
    // board
    for (FHelixRung &R : Group.Rungs) {
      for (int32 i = R.BondsToBoard.Num() - 1; i >= 0; --i) {
        int32 BoardId = R.BondsToBoard[i].Value;
        bool bStillExists = false;
        for (const auto &KV : ItemMap) {
          if (KV.Value.Id == BoardId ||
              (BoardId > 1000000 && KV.Value.Id == (BoardId >> 8))) {
            bStillExists = true;
            break;
          }
        }
        if (!bStillExists) {
          R.BondsToBoard.RemoveAt(i);
        }
      }
    }
  }
}

bool USimWorldHex::IsItemInHelix(int32 ItemId) const {
  if (ItemId <= 0)
    return false;
  for (const FHelixOutputGroup &Group : HelixGroups) {
    for (const FHelixRung &Rung : Group.Rungs) {
      for (const FOrbData &Orb : Rung.Orbs) {
        if (Orb.ItemId == ItemId)
          return true;
        // Sub-orb matching: also check if this is a parent ID of the sub-orb
        if (Orb.ItemId > 1000000 && (Orb.ItemId >> 8) == ItemId)
          return true;
        // Or if ItemId is a sub-orb and Orb.ItemId is the parent (less common
        // but for completeness)
        if (ItemId > 1000000 && (ItemId >> 8) == Orb.ItemId)
          return true;
      }
    }
  }
  return false;
}

bool USimWorldHex::TryEjectHelixCluster(FHelixOutputGroup &Group,
                                        bool bIsForcedByPush) {
  // Check if this group has paired input tiles
  if (!Group.bHasPairedInput || Group.PairedInputTiles.Num() == 0) {
    return false;
  }

  // Check if the oldest rung is ready to exit
  if (Group.Rungs.Num() == 0) {
    return false;
  }

  // The oldest rung is at the end of the array
  FHelixRung &OldestRung = Group.Rungs.Last();

  // Ejection timing: only eject when "full" (reached max height)
  // Or if transit time has passed (to prevent stuck rungs if max height is
  // large) BUT the user specifically wants max length, so let's stick to that.
  if (Group.Rungs.Num() < Group.MaxVisibleRungs) {
    // If not full, maybe we still want to eject if it's very old?
    // Let's keep a backup transit time check but prioritize fullness.
    int32 TicksInHelix = StepCounter - OldestRung.SpawnTick;
    if (!bIsForcedByPush && TicksInHelix < (Group.RungTransitTime * 5)) {
      return false;
    }
  }

  // Get the portal exit direction for this rung
  uint8 PortalExitDir = OldestRung.EntryDirection;
  FHex StepDir = HexUtil::Step(PortalExitDir);

  // Check what's currently on the target hexes
  const FHex &InputCenter = Group.PairedInputCenter;

  // UE_LOG(LogTemp, Warning,
  //        TEXT("Helix Eject: OutputCenter=(%d,%d), InputCenter=(%d,%d), "
  //             "PortalExitDir=%d, StepDir=(%d,%d)"),
  //        Group.CenterHex.Q, Group.CenterHex.R, InputCenter.Q, InputCenter.R,
  //        PortalExitDir, StepDir.Q, StepDir.R);
  // UE_LOG(LogTemp, Warning, TEXT("  InputTiles count=%d, Rung orbs count=%d"),
  //        Group.PairedInputTiles.Num(), OldestRung.Orbs.Num());
  for (int32 i = 0; i < OldestRung.Orbs.Num(); ++i) {
    const FOrbData &Orb = OldestRung.Orbs[i];
    FHex TargetHex(InputCenter.Q + Orb.dQ, InputCenter.R + Orb.dR,
                   InputCenter.Level);
    // UE_LOG(LogTemp, Warning,
    //  TEXT("  Orb[%d]: dQ=%d, dR=%d -> TargetHex=(%d,%d)"), i, Orb.dQ,
    //  Orb.dR, TargetHex.Q, TargetHex.R);
  }
  for (int32 i = 0; i < Group.PairedInputTiles.Num(); ++i) {
    // UE_LOG(LogTemp, Warning, TEXT("  InputTile[%d]: (%d,%d)"), i,
    //        Group.PairedInputTiles[i].Q, Group.PairedInputTiles[i].R);
  }
  TArray<FHex> TargetHexes;
  TSet<int32> BlockingItemIds;

  for (const FOrbData &Orb : OldestRung.Orbs) {
    FHex TargetHex(InputCenter.Q + Orb.dQ, InputCenter.R + Orb.dR,
                   InputCenter.Level);
    TargetHexes.Add(TargetHex);

    if (const FItem *BlockingItem = ItemMap.Find(TargetHex)) {
      BlockingItemIds.Add(BlockingItem->Id);
    }
  }

  // If any target hex has an item, we need to push those items first
  if (BlockingItemIds.Num() > 0) {
    // UE_LOG(
    //     LogTemp, Warning,
    //     TEXT("Helix Eject: %d items blocking input tiles, pushing them
    //     first"),
    //     BlockingItemIds.Num());

    TSet<int32> ItemsToPrePush;
    for (int32 Id : BlockingItemIds) {
      ItemsToPrePush.Append(GetFullClusterParentIds(Id));
    }

    // Collect positions of all items to pre-push
    TMap<FHex, FItem> PrePushItems;
    for (const auto &KV : ItemMap) {
      if (ItemsToPrePush.Contains(KV.Value.Id)) {
        PrePushItems.Add(KV.Key, KV.Value);
      }
    }

    // Check for physical obstacles
    bool bCanPrePush = true;
    int32 MaxIterations = 50;
    for (int32 Iteration = 0; Iteration < MaxIterations; ++Iteration) {
      bool bNeedRestart = false;

      for (const auto &KV : PrePushItems) {
        FHex PushDest(KV.Key.Q + StepDir.Q, KV.Key.R + StepDir.R, KV.Key.Level);

        // Skip internal moves
        bool bIsInternal = false;
        for (const auto &OtherKV : PrePushItems) {
          if (OtherKV.Key == PushDest) {
            bIsInternal = true;
            break;
          }
        }
        if (bIsInternal)
          continue;

        // Check for external blockers
        if (const FItem *BlockingItem = ItemMap.Find(PushDest)) {
          if (!ItemsToPrePush.Contains(BlockingItem->Id)) {
            // UE_LOG(LogTemp, Warning,
            //        TEXT("  Helix Eject: Expanding push set to include blocker
            //        "
            //             "%d at (%d,%d)"),
            //        BlockingItem->Id, PushDest.Q, PushDest.R);
            ItemsToPrePush.Append(GetFullClusterParentIds(BlockingItem->Id));
            bNeedRestart = true;
            break;
          }
        }

        // Physical obstacles (Arms/Walkers)
        if (CachedHeldOrbHexes.Contains(PushDest) ||
            CachedBlockingFootHexes.Contains(PushDest)) {
          bCanPrePush = false;
          // UE_LOG(LogTemp, Warning,
          //        TEXT("  Helix Eject: Push BLOCKED by arm/walker at
          //        (%d,%d)"), PushDest.Q, PushDest.R);
          break;
        }

        // Tile obstructions
        if (const FTile *DestTile = TileMap.Find(PushDest)) {
          if (DestTile->Kind == ETileKind::Output &&
              DestTile->Kind != ETileKind::HelixOutput) {
            bCanPrePush = false;
            // UE_LOG(
            //     LogTemp, Warning,
            //     TEXT("  Helix Eject: Push BLOCKED by Output tile at
            //     (%d,%d)"), PushDest.Q, PushDest.R);
            break;
          }
        }
      }

      if (!bCanPrePush)
        break;
      if (bNeedRestart) {
        PrePushItems.Empty();
        for (const auto &KV2 : ItemMap) {
          if (ItemsToPrePush.Contains(KV2.Value.Id))
            PrePushItems.Add(KV2.Key, KV2.Value);
        }
        continue;
      }
      break; // Path is clear!
    }

    if (!bCanPrePush)
      return false;

    // --- Execute pre-push with full state updates ---
    TMap<FHex, uint8> OldKinds;
    TSet<int32> MovedItemIds;
    for (const auto &KV : PrePushItems) {
      OldKinds.Add(KV.Key, OrbAt.FindRef(KV.Key));
      MovedItemIds.Add(KV.Value.Id);
      ItemMap.Remove(KV.Key);
      OrbAt.Remove(KV.Key);
    }

    for (const auto &KV : PrePushItems) {
      FHex NewPos(KV.Key.Q + StepDir.Q, KV.Key.R + StepDir.R, KV.Key.Level);
      ItemMap.Add(NewPos, KV.Value);
      OrbAt.Add(NewPos, OldKinds[KV.Key]);
      OrbsNewlyMovedByBelts.Add(KV.Value.Id);
    }

    // Update Bond visual roots
    for (int32 ItemId : MovedItemIds) {
      if (const TArray<int32> *BondList = ItemToBonds.Find(ItemId)) {
        for (int32 BIdx : *BondList) {
          if (Bonds.IsValidIndex(BIdx)) {
            FBond &B = Bonds[BIdx];
            if (B.ItemA == ItemId) {
              B.Root = FHex(B.Root.Q + StepDir.Q, B.Root.R + StepDir.R,
                            B.Root.Level);
            }
          }
        }
      }
    }
    // UE_LOG(LogTemp, Warning,
    //        TEXT("  Helix Eject: Pre-push successful for %d items"),
    //        MovedItemIds.Num());
  }

  // --- EXECUTE EJECTION ---
  // UE_LOG(LogTemp, Warning,
  //        TEXT("Helix Eject: Executing ejection of rung with %d orbs"),
  //        OldestRung.Orbs.Num());

  TMap<int32, int32> RungIdxToItemId;

  // 1. Create individual items for each orb in the rung
  for (int32 i = 0; i < OldestRung.Orbs.Num(); ++i) {
    const FOrbData &OrbData = OldestRung.Orbs[i];
    FHex TargetHex(InputCenter.Q + OrbData.dQ, InputCenter.R + OrbData.dR,
                   InputCenter.Level);

    FOrb O;
    O.Type = (EOrbType)OrbData.Type;
    O.Color = (EOrbColor)OrbData.Color;

    FItem NewItem = FItem::MakeSingle(O);
    NewItem.Id = NextItemId++;
    NewItem.bIsReady = false;

    ItemMap.Add(TargetHex, NewItem);
    OrbAt.Add(TargetHex, (uint8)O.Type);
    RungIdxToItemId.Add(i, NewItem.Id);
    NewlyPlacedItems.Add(NewItem.Id);

    FEjectedItemInfo Info;
    Info.Center = Group.PairedInputCenterWorld;
    Info.StartZ = Group.MaxVisibleRungs * Group.RungSpacing;
    EjectedItems.Add(NewItem.Id, Info);
  }

  // 2. Create bonds between the new items
  for (const TPair<int32, int32> &BondIdxPair : OldestRung.Bonds) {
    if (const int32 *IdAPtr = RungIdxToItemId.Find(BondIdxPair.Key)) {
      if (const int32 *IdBPtr = RungIdxToItemId.Find(BondIdxPair.Value)) {
        const FOrbData &OrbA = OldestRung.Orbs[BondIdxPair.Key];
        const FOrbData &OrbB = OldestRung.Orbs[BondIdxPair.Value];

        FBond NewBond;
        NewBond.ItemA = *IdAPtr;
        NewBond.ItemB = *IdBPtr;
        NewBond.Root = FHex(InputCenter.Q + OrbA.dQ, InputCenter.R + OrbA.dR,
                            InputCenter.Level);
        NewBond.dQ = OrbB.dQ - OrbA.dQ;
        NewBond.dR = OrbB.dR - OrbA.dR;
        NewBond.bActive = true;

        int32 BondIdx = Bonds.Add(NewBond);
        ItemToBonds.FindOrAdd(*IdAPtr).Add(BondIdx);
        ItemToBonds.FindOrAdd(*IdBPtr).Add(BondIdx);
      }
    }
  }

  // 3. Create bonds to the BOARD
  for (const TPair<int32, int32> &BondPair : OldestRung.BondsToBoard) {
    if (const int32 *NewItemIdPtr = RungIdxToItemId.Find(BondPair.Key)) {
      int32 NewId = *NewItemIdPtr;
      int32 BoardOrbId = BondPair.Value;

      // Find partner position
      FHex BoardPos;
      bool bFound = false;
      for (const auto &KV : ItemMap) {
        if (KV.Value.Id == BoardOrbId ||
            (BoardOrbId > 1000000 && KV.Value.Id == (BoardOrbId >> 8))) {
          BoardPos = KV.Key;
          bFound = true;
          break;
        }
      }

      if (bFound) {
        const FOrbData &OrbData = OldestRung.Orbs[BondPair.Key];
        FHex TargetHex(InputCenter.Q + OrbData.dQ, InputCenter.R + OrbData.dR,
                       InputCenter.Level);

        if (HexUtil::Dist(BoardPos, TargetHex) <= 1) {
          FBond NewBond;
          NewBond.ItemA = NewId;
          NewBond.ItemB =
              (BoardOrbId > 1000000) ? (BoardOrbId >> 8) : BoardOrbId;
          NewBond.Root = TargetHex;
          NewBond.dQ = BoardPos.Q - TargetHex.Q;
          NewBond.dR = BoardPos.R - TargetHex.R;
          NewBond.bActive = true;

          int32 BondIdx = Bonds.Add(NewBond);
          ItemToBonds.FindOrAdd(NewBond.ItemA).Add(BondIdx);
          ItemToBonds.FindOrAdd(NewBond.ItemB).Add(BondIdx);
        }
      }
    }
  }

  // 4. Portal Behavior: Transfer bonds from the strand inside the helix
  if (Group.Rungs.Num() > 1) {
    FHelixRung &NextOldest = Group.Rungs[Group.Rungs.Num() - 2];
    for (int32 i = NextOldest.BondsToPrevious.Num() - 1; i >= 0; --i) {
      TPair<int32, int32> BondIdxPair = NextOldest.BondsToPrevious[i];
      if (const int32 *BoardItemIdPtr =
              RungIdxToItemId.Find(BondIdxPair.Value)) {
        NextOldest.BondsToBoard.AddUnique(
            TPair<int32, int32>(BondIdxPair.Key, *BoardItemIdPtr));
        NextOldest.BondsToPrevious.RemoveAt(i);
      }
    }
  }

  Group.Rungs.RemoveAt(Group.Rungs.Num() - 1);

  // visual interpolation tracking
  for (int32 i = 0; i < RungIdxToItemId.Num(); ++i) {
    if (const int32 *IdPtr = RungIdxToItemId.Find(i)) {
      FEjectedItemInfo Info;
      Info.Center = Group.PairedInputCenterWorld;
      // The oldest rung was at (MaxVisibleRungs-1) spacing before the gulp
      // shift. Since it was just removed, its ghost starts from that height.
      Info.StartZ = (Group.MaxVisibleRungs - 1) * Group.RungSpacing;
      EjectedItems.Add(*IdPtr, Info);
    }
  }

  return true;
}

void USimWorldHex::UpdateLockedItems() {
  TMap<int32, FItem *> IdToItem;
  TMap<FHex, int32> HexToParentId;
  for (auto &KV : ItemMap) {
    KV.Value.bIsLocked = false;
    IdToItem.Add(KV.Value.Id, &KV.Value);
    HexToParentId.Add(KV.Key, KV.Value.Id);
    if (KV.Value.bIsCluster) {
      for (const auto &Sub : KV.Value.Orbs) {
        HexToParentId.Add(
            FHex(KV.Key.Q + Sub.dQ, KV.Key.R + Sub.dR, KV.Key.Level),
            KV.Value.Id);
      }
    }
  }

  for (const FHelixOutputGroup &Group : HelixGroups) {
    if (!Group.bHasPairedInput)
      continue;

    TSet<int32> GroupLockedIds;
    TArray<int32> Stack;

    for (const FHex &EntryTile : Group.MemberTiles) {
      if (const int32 *FoundParentId = HexToParentId.Find(EntryTile)) {
        if (!GroupLockedIds.Contains(*FoundParentId)) {
          GroupLockedIds.Add(*FoundParentId);
          Stack.Add(*FoundParentId);
        }
      }
    }

    for (const FHelixRung &R : Group.Rungs) {
      for (const auto &Bond : R.BondsToBoard) {
        int32 ParentId =
            (Bond.Value > 1000000) ? (Bond.Value >> 8) : Bond.Value;
        if (!GroupLockedIds.Contains(ParentId)) {
          GroupLockedIds.Add(ParentId);
          Stack.Add(ParentId);
        }
      }
    }

    for (int32 StartId : Stack) {
      GroupLockedIds.Append(GetFullClusterParentIds(StartId));
    }

    for (int32 LockedId : GroupLockedIds) {
      if (FItem **ItemPtrPtr = IdToItem.Find(LockedId)) {
        (*ItemPtrPtr)->bIsLocked = true;
      }
    }
  }
}
