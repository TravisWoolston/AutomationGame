#include "EngineUtils.h"
#include "Framework/GameManager.h"
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
    if (Tiles.Num() == 0)
      return FHex(0, 0, 0);
    if (Tiles.Num() == 1)
      return Tiles[0];

    // Calculate average world position
    FVector AvgPos = FVector::ZeroVector;
    for (const FHex &H : Tiles) {
      AvgPos += HexCenterToWorld(H);
    }
    AvgPos /= Tiles.Num();

    // Find tile closest to average
    FHex BestHex = Tiles[0];
    float BestDist = FLT_MAX;
    for (const FHex &H : Tiles) {
      float Dist = FVector::DistSquared(HexCenterToWorld(H), AvgPos);
      if (Dist < BestDist) {
        BestDist = Dist;
        BestHex = H;
      }
    }
    return BestHex;
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
        // Recalculate center - find center-most hex
        FVector AvgPos = FVector::ZeroVector;
        for (const FHex &MemberHex : Group.MemberTiles) {
          AvgPos += HexCenterToWorld(MemberHex);
        }
        AvgPos /= Group.MemberTiles.Num();

        FHex BestHex = Group.MemberTiles[0];
        float BestDist = FLT_MAX;
        for (const FHex &H : Group.MemberTiles) {
          float Dist = FVector::DistSquared(HexCenterToWorld(H), AvgPos);
          if (Dist < BestDist) {
            BestDist = Dist;
            BestHex = H;
          }
        }
        Group.CenterHex = BestHex;
        Group.CenterWorld = HexCenterToWorld(Group.CenterHex);
      }
      return;
    }
  }
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

bool USimWorldHex::TryProcessHelixCluster(FHelixOutputGroup &Group) {
  if (Group.MemberTiles.Num() == 0) {
    return false;
  }

  // Get helix settings from GameManager
  bool bRequiresFull = false;
  bool bRequiresComplete = false;
  if (UWorld *World = GetWorld()) {
    for (TActorIterator<AGameManager> It(World); It; ++It) {
      if (AGameManager *GM = *It) {
        bRequiresFull = GM->bHelixRequiresFullOccupancy;
        bRequiresComplete = GM->bHelixRequiresCompleteClusters;
        break;
      }
    }
  }

  // Collect orbs that ARE on helix tiles and ARE ready (not newly placed/moved)
  TSet<int32> UniqueItemIds;
  TArray<const FItem *> ItemsToProcess;
  TSet<int32> OrbIdSet;          // All unique item IDs in this group
  TMap<int32, FHex> ItemIdToHex; // Map of item ID to its hex on the board

  for (const FHex &TileHex : Group.MemberTiles) {
    // Check if an orb just arrived here - if so, wait a tick for it to 'settle'
    if (OrbsNewlyMovedToOutput.Contains(TileHex)) {
      return false;
    }

    const FItem *TileItem = ItemMap.Find(TileHex);
    if (!TileItem) {
      continue;
    }

    if (!UniqueItemIds.Contains(TileItem->Id)) {
      if (!TileItem->bIsReady) {
        return false;
      }

      UniqueItemIds.Add(TileItem->Id);
      ItemsToProcess.Add(TileItem);
      OrbIdSet.Add(TileItem->Id);
      ItemIdToHex.Add(TileItem->Id, TileHex);
    }
  }

  // Need at least one item to process
  if (UniqueItemIds.Num() == 0) {
    return false;
  }

  // Check full occupancy if requested
  int32 TotalOrbsFound = 0;
  for (const FItem *Item : ItemsToProcess) {
    TotalOrbsFound += Item->bIsCluster ? Item->Orbs.Num() : 1;
  }
  if (bRequiresFull && TotalOrbsFound < Group.MemberTiles.Num()) {
    return false;
  }

  // Integrity Check: cluster must be fully on helix group (no bonds to outside)
  if (bRequiresComplete) {
    for (int32 Id : UniqueItemIds) {
      if (const TArray<int32> *BondList = ItemToBonds.Find(Id)) {
        for (int32 BondIdx : *BondList) {
          if (!Bonds.IsValidIndex(BondIdx))
            continue;
          const FBond &Bond = Bonds[BondIdx];
          if (!Bond.bActive)
            continue;

          int32 OtherId = (Bond.ItemA == Id) ? Bond.ItemB : Bond.ItemA;
          if (!OrbIdSet.Contains(OtherId)) {
            return false;
          }
        }
      }
    }
  }

  // All conditions met! Process orbs into a rung.
  for (FHelixRung &Rung : Group.Rungs) {
    Rung.TargetZOffset += Group.RungSpacing;
    Rung.RotationDegrees += Group.DegreesPerRung;
  }
  Group.HelixRotationOffset -= Group.DegreesPerRung;

  FHelixRung NewRung;
  NewRung.SpawnTime = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;
  NewRung.ZOffset = -Group.RungSpacing;
  NewRung.TargetZOffset = 0.f;
  NewRung.RotationDegrees = 0.f;

  const FHex &CenterHex = Group.CenterHex;
  TMap<int32, int32> ItemIdToOrbIndex;

  for (const FItem *Item : ItemsToProcess) {
    FHex ItemHex;
    if (const FHex *FoundHex = ItemIdToHex.Find(Item->Id)) {
      ItemHex = *FoundHex;
    } else {
      continue;
    }

    if (!Item->bIsCluster) {
      // Single orb - use its actual type
      FOrbData OrbData;
      OrbData.Type = (uint8)Item->Single.Type;
      OrbData.Color = (uint8)Item->Single.Color;
      OrbData.dQ = ItemHex.Q - CenterHex.Q;
      OrbData.dR = ItemHex.R - CenterHex.R;
      OrbData.ItemId = Item->Id;

      int32 Index = NewRung.Orbs.Add(OrbData);
      ItemIdToOrbIndex.Add(Item->Id, Index);
    } else {
      // Cluster - iterate through sub-orbs and use THEIR types
      for (int32 i = 0; i < Item->Orbs.Num(); ++i) {
        const FOrbData &Sub = Item->Orbs[i];
        FOrbData NewOrbData;
        NewOrbData.Type = Sub.Type;   // Use the sub-orb's actual type
        NewOrbData.Color = Sub.Color; // Use the sub-orb's actual color
        NewOrbData.dQ = (ItemHex.Q + Sub.dQ) - CenterHex.Q;
        NewOrbData.dR = (ItemHex.R + Sub.dR) - CenterHex.R;
        NewOrbData.ItemId = MakeSubOrbId(Item->Id, i);

        int32 Index = NewRung.Orbs.Add(NewOrbData);
        ItemIdToOrbIndex.Add(NewOrbData.ItemId, Index);
      }
    }
  }

  // Build bonds between orbs in this rung
  for (int32 ItemId : OrbIdSet) {
    if (const TArray<int32> *BondList = ItemToBonds.Find(ItemId)) {
      for (int32 BondIdx : *BondList) {
        if (!Bonds.IsValidIndex(BondIdx))
          continue;
        const FBond &Bond = Bonds[BondIdx];
        if (!Bond.bActive)
          continue;

        int32 OtherId = (Bond.ItemA == ItemId) ? Bond.ItemB : Bond.ItemA;

        if (ItemIdToOrbIndex.Contains(ItemId) &&
            ItemIdToOrbIndex.Contains(OtherId)) {
          int32 IdxA = ItemIdToOrbIndex[ItemId];
          int32 IdxB = ItemIdToOrbIndex[OtherId];

          if (IdxA < IdxB) {
            NewRung.Bonds.Add(TPair<int32, int32>(IdxA, IdxB));
          }
        }
      }
    }
  }

  // Insert new rung at the beginning
  Group.Rungs.Insert(NewRung, 0);
  Group.TotalClustersProcessed++;

  // Remove processed orbs from the board
  for (const FItem *Item : ItemsToProcess) {
    if (const FHex *HexPtr = ItemIdToHex.Find(Item->Id)) {
      const FHex &TileHex = *HexPtr;

      // Deactivate bonds
      if (const TArray<int32> *BondList = ItemToBonds.Find(Item->Id)) {
        for (int32 BondIdx : *BondList) {
          if (Bonds.IsValidIndex(BondIdx)) {
            Bonds[BondIdx].bActive = false;
          }
        }
      }

      ItemMap.Remove(TileHex);
      OrbAt.Remove(TileHex);
    }
  }

  // Limit max rungs
  while (Group.Rungs.Num() > Group.MaxVisibleRungs) {
    Group.Rungs.RemoveAt(Group.Rungs.Num() - 1);
  }

  return true;
}

void USimWorldHex::UpdateHelixGroups(float DeltaTime) {
  // Find GameManager for settings
  bool bContinuous = false;
  if (UWorld *World = GetWorld()) {
    for (TActorIterator<AGameManager> It(World); It; ++It) {
      if (AGameManager *GM = *It) {
        bContinuous = GM->bContinuousHelixAnimation;
        break;
      }
    }
  }

  for (FHelixOutputGroup &Group : HelixGroups) {
    // Synchronize animation with simulation tick rate
    float StepSec = FixedStepSec > 0.001f ? FixedStepSec : 0.5f;
    float RiseRate = Group.RungSpacing / StepSec;     // Units per second
    float TwistRate = Group.DegreesPerRung / StepSec; // Degrees per second

    if (bContinuous) {
      // Continuous mode: Constantly twist and rise
      Group.HelixRotationOffset += TwistRate * DeltaTime;
      // Keep rotation in [0, 360) for safety
      if (Group.HelixRotationOffset > 360.f) {
        Group.HelixRotationOffset -= 360.f;
      }

      for (FHelixRung &Rung : Group.Rungs) {
        Rung.ZOffset += RiseRate * DeltaTime;
        Rung.TargetZOffset = Rung.ZOffset;
      }
    } else {
      // Non-continuous: Animate rotation debt back to 0
      // This creates the "Twist" effect only when a new rung is added
      if (Group.HelixRotationOffset < 0.f) {
        Group.HelixRotationOffset += TwistRate * DeltaTime;
        if (Group.HelixRotationOffset > 0.f) {
          Group.HelixRotationOffset = 0.f;
        }
      } else {
        // Clamp to 0 if positive (residue)
        Group.HelixRotationOffset = 0.f;
      }

      // Elevate rungs if below target
      for (FHelixRung &Rung : Group.Rungs) {
        if (Rung.ZOffset < Rung.TargetZOffset) {
          Rung.ZOffset = FMath::Min(Rung.ZOffset + RiseRate * DeltaTime,
                                    Rung.TargetZOffset);
        }
      }
    }
  }
}
