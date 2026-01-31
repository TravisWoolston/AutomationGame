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

  // Hardcoded settings as requested: no occupancy/completeness requirements
  const bool bRequiresFull = false;
  const bool bRequiresComplete = false;

  // Collect orbs that ARE on helix tiles and ARE ready (not newly placed/moved)
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
  NewRung.TargetZOffset = Group.RungSpacing;
  NewRung.RotationDegrees = 0.f;

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

    // Check all IDs (including sub-orb IDs)
    int32 BaseId = ItemId;
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

        // Determine the target coordinate in the helix spatial system
        // ItemA is at Root, ItemB is at Root + (dQ, dR)
        // If our current OrbA matches ItemA, other is at Pos + (dQ, dR)
        // If our current OrbA matches ItemB, other is at Pos - (dQ, dR)
        int32 TargetDQ =
            (Bond.ItemA == BaseId) ? (OrbA.dQ + Bond.dQ) : (OrbA.dQ - Bond.dQ);
        int32 TargetDR =
            (Bond.ItemA == BaseId) ? (OrbA.dR + Bond.dR) : (OrbA.dR - Bond.dR);
        TPair<int32, int32> TargetPos(TargetDQ, TargetDR);

        // 1. Handle connections to other orbs in the SAME new rung
        if (const int32 *FoundIdxPtr = NewPosToOrbIndex.Find(TargetPos)) {
          int32 IdxB = *FoundIdxPtr;
          if (IdxA < IdxB) {
            NewRung.Bonds.AddUnique(TPair<int32, int32>(IdxA, IdxB));
          }
        }
        // 2. Handle connections to the PREVIOUS rung
        else if (Group.Rungs.Num() > 0) {
          if (const int32 *FoundPrevIdxPtr =
                  PrevPosToOrbIndex.Find(TargetPos)) {
            int32 IdxB = *FoundPrevIdxPtr;
            NewRung.BondsToPrevious.AddUnique(TPair<int32, int32>(IdxA, IdxB));
          } else if (BoardItemIdToHex.Contains(OtherId)) {
            // 3. Handle connections to the BOARD
            // Verify if the other side of the bond is actually at this Board
            // hex
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
        // 3. Handle connections to the BOARD (if no previous rungs either)
        else if (BoardItemIdToHex.Contains(OtherId)) {
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

  // Also handle internal cluster structure bonds (orbs of the same Item)
  // For clusters, they are rigid on the board, so we replicate that in the
  // helix.
  for (const FItem *Item : ItemsToProcess) {
    if (Item->bIsCluster) {
      // Find all sub-orbs of this item in the new rung
      TArray<int32> InRungIndices;
      for (int32 i = 0; i < Item->Orbs.Num(); ++i) {
        int32 SubId = MakeSubOrbId(Item->Id, i);
        if (const int32 *IdxPtr = ItemIdToOrbIndex.Find(SubId)) {
          InRungIndices.Add(*IdxPtr);
        }
      }

      // Add horizontal bonds between adjacent sub-orbs
      for (int32 i = 0; i < InRungIndices.Num(); ++i) {
        for (int32 j = i + 1; j < InRungIndices.Num(); ++j) {
          const FOrbData &OA = NewRung.Orbs[InRungIndices[i]];
          const FOrbData &OB = NewRung.Orbs[InRungIndices[j]];

          FHex HexA(OA.dQ, OA.dR, 0);
          FHex HexB(OB.dQ, OB.dR, 0);
          if (HexUtil::Dist(HexA, HexB) == 1) {
            NewRung.Bonds.AddUnique(
                TPair<int32, int32>(InRungIndices[i], InRungIndices[j]));
          }
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
      const FHex &RootHex = *HexPtr;

      // Deactivate board bonds if the partner is also in the helix
      if (const TArray<int32> *BondList = ItemToBonds.Find(Item->Id)) {
        for (int32 BondIdx : *BondList) {
          if (Bonds.IsValidIndex(BondIdx)) {
            FBond &B = Bonds[BondIdx];
            int32 OtherId = (B.ItemA == Item->Id) ? B.ItemB : B.ItemA;

            // If other item is also in this group or already in the helix
            bool bInHelix = false;
            if (OrbIdSet.Contains(OtherId)) {
              bInHelix = true;
            } else {
              for (const FHelixOutputGroup &G : HelixGroups) {
                for (const FHelixRung &R : G.Rungs) {
                  for (const FOrbData &O : R.Orbs) {
                    if (O.ItemId == OtherId ||
                        (OtherId > 1000000 && O.ItemId == OtherId) ||
                        (O.ItemId > 1000000 && (O.ItemId >> 8) == OtherId)) {
                      bInHelix = true;
                      break;
                    }
                  }
                  if (bInHelix)
                    break;
                }
                if (bInHelix)
                  break;
              }
            }

            if (bInHelix) {
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

    // ALWAYS run bond cleanup and management
    if (Group.LastProcessedTick == StepCounter) {
      for (int32 RungIdx = 1; RungIdx < Group.Rungs.Num(); ++RungIdx) {
        FHelixRung &R = Group.Rungs[RungIdx];
        for (int32 i = R.BondsToBoard.Num() - 1; i >= 0; --i) {
          int32 BoardId = R.BondsToBoard[i].Value;

          // 1. Is it now in the newest cluster? (Rungs[0])
          bool bFoundInNewest = false;
          for (const FOrbData &O : Group.Rungs[0].Orbs) {
            if (O.ItemId == BoardId) {
              bFoundInNewest = true;
              break;
            }
          }

          if (bFoundInNewest) {
            R.BondsToBoard.RemoveAt(i);
          } else {
            // 2. Is it still on the board?
            bool bStillOnBoard = false;
            for (const auto &KV : ItemMap) {
              if (KV.Value.Id == BoardId) {
                bStillOnBoard = true;
                break;
              }
            }

            if (!bStillOnBoard || true) { // drop if not processed next tick
              R.BondsToBoard.RemoveAt(i);
            }
          }
        }
      }
    }
  }
}
