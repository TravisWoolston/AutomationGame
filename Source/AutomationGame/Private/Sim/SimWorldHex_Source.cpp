#include "AutomationGameMinimal.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Sim/HexTypes.h"
#include "Sim/OrbTypes.h"
#include "Sim/SimWorldHex.h"

// ============================================================
// SOURCE GROUP SYSTEM
// ============================================================

namespace {
bool AreCollinear(const FHex &A, const FHex &B, const FHex &C) {
  int32 dq1 = B.Q - A.Q;
  int32 dr1 = B.R - A.R;
  int32 dq2 = C.Q - B.Q;
  int32 dr2 = C.R - B.R;
  return (dq1 * dr2 - dr1 * dq2) == 0;
}

bool AreHexNeighbors(const FHex &A, const FHex &B) {
  for (int32 d = 0; d < 6; ++d) {
    if (HexUtil::Neighbor(A, (EHexDir)d) == B)
      return true;
  }
  return false;
}
} // namespace

bool USimWorldHex::CanPlaceSourceTileAt(const FHex &Hex) const {
  TArray<FHex> Neighbors;
  for (int32 i = 0; i < 6; ++i) {
    FHex N = HexUtil::Neighbor(Hex, (EHexDir)i);
    if (const FTile *T = TileMap.Find(N)) {
      if (T->Kind == ETileKind::Source) {
        Neighbors.Add(N);
      }
    }
  }

  if (Neighbors.Num() == 0)
    return true;

  if (Neighbors.Num() == 1) {
    const FHex &N1 = Neighbors[0];
    for (int32 i = 0; i < 6; ++i) {
      FHex N2 = HexUtil::Neighbor(N1, (EHexDir)i);
      if (N2 == Hex)
        continue;
      if (const FTile *T = TileMap.Find(N2)) {
        if (T->Kind == ETileKind::Source) {
          if (!AreCollinear(N2, N1, Hex))
            return false;
        }
      }
    }
    return true;
  }

  if (Neighbors.Num() == 2) {
    if (!AreCollinear(Neighbors[0], Hex, Neighbors[1]))
      return false;
    return true;
  }

  return false;
}

void USimWorldHex::RegisterSourceTile(const FHex &Hex) {
  TArray<int32> AdjIndices;
  for (int32 i = 0; i < 6; ++i) {
    FHex N = HexUtil::Neighbor(Hex, (EHexDir)i);
    for (auto &KV : SourceGroups) {
      if (KV.Value.MemberTiles.Contains(N))
        AdjIndices.AddUnique(KV.Key);
    }
  }

  if (AdjIndices.Num() == 0) {
    FSourceGroup NewG;
    NewG.Id = NextSourceGroupId++;
    NewG.MemberTiles.Add(Hex);
    NewG.MassBudget = DefaultMassBudget;
    NewG.MassRemaining = DefaultMassBudget;
    UpdateSourceGroupTopology(NewG);
    SourceGroups.Add(NewG.Id, NewG);
  } else if (AdjIndices.Num() == 1) {
    FSourceGroup &G = SourceGroups[AdjIndices[0]];
    G.MemberTiles.AddUnique(Hex);
    UpdateSourceGroupTopology(G);
  } else {
    int32 PrimaryId = AdjIndices[0];
    FSourceGroup &Primary = SourceGroups[PrimaryId];
    Primary.MemberTiles.AddUnique(Hex);
    for (int32 k = 1; k < AdjIndices.Num(); ++k) {
      FSourceGroup &Other = SourceGroups[AdjIndices[k]];
      Primary.MemberTiles.Append(Other.MemberTiles);
      Primary.PendingRows.Empty();
      Primary.PrevRowTypes.Empty();
      SourceGroups.Remove(AdjIndices[k]);
    }
    UpdateSourceGroupTopology(Primary);
  }
}

void USimWorldHex::UnregisterSourceTile(const FHex &Hex) {
  for (auto It = SourceGroups.CreateIterator(); It; ++It) {
    FSourceGroup &G = It.Value();
    if (G.MemberTiles.Contains(Hex)) {
      G.MemberTiles.Remove(Hex);
      if (G.MemberTiles.Num() == 0)
        It.RemoveCurrent();
      else
        UpdateSourceGroupTopology(G);
      return;
    }
  }
}

void USimWorldHex::UpdateSourceGroupTopology(FSourceGroup &Group) {
  if (Group.MemberTiles.Num() < 2) {
    Group.SortedTiles = Group.MemberTiles;
    Group.PendingRows.Empty();
    Group.PrevRowTypes.Empty();
    return;
  }

  TMap<FHex, int32> NeighborCounts;
  for (const FHex &T : Group.MemberTiles)
    NeighborCounts.Add(T, 0);

  for (int i = 0; i < Group.MemberTiles.Num(); ++i) {
    for (int j = i + 1; j < Group.MemberTiles.Num(); ++j) {
      if (HexUtil::Dist(Group.MemberTiles[i], Group.MemberTiles[j]) == 1) {
        NeighborCounts[Group.MemberTiles[i]]++;
        NeighborCounts[Group.MemberTiles[j]]++;
      }
    }
  }

  FHex Start = Group.MemberTiles[0];
  for (auto &KV : NeighborCounts) {
    if (KV.Value == 1) {
      Start = KV.Key;
      break;
    }
  }

  Group.SortedTiles.Empty();
  Group.SortedTiles.Add(Start);
  FHex Current = Start;
  TSet<FHex> Visited;
  Visited.Add(Start);

  while (Group.SortedTiles.Num() < Group.MemberTiles.Num()) {
    bool bFound = false;
    for (const FHex &T : Group.MemberTiles) {
      if (!Visited.Contains(T) && HexUtil::Dist(Current, T) == 1) {
        Group.SortedTiles.Add(T);
        Visited.Add(T);
        Current = T;
        bFound = true;
        break;
      }
    }
    if (!bFound)
      break;
  }

  Group.PendingRows.Empty();
  Group.PrevRowTypes.Empty();
}

void USimWorldHex::StepSourceGroups() {
  // Clear the source-tile visual exclusion set up front so that items spawned
  // in a previous tick are no longer treated as "arriving from helix" by the
  // OrbRenderer. Each group will re-add its newly spawned items below.
  SourceTileItemIds.Empty();

  for (auto &KV : SourceGroups) {
    FSourceGroup &G = KV.Value;

    if (G.SortedTiles.Num() < 3)
      continue;

    while (G.PendingRows.Num() < 20) {
      GenerateNextSourceRow(G);
    }

    if (G.PendingRows.Num() == 0)
      continue;

    FSourceGroupItem &Row = G.PendingRows[0];
    int32 ArrivalTick = Row.SpawnTick + 19;

    if (StepCounter < ArrivalTick)
      continue;

    // 0. Orb cap check — pause if any orb type on the grid exceeds the limit
    if (bGridOrbLimit) {
      FSourceGroupItem &NextRow = G.PendingRows[0];
      // Count current board orbs by type
      TMap<uint8, int32> TypeCounts;
      for (const auto &OrbKV : OrbAt) {
        if (OrbKV.Value != 0)
          TypeCounts.FindOrAdd(OrbKV.Value)++;
      }
      // Add incoming row's orbs
      for (uint8 OT : NextRow.OrbTypes) {
        if (OT != 0)
          TypeCounts.FindOrAdd(OT)++;
      }
      bool bOverCap = false;
      for (const auto &TC : TypeCounts) {
        if (TC.Value > GridOrbCapPerType) {
          bOverCap = true;
          break;
        }
      }
      if (bOverCap) {
        if (!G.bPaused) {
          FString CountsStr;
          for (const auto &TC : TypeCounts) {
            if (!CountsStr.IsEmpty()) CountsStr += TEXT(", ");
            CountsStr += FString::Printf(TEXT("Type %d: %d"), TC.Key, TC.Value);
          }
          UE_LOG(LogTemp, Warning, TEXT("Source helix [%d] paused: Orb Type Cap reached. Counts: %s"), G.Id, *CountsStr);
        }
        G.bPaused = true;
        for (FSourceGroupItem &PR : G.PendingRows)
          PR.SpawnTick++;
        continue;
      }
    }

    // 1. Determine Push Direction
    FHex DirHex = G.SortedTiles[1] - G.SortedTiles[0];

    int32 LineDirIdx = -1;
    for (int i = 0; i < 6; ++i) {
      if (HexUtil::Neighbor(FHex(0, 0, 0), (EHexDir)i) == DirHex) {
        LineDirIdx = i;
        break;
      }
    }
    if (LineDirIdx < 0) {
      // Logic error or bad topology? Delay spawn to avoid losing the row visual
      for (FSourceGroupItem &PR : G.PendingRows)
        PR.SpawnTick++;
      continue;
    }

    int32 PushDirIdx = (LineDirIdx + 5) % 6;
    FHex StepDir = HexUtil::Neighbor(FHex(0, 0, 0), (EHexDir)PushDirIdx);

    // 2. Push existing items off source tiles (cascading)
    TSet<int32> ItemsToPrePush;
    TMap<int32, int32> ItemIdToSortedIndex;

    for (int32 i = 0; i < G.SortedTiles.Num(); ++i) {
      const FHex &SourceHex = G.SortedTiles[i];
      if (const FItem *Item = ItemMap.Find(SourceHex)) {
        TSet<int32> ClusterIds = GetFullClusterParentIds(Item->Id);
        ItemsToPrePush.Append(ClusterIds);
        ItemIdToSortedIndex.Add(Item->Id, i);
      }
    }

    // Track pushed items per slot for inter-row bonding
    TArray<int32> PushedItemPerSlot;
    PushedItemPerSlot.SetNumZeroed(G.SortedTiles.Num());

    if (ItemsToPrePush.Num() > 0) {
      TMap<FHex, FItem> PrePushItems;
      for (const auto &ItemKV : ItemMap) {
        if (ItemsToPrePush.Contains(ItemKV.Value.Id))
          PrePushItems.Add(ItemKV.Key, ItemKV.Value);
      }

      // Cascading push expansion — collect ALL blockers per pass
      bool bCanPush = true;
      bool bCascadeComplete = false;
      FString PauseReason = TEXT("Max Eject Weight Exceeded");
      for (int32 Iteration = 0; Iteration < MaxEjectWeight; ++Iteration) {
        bool bNeedRestart = false;

        for (const auto &PushKV : PrePushItems) {
          FHex PushDest(PushKV.Key.Q + StepDir.Q, PushKV.Key.R + StepDir.R,
                        PushKV.Key.Level);

          bool bIsInternal = false;
          for (const auto &OtherKV : PrePushItems) {
            if (OtherKV.Key == PushDest) {
              bIsInternal = true;
              break;
            }
          }
          if (bIsInternal)
            continue;

          if (const FItem *BlockingItem = ItemMap.Find(PushDest)) {
            if (!ItemsToPrePush.Contains(BlockingItem->Id)) {
              ItemsToPrePush.Append(GetFullClusterParentIds(BlockingItem->Id));
              bNeedRestart = true;
              // DON'T break — keep scanning to find all blockers in this pass
              continue;
            }
          }

          if (CachedHeldOrbHexes.Contains(PushDest) ||
              CachedBlockingFootHexes.Contains(PushDest) ||
              CachedArmBaseHexes.Contains(PushDest)) {
            bCanPush = false;
            PauseReason = TEXT("Blocked by Entity Obstacle");
            break;
          }

          if (const FTile *DestTile = TileMap.Find(PushDest)) {
            if (DestTile->Kind == ETileKind::Output) {
              bCanPush = false;
              PauseReason = TEXT("Blocked by Output Tile");
              break;
            }
          }
        }

        if (!bCanPush)
          break;
        if (bNeedRestart) {
          PrePushItems.Empty();
          for (const auto &ItemKV : ItemMap) {
            if (ItemsToPrePush.Contains(ItemKV.Value.Id))
              PrePushItems.Add(ItemKV.Key, ItemKV.Value);
          }
          continue;
        }
        bCascadeComplete = true;
        break; // Path is clear!
      }

      // Safety: if cascade didn't finish, block the push
      if (!bCascadeComplete)
        bCanPush = false;

      if (!bCanPush) {
        // Blocked - pause the helix animation by delaying all pending rows
        // This keeps ZOffset constant (at approx 0) so the top rung doesn't
        // vanish
        if (!G.bPaused) {
          UE_LOG(LogTemp, Warning, TEXT("Source helix [%d] paused: %s"), G.Id, *PauseReason);
        }
        G.bPaused = true;
        for (FSourceGroupItem &PR : G.PendingRows)
          PR.SpawnTick++;
        continue; // Try next tick
      }

      // Push is possible — clear paused state
      G.bPaused = false;

      // --- Execute push ---
      TMap<FHex, uint8> OldKinds;
      TSet<int32> MovedItemIds;
      for (const auto &PushKV : PrePushItems) {
        OldKinds.Add(PushKV.Key, OrbAt.FindRef(PushKV.Key));
        MovedItemIds.Add(PushKV.Value.Id);
        ItemMap.Remove(PushKV.Key);
        OrbAt.Remove(PushKV.Key);
      }

      for (const auto &PushKV : PrePushItems) {
        FHex NewPos(PushKV.Key.Q + StepDir.Q, PushKV.Key.R + StepDir.R,
                    PushKV.Key.Level);
        // Diagnostic: detect overwriting an item that shouldn't be here
        if (const FItem *Existing = ItemMap.Find(NewPos)) {
          if (!MovedItemIds.Contains(Existing->Id)) {
            UE_LOG(
                LogTemp, Error,
                TEXT("Source push OVERWRITES item %d at (%d,%d) with item %d!"),
                Existing->Id, NewPos.Q, NewPos.R, PushKV.Value.Id);
          }
        }
        ItemMap.Add(NewPos, PushKV.Value);
        OrbAt.Add(NewPos, OldKinds[PushKV.Key]);
        OrbsNewlyMovedByBelts.Add(PushKV.Value.Id);
      }

      // Update bond visual roots
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

      // Record which items were pushed from which slot
      for (const auto &IdxKV : ItemIdToSortedIndex) {
        if (PushedItemPerSlot.IsValidIndex(IdxKV.Value))
          PushedItemPerSlot[IdxKV.Value] = IdxKV.Key;
      }
    } else {
      // No items to push — source tiles are clear, not blocked
      G.bPaused = false;
    }

    // 3. Spawn new items on now-clear source tiles
    TArray<int32> SlotToItemId;
    SlotToItemId.SetNumZeroed(G.SortedTiles.Num());

    // Mass budget check: count how many orbs this row will spawn
    int32 OrbsInRow = 0;
    if (G.MassBudget > 0) {
      for (int32 i = 0; i < G.SortedTiles.Num(); ++i) {
        if (Row.OrbTypes.IsValidIndex(i) && Row.OrbTypes[i] != 0)
          OrbsInRow++;
      }
      if (OrbsInRow > G.MassRemaining) {
        // Not enough mass budget — pause
        if (!G.bPaused) {
          UE_LOG(LogTemp, Warning, TEXT("Source helix [%d] paused: Mass Budget depleted"), G.Id);
        }
        G.bPaused = true;
        for (FSourceGroupItem &PR : G.PendingRows)
          PR.SpawnTick++;
        continue;
      }
    }

    for (int32 i = 0; i < G.SortedTiles.Num(); ++i) {
      if (!Row.OrbTypes.IsValidIndex(i) || Row.OrbTypes[i] == 0)
        continue;

      FHex SpawnHex = G.SortedTiles[i];

      // Safety: don't overwrite an existing item that wasn't pushed off
      if (ItemMap.Contains(SpawnHex)) {
        UE_LOG(
            LogTemp, Warning,
            TEXT("Source spawn skipped: hex (%d,%d) still occupied by item %d"),
            SpawnHex.Q, SpawnHex.R, ItemMap[SpawnHex].Id);
        continue;
      }

      FItem NewItem;
      NewItem.Id = NextItemId++;
      NewItem.Single.Type = (EOrbType)Row.OrbTypes[i];
      NewItem.Single.Color = EOrbColor::Blue;
      NewItem.bIsCluster = false;

      // Per-type lifetime (Phase 4)
      if (const int32 *TypeLife = OrbLifetimeByType.Find(Row.OrbTypes[i])) {
        NewItem.Single.Stability = *TypeLife;
      } else if (DefaultOrbLifetimeTicks > 0) {
        NewItem.Single.Stability = DefaultOrbLifetimeTicks;
      }

      ItemMap.Add(SpawnHex, NewItem);
      OrbAt.Add(SpawnHex, Row.OrbTypes[i]);
      SlotToItemId[i] = NewItem.Id;
    }

    // Decrement mass budget (Phase 5)
    if (G.MassBudget > 0) {
      int32 ActualSpawned = 0;
      for (int32 i = 0; i < G.SortedTiles.Num(); ++i) {
        if (SlotToItemId[i] != 0)
          ActualSpawned++;
      }
      G.MassRemaining -= ActualSpawned;
      G.TotalMassSpawned += ActualSpawned;
      if (G.MassRemaining < 0)
        G.MassRemaining = 0;
    }

    // 4. Create bonds at spawn time with hex adjacency validation.
    //    Use Kruskal's (spanning tree) to select minimum bonds for
    //    connectivity. New items are at SortedTile[i], pushed items at
    //    SortedTile[j]+StepDir.

    // Collect all candidate bond edges with adjacency checks
    struct FBondCandidate {
      int32 ItemIdA;
      int32 ItemIdB;
      FHex HexA;
      FHex HexB;
    };
    TArray<FBondCandidate> Candidates;

    for (int32 i = 0; i < G.SortedTiles.Num(); ++i) {
      int32 NewId = SlotToItemId[i];
      if (NewId == 0)
        continue;
      FHex NewHex = G.SortedTiles[i];

      // Intra-row: bond to left neighbor (i-1) in same row
      if (i > 0 && SlotToItemId[i - 1] != 0) {
        FHex LeftHex = G.SortedTiles[i - 1];
        if (AreHexNeighbors(NewHex, LeftHex)) {
          Candidates.Add({NewId, SlotToItemId[i - 1], NewHex, LeftHex});
        }
      }

      // Inter-row: bond to pushed item at same slot (vertical)
      if (PushedItemPerSlot.IsValidIndex(i) && PushedItemPerSlot[i] != 0) {
        FHex PrevHex(NewHex.Q + StepDir.Q, NewHex.R + StepDir.R, NewHex.Level);
        if (AreHexNeighbors(NewHex, PrevHex)) {
          Candidates.Add({NewId, PushedItemPerSlot[i], NewHex, PrevHex});
        }
      }

      // Inter-row: diagonal bonds to pushed items at i-1 and i+1
      for (int32 DiagOffset : {-1, 1}) {
        int32 DiagIdx = i + DiagOffset;
        if (!PushedItemPerSlot.IsValidIndex(DiagIdx))
          continue;
        if (PushedItemPerSlot[DiagIdx] == 0)
          continue;

        FHex DiagHex(G.SortedTiles[DiagIdx].Q + StepDir.Q,
                     G.SortedTiles[DiagIdx].R + StepDir.R,
                     G.SortedTiles[DiagIdx].Level);

        if (AreHexNeighbors(NewHex, DiagHex)) {
          Candidates.Add({NewId, PushedItemPerSlot[DiagIdx], NewHex, DiagHex});
        }
      }
    }

    // Shuffle candidates for random spanning tree variety
    for (int32 i = Candidates.Num() - 1; i > 0; --i) {
      int32 j = FMath::RandRange(0, i);
      Candidates.Swap(i, j);
    }

    // Union-Find for spanning tree selection
    TMap<int32, int32> UfParent; // ItemId -> parent ItemId
    auto UfFind = [&](int32 x) -> int32 {
      while (true) {
        int32 *P = UfParent.Find(x);
        if (!P || *P == x)
          return x;
        // Path compression
        int32 *PP = UfParent.Find(*P);
        if (PP)
          *P = *PP;
        x = *P;
      }
    };
    auto UfUnion = [&](int32 a, int32 b) -> bool {
      a = UfFind(a);
      b = UfFind(b);
      if (a == b)
        return false;
      UfParent.FindOrAdd(a) = b;
      return true;
    };

    // Initialize all new and pushed items
    for (int32 i = 0; i < G.SortedTiles.Num(); ++i) {
      if (SlotToItemId[i] != 0)
        UfParent.Add(SlotToItemId[i], SlotToItemId[i]);
      if (PushedItemPerSlot[i] != 0)
        UfParent.Add(PushedItemPerSlot[i], PushedItemPerSlot[i]);
    }

    // Connect all pushed items together (they're already one structure)
    // REMOVED: Do not assume pushed items are already connected.
    // Treating them as a single component prevents Kruskal's from adding
    // necessary bonds if the previous row was actually fragmented.
    // Allowing Kruskal's to bond them ensures better connectivity.

    // Kruskal's: add edges that connect new components
    for (const FBondCandidate &C : Candidates) {
      if (UfUnion(C.ItemIdA, C.ItemIdB)) {
        FBond NewBond;
        NewBond.ItemA = C.ItemIdA;
        NewBond.ItemB = C.ItemIdB;
        NewBond.Root = C.HexA;
        NewBond.dQ = C.HexB.Q - C.HexA.Q;
        NewBond.dR = C.HexB.R - C.HexA.R;
        NewBond.bActive = true;
        int32 BIdx = Bonds.Add(NewBond);
        ItemToBonds.FindOrAdd(C.ItemIdA).Add(BIdx);
        ItemToBonds.FindOrAdd(C.ItemIdB).Add(BIdx);
      }
    }

    // Safety: ensure every spawned item has at least 1 bond
    for (int32 i = 0; i < G.SortedTiles.Num(); ++i) {
      int32 ItemId = SlotToItemId[i];
      if (ItemId == 0)
        continue;
      const TArray<int32> *BondList = ItemToBonds.Find(ItemId);
      if (BondList && BondList->Num() > 0)
        continue;

      // No bonds — try to bond to any adjacent item
      FHex MyHex = G.SortedTiles[i];
      for (int32 d = 0; d < 6; ++d) {
        FHex NbrHex = HexUtil::Neighbor(MyHex, (EHexDir)d);
        if (const FItem *NbrItem = ItemMap.Find(NbrHex)) {
          if (NbrItem->Id != ItemId) {
            FBond FallbackBond;
            FallbackBond.ItemA = ItemId;
            FallbackBond.ItemB = NbrItem->Id;
            FallbackBond.Root = MyHex;
            FallbackBond.dQ = NbrHex.Q - MyHex.Q;
            FallbackBond.dR = NbrHex.R - MyHex.R;
            FallbackBond.bActive = true;
            int32 BIdx = Bonds.Add(FallbackBond);
            ItemToBonds.FindOrAdd(ItemId).Add(BIdx);
            ItemToBonds.FindOrAdd(NbrItem->Id).Add(BIdx);
            break;
          }
        }
      }
    }

    // 5. Mark newly spawned items as "on source tiles" so the board renderer
    //    can detect them and set their PrevPos to create a smooth arrival
    //    animation.
    for (int32 i = 0; i < G.SortedTiles.Num(); ++i) {
      if (SlotToItemId[i] != 0)
        SourceTileItemIds.Add(SlotToItemId[i]);
    }

    // Consume row
    G.PendingRows.RemoveAt(0);
  }
}

void USimWorldHex::GenerateNextSourceRow(FSourceGroup &Group) {
  FSourceGroupItem Row;

  const int32 TicksPerRung = Group.GenerationInterval;
  int32 BaseTick = StepCounter;
  if (Group.PendingRows.Num() > 0) {
    BaseTick = Group.PendingRows.Last().SpawnTick;
    if (BaseTick < StepCounter)
      BaseTick = StepCounter;
  }

  Row.SpawnTick = BaseTick + TicksPerRung;

  const int32 Width = Group.SortedTiles.Num();
  Row.OrbTypes.SetNumZeroed(Width);

  // Frequency-weighted type selection
  struct FTypeWeight {
    uint8 Type;
    int32 Weight;
  };
  const FTypeWeight TypeWeights[] = {{1, 80}, {2, 10}, {3, 5},
                                     {4, 3},  {5, 3},  {6, 0}};
  const int32 TotalWeight = 100;

  auto PickWeightedType = [&]() -> uint8 {
    int32 Roll = FMath::RandRange(0, TotalWeight - 1);
    for (const FTypeWeight &TW : TypeWeights) {
      Roll -= TW.Weight;
      if (Roll < 0)
        return TW.Type;
    }
    return 1;
  };

  // --- Phase 1: Generate fill pattern with holes ---
  // Fill 50-80% of slots (at least 2, never 0)
  float FillRate = FMath::RandRange(0.5f, 0.8f);
  int32 TargetCount =
      FMath::Clamp(FMath::RoundToInt32(Width * FillRate), 2, Width);

  TArray<bool> Filled;
  Filled.SetNumZeroed(Width);

  // Shuffle indices and pick first TargetCount
  TArray<int32> Indices;
  for (int32 i = 0; i < Width; ++i)
    Indices.Add(i);
  for (int32 i = Width - 1; i > 0; --i) {
    int32 j = FMath::RandRange(0, i);
    Indices.Swap(i, j);
  }
  for (int32 k = 0; k < TargetCount && k < Indices.Num(); ++k)
    Filled[Indices[k]] = true;

  // --- Phase 2: Ensure connectivity via union-find ---
  // Intra-row connectivity only (slots i and i-1 are connected if both filled)
  // Inter-row connectivity checked against PrevRowTypes
  const int32 NodeCount = 2 * Width + 1;
  const int32 PrevClusterNode = 2 * Width;
  TArray<int32> Parent;
  Parent.SetNum(NodeCount);
  for (int32 i = 0; i < NodeCount; ++i)
    Parent[i] = i;

  TFunction<int32(int32)> Find = [&](int32 x) -> int32 {
    while (Parent[x] != x) {
      Parent[x] = Parent[Parent[x]];
      x = Parent[x];
    }
    return x;
  };
  auto Union = [&](int32 a, int32 b) {
    a = Find(a);
    b = Find(b);
    if (a != b)
      Parent[a] = b;
  };

  // Connect prev row orbs together
  for (int32 i = 0; i < Width; ++i) {
    if (Group.PrevRowTypes.IsValidIndex(i) && Group.PrevRowTypes[i] != 0)
      Union(Width + i, PrevClusterNode);
  }

  // Build connections —  only use REAL hex adjacency for inter-row
  // For this we need actual hex positions (SortedTiles)
  auto BuildConnections = [&]() {
    for (int32 i = 0; i < Width; ++i) {
      if (!Filled[i])
        continue;
      // Intra-row: adjacent slots are always hex neighbors
      if (i > 0 && Filled[i - 1])
        Union(i, i - 1);
      // Inter-row: check actual hex adjacency
      FHex MyHex = Group.SortedTiles[i];
      for (int32 j = 0; j < Width; ++j) {
        if (!Group.PrevRowTypes.IsValidIndex(j) || Group.PrevRowTypes[j] == 0)
          continue;
        // Prev row item j would be at SortedTiles[j]+PushDir after being pushed
        // But in the generation context, we use slot adjacency (|i-j| <= 1)
        // to approximate. The actual adjacency is validated at bond creation.
        if (FMath::Abs(i - j) <= 1) {
          Union(i, Width + j);
        }
      }
    }
  };

  BuildConnections();

  // Ensure at least one slot connects to previous row (if it exists)
  bool bHasPrevRow = Group.PrevRowTypes.Num() > 0;
  if (bHasPrevRow) {
    int32 PrevRoot = Find(PrevClusterNode);
    bool bConnectedToPrev = false;
    for (int32 i = 0; i < Width; ++i) {
      if (Filled[i] && Find(i) == PrevRoot) {
        bConnectedToPrev = true;
        break;
      }
    }
    if (!bConnectedToPrev) {
      // Find a slot adjacent to prev row and force-fill it
      for (int32 i = 0; i < Width; ++i) {
        bool bAdjacentToPrev = false;
        for (int32 j = FMath::Max(0, i - 1); j <= FMath::Min(Width - 1, i + 1);
             ++j) {
          if (Group.PrevRowTypes.IsValidIndex(j) &&
              Group.PrevRowTypes[j] != 0) {
            bAdjacentToPrev = true;
            break;
          }
        }
        if (bAdjacentToPrev) {
          Filled[i] = true;
          // Rebuild connectivity
          for (int32 n = 0; n < NodeCount; ++n)
            Parent[n] = n;
          for (int32 n = 0; n < Width; ++n) {
            if (Group.PrevRowTypes.IsValidIndex(n) &&
                Group.PrevRowTypes[n] != 0)
              Union(Width + n, PrevClusterNode);
          }
          BuildConnections();
          break;
        }
      }
    }
  }

  // Merge disconnected components by filling gaps
  TSet<int32> Components;
  for (int32 i = 0; i < Width; ++i) {
    if (Filled[i])
      Components.Add(Find(i));
  }

  if (Components.Num() > 1) {
    TArray<int32> CompRoots = Components.Array();
    for (int32 c = 1; c < CompRoots.Num(); ++c) {
      // Find bridge slot between component 0 and component c
      int32 BestBridge = -1;
      for (int32 i = 0; i < Width; ++i) {
        if (Filled[i])
          continue;
        bool bTouchesA = false, bTouchesB = false;
        if (i > 0 && Filled[i - 1]) {
          int32 r = Find(i - 1);
          if (r == CompRoots[0])
            bTouchesA = true;
          if (r == CompRoots[c])
            bTouchesB = true;
        }
        if (i + 1 < Width && Filled[i + 1]) {
          int32 r = Find(i + 1);
          if (r == CompRoots[0])
            bTouchesA = true;
          if (r == CompRoots[c])
            bTouchesB = true;
        }
        if (bTouchesA && bTouchesB) {
          BestBridge = i;
          break;
        }
      }
      if (BestBridge >= 0) {
        Filled[BestBridge] = true;
        Union(BestBridge, CompRoots[0]);
        Union(BestBridge, CompRoots[c]);
      } else {
        // Fallback: fill all slots between the two components
        int32 Left = -1, Right = -1;
        for (int32 i = 0; i < Width; ++i) {
          if (Filled[i] && Find(i) == CompRoots[0] && Left < 0)
            Left = i;
          if (Filled[i] && Find(i) == CompRoots[c])
            Right = i;
        }
        if (Left >= 0 && Right >= 0) {
          int32 lo = FMath::Min(Left, Right);
          int32 hi = FMath::Max(Left, Right);
          for (int32 i = lo; i <= hi; ++i) {
            Filled[i] = true;
            if (i > 0 && Filled[i - 1])
              Union(i, i - 1);
          }
        }
      }
    }
  }

  // --- Phase 3: Assign types to filled slots ---
  for (int32 i = 0; i < Width; ++i) {
    if (!Filled[i])
      continue;
    uint8 Chosen = 0;
    for (int32 Attempt = 0; Attempt < 8; ++Attempt) {
      uint8 Candidate = PickWeightedType();
      bool bConflict = false;
      // Only check left horizontal neighbor
      if (i > 0 && Row.OrbTypes[i - 1] != 0 && Row.OrbTypes[i - 1] == Candidate)
        bConflict = true;
      if (!bConflict || Attempt == 7) {
        Chosen = Candidate;
        break;
      }
    }
    Row.OrbTypes[i] = Chosen;
  }

  // --- Post-Generation Validation: Ensure at least one orb exists ---
  // Prevents "empty sets" which cause visual gaps and transition jumps
  bool bHasOrb = false;
  for (uint8 T : Row.OrbTypes) {
    if (T != 0) {
      bHasOrb = true;
      break;
    }
  }

  if (!bHasOrb && Width > 0) {
    // Force at least one orb (random slot)
    int32 Idx = FMath::RandRange(0, Width - 1);
    Row.OrbTypes[Idx] = PickWeightedType();
  }

  Group.PrevRowTypes = Row.OrbTypes;
  Row.ZOffset = -3000.f;
  Group.PendingRows.Add(Row);
}

void USimWorldHex::UpdateSourceGroups(float DeltaTime) {
  float Alpha = GetFrameAlpha();
  const float DelayTicks = 20.f;

  for (auto &Pair : SourceGroups) {
    FSourceGroup &G = Pair.Value;

    // If the helix is paused (blocked), freeze animation
    if (G.bPaused)
      continue;

    float UnitsPerTick = G.RungSpacing;
    float StartDepth = DelayTicks * UnitsPerTick;

    for (int32 i = 0; i < G.PendingRows.Num(); ++i) {
      FSourceGroupItem &Row = G.PendingRows[i];

      float AgeTicks = (float)(StepCounter - Row.SpawnTick) + Alpha;
      Row.ZOffset = -StartDepth + (AgeTicks * UnitsPerTick);

      if (Row.ZOffset > 0.f)
        Row.ZOffset = 0.f;
    }
  }
}
