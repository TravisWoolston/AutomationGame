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
    // REMOVED: OrbsNewlyMovedToOutput check to allow continuous processing

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
            // Check if it's bonded to the previous rung - if so, that's okay!
            bool bBondedToPrevious = false;
            if (Group.Rungs.Num() > 0) {
              for (const FOrbData &PrevOrb : Group.Rungs[0].Orbs) {
                if (PrevOrb.ItemId == OtherId) {
                  bBondedToPrevious = true;
                  break;
                }
              }
            }

            if (!bBondedToPrevious) {
              return false;
            }
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
  // Set ZOffset to 0 for smooth transition from board orbs
  NewRung.ZOffset = 0.f;
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

  // New map for previous rung lookups
  TMap<int32, int32> PrevItemIdToOrbIndex;
  if (Group.Rungs.Num() > 0) {
    for (int32 i = 0; i < Group.Rungs[0].Orbs.Num(); ++i) {
      PrevItemIdToOrbIndex.Add(Group.Rungs[0].Orbs[i].ItemId, i);
    }
  }

  // Build bonds between orbs in this rung AND to previous rung
  for (const auto &OrbPair : ItemIdToOrbIndex) {
    int32 ItemId = OrbPair.Key;
    int32 IdxA = OrbPair.Value;

    // Check all IDs (including sub-orb IDs)
    // For simplicity, we check the base ItemId's bonds
    int32 BaseId = ItemId;
    // If it's a sub-orb ID, we need the base item ID to look up bonds in
    // ItemToBonds assuming sub-orbs don't have separate entries in ItemToBonds
    // (which is true) Wait, sub-orbs share the same ID in ItemToBonds usually?
    // Actually, ItemToBonds is at the FItem level.

    // Check if this is a sub-orb ID
    if (BaseId > 1000000) { // Simple heuristic for sub-orb IDs
      BaseId = BaseId >> 8;
    }

    if (const TArray<int32> *BondList = ItemToBonds.Find(BaseId)) {
      for (int32 BondIdx : *BondList) {
        if (!Bonds.IsValidIndex(BondIdx))
          continue;
        const FBond &Bond = Bonds[BondIdx];
        if (!Bond.bActive)
          continue;

        int32 OtherId = (Bond.ItemA == BaseId) ? Bond.ItemB : Bond.ItemA;

        // Handle connections to other orbs in the SAME new cluster
        // We need to verify if the other orb is actually part of THIS cluster's
        // sub-orbs if both are clusters For now, simpler check:
        if (ItemIdToOrbIndex.Contains(OtherId)) {
          int32 IdxB = ItemIdToOrbIndex[OtherId];
          if (IdxA < IdxB) {
            NewRung.Bonds.AddUnique(TPair<int32, int32>(IdxA, IdxB));
          }
        }
        // Handle connections to the PREVIOUS rung
        else if (PrevItemIdToOrbIndex.Contains(OtherId)) {
          int32 IdxB = PrevItemIdToOrbIndex[OtherId];
          NewRung.BondsToPrevious.AddUnique(TPair<int32, int32>(IdxA, IdxB));
        }
      }
    }
  }

  // Insert new rung at the beginning
  Group.Rungs.Insert(NewRung, 0);
  Group.TotalClustersProcessed++;
  Group.LastProcessedTick = StepCounter;

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

  float Alpha = GetFrameAlpha();

  for (FHelixOutputGroup &Group : HelixGroups) {
    if (bContinuous) {
      // Continuous mode: Constantly twist and rise using real DeltaTime
      // Find real DeltaTime by checking if we have a world
      float RealDeltaTime = DeltaTime;
      // If called from FactoryActor with scaled time, we might want to un-scale
      // it but for continuous rotation it's fine.

      float StepSec = FixedStepSec > 0.001f ? FixedStepSec : 0.5f;
      float RiseRate = Group.RungSpacing / StepSec;
      float TwistRate = Group.DegreesPerRung / StepSec;

      Group.HelixRotationOffset += TwistRate * RealDeltaTime;
      if (Group.HelixRotationOffset > 360.f) {
        Group.HelixRotationOffset -= 360.f;
      }

      for (FHelixRung &Rung : Group.Rungs) {
        Rung.ZOffset += RiseRate * RealDeltaTime;
        Rung.TargetZOffset = Rung.ZOffset;
      }
    } else {
      // Non-continuous: SYNCED with FrameAlpha (0 to 1)
      // This ensures it finishes exactly when the next tick arrives.

      // Animate rotation offset back to 0
      // If we processed this tick, we go from -DegreesPerRung to 0
      if (Group.LastProcessedTick == StepCounter) {
        Group.HelixRotationOffset = -Group.DegreesPerRung * (1.0f - Alpha);
      } else {
        Group.HelixRotationOffset = 0.f;
      }

      // Elevate rungs
      for (FHelixRung &Rung : Group.Rungs) {
        // If this group was processed this tick, it's animating from OldTarget
        // to NewTarget Since NewTarget = OldTarget + Spacing, we can say:
        // Position = NewTarget - Spacing * (1.0 - Alpha)
        if (Group.LastProcessedTick == StepCounter) {
          Rung.ZOffset =
              Rung.TargetZOffset - Group.RungSpacing * (1.0f - Alpha);
        } else {
          Rung.ZOffset = Rung.TargetZOffset;
        }
      }
    }
  }
}
