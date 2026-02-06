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

  // Recalculate input center
  FVector AvgPos = FVector::ZeroVector;
  for (const FHex &InputHex : Group.PairedInputTiles) {
    AvgPos += HexCenterToWorld(InputHex);
  }
  AvgPos /= Group.PairedInputTiles.Num();

  // Find center-most hex
  FHex BestHex = Group.PairedInputTiles[0];
  float BestDist = FLT_MAX;
  for (const FHex &H : Group.PairedInputTiles) {
    float Dist = FVector::DistSquared(HexCenterToWorld(H), AvgPos);
    if (Dist < BestDist) {
      BestDist = Dist;
      BestHex = H;
    }
  }
  Group.PairedInputCenter = BestHex;
  Group.PairedInputCenterWorld = HexCenterToWorld(Group.PairedInputCenter);
}

void USimWorldHex::UnregisterHelixInputTile(const FHex &Hex) {
  for (FHelixOutputGroup &Group : HelixGroups) {
    int32 TileIdx = Group.PairedInputTiles.Find(Hex);
    if (TileIdx != INDEX_NONE) {
      Group.PairedInputTiles.RemoveAt(TileIdx);

      if (Group.PairedInputTiles.Num() == 0) {
        Group.bHasPairedInput = false;
        Group.PairedInputCenter = FHex(0, 0, 0);
        Group.PairedInputCenterWorld = FVector::ZeroVector;
      } else {
        // Recalculate input center
        FVector AvgPos = FVector::ZeroVector;
        for (const FHex &InputHex : Group.PairedInputTiles) {
          AvgPos += HexCenterToWorld(InputHex);
        }
        AvgPos /= Group.PairedInputTiles.Num();

        FHex BestHex = Group.PairedInputTiles[0];
        float BestDist = FLT_MAX;
        for (const FHex &H : Group.PairedInputTiles) {
          float Dist = FVector::DistSquared(HexCenterToWorld(H), AvgPos);
          if (Dist < BestDist) {
            BestDist = Dist;
            BestHex = H;
          }
        }
        Group.PairedInputCenter = BestHex;
        Group.PairedInputCenterWorld =
            HexCenterToWorld(Group.PairedInputCenter);
      }
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

  // Collect orbs that ARE on helix tiles and ARE ready (not newly
  // placed/moved)
  TSet<int32> UniqueItemIds;
  TArray<const FItem *> ItemsToProcess;
  TSet<int32> OrbIdSet;          // All unique item IDs in this group
  TMap<int32, FHex> ItemIdToHex; // Map of item ID to its hex on the board

  for (const FHex &TileHex : Group.MemberTiles) {
    const FItem *TileItem = ItemMap.Find(TileHex);
    if (!TileItem) {
      continue;
    }

    if (!UniqueItemIds.Contains(TileItem->Id)) {
      UniqueItemIds.Add(TileItem->Id);
      ItemsToProcess.Add(TileItem);
      OrbIdSet.Add(TileItem->Id);

      // CRITICAL: Find the ROOT hex of this item if it's a cluster
      FHex RootHex = TileHex;
      if (TileItem->bIsCluster) {
        bool bFoundRoot = false;
        for (const FOrbData &Sub : TileItem->Orbs) {
          FHex PotentialRoot(TileHex.Q - Sub.dQ, TileHex.R - Sub.dR,
                             TileHex.Level);
          if (const FItem *RootItem = ItemMap.Find(PotentialRoot)) {
            if (RootItem->Id == TileItem->Id) {
              RootHex = PotentialRoot;
              bFoundRoot = true;
              break;
            }
          }
        }
      }
      ItemIdToHex.Add(TileItem->Id, RootHex);
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

  // Integrity Check: cluster must be fully on helix group (no bonds to
  // outside)
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

  // PRESSURE-BASED EJECTION:
  // If the helix is full, we MUST eject the oldest cluster to make room.
  // If ejection is blocked by an obstacle, the entire entry sequence is
  // blocked.
  if (Group.Rungs.Num() >= Group.MaxVisibleRungs) {
    if (!TryEjectHelixCluster(Group, true)) {
      return false; // Entrance blocked by exit obstruction
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
  NewRung.SpawnTick = StepCounter; // Track tick for transit timing
  // Set ZOffset to 0 for smooth transition from board orbs
  NewRung.ZOffset = 0.f;
  NewRung.TargetZOffset = Group.RungSpacing;
  NewRung.RotationDegrees = 0.f;

  // Detect entry direction - find where items came from
  // Look for the direction from a non-output-helix tile toward the helix center
  TSet<FHex> OutputTileSet;
  for (const FHex &MemberTile : Group.MemberTiles) {
    OutputTileSet.Add(MemberTile);
  }

  // Find the entry direction by checking which adjacent belt tile points INTO
  // the helix (the direction items traveled when entering).
  // We check ALL member tiles for an incoming belt to be robust.
  uint8 DetectedEntryDir = 0;
  bool bFoundEntryDir = false;

  for (const FHex &HelixTile : Group.MemberTiles) {
    if (bFoundEntryDir)
      break;
    for (uint8 Dir = 0; Dir < 6; ++Dir) {
      FHex NeighborHex = HexUtil::Neighbor(HelixTile, (EHexDir)Dir);
      if (OutputTileSet.Contains(NeighborHex))
        continue;

      if (const FTile *NeighborTile = TileMap.Find(NeighborHex)) {
        if (NeighborTile->Kind == ETileKind::Belt) {
          uint8 BeltDir = NeighborTile->Arg;
          FHex BeltDest = HexUtil::Neighbor(NeighborHex, (EHexDir)BeltDir);
          if (BeltDest == HelixTile) {
            DetectedEntryDir = BeltDir;
            bFoundEntryDir = true;
            UE_LOG(LogTemp, Warning,
                   TEXT("Helix Entry: Found belt at (%d,%d) pointing dir %d "
                        "into helix tile (%d,%d), EntryDir = %d"),
                   NeighborHex.Q, NeighborHex.R, BeltDir, HelixTile.Q,
                   HelixTile.R, DetectedEntryDir);
            break;
          }
        }
      }
    }
  }

  // Fallback: if no belt found, look for an open edge from the first item
  if (!bFoundEntryDir && ItemsToProcess.Num() > 0) {
    const FItem *FirstItem = ItemsToProcess[0];
    if (const FHex *FirstHex = ItemIdToHex.Find(FirstItem->Id)) {
      for (uint8 Dir = 0; Dir < 6; ++Dir) {
        FHex NeighborHex = HexUtil::Neighbor(*FirstHex, (EHexDir)Dir);
        if (!OutputTileSet.Contains(NeighborHex)) {
          // Items came FROM Neighbor TO FirstHex, so travel direction is
          // OPPOSITE of Dir
          DetectedEntryDir = (Dir + 3) % 6;
          bFoundEntryDir = true;
          UE_LOG(LogTemp, Warning,
                 TEXT("Helix Entry: Fallback - open edge at dir %d, assuming "
                      "EntryDir = %d"),
                 Dir, DetectedEntryDir);
          break;
        }
      }
    }
  }

  NewRung.EntryDirection = DetectedEntryDir;
  UE_LOG(LogTemp, Warning, TEXT("Helix: New rung stored EntryDirection = %d"),
         DetectedEntryDir);

  const FHex &CenterHex = Group.CenterHex;
  TMap<int32, int32> ItemIdToOrbIndex;

  // Track all board orbs to find bonds to the board
  TMap<int32, FHex> BoardItemIdToHex;
  for (const auto &KV : ItemMap) {
    BoardItemIdToHex.Add(KV.Value.Id, KV.Key);
  }

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

  // Maps for previous rung lookups
  TMap<int32, int32> PrevItemIdToOrbIndex;
  TMap<TPair<int32, int32>, int32> PrevPosToOrbIndex;
  if (Group.Rungs.Num() > 0) {
    const FHelixRung &PrevRung = Group.Rungs[0];
    for (int32 i = 0; i < PrevRung.Orbs.Num(); ++i) {
      const FOrbData &O = PrevRung.Orbs[i];
      PrevItemIdToOrbIndex.Add(O.ItemId, i);
      PrevPosToOrbIndex.Add(TPair<int32, int32>(O.dQ, O.dR), i);
    }
  }

  // Build lookup for orbs in the NEW rung
  TMap<TPair<int32, int32>, int32> NewPosToOrbIndex;
  for (int32 i = 0; i < NewRung.Orbs.Num(); ++i) {
    NewPosToOrbIndex.Add(
        TPair<int32, int32>(NewRung.Orbs[i].dQ, NewRung.Orbs[i].dR), i);
  }

  // Build bonds between orbs in this rung AND to previous rung
  for (int32 IdxA = 0; IdxA < NewRung.Orbs.Num(); ++IdxA) {
    const FOrbData &OrbA = NewRung.Orbs[IdxA];
    int32 ItemId = OrbA.ItemId;

    // Check BOTH the specific sub-orb ID and the parent cluster ID for bonds
    TArray<int32> CheckIds;
    CheckIds.Add(ItemId);
    if (ItemId > 1000000) {
      CheckIds.Add(ItemId >> 8);
    }

    for (int32 BaseId : CheckIds) {
      if (const TArray<int32> *BondList = ItemToBonds.Find(BaseId)) {
        for (int32 BondIdx : *BondList) {
          if (!Bonds.IsValidIndex(BondIdx))
            continue;
          const FBond &Bond = Bonds[BondIdx];
          if (!Bond.bActive)
            continue;

          int32 OtherId = (Bond.ItemA == BaseId) ? Bond.ItemB : Bond.ItemA;

          // Determine the target coordinate in the helix spatial system
          int32 TargetDQ = (Bond.ItemA == BaseId) ? (OrbA.dQ + Bond.dQ)
                                                  : (OrbA.dQ - Bond.dQ);
          int32 TargetDR = (Bond.ItemA == BaseId) ? (OrbA.dR + Bond.dR)
                                                  : (OrbA.dR - Bond.dR);
          TPair<int32, int32> TargetPos(TargetDQ, TargetDR);

          // 1. Handle connections to other orbs in the SAME new rung
          if (const int32 *FoundIdxPtr = NewPosToOrbIndex.Find(TargetPos)) {
            int32 IdxB = *FoundIdxPtr;
            if (IdxA < IdxB) {
              NewRung.Bonds.AddUnique(TPair<int32, int32>(IdxA, IdxB));
            }
          }

          // 2. Handle connections to the PREVIOUS rung
          if (Group.Rungs.Num() > 0) {
            if (const int32 *FoundPrevIdxPtr =
                    PrevPosToOrbIndex.Find(TargetPos)) {
              int32 IdxB = *FoundPrevIdxPtr;
              NewRung.BondsToPrevious.AddUnique(
                  TPair<int32, int32>(IdxA, IdxB));
            }
          }

          // 3. Handle connections to the BOARD
          if (BoardItemIdToHex.Contains(OtherId)) {
            FHex TargetBoardHex(CenterHex.Q + TargetDQ, CenterHex.R + TargetDR,
                                CenterHex.Level);
            if (const FItem *BI = ItemMap.Find(TargetBoardHex)) {
              if (BI->Id == OtherId ||
                  (OtherId > 1000000 && BI->Id == (OtherId >> 8))) {
                NewRung.BondsToBoard.AddUnique(
                    TPair<int32, int32>(IdxA, OtherId));
              }
            }
          }
        }
      }
    }
  }

  // 4. Ensure internal connectivity for the new rung
  for (int32 i = 0; i < NewRung.Orbs.Num(); ++i) {
    FHex PosI(NewRung.Orbs[i].dQ, NewRung.Orbs[i].dR, 0);
    for (int32 j = i + 1; j < NewRung.Orbs.Num(); ++j) {
      FHex PosJ(NewRung.Orbs[j].dQ, NewRung.Orbs[j].dR, 0);
      if (HexUtil::Dist(PosI, PosJ) == 1) {
        NewRung.Bonds.AddUnique(TPair<int32, int32>(i, j));
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
      const FHex &RootHex = *HexPtr;

      // Deactivate board bonds if the partner is also in the helix
      if (const TArray<int32> *BondList = ItemToBonds.Find(Item->Id)) {
        for (int32 BondIdx : *BondList) {
          if (Bonds.IsValidIndex(BondIdx)) {
            FBond &B = Bonds[BondIdx];
            int32 OtherId = (B.ItemA == Item->Id) ? B.ItemB : B.ItemA;

            // If other item is also in this group or already in the helix
            if (OrbIdSet.Contains(OtherId) || IsItemInHelix(OtherId)) {
              B.bActive = false;
            }
          }
        }
      }

      // Remove ALL orbs of this item from the board
      if (Item->bIsCluster) {
        for (const FOrbData &Sub : Item->Orbs) {
          FHex SubHex(RootHex.Q + Sub.dQ, RootHex.R + Sub.dR, RootHex.Level);
          ItemMap.Remove(SubHex);
          OrbAt.Remove(SubHex);
        }
      } else {
        ItemMap.Remove(RootHex);
        OrbAt.Remove(RootHex);
      }
    }
  }

  // Limit max rungs
  while (Group.Rungs.Num() > Group.MaxVisibleRungs) {
    Group.Rungs.RemoveAt(Group.Rungs.Num() - 1);
  }

  // Auto-bonding: if this item entered on sequential tick after last item,
  // auto-bond them together to create continuous clusters
  if (ItemsToProcess.Num() > 0) {
    const FItem *FirstItem = ItemsToProcess[0];

    // Check if we should auto-bond to the previous item
    if (Group.LastEnteredItemId > 0 &&
        Group.LastEnteredItemTick == StepCounter - 1 && Group.Rungs.Num() > 1) {
      FHelixRung &CurrentRung = Group.Rungs[0];
      const FHelixRung &PrevRung = Group.Rungs[1];

      // Use a set to identify which orbs in current rung are already connected
      // to the previous rung via original board bonds (BondsToPrevious already
      // populated in Step 2 above)
      TSet<int32> AlreadyBondedOrbs;
      for (const auto &B : CurrentRung.BondsToPrevious) {
        AlreadyBondedOrbs.Add(B.Key);
      }

      // If NO orbs are connected to the previous rung, add ONE fallback bond
      // (the "DNA strand" link)
      if (AlreadyBondedOrbs.Num() == 0) {
        bool bBonded = false;
        for (int32 CurrIdx = 0; CurrIdx < CurrentRung.Orbs.Num() && !bBonded;
             ++CurrIdx) {
          FHex CurrHex(CurrentRung.Orbs[CurrIdx].dQ,
                       CurrentRung.Orbs[CurrIdx].dR, 0);
          for (int32 PrevIdx = 0; PrevIdx < PrevRung.Orbs.Num(); ++PrevIdx) {
            FHex PrevHex(PrevRung.Orbs[PrevIdx].dQ, PrevRung.Orbs[PrevIdx].dR,
                         0);
            if (HexUtil::Dist(CurrHex, PrevHex) <= 1) {
              CurrentRung.BondsToPrevious.AddUnique(
                  TPair<int32, int32>(CurrIdx, PrevIdx));
              bBonded = true;
              break;
            }
          }
        }
      }
    }

    // Update tracking for next iteration
    Group.LastEnteredItemId = FirstItem->Id;
    Group.LastEnteredItemTick = StepCounter;
  }

  return true;
}

void USimWorldHex::UpdateHelixGroups(float DeltaTime) {
  float Alpha = GetFrameAlpha();

  for (FHelixOutputGroup &Group : HelixGroups) {
    // Non-continuous: SYNCED with FrameAlpha (0 to 1)
    if (Group.LastProcessedTick == StepCounter) {
      Group.HelixRotationOffset = -Group.DegreesPerRung * (1.0f - Alpha);
    } else {
      Group.HelixRotationOffset = 0.f;
    }

    // Elevate rungs
    for (FHelixRung &Rung : Group.Rungs) {
      if (Group.LastProcessedTick == StepCounter) {
        Rung.ZOffset = Rung.TargetZOffset - Group.RungSpacing * (1.0f - Alpha);
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

  UE_LOG(LogTemp, Warning,
         TEXT("Helix Eject: OutputCenter=(%d,%d), InputCenter=(%d,%d), "
              "PortalExitDir=%d, StepDir=(%d,%d)"),
         Group.CenterHex.Q, Group.CenterHex.R, InputCenter.Q, InputCenter.R,
         PortalExitDir, StepDir.Q, StepDir.R);
  UE_LOG(LogTemp, Warning, TEXT("  InputTiles count=%d, Rung orbs count=%d"),
         Group.PairedInputTiles.Num(), OldestRung.Orbs.Num());
  for (int32 i = 0; i < OldestRung.Orbs.Num(); ++i) {
    const FOrbData &Orb = OldestRung.Orbs[i];
    FHex TargetHex(InputCenter.Q + Orb.dQ, InputCenter.R + Orb.dR,
                   InputCenter.Level);
    UE_LOG(LogTemp, Warning,
           TEXT("  Orb[%d]: dQ=%d, dR=%d -> TargetHex=(%d,%d)"), i, Orb.dQ,
           Orb.dR, TargetHex.Q, TargetHex.R);
  }
  for (int32 i = 0; i < Group.PairedInputTiles.Num(); ++i) {
    UE_LOG(LogTemp, Warning, TEXT("  InputTile[%d]: (%d,%d)"), i,
           Group.PairedInputTiles[i].Q, Group.PairedInputTiles[i].R);
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
    UE_LOG(
        LogTemp, Warning,
        TEXT("Helix Eject: %d items blocking input tiles, pushing them first"),
        BlockingItemIds.Num());

    // Find ALL bonded items to the blocking items - they must all move together
    TSet<int32> ItemsToPrePush = BlockingItemIds;
    TArray<int32> Stack;
    for (int32 Id : BlockingItemIds) {
      Stack.Add(Id);
    }

    while (Stack.Num() > 0) {
      int32 CurrentId = Stack.Pop();
      if (const TArray<int32> *BondList = ItemToBonds.Find(CurrentId)) {
        for (int32 BondIdx : *BondList) {
          if (!Bonds.IsValidIndex(BondIdx))
            continue;
          const FBond &Bond = Bonds[BondIdx];
          if (!Bond.bActive)
            continue;

          int32 OtherId = (Bond.ItemA == CurrentId) ? Bond.ItemB : Bond.ItemA;
          if (OtherId > 0 && !ItemsToPrePush.Contains(OtherId)) {
            ItemsToPrePush.Add(OtherId);
            Stack.Add(OtherId);
          }
        }
      }
    }

    // Collect positions of all items to pre-push
    TMap<FHex, FItem> PrePushItems;
    for (const auto &KV : ItemMap) {
      if (ItemsToPrePush.Contains(KV.Value.Id)) {
        PrePushItems.Add(KV.Key, KV.Value);
      }
    }

    // Check if we CAN push (destination must be clear or another pre-push item
    // leaving). If we find blockers, add them to the push set and restart.
    bool bCanPrePush = true;
    int32 MaxIterations = 50;
    for (int32 Iteration = 0; Iteration < MaxIterations; ++Iteration) {
      bool bNeedRestart = false;

      for (const auto &KV : PrePushItems) {
        FHex PushDest(KV.Key.Q + StepDir.Q, KV.Key.R + StepDir.R, KV.Key.Level);

        // Skip if destination is another pre-push item (internal move)
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
          // Check if this blocking item is already in our push set
          if (!ItemsToPrePush.Contains(BlockingItem->Id)) {
            // Add this blocking item and all its bonded items to the push set
            TArray<int32> BlockerStack;
            BlockerStack.Add(BlockingItem->Id);
            while (BlockerStack.Num() > 0) {
              int32 BlockerId = BlockerStack.Pop();
              if (ItemsToPrePush.Contains(BlockerId))
                continue;
              ItemsToPrePush.Add(BlockerId);

              if (const TArray<int32> *BondList = ItemToBonds.Find(BlockerId)) {
                for (int32 BondIdx : *BondList) {
                  if (!Bonds.IsValidIndex(BondIdx))
                    continue;
                  const FBond &Bond = Bonds[BondIdx];
                  if (!Bond.bActive)
                    continue;
                  int32 OtherId =
                      (Bond.ItemA == BlockerId) ? Bond.ItemB : Bond.ItemA;
                  if (OtherId > 0 && !ItemsToPrePush.Contains(OtherId)) {
                    BlockerStack.Add(OtherId);
                  }
                }
              }
            }
            // Need to recalculate PrePushItems and restart
            PrePushItems.Empty();
            for (const auto &KV2 : ItemMap) {
              if (ItemsToPrePush.Contains(KV2.Value.Id)) {
                PrePushItems.Add(KV2.Key, KV2.Value);
              }
            }
            // Safety check
            if (ItemsToPrePush.Num() > 200) {
              bCanPrePush = false;
              UE_LOG(LogTemp, Warning,
                     TEXT("  Cannot pre-push: too many items (%d)"),
                     ItemsToPrePush.Num());
              break;
            }
            UE_LOG(LogTemp, Warning,
                   TEXT("  Expanded pre-push to %d items, including blocker at "
                        "(%d,%d)"),
                   ItemsToPrePush.Num(), PushDest.Q, PushDest.R);
            bNeedRestart = true;
            break; // Break inner loop to restart
          }
        }

        // Check tile type
        if (const FTile *DestTile = TileMap.Find(PushDest)) {
          if (DestTile->Kind == ETileKind::Output ||
              DestTile->Kind == ETileKind::HelixOutput) {
            bCanPrePush = false;
            UE_LOG(LogTemp, Warning,
                   TEXT("  Cannot pre-push: blocked by tile at (%d,%d)"),
                   PushDest.Q, PushDest.R);
            break;
          }
        }
      }

      if (!bCanPrePush)
        break;
      if (!bNeedRestart)
        break; // Done - no more expansion needed
    }

    if (!bCanPrePush) {
      // Can't push, can't eject
      return false;
    }

    // Execute pre-push
    for (const auto &KV : PrePushItems) {
      ItemMap.Remove(KV.Key);
      OrbAt.Remove(KV.Key);
    }

    for (const auto &KV : PrePushItems) {
      FHex NewPos(KV.Key.Q + StepDir.Q, KV.Key.R + StepDir.R, KV.Key.Level);
      FItem MovedItem = KV.Value;

      ItemMap.Add(NewPos, MovedItem);
      OrbAt.Add(NewPos, (uint8)MovedItem.Single.Type);

      // Update bonds
      if (const TArray<int32> *BondList = ItemToBonds.Find(MovedItem.Id)) {
        for (int32 BondIdx : *BondList) {
          if (Bonds.IsValidIndex(BondIdx)) {
            FBond &B = Bonds[BondIdx];
            if (B.ItemA == MovedItem.Id) {
              B.Root = NewPos;
            }
          }
        }
      }

      OrbsNewlyMovedByBelts.Add(MovedItem.Id);
      UE_LOG(LogTemp, Warning, TEXT("  Pre-pushed item %d: (%d,%d) -> (%d,%d)"),
             MovedItem.Id, KV.Key.Q, KV.Key.R, NewPos.Q, NewPos.R);
    }
  }

  // Map to track newly created item IDs for bond creation
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
    NewItem.bIsReady = false; // Give it one tick to settle

    ItemMap.Add(TargetHex, NewItem);
    OrbAt.Add(TargetHex, (uint8)O.Type);

    RungIdxToItemId.Add(i, NewItem.Id);

    // Mark as newly placed and ejected for smooth visuals
    NewlyPlacedItems.Add(NewItem.Id);

    FEjectedItemInfo Info;
    Info.Center = Group.PairedInputCenterWorld;
    Info.StartZ = Group.MaxVisibleRungs * Group.RungSpacing;
    EjectedItems.Add(NewItem.Id, Info);
  }

  // 2. Create bonds between the new items based on the rung's internal bond
  // structure
  for (const TPair<int32, int32> &BondIdxPair : OldestRung.Bonds) {
    if (const int32 *IdAPtr = RungIdxToItemId.Find(BondIdxPair.Key)) {
      if (const int32 *IdBPtr = RungIdxToItemId.Find(BondIdxPair.Value)) {
        int32 IdA = *IdAPtr;
        int32 IdB = *IdBPtr;

        const FOrbData &OrbA = OldestRung.Orbs[BondIdxPair.Key];
        const FOrbData &OrbB = OldestRung.Orbs[BondIdxPair.Value];

        FBond NewBond;
        NewBond.ItemA = IdA;
        NewBond.ItemB = IdB;
        NewBond.Root = FHex(InputCenter.Q + OrbA.dQ, InputCenter.R + OrbA.dR,
                            InputCenter.Level);
        NewBond.dQ = OrbB.dQ - OrbA.dQ;
        NewBond.dR = OrbB.dR - OrbA.dR;
        NewBond.bActive = true;

        int32 BondIdx = Bonds.Add(NewBond);
        ItemToBonds.FindOrAdd(IdA).Add(BondIdx);
        ItemToBonds.FindOrAdd(IdB).Add(BondIdx);
      }
    }
  }

  // 3. Create bonds to the BOARD (previously ejected clusters or existing board
  // items)
  for (const TPair<int32, int32> &BondPair : OldestRung.BondsToBoard) {
    if (const int32 *NewItemIdPtr = RungIdxToItemId.Find(BondPair.Key)) {
      int32 NewId = *NewItemIdPtr;
      int32 BoardId = BondPair.Value;

      // Find the board partner item
      FHex BoardPos;
      bool bFound = false;
      for (const auto &KV : ItemMap) {
        if (KV.Value.Id == BoardId ||
            (BoardId > 1000000 && KV.Value.Id == (BoardId >> 8))) {
          BoardPos = KV.Key;
          bFound = true;
          break;
        }
      }

      if (bFound) {
        const FOrbData &OrbData = OldestRung.Orbs[BondPair.Key];
        FHex TargetHex(InputCenter.Q + OrbData.dQ, InputCenter.R + OrbData.dR,
                       InputCenter.Level);

        FBond NewBond;
        NewBond.ItemA = NewId;
        NewBond.ItemB = BoardId;
        NewBond.Root = TargetHex;
        NewBond.dQ = BoardPos.Q - TargetHex.Q;
        NewBond.dR = BoardPos.R - TargetHex.R;
        NewBond.bActive = true;

        int32 BondIdx = Bonds.Add(NewBond);
        ItemToBonds.FindOrAdd(NewId).Add(BondIdx);
        ItemToBonds.FindOrAdd(BoardId).Add(BondIdx);

        UE_LOG(LogTemp, Warning,
               TEXT("Helix Eject: Created board bond between NewItem %d and "
                    "BoardItem %d"),
               NewId, BoardId);
      }
    }
  }

  // Items stay on input tiles after placement
  // Pre-push logic above already pushed any blocking items before we placed
  // these new ones
  UE_LOG(LogTemp, Warning, TEXT("Helix Eject: Placed %d items on input tiles"),
         OldestRung.Orbs.Num());

  // 3. PORTAL BEHAVIOR: Handle bonds from the NEXT rung in the helix to this
  // ejected set
  if (Group.Rungs.Num() > 1) {
    FHelixRung &NextOldest = Group.Rungs[Group.Rungs.Num() - 2];
    for (int32 i = NextOldest.BondsToPrevious.Num() - 1; i >= 0; --i) {
      TPair<int32, int32> BondIdxPair = NextOldest.BondsToPrevious[i];
      // Value is the orb index in the rung being ejected (OldestRung)
      if (const int32 *BoardItemIdPtr =
              RungIdxToItemId.Find(BondIdxPair.Value)) {
        // Move the bond from "Previous Rung" to "Board Item"
        NextOldest.BondsToBoard.AddUnique(
            TPair<int32, int32>(BondIdxPair.Key, *BoardItemIdPtr));
        NextOldest.BondsToPrevious.RemoveAt(i);
      }
    }
  }

  // Remove the ejected rung from the helix
  Group.Rungs.RemoveAt(Group.Rungs.Num() - 1);

  return true;
}

void USimWorldHex::UpdateLockedItems() {
  // 1. Reset all locks across the board and build a lookup map
  TMap<int32, FItem *> IdToItem;
  for (auto &KV : ItemMap) {
    KV.Value.bIsLocked = false;
    IdToItem.Add(KV.Value.Id, &KV.Value);
  }

  // 2. Identify and propagate locks for each helix portal
  for (const FHelixOutputGroup &Group : HelixGroups) {
    if (!Group.bHasPairedInput)
      continue;

    TSet<int32> GroupLockedIds;
    TArray<int32> Stack;

    // Anything currently on a HelixInput tile is an anchor point
    for (const FHex &InputTile : Group.PairedInputTiles) {
      if (FItem *Item = ItemMap.Find(InputTile)) {
        if (!GroupLockedIds.Contains(Item->Id)) {
          GroupLockedIds.Add(Item->Id);
          Stack.Add(Item->Id);
        }
      }
    }

    // BFS to find the whole cluster (respecting cluster/bond logic)
    while (Stack.Num() > 0) {
      int32 CurrentItemId = Stack.Pop();
      FItem **ItemPtrPtr = IdToItem.Find(CurrentItemId);
      if (!ItemPtrPtr)
        continue;
      FItem *CurrentItem = *ItemPtrPtr;

      // Collect IDs of this item and its sub-orbs
      TArray<int32> AllOrbsInItem;
      AllOrbsInItem.Add(CurrentItemId);
      if (CurrentItem->bIsCluster) {
        for (const auto &Sub : CurrentItem->Orbs)
          AllOrbsInItem.Add(Sub.ItemId);
      }

      // Check all bonds from these orbs
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
              if (!GroupLockedIds.Contains(OtherParentId)) {
                GroupLockedIds.Add(OtherParentId);
                Stack.Add(OtherParentId);
              }
            }
          }
        }
      }
    }

    // Apply the locks to the actual items in the lookup map
    for (int32 LockedId : GroupLockedIds) {
      if (FItem **ItemPtrPtr = IdToItem.Find(LockedId)) {
        (*ItemPtrPtr)->bIsLocked = true;
      }
    }
  }
}
