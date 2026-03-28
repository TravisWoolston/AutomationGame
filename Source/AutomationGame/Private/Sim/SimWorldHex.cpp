#include "Sim/SimWorldHex.h"
#include "AGLogChannels.h"
#include "Actors/BasicArm.h"
#include "Actors/OrbSimDriver.h"
#include "Algo/Reverse.h"
#include "DrawDebugHelpers.h"
#include "Engine/Engine.h"
#include "EngineUtils.h"
#include "Sim/HexCoord.h"

// Constructor
USimWorldHex::USimWorldHex() {
  // Initialize all containers to prevent memory corruption
  TileMap.Empty();
  ItemMap.Empty();
  Arms.Empty();
  Walkers.Empty();
  OrbAt.Empty();
  BeltProgress.Empty();
  Bonds.Empty();
  ItemToBonds.Empty();
  SimFacingByArm.Empty();
  SimArmLenByArm.Empty();
  SimGrabFacingByArm.Empty();
  PendingFacingByArm.Empty();
  PendingArmLenByArm.Empty();
  PendingGrabFacingByArm.Empty();
  SkipInputThisFrame.Empty();
  SkipOutputThisFrame.Empty();
  NewlyPlacedItems.Empty();
  ArmsReadyForUnbond.Empty();
  PendingUnbonds.Empty();
  RecentlyDroppedByArm.Empty();
  OrbsNewlyMovedToOutput.Empty();
  OrbsNewlyMovedByBelts.Empty();
  OutputTileGates.Empty();
  BeltTriggerSignals.Empty();
  AnimatedOrbs.Empty();
  HeldItemLastHex.Empty();
  PendingBondFormations.Empty();
  PendingGrabs.Empty();
  BondedArmPairs.Empty();
  PendingHeldMerges.Empty();
  CollisionDebugData.Empty();
  PredictiveCollisionDebugData.Empty();
  ItemsMovedLastStep.Empty();
  PrevItemHex.Empty();
  HelixGroups.Empty();
}

// Destructor - explicit cleanup to prevent memory corruption
USimWorldHex::~USimWorldHex() {
  // Clear all containers in a safe order to prevent corruption
  // Clear complex nested structures first
  ItemToBonds.Empty();

  // Clear all other containers
  TileMap.Empty();
  ItemMap.Empty();
  Arms.Empty();
  OrbAt.Empty();
  BeltProgress.Empty();
  Bonds.Empty();
  SimFacingByArm.Empty();
  SimArmLenByArm.Empty();
  SimGrabFacingByArm.Empty();
  PendingFacingByArm.Empty();
  PendingArmLenByArm.Empty();
  PendingGrabFacingByArm.Empty();
  SkipInputThisFrame.Empty();
  SkipOutputThisFrame.Empty();
  NewlyPlacedItems.Empty();
  ArmsReadyForUnbond.Empty();
  PendingUnbonds.Empty();
  RecentlyDroppedByArm.Empty();
  OrbsNewlyMovedToOutput.Empty();
  OrbsNewlyMovedByBelts.Empty();
  OutputTileGates.Empty();
  BeltTriggerSignals.Empty();
  AnimatedOrbs.Empty();
  HeldItemLastHex.Empty();
  PendingBondFormations.Empty();
  PendingGrabs.Empty();
  BondedArmPairs.Empty();
  PendingHeldMerges.Empty();
  CollisionDebugData.Empty();
  PredictiveCollisionDebugData.Empty();
  ItemsMovedLastStep.Empty();
  PrevItemHex.Empty();
  ItemsMovedLastStep.Empty();
  PrevItemHex.Empty();
  HelixGroups.Empty();
  DiscoveryTileOrbTypes.Empty();
  BeltTileOrbTypes.Empty();
}

// Near top (after includes) add if not already in header:
static constexpr float DEFAULT_BELT_SPEED_TILES_PER_SECOND = 0.0f;
// Helper used only inside StepBonds to index held cluster sub‑orbs.
struct FHeldOrbRef {
  int32 ArmIndex = -1;
  int32 SubIdx = -1;
};

void USimWorldHex::Initialize(int32 /*Radius*/) {
  UE_LOG(LogTemp, Warning,
         TEXT("SimWorldHex::Initialize() clearing %d arms and %d walkers from "
              "simulation"),
         Arms.Num(), Walkers.Num());
  TileMap.Empty();
  ItemMap.Empty();
  Arms.Empty();
  Walkers.Empty();
  OutputCount = 0;
  TileRevision = 0;
  NextItemId = 1;
  OrbAt.Empty();
  BeltProgress.Empty();
  Bonds.Empty();
  ItemToBonds.Empty();
  SimFacingByArm.Empty();
  SimArmLenByArm.Empty();
  SimGrabFacingByArm.Empty();
  PendingFacingByArm.Empty();
  PendingArmLenByArm.Empty();
  PendingGrabFacingByArm.Empty();
  PendingGrabFacingByArm.Empty();
  HelixGroups.Empty();
  DiscoveryTileOrbTypes.Empty();
  BeltTileOrbTypes.Empty();
}

// ---------- NEW: Add missing definitions ----------

int32 USimWorldHex::AddArm(const FArmState &Arm) {
  int32 NewIndex = Arms.Add(Arm);
  // Clamp / sanitize a few runtime fields (defensive)
  FArmState &S = Arms[NewIndex];
  S.PC = S.PC % FMath::Max(1, S.Tape.Num());
  S.WaitCounter = 0;
  // Ensure facing in 0..5
  S.Facing = (EHexDir)((uint8)S.Facing % 6);
  S.HomeFacing = (EHexDir)((uint8)S.HomeFacing % 6);
  S.ArmLen = FMath::Clamp<int32>(S.ArmLen <= 0 ? 1 : S.ArmLen, 1, 6);
  S.HomeArmLen =
      FMath::Clamp<int32>(S.HomeArmLen <= 0 ? 1 : S.HomeArmLen, 1, 6);
  // Initialize rotation completed to true (no rotation in progress)
  S.bRotationCompleted = true;

  // Seed sim state to current values
  SimFacingByArm.Add(NewIndex, static_cast<uint8>(S.Facing));
  SimArmLenByArm.Add(NewIndex, S.ArmLen);
  SimGrabFacingByArm.Add(NewIndex, int8(-1)); // no held cluster yet

  UE_LOG(LogTemp, Warning,
         TEXT("SimWorldHex::AddArm - Added arm at (%d,%d) facing=%d assigned "
              "SimIndex=%d (total arms: %d)"),
         Arm.BaseHex.Q, Arm.BaseHex.R, (int32)Arm.Facing, NewIndex, Arms.Num());

  // Mark arm base cache as dirty so it gets rebuilt
  bArmBaseCacheDirty = true;

  return NewIndex;
}

int32 USimWorldHex::AddWalker(const FWalkerState &Walker) {
  int32 NewIndex = Walkers.Add(Walker);
  FWalkerState &S = Walkers[NewIndex];
  S.BaseHex = Walker.BaseHex;
  S.HomeHex = Walker.HomeHex;
  S.Stance = EWalkerStance::Sitting;
  S.StanceTransitionProgress = 0;
  S.bProcessingRequest = false;

  // Initialize foot positions to outer tiles at ReachDistance
  // Find the axis-aligned tiles at ReachDistance for each direction
  // Quadrant mapping: D->S (R+), L->W (Q-), R->E (Q+), U->N (R-)
  const int32 ReachDist = FMath::Max(1, S.ReachDistance); // Ensure at least 1

  // For axis-aligned directions at ReachDistance:
  // D (South): Q unchanged, R + ReachDist
  // L (West): Q - ReachDist, R unchanged
  // R (East): Q + ReachDist, R unchanged
  // U (North): Q unchanged, R - ReachDist
  FHex OuterTiles[4] = {
      FHex(S.HomeHex.Q, S.HomeHex.R + ReachDist,
           S.HomeHex.Level), // D -> S (South)
      FHex(S.HomeHex.Q - ReachDist, S.HomeHex.R,
           S.HomeHex.Level), // L -> W (West)
      FHex(S.HomeHex.Q + ReachDist, S.HomeHex.R,
           S.HomeHex.Level), // R -> E (East)
      FHex(S.HomeHex.Q, S.HomeHex.R - ReachDist,
           S.HomeHex.Level) // U -> N (North)
  };

  // Set foot positions to outer tiles
  S.DFoot.CurrentTile = OuterTiles[0]; // D -> S
  S.DFoot.TargetTile = S.DFoot.CurrentTile;
  S.DFoot.StepState = EFootStepState::Planted;

  S.LFoot.CurrentTile = OuterTiles[1]; // L -> W
  S.LFoot.TargetTile = S.LFoot.CurrentTile;
  S.LFoot.StepState = EFootStepState::Planted;

  S.RFoot.CurrentTile = OuterTiles[2]; // R -> E
  S.RFoot.TargetTile = S.RFoot.CurrentTile;
  S.RFoot.StepState = EFootStepState::Planted;

  S.UFoot.CurrentTile = OuterTiles[3]; // U -> N
  S.UFoot.TargetTile = S.UFoot.CurrentTile;
  S.UFoot.StepState = EFootStepState::Planted;

  // Legacy support
  S.PC = S.PC % FMath::Max(1, S.Program.Num());
  S.WaitCounter = 0;
  S.bMoving = false;
  S.MovementProgress = 0.0f;

  UE_LOG(LogTemp, Warning,
         TEXT("SimWorldHex::AddWalker - Added walker at (%d,%d) assigned "
              "SimIndex=%d (total walkers: %d)"),
         Walker.BaseHex.Q, Walker.BaseHex.R, NewIndex, Walkers.Num());

  return NewIndex;
}

int32 USimWorldHex::FindNearestAvailableWalker(const FHex &FromHex) {
  int32 NearestIndex = INDEX_NONE;
  int32 NearestIdleIndex = INDEX_NONE;
  float NearestDist = MAX_flt;
  float NearestIdleDist = MAX_flt;

  for (int32 i = 0; i < Walkers.Num(); ++i) {
    const FWalkerState &W = Walkers[i];
    const bool bIsBusy = W.bProcessingRequest || W.Requests.Num() > 0;

    // Calculate hex distance
    const int32 dQ = W.BaseHex.Q - FromHex.Q;
    const int32 dR = W.BaseHex.R - FromHex.R;
    const float Dist =
        FMath::Max(FMath::Max(FMath::Abs(float(dQ)), FMath::Abs(float(dR))),
                   FMath::Abs(float(dQ + dR)));

    // Track nearest ANY walker
    if (Dist < NearestDist) {
      NearestDist = Dist;
      NearestIndex = i;
    }

    // Track nearest IDLE walker
    if (!bIsBusy && Dist < NearestIdleDist) {
      NearestIdleDist = Dist;
      NearestIdleIndex = i;
    }
  }

  // Prefer idle walkers
  if (NearestIdleIndex != INDEX_NONE) {
    return NearestIdleIndex;
  }

  return NearestIndex;
}

bool USimWorldHex::RequestWalkerMoveTo(int32 WalkerIndex, const FHex &TargetHex,
                                       int32 Priority) {
  if (WalkerIndex < 0 || WalkerIndex >= Walkers.Num()) {
    UE_LOG(LogTemp, Warning,
           TEXT("SimWorldHex::RequestWalkerMoveTo: Invalid walker index %d "
                "(total: %d)"),
           WalkerIndex, Walkers.Num());
    return false;
  }

  FWalkerState &W = Walkers[WalkerIndex];

  FWalkerRequest Request;
  Request.TargetHex = TargetHex;
  Request.Priority = Priority;

  // Insert request sorted by priority (higher priority first)
  int32 InsertIndex = W.Requests.Num();
  for (int32 i = 0; i < W.Requests.Num(); ++i) {
    if (Request.Priority > W.Requests[i].Priority) {
      InsertIndex = i;
      break;
    }
  }
  W.Requests.Insert(Request, InsertIndex);

  UE_LOG(LogTemp, Log,
         TEXT("SimWorldHex::RequestWalkerMoveTo: Added request to walker %d to "
              "move to (%d, %d) - Total requests: %d, Stance: %d"),
         WalkerIndex, TargetHex.Q, TargetHex.R, W.Requests.Num(),
         (int32)W.Stance);

  return true;
}

bool USimWorldHex::TryPutItem(const FHex &H, const FItem &I) {
  if (ItemMap.Contains(H)) {
    return false; // already occupied
  }

  // Check for walker feet (Current or Target tiles)
  // EXCLUDE feet that are holding items (they're raised and not obstacles)
  for (const FWalkerState &W : Walkers) {
    if (!W.DFoot.bHoldingItem &&
        (W.DFoot.CurrentTile == H || W.DFoot.TargetTile == H))
      return false; // occupied by walker foot
    if (!W.LFoot.bHoldingItem &&
        (W.LFoot.CurrentTile == H || W.LFoot.TargetTile == H))
      return false;
    if (!W.RFoot.bHoldingItem &&
        (W.RFoot.CurrentTile == H || W.RFoot.TargetTile == H))
      return false;
    if (!W.UFoot.bHoldingItem &&
        (W.UFoot.CurrentTile == H || W.UFoot.TargetTile == H))
      return false;
  }

  FItem Copy = I;
  // Assign an Id if caller forgot (StepGlyphs sets it, but be safe)
  if (Copy.Id == 0) {
    Copy.Id = NextItemId++;
  }

  // Items placed by arms should start with bIsReady = false to prevent
  // immediate belt pulling This gives items at least one tick to settle before
  // being moved by belts
  Copy.bIsReady = false;

  // Special case: Check if placing on a bond/unbond tile area
  const FTile *Tile = TileMap.Find(H);
  bool bIsOnBondArea = false;

  if (Tile &&
      (Tile->Kind == ETileKind::Bond || Tile->Kind == ETileKind::Unbond)) {
    // Placing directly on a primary bond/unbond tile
    bIsOnBondArea = true;
  } else {
    // Check if placing on a partner area of a bond/unbond glyph
    for (const auto &TileKV : TileMap) {
      const FTile &CheckTile = TileKV.Value;
      if (CheckTile.Kind != ETileKind::Bond &&
          CheckTile.Kind != ETileKind::Unbond)
        continue;

      // Calculate this glyph's partner hex
      const uint8 CheckGlyphDir = (uint8)(CheckTile.Arg & 0xFF);
      const FHex CheckPartnerHex =
          HexUtil::Neighbor(TileKV.Key, (EHexDir)(CheckGlyphDir % 6));

      // If we're placing on this glyph's partner hex
      if (CheckPartnerHex == H) {
        bIsOnBondArea = true;
        break;
      }
    }
  }

  // Note: Items placed on bond/unbond areas (primary or partner) keep bIsReady
  // = false Items placed elsewhere also start with bIsReady = false to prevent
  // immediate pulling The bIsReady flag will be set to true at the end of the
  // current simulation tick

  // Initialize Stability if it's still at default uninitialized value
  if (Copy.bIsCluster) {
    for (FOrbData &Orb : Copy.Orbs) {
      if (Orb.Stability == 1000 && DefaultOrbLifetimeTicks > 0) {
        Orb.Stability = DefaultOrbLifetimeTicks;
      }
    }
  } else {
    if (Copy.Single.Stability == 1000 && DefaultOrbLifetimeTicks > 0) {
      Copy.Single.Stability = DefaultOrbLifetimeTicks;
    }
  }

  ItemMap.Add(H, Copy);
  OrbAt.Add(H, KindFromItem(Copy));

  // Add to NewlyPlacedItems to prevent belt processing this tick
  NewlyPlacedItems.Add(Copy.Id);

  return true;
}

bool USimWorldHex::TryTakeItem(const FHex &H, FItem &Out) {
  if (FItem *Found = ItemMap.Find(H)) {
    Out = *Found;
    ItemMap.Remove(H);
    OrbAt.Remove(H);
    // Belt progress bookkeeping
    if (Out.Id != 0) {
      BeltProgress.Remove(Out.Id);
    }
    return true;
  }
  return false;
}

// --------------------------------------------------

bool USimWorldHex::PlaceTile(const FHex &H, const FTile &T) {
  // Create a mutable copy of the tile to potentially convert Bond ->
  // InputBond/OutputBond/ClusterOutput
  FTile TileToPlace = T;

  if (T.Kind == ETileKind::Bond) {
    const FHex Partner = HexUtil::Neighbor(H, (EHexDir)(T.Arg % 6));
    const FTile *PartnerTile = TileMap.Find(Partner);
    const FTile *CurrentTile = TileMap.Find(H);

    // Auto-convert Bond tiles based on what they're placed between
    if (PartnerTile) {
      // Check if placing on Input tile with Input partner -> InputBond
      if (CurrentTile && CurrentTile->Kind == ETileKind::Input &&
          PartnerTile->Kind == ETileKind::Input) {
        TileToPlace.Kind = ETileKind::InputBond;
        UE_LOG(LogTemp, Log,
               TEXT("Bond placed on Input tile with Input partner - converting "
                    "to InputBond"));
      } else if (CurrentTile && CurrentTile->Kind == ETileKind::Output &&
                 PartnerTile->Kind == ETileKind::Output) {
        TileToPlace.Kind = ETileKind::OutputBond;
        UE_LOG(LogTemp, Log,
               TEXT("Bond placed on Output tile with Output partner - "
                    "converting to OutputBond"));
      } else if (PartnerTile->Kind != ETileKind::Empty &&
                 PartnerTile->Kind != ETileKind::Bond &&
                 PartnerTile->Kind != ETileKind::Input &&
                 PartnerTile->Kind != ETileKind::Output &&
                 PartnerTile->Kind != ETileKind::InputBond &&
                 PartnerTile->Kind != ETileKind::OutputBond &&
                 PartnerTile->Kind != ETileKind::ClusterInput &&
                 PartnerTile->Kind != ETileKind::ClusterOutput &&
                 PartnerTile->Kind != ETileKind::DiscoveryClusterInput) {
        UE_LOG(LogTemp, Warning,
               TEXT("Bond placement blocked: partner hex occupied by "
                    "incompatible tile type."));
        return false;
      }
    }
  }
  if (TileToPlace.Kind == ETileKind::Unbond) {
    const FHex Partner = HexUtil::Neighbor(H, (EHexDir)(TileToPlace.Arg % 6));
    if (TileMap.Contains(Partner) &&
        TileMap[Partner].Kind != ETileKind::Empty &&
        TileMap[Partner].Kind != ETileKind::Unbond) {
      UE_LOG(LogTemp, Warning,
             TEXT("Unbond placement blocked: partner hex occupied."));
      return false;
    }
  }
  if (TileToPlace.Kind == ETileKind::InputBond) {
    const FHex Partner = HexUtil::Neighbor(H, (EHexDir)(TileToPlace.Arg % 6));
    // Verify partner tile is an Input tile
    const FTile *PartnerTile = TileMap.Find(Partner);
    if (!PartnerTile || PartnerTile->Kind != ETileKind::Input) {
      UE_LOG(
          LogTemp, Warning,
          TEXT("InputBond placement blocked: partner must be an Input tile."));
      return false;
    }
    // Register the bond partnership
    IOBondPartners.Add(H, Partner);
    IOBondPartners.Add(Partner, H);
  }
  if (TileToPlace.Kind == ETileKind::OutputBond) {
    const FHex Partner = HexUtil::Neighbor(H, (EHexDir)(TileToPlace.Arg % 6));
    // Verify partner tile is an Output tile
    const FTile *PartnerTile = TileMap.Find(Partner);
    if (!PartnerTile || PartnerTile->Kind != ETileKind::Output) {
      UE_LOG(
          LogTemp, Warning,
          TEXT(
              "OutputBond placement blocked: partner must be an Output tile."));
      return false;
    }
    // Register the bond partnership
    IOBondPartners.Add(H, Partner);
    IOBondPartners.Add(Partner, H);
  }
  // --- OVERWRITE CLEANUP ---
  // If we're placing a new tile (including Empty), we must unregister any
  // legacy metadata associated with the previous tile at this location.
  if (TileMap.Contains(H)) {
    // Remove any IO bond partnerships
    if (const FHex *Partner = IOBondPartners.Find(H)) {
      IOBondPartners.Remove(*Partner);
      IOBondPartners.Remove(H);
    }

    // Unregister helix associations
    UnregisterHelixOutputTile(H);
    UnregisterHelixInputTile(H);

    // Unregister source definitions
    UnregisterSourceTile(H);

    // Clear metadata
    BeltTileOrbTypes.Remove(H);
    DiscoveryTileOrbTypes.Remove(H);

    // If we're placing Empty, finish here
    if (TileToPlace.Kind == ETileKind::Empty) {
      TileMap.Remove(H);
      ItemMap.Remove(H);
      OrbAt.Remove(H);
      ++TileRevision;
      return true;
    }
  } else if (TileToPlace.Kind == ETileKind::Empty) {
    // Already empty
    return true;
  }

  // --- EXECUTE PLACEMENT ---
  if (TileToPlace.Kind == ETileKind::Source && !CanPlaceSourceTileAt(H)) {
    UE_LOG(
        LogTemp, Warning,
        TEXT("Source placement blocked: invalid geometry (must be linear)."));
    return false;
  }

  TileMap.Add(H, TileToPlace);
  ++TileRevision;

  if (TileToPlace.Kind == ETileKind::Source) {
    RegisterSourceTile(H);
  }

  // Register helix output tiles for group tracking
  if (TileToPlace.Kind == ETileKind::HelixOutput) {
    RegisterHelixOutputTile(H);
  }

  // Note: HelixInput tiles are registered by HexPlacementComponent
  // since they require the output group index to pair with

  // ClusterOutput tiles are now directly placeable, no auto-conversion

  return true;
}

// --- Cluster Template API ---
void USimWorldHex::RegisterClusterTemplate(
    uint8 TemplateId, const TArray<FOrbData> &Orbs,
    const TArray<TPair<int32, int32>> &BondPairs, const FString &Name) {
  FClusterTemplate NewTemplate;
  NewTemplate.Orbs = Orbs;
  NewTemplate.Bonds = BondPairs;
  NewTemplate.Name = Name;
  ClusterTemplates.Add(TemplateId, NewTemplate);

  // UE_LOG(LogTemp, Log, TEXT("======================================"));
  // UE_LOG(LogTemp, Log, TEXT("Registered cluster template %d: %s"),
  // TemplateId, *Name); UE_LOG(LogTemp, Log, TEXT("  Orbs: %d"), Orbs.Num());
  // for (int32 i = 0; i < Orbs.Num(); i++)
  // {
  // 	const FOrbData& Orb = Orbs[i];
  // 	UE_LOG(LogTemp, Log, TEXT("    [%d] Type=%d at relative (%d,%d)"), i,
  // Orb.Type, Orb.dQ, Orb.dR);
  // }
  // UE_LOG(LogTemp, Log, TEXT("  Bonds: %d"), BondPairs.Num());
  // for (int32 i = 0; i < BondPairs.Num(); i++)
  // {
  // 	const TPair<int32, int32>& Bond = BondPairs[i];
  // 	UE_LOG(LogTemp, Log, TEXT("    Bond %d <-> %d"), Bond.Key, Bond.Value);
  // }
  // UE_LOG(LogTemp, Log, TEXT("======================================"));
}

bool USimWorldHex::ValidateClusterAgainstTemplate(const FItem &Cluster,
                                                  uint8 TemplateId) const {
  const FClusterTemplate *Template = ClusterTemplates.Find(TemplateId);
  if (!Template)
    return false;

  if (!Cluster.bIsCluster)
    return false;
  if (Cluster.Orbs.Num() != Template->Orbs.Num())
    return false;

  // TODO: Implement full pattern matching with rotation support
  // For now, just check orb count and types
  for (int32 i = 0; i < Template->Orbs.Num(); i++) {
    bool bFoundMatch = false;
    for (const FOrbData &ClusterOrb : Cluster.Orbs) {
      if (ClusterOrb.Type == Template->Orbs[i].Type) {
        bFoundMatch = true;
        break;
      }
    }
    if (!bFoundMatch)
      return false;
  }

  return true;
}

uint8 USimWorldHex::AutoGenerateClusterTemplateForOutputs(const FHex &Hex1,
                                                          const FHex &Hex2,
                                                          uint8 OrbType1,
                                                          uint8 OrbType2) {
  // Calculate relative position of Hex2 from Hex1
  int32 dQ = Hex2.Q - Hex1.Q;
  int32 dR = Hex2.R - Hex1.R;

  // Create a 2-orb cluster template
  TArray<FOrbData> Orbs;

  FOrbData Orb1;
  Orb1.Type = OrbType1;
  Orb1.dQ = 0;
  Orb1.dR = 0;
  Orbs.Add(Orb1);

  FOrbData Orb2;
  Orb2.Type = OrbType2;
  Orb2.dQ = dQ;
  Orb2.dR = dR;
  Orbs.Add(Orb2);

  // Create bond between the two orbs
  TArray<TPair<int32, int32>> BondPairs;
  BondPairs.Add(TPair<int32, int32>(0, 1));

  // Generate unique template ID
  uint8 TemplateId = NextAutoTemplateId++;

  // Register the template
  FString TemplateName = FString::Printf(TEXT("AutoGen_%d_%d_at_%d,%d"),
                                         OrbType1, OrbType2, Hex1.Q, Hex1.R);
  RegisterClusterTemplate(TemplateId, Orbs, BondPairs, TemplateName);

  return TemplateId;
}

uint8 USimWorldHex::ExpandClusterTemplate(
    uint8 ExistingTemplateId, const FHex &OldBaseHex, const FHex &NewBaseHex,
    const TArray<FHex> &AllClusterHexes,
    const TMap<FHex, uint8> &HexToOrbType) {
  UE_LOG(LogTemp, Warning, TEXT("╔════════════════════════════════════════"));
  UE_LOG(LogTemp, Warning, TEXT("║ EXPANDING CLUSTER TEMPLATE %d"),
         ExistingTemplateId);
  UE_LOG(LogTemp, Warning, TEXT("╠════════════════════════════════════════"));

  // Get the existing template
  const FClusterTemplate *ExistingTemplate =
      ClusterTemplates.Find(ExistingTemplateId);
  if (!ExistingTemplate) {
    UE_LOG(LogTemp, Error, TEXT("║ ERROR: Template %d not found!"),
           ExistingTemplateId);
    UE_LOG(LogTemp, Warning, TEXT("╚════════════════════════════════════════"));
    return ExistingTemplateId;
  }

  UE_LOG(LogTemp, Warning, TEXT("║ Old template had %d orbs, %d bonds"),
         ExistingTemplate->Orbs.Num(), ExistingTemplate->Bonds.Num());
  UE_LOG(LogTemp, Warning, TEXT("║ OldBaseHex: (%d,%d)"), OldBaseHex.Q,
         OldBaseHex.R);
  UE_LOG(LogTemp, Warning, TEXT("║ NewBaseHex: (%d,%d) %s"), NewBaseHex.Q,
         NewBaseHex.R,
         (OldBaseHex.Q == NewBaseHex.Q && OldBaseHex.R == NewBaseHex.R)
             ? TEXT("[SAME]")
             : TEXT("[CHANGED]"));
  UE_LOG(LogTemp, Warning, TEXT("║ Total cluster tiles: %d"),
         AllClusterHexes.Num());

  // Rebuild the entire template relative to the new BaseHex
  // This ensures all coordinates are correct even if BaseHex changed
  TArray<FOrbData> NewOrbs;
  UE_LOG(LogTemp, Warning, TEXT("║"));
  UE_LOG(LogTemp, Warning, TEXT("║ Rebuilding orbs relative to NewBaseHex:"));
  for (const FHex &ClusterHex : AllClusterHexes) {
    FOrbData NewOrb;
    const uint8 *OrbType = HexToOrbType.Find(ClusterHex);
    NewOrb.Type = OrbType ? *OrbType : 0;
    NewOrb.dQ = ClusterHex.Q - NewBaseHex.Q;
    NewOrb.dR = ClusterHex.R - NewBaseHex.R;
    NewOrbs.Add(NewOrb);

    UE_LOG(LogTemp, Warning,
           TEXT("║   Orb %d: World(%d,%d) -> Relative(%d,%d) Type=%d"),
           NewOrbs.Num() - 1, ClusterHex.Q, ClusterHex.R, NewOrb.dQ, NewOrb.dR,
           NewOrb.Type);
  }

  // Rebuild bonds between all adjacent tiles in the cluster
  TArray<TPair<int32, int32>> NewBondPairs;
  UE_LOG(LogTemp, Warning, TEXT("║"));
  UE_LOG(LogTemp, Warning, TEXT("║ Rebuilding bonds between adjacent tiles:"));
  for (int32 i = 0; i < AllClusterHexes.Num(); i++) {
    for (int32 j = i + 1; j < AllClusterHexes.Num(); j++) {
      const FHex &Hex1 = AllClusterHexes[i];
      const FHex &Hex2 = AllClusterHexes[j];

      // Check if adjacent (hex distance == 1)
      int32 dQ = Hex2.Q - Hex1.Q;
      int32 dR = Hex2.R - Hex1.R;
      int32 dist = (FMath::Abs(dQ) + FMath::Abs(dR) + FMath::Abs(dQ + dR)) / 2;

      if (dist == 1) {
        NewBondPairs.Add(TPair<int32, int32>(i, j));
        UE_LOG(LogTemp, Warning,
               TEXT("║   Bond: Orb[%d] <-> Orb[%d]  (Hexes (%d,%d)-(%d,%d))"),
               i, j, Hex1.Q, Hex1.R, Hex2.Q, Hex2.R);
      }
    }
  }

  // REUSE the existing template ID instead of generating a new one
  UE_LOG(LogTemp, Warning, TEXT("║"));
  UE_LOG(LogTemp, Warning, TEXT("║ REUSING TEMPLATE ID: %d (keeping same ID)"),
         ExistingTemplateId);
  UE_LOG(LogTemp, Warning, TEXT("║ Total orbs: %d"), NewOrbs.Num());
  UE_LOG(LogTemp, Warning, TEXT("║ Total bonds: %d"), NewBondPairs.Num());
  UE_LOG(LogTemp, Warning, TEXT("╚════════════════════════════════════════"));

  // Overwrite the existing template with the expanded one
  FString TemplateName = FString::Printf(TEXT("Expanded_%d_%dorbs"),
                                         ExistingTemplateId, NewOrbs.Num());
  RegisterClusterTemplate(ExistingTemplateId, NewOrbs, NewBondPairs,
                          TemplateName);

  return ExistingTemplateId;
}

TArray<FHex> USimWorldHex::FindAllTilesInCluster(const FHex &StartHex) const {
  TArray<FHex> ClusterTiles;
  TSet<FHex> Visited;
  TArray<FHex> ToVisit;

  ToVisit.Add(StartHex);
  Visited.Add(StartHex);

  UE_LOG(LogTemp, Warning, TEXT("FindAllTilesInCluster: Starting from (%d,%d)"),
         StartHex.Q, StartHex.R);

  while (ToVisit.Num() > 0) {
    FHex CurrentHex = ToVisit.Pop();
    ClusterTiles.Add(CurrentHex);

    UE_LOG(LogTemp, Warning, TEXT("  Processing tile (%d,%d)"), CurrentHex.Q,
           CurrentHex.R);

    // Find all partners of this hex (check BOTH directions since IOBondPartners
    // is not symmetric)
    for (const auto &PartnerKV : IOBondPartners) {
      FHex PartnerHex;
      bool bFoundPartner = false;

      // Check if CurrentHex is the KEY (A->B, we're at A)
      if (PartnerKV.Key == CurrentHex) {
        PartnerHex = PartnerKV.Value;
        bFoundPartner = true;
      }
      // Check if CurrentHex is the VALUE (A->B, we're at B)
      else if (PartnerKV.Value == CurrentHex) {
        PartnerHex = PartnerKV.Key;
        bFoundPartner = true;
      }

      if (bFoundPartner) {
        // Check if partner is a ClusterOutput tile
        const FTile *PartnerTile = TileMap.Find(PartnerHex);
        if (PartnerTile && PartnerTile->Kind == ETileKind::ClusterOutput) {
          if (!Visited.Contains(PartnerHex)) {

            Visited.Add(PartnerHex);
            ToVisit.Add(PartnerHex);
          }
        }
      }
    }
  }

  UE_LOG(LogTemp, Warning, TEXT("  Total tiles found in cluster: %d"),
         ClusterTiles.Num());

  return ClusterTiles;
}

// --- Bond-breaking public API (called by future glyph logic) ---
bool USimWorldHex::BreakBondBetween(int32 ItemA, int32 ItemB) {
  for (int32 i = 0; i < Bonds.Num(); ++i) {
    const FBond &B = Bonds[i];
    if ((B.ItemA == ItemA && B.ItemB == ItemB) ||
        (B.ItemA == ItemB && B.ItemB == ItemA)) {
      RemoveBondIndex(i);
      return true;
    }
  }
  return false;
}

int32 USimWorldHex::BreakAllBondsForItem(int32 ItemId) {
  int32 Removed = 0;
  // Gather first to avoid index churn while removing
  TArray<int32> ToRemove;
  for (int32 i = 0; i < Bonds.Num(); ++i) {
    const FBond &B = Bonds[i];
    if (B.ItemA == ItemId || B.ItemB == ItemId)
      ToRemove.Add(i);
  }
  // Remove in reverse order
  for (int32 idx = ToRemove.Num() - 1; idx >= 0; --idx) {
    RemoveBondIndex(ToRemove[idx]);
    ++Removed;
  }
  return Removed;
}

// --- CleanupBrokenBonds: only remove if an item vanished; otherwise update
// offsets ---
void USimWorldHex::CleanupBrokenBonds(const TSet<int32> &HeldIds) {
  if (Bonds.Num() == 0)
    return;

  // Reconstruct item positions, including synthetic positions for held items
  // (so bonds persist while held)
  TMap<int32, FHex> ItemPos;
  ItemPos.Reserve(ItemMap.Num() + HeldIds.Num());

  for (const auto &KV : ItemMap) {
    if (KV.Value.Id != 0)
      ItemPos.Add(KV.Value.Id, KV.Key);
  }

  for (int32 HeldId : HeldIds) {
    if (const FHex *Last = HeldItemLastHex.Find(HeldId)) {
      // Only add if not already present (should not be, since held items are
      // removed from ItemMap)
      ItemPos.FindOrAdd(HeldId, *Last);
    }
  }

  for (int32 i = Bonds.Num() - 1; i >= 0; --i) {
    FBond &B = Bonds[i];
    const FHex *AHex = ItemPos.Find(B.ItemA);
    const FHex *BHex = ItemPos.Find(B.ItemB);

    // If either truly gone (not on board and no last-held position), remove
    if (!AHex || !BHex) {
      RemoveBondIndex(i);
      continue;
    }

    // Update stored relative offset if it changed (items may have translated /
    // rotated logically)
    const int32 dq = BHex->Q - AHex->Q;
    const int32 dr = BHex->R - AHex->R;
    if (dq != B.dQ || dr != B.dR) {
      B.dQ = (int8)dq;
      B.dR = (int8)dr;
      B.Root = *AHex; // keep root near first item for debug
      if (bDevDebugBonds && GEngine) {
        GEngine->AddOnScreenDebugMessage(
            -1, 0.3f, FColor::Silver,
            FString::Printf(
                TEXT("Bond offset updated (held ok): %d-%d dq=%d dr=%d"),
                B.ItemA, B.ItemB, dq, dr));
      }
    }

    // Update HeldItemLastHex for held items to their current virtual positions
    // This ensures the virtual positions stay current as the arm moves/rotates
    if (HeldIds.Contains(B.ItemA)) {
      HeldItemLastHex.FindOrAdd(B.ItemA, *AHex);
    }
    if (HeldIds.Contains(B.ItemB)) {
      HeldItemLastHex.FindOrAdd(B.ItemB, *BHex);
    }
  }
}

void USimWorldHex::MoveBondedClusterOnBelt(int32 SeedItemId,
                                           const FHex &FromHex,
                                           const FHex &ToHex,
                                           const FTile &BeltTile,
                                           TMap<int32, FHex> &ItemHex) {
  // Recursively move all bonded items when one item in a cluster moves on a
  // belt This ensures the entire cluster moves as a cohesive unit

  // Check if the seed item has already been moved this tick (prevents infinite
  // loops)
  FItem *SeedItem = ItemMap.Find(FromHex);
  if (!SeedItem || SeedItem->Id != SeedItemId) {
    return; // Item doesn't exist at expected location
  }

  // If the item was already moved this tick, check if it's at the target
  // location
  if (SeedItem->LocationUpdatedThisTick) {
    // Verify if the item is already at the destination - if so, we're done
    if (FItem *DestItem = ItemMap.Find(ToHex)) {
      if (DestItem->Id == SeedItemId) {
        return; // Already moved to destination successfully
      }
    }
    return; // Already moved but not to our destination, let other movement
            // handle it
  }

  // Calculate the movement vector
  const int32 dQ = ToHex.Q - FromHex.Q;
  const int32 dR = ToHex.R - FromHex.R;

  // First, gather all bonded items that need to be moved as part of this
  // cluster
  TSet<int32> ClusterItemIds = GetFullClusterParentIds(SeedItemId);

  // Check if all destination hexes are available
  bool bCanMoveCluster = true;
  TArray<TPair<int32, FHex>> MovePairs; // ItemId -> destination hex

  for (int32 ItemId : ClusterItemIds) {
    const FHex *CurrentHex = ItemHex.Find(ItemId);
    if (!CurrentHex) {
      bCanMoveCluster = false;
      break;
    }

    // Check if item is locked (e.g. by helix portal)
    FItem *ItemPtr = ItemMap.Find(*CurrentHex);
    if (ItemPtr && ItemPtr->bIsLocked) {
      bCanMoveCluster = false;
      break;
    }

    FHex DestHex(CurrentHex->Q + dQ, CurrentHex->R + dR, CurrentHex->Level);

    // Check if destination is blocked by non-cluster items
    if (ItemMap.Contains(DestHex)) {
      FItem *DestItem = ItemMap.Find(DestHex);
      if (DestItem && !ClusterItemIds.Contains(DestItem->Id)) {
        bCanMoveCluster = false;
        break;
      }
    }

    MovePairs.Add(TPair<int32, FHex>(ItemId, DestHex));
  }

  if (!bCanMoveCluster) {
    // Cannot move the entire cluster, abort
    return;
  }

  // Now move all items in the cluster atomically
  TMap<FHex, FItem> TempStorage; // Store items temporarily to avoid conflicts

  // First pass: remove all items from their current positions
  for (const TPair<int32, FHex> &MovePair : MovePairs) {
    int32 ItemId = MovePair.Key;
    const FHex *CurrentHex = ItemHex.Find(ItemId);
    if (!CurrentHex)
      continue;

    FItem *Item = ItemMap.Find(*CurrentHex);
    if (!Item || Item->Id != ItemId)
      continue;

    // Store item temporarily
    FItem MovedItem = *Item;
    MovedItem.LocationUpdatedThisTick = true;
    TempStorage.Add(MovePair.Value, MovedItem);

    // Remove from current position
    ItemMap.Remove(*CurrentHex);
    OrbAt.Remove(*CurrentHex);
  }

  // Second pass: place all items at their new positions
  for (const TPair<FHex, FItem> &TempPair : TempStorage) {
    const FHex &DestHex = TempPair.Key;
    const FItem &Item = TempPair.Value;

    // Place item at destination
    ItemMap.Add(DestHex, Item);
    OrbAt.Add(DestHex, KindFromItem(Item));

    // Update ItemHex lookup
    ItemHex[Item.Id] = DestHex;

    // Update belt progress
    if (float *Progress = BeltProgress.Find(Item.Id)) {
      *Progress -= 1.f;
      if (*Progress < 0.f)
        *Progress = 0.f;
    }
  }
}

void USimWorldHex::MoveBondedItemRecursive(int32 ItemId, const FHex &FromHex,
                                           int32 dQ, int32 dR,
                                           TMap<int32, FHex> &ItemHex) {
  // This function is now deprecated in favor of the atomic cluster movement in
  // MoveBondedClusterOnBelt Keep it for compatibility but make it do nothing to
  // avoid conflicts
  return;
}

// ============================================================
// OCCUPANCY CACHE - computed once per tick, shared across steps
// ============================================================
void USimWorldHex::UpdateArmBaseCache() {
  CachedArmBaseHexes.Reset();
  for (const FArmState &A : Arms) {
    CachedArmBaseHexes.Add(A.BaseHex);
  }
  bArmBaseCacheDirty = false;
}

void USimWorldHex::UpdateHeldOrbCache() {
  CachedHeldOrbHexes.Reset();

  // Also update arm base cache if dirty
  if (bArmBaseCacheDirty) {
    UpdateArmBaseCache();
  }

  // Helper lambda for rotation
  auto Rot60 = [](FHex D, uint8 StepsCCW) -> FHex {
    for (uint8 i = 0; i < (StepsCCW % 6); ++i) {
      int32 q = D.Q;
      int32 r = D.R;
      D.Q = -r;
      D.R = q + r;
    }
    return D;
  };

  for (const FArmState &A : Arms) {
    if (!A.bHolding)
      continue;

    // Calculate grip hex
    FHex GripHex = A.BaseHex;
    for (int i = 0; i < A.ArmLen; ++i)
      GripHex = HexUtil::Neighbor(GripHex, A.Facing);

    if (!A.Held.bIsCluster) {
      CachedHeldOrbHexes.Add(GripHex);
    } else {
      // Add all orbs in held cluster
      const uint8 cur = (uint8)A.Facing % 6;
      const int8 grab = (int8)A.Held.GrabFacing;
      const uint8 facingDelta =
          (grab >= 0) ? (uint8)(((int32)cur - (int32)(grab % 6) + 6) % 6) : cur;
      const uint8 ccw = (uint8)((6 - facingDelta) % 6);

      for (const FOrbData &Sub : A.Held.Orbs) {
        FHex Local(Sub.dQ, Sub.dR);
        FHex Rot = Rot60(Local, ccw);
        FHex WorldPos(GripHex.Q + Rot.Q, GripHex.R + Rot.R, GripHex.Level);
        CachedHeldOrbHexes.Add(WorldPos);
      }
    }
  }
}

void USimWorldHex::UpdateWalkerFootCache() {
  CachedBlockingFootHexes.Reset();
  CachedIntendedFootHexes.Reset();
  for (const FWalkerState &W : Walkers) {
    auto AddFootToCaches = [&](const FFootState &F) {
      if (!F.bHoldingItem) {
        // Intended occupancy (for other walkers to avoid)
        CachedIntendedFootHexes.Add(F.CurrentTile);
        CachedIntendedFootHexes.Add(F.TargetTile);

        // Blocking occupancy (physically on ground, for belts/items to avoid)
        if (F.StepState == EFootStepState::Planted ||
            F.StepState == EFootStepState::Lowering) {
          CachedBlockingFootHexes.Add(F.CurrentTile);
        }
      }
    };
    AddFootToCaches(W.DFoot);
    AddFootToCaches(W.LFoot);
    AddFootToCaches(W.RFoot);
    AddFootToCaches(W.UFoot);
  }
}

void USimWorldHex::StepSim() {
  StepOrbStability();
  PrevItemHex.Empty();
  for (const auto &KV : ItemMap)
    if (KV.Value.Id != 0)
      PrevItemHex.Add(KV.Value.Id, KV.Key);
  ItemsMovedLastStep.Reset();

  // Reset LocationUpdatedThisTick flag for all items at the start of each tick
  for (auto &KV : ItemMap) {
    KV.Value.LocationUpdatedThisTick = false;
  }

  // Clear collision debug data for this frame
  CollisionDebugData.Empty();

  ++StepCounter;
  SkipInputThisFrame.Reset();
  SkipOutputThisFrame.Reset();
  OrbsNewlyMovedToOutput.Reset();
  OrbsMovedLastTick =
      OrbsNewlyMovedByBelts; // Preserve move state for grab checks
  OrbsNewlyMovedByBelts.Reset();
  EjectedItems.Empty();
  NewlyPlacedItems.Empty();
  RecentlyDroppedByArm.Empty();

  ApplyPendingArmMotions();
  ProcessUnbondsForHeldClusters();
  // pass 1 (empty at start)
  StepArms();
  OnArmsStepped.Broadcast();

  // Interaction pass 1: bond things sitting from last tick (including
  // ClusterSend arrivals)
  StepBonds();

  // Update occupancy cache after StepArms AND StepBonds - HeldOrbHexes and
  // ArmBaseHexes are now valid for use by StepWalkers and StepBelts.
  // Crucial to do this AFTER StepBonds because assimilation can move orbs from
  // board to held state.
  UpdateHeldOrbCache();
  UpdateWalkerFootCache();

  StepWalkers();
  UpdateLockedItems();
  StepBelts();

  // Portal Push: Allow helix input tiles to push items outward automatically
  // (REMOVED: This was pulling bonded clusters apart. Pushing is now handled by
  // TryEjectHelixCluster's pre-push logic)

  SpawnInputsAfterBelts(); // NEW pass 2 (inputs just vacated by belt moves)

  StepSourceGroups(); // Spawn source inputs if clear

  // Interaction pass 2: bond things that just arrived (Inputs, moved items)
  StepBonds();
  CheckAndRemoveDisconnectedOrbsFromHeldClusters();
  // Second pass for arms - allows immediate grab after unbonding
  StepArmsSecondPass();
  // Second pass for bonds - allows bond formation after rotations complete
  StepBondsSecondPass();

  // Check for automatic transmutations (7-Void ring → CatalystX)
  StepTransmutations();

  ProcessOutputs();

  EjectedItems.Empty();

  // Debug visualization - draw bond cylinders when DebugSpheres is enabled
  DrawBondDebugLines();
}
void USimWorldHex::StepGlyphs() {
  // Legacy entry point if other code still calls it:
  ProcessOutputs();
}

// SpawnInputs() moved to SimWorldHex_InputOutput.cpp

// SpawnInputsAfterBelts() moved to SimWorldHex_InputOutput.cpp

// DoesItemMatchOutputType() moved to SimWorldHex_InputOutput.cpp

// ProcessOutputs() moved to SimWorldHex_InputOutput.cpp

// TryArmPickItem() and TryArmPlaceItem() moved to SimWorldHex_InputOutput.cpp

// PeekItemAt(), TryPlaceOrbAt(), TryPickupOrbAt(), HasOrbAt(), GetOrbKindAt()
// moved to SimWorldHex_InputOutput.cpp

void USimWorldHex::AddItemBondLink(int32 ItemId, int32 BondIndex) {
  // Safety check: validate parameters
  if (ItemId <= 0 || BondIndex < 0)
    return;

  TArray<int32> &List = ItemToBonds.FindOrAdd(ItemId);
  List.Add(BondIndex);
}

void USimWorldHex::RemoveItemBondLink(int32 ItemId, int32 BondIndex) {
  // Safety check: validate parameters
  if (ItemId <= 0 || BondIndex < 0)
    return;

  if (TArray<int32> *List = ItemToBonds.Find(ItemId)) {
    List->Remove(BondIndex);
    if (List->Num() == 0)
      ItemToBonds.Remove(ItemId);
  }
}

void USimWorldHex::TryFormBondAt(const FHex &Root, uint8 Dir,
                                 const TSet<int32> &HeldIds,
                                 TSet<int32> &Newly) {
  const FHex AHex = Root;
  const FHex BHex = HexUtil::Neighbor(Root, (EHexDir)(Dir % 6));

  FItem *ItemA = ItemMap.Find(AHex);
  FItem *ItemB = ItemMap.Find(BHex);
  if (!ItemA || !ItemB)
    return;
  if (ItemA->Id == 0 || ItemB->Id == 0)
    return;
  if (ItemA->Id == ItemB->Id)
    return;

  if (HeldIds.Contains(ItemA->Id) || HeldIds.Contains(ItemB->Id))
    return;
  if (Newly.Contains(ItemA->Id) ||
      Newly.Contains(ItemB->Id)) { /* allow multiple bonds same tick but avoid
                                      duplication */
  }

  // Prevent duplicate bond between same two items (order-agnostic)
  auto AlreadyBondedPair = [&](int32 AId, int32 BId) {
    if (const TArray<int32> *AList = ItemToBonds.Find(AId)) {
      for (int32 BondIdx : *AList) {
        const FBond &Existing = Bonds[BondIdx];
        if ((Existing.ItemA == AId && Existing.ItemB == BId) ||
            (Existing.ItemA == BId && Existing.ItemB == AId))
          return true;
      }
    }
    return false;
  };
  if (AlreadyBondedPair(ItemA->Id, ItemB->Id))
    return;

  FBond NewB;
  NewB.ItemA = ItemA->Id;
  NewB.ItemB = ItemB->Id;
  NewB.Root = Root;
  NewB.dQ = (int8)(BHex.Q - AHex.Q);
  NewB.dR = (int8)(BHex.R - AHex.R);
  NewB.bActive = true;

  const int32 Index = Bonds.Add(NewB);
  AddItemBondLink(NewB.ItemA, Index);
  AddItemBondLink(NewB.ItemB, Index);
  Newly.Add(NewB.ItemA);
  Newly.Add(NewB.ItemB);

  if (GEngine && bDevDebugBonds) {
    GEngine->AddOnScreenDebugMessage(
        -1, 0.75f, FColor::Green,
        FString::Printf(TEXT("Bond formed: %d-%d (%d)"), NewB.ItemA, NewB.ItemB,
                        Index));
  }
}

void USimWorldHex::RemoveBondIndex(int32 Index) {
  if (!Bonds.IsValidIndex(Index))
    return;

  // Safety check: ensure the bond is valid before proceeding
  const FBond *BondPtr = &Bonds[Index];
  if (!BondPtr)
    return;

  const int32 LastIndex = Bonds.Num() - 1;
  const FBond Dead = *BondPtr;

  // Safety check: validate item IDs before removing links
  if (Dead.ItemA > 0 && Dead.ItemB > 0) {
    // Remove links for dead bond
    RemoveItemBondLink(Dead.ItemA, Index);
    RemoveItemBondLink(Dead.ItemB, Index);
  }

  // Swap removal if not last
  if (Index != LastIndex) {
    const FBond Moved = Bonds[LastIndex];
    Bonds.RemoveAtSwap(Index);

    // Fix moved bond indices in its items' lists
    auto FixList = [&](int32 ItemId) {
      if (ItemId > 0) // Safety check
      {
        if (TArray<int32> *L = ItemToBonds.Find(ItemId)) {
          for (int32 &Val : *L) {
            if (Val == LastIndex) {
              Val = Index;
              break;
            }
          }
        }
      }
    };
    FixList(Moved.ItemA);
    FixList(Moved.ItemB);
  } else {
    Bonds.RemoveAt(Index);
  }

  if (GEngine && bDevDebugBonds) {
    GEngine->AddOnScreenDebugMessage(
        -1, 0.75f, FColor::Red,
        FString::Printf(TEXT("Bond removed: %d-%d"), Dead.ItemA, Dead.ItemB));
  }

  // Check if either item was a Void orb that now has no bonds - delete it
  auto CheckAndDeleteVoidOrb = [&](int32 ItemId) {
    if (ItemId <= 0)
      return;

    // Check if this item still has any bonds
    if (const TArray<int32> *BondList = ItemToBonds.Find(ItemId)) {
      if (BondList->Num() > 0) {
        return; // Still has bonds, don't delete
      }
    }

    // Find the item on the board
    for (auto It = ItemMap.CreateIterator(); It; ++It) {
      FItem &Item = It->Value;
      if (Item.Id == ItemId) {
        // Check if it's a Void type orb
        bool bIsVoid = false;
        if (Item.bIsCluster) {
          // Check if any orb in the cluster is Void
          for (const FOrbData &Orb : Item.Orbs) {
            if (Orb.Type == (uint8)EOrbType::Void) {
              bIsVoid = true;
              break;
            }
          }
        } else {
          bIsVoid = (Item.Single.Type == EOrbType::Void);
        }

        if (bIsVoid) {
          // Delete the void orb from the board
          FHex HexToRemove = It->Key;
          OrbAt.Remove(HexToRemove);
          It.RemoveCurrent();

          if (GEngine && bDevDebugBonds) {
            GEngine->AddOnScreenDebugMessage(
                -1, 1.0f, FColor::Purple,
                FString::Printf(TEXT("Void orb deleted (no bonds): %d"),
                                ItemId));
          }
        }
        return;
      }
    }
  };

  CheckAndDeleteVoidOrb(Dead.ItemA);
  CheckAndDeleteVoidOrb(Dead.ItemB);
}

FVector USimWorldHex::HexCenterToWorld(const FHex &H) const {
  const float Tile = TileSizeUU;
  const EHexOrientation Ori = GridOrientation;
  const float Sqrt3 = 1.7320508075688772f;
  const float q = (float)H.Q;
  const float r = (float)H.R;

  FVector2D Local;
  if (Ori == EHexOrientation::PointyTop) {
    Local.X = Tile * (Sqrt3 * q + (Sqrt3 * 0.5f) * r);
    Local.Y = Tile * (1.5f * r);
  } else {
    Local.X = Tile * (1.5f * q);
    Local.Y = Tile * (Sqrt3 * r + (Sqrt3 * 0.5f) * q);
  }

  const float Rad = FMath::DegreesToRadians(GridYawDeg);
  const float CosA = FMath::Cos(Rad);
  const float SinA = FMath::Sin(Rad);
  const float RX = Local.X * CosA - Local.Y * SinA;
  const float RY = Local.X * SinA + Local.Y * CosA;
  const float Z = (H.Level == 0) ? OrbZOffset : -OrbZOffset;
  return WorldAnchor + FVector(RX, RY, Z);
}

bool USimWorldHex::GetItemVisualLocation(int32 ItemId,
                                         FVector &OutWorld) const {
  const FHex *CurHexPtr = nullptr;
  for (const auto &KV : ItemMap) {
    if (KV.Value.Id == ItemId) {
      CurHexPtr = &KV.Key;
      break;
    }
  }
  if (!CurHexPtr)
    return false;

  const FHex CurHex = *CurHexPtr;
  const FVector CurCenter = HexCenterToWorld(CurHex);

  // 1. Check if this item just exited a helix
  if (EjectedItems.Contains(ItemId)) {
    const FEjectedItemInfo &Info = EjectedItems[ItemId];
    float Alpha = FMath::Clamp(FrameAlpha, 0.f, 1.f);
    // Oldest rung was at the top of the helix column
    FVector StartPos = Info.Center + FVector(0.f, 0.f, Info.StartZ);
    OutWorld = FMath::Lerp(StartPos, CurCenter, Alpha);
    return true;
  }

  // 2. If this item advanced exactly one belt hex this logical step,
  // interpolate previous->current
  if (ItemsMovedLastStep.Contains(ItemId)) {
    if (const FHex *PrevHex = PrevItemHex.Find(ItemId)) {
      if (*PrevHex != CurHex) {
        const FVector PrevCenter = HexCenterToWorld(*PrevHex);
        float Alpha = FMath::Clamp(FrameAlpha, 0.f, 1.f);
        OutWorld = FMath::Lerp(PrevCenter, CurCenter, Alpha);
        return true;
      }
    }
  }

  // Standard belt in‑tile interpolation (current center -> next center)
  const FTile *Tile = TileMap.Find(CurHex);
  if (!Tile || Tile->Kind != ETileKind::Belt) {
    OutWorld = CurCenter;
    return true;
  }

  // Compute next hex now so we can check runtime blocking
  const uint8 Dir = (uint8)(Tile->Arg & 0xFF);
  const FHex NextHex = HexUtil::Neighbor(CurHex, (EHexDir)(Dir % 6));
  const FVector NextCenter = HexCenterToWorld(NextHex);

  // Determine whether the destination is blocked by a board item or any held
  // orb/hand
  auto IsDestBlocked = [&](const FHex &Dest) -> bool {
    // Board occupancy
    if (ItemMap.Contains(Dest))
      return true;

    // Held occupancy (hands and sub‑orbs)
    for (int32 ArmIdx = 0; ArmIdx < Arms.Num(); ++ArmIdx) {
      const FArmState &A = Arms[ArmIdx];
      if (!A.bHolding)
        continue;

      // Hand hex at committed sim reach/facing
      const FHex Grip = GetArmGripHex_Sim(ArmIdx);

      // Single held orb occupies the grip hex
      if (!A.Held.bIsCluster) {
        if (Grip == Dest)
          return true;
        continue;
      }

      // Cluster: each sub‑orb at rotated offsets
      const uint8 ccw = GetHeldCCW_Steps_Sim(ArmIdx);
      for (const FOrbData &Sub : A.Held.Orbs) {
        FHex Local(Sub.dQ, Sub.dR);
        FHex Rot = Rot60CCW(Local, ccw);
        FHex WorldHex(Grip.Q + Rot.Q, Grip.R + Rot.R);
        if (WorldHex == Dest)
          return true;
      }
    }
    return false;
  };

  const bool bDestBlocked = IsDestBlocked(NextHex);

  // Use belt progress for smooth visual interpolation
  const float *PLogicPtr = BeltProgress.Find(ItemId);
  float LogicalP = PLogicPtr ? *PLogicPtr : 0.f;
  const bool bBlockedEdge = LogicalP >= BeltBlockedClamp - 0.0001f;

  float VisualP = LogicalP;
  if (!bBlockedEdge) {
    const float Dt = FMath::Max(0.f, FixedStepSec);
    VisualP = LogicalP + FrameAlpha * Dt;

    // Never render beyond the belt's blocked edge
    VisualP = FMath::Min(VisualP, BeltBlockedClamp - 0.0001f);
  }

  // If destination is currently blocked, enforce blocked-edge clamping as an
  // extra safety
  if (bDestBlocked) {
    VisualP = FMath::Min(VisualP, BeltBlockedClamp - 0.0001f);
  }

  // Clamp to [0,1) for interpolation across THIS belt segment
  VisualP = FMath::Clamp(VisualP, 0.f, 0.9999f);

  OutWorld = FMath::Lerp(CurCenter, NextCenter, VisualP);
  return true;
}

bool USimWorldHex::GetOrbVisualLocation(int32 OrbId, FVector &OutWorld) const {
  if (OrbId <= 0)
    return false;

  // 1. Check ItemMap (on grid)
  for (const auto &KV : ItemMap) {
    const FItem &Item = KV.Value;
    if (Item.Id == OrbId) {
      // Found primary/single item
      return GetItemVisualLocation(Item.Id, OutWorld);
    }
    if (Item.bIsCluster) {
      for (const FOrbData &Sub : Item.Orbs) {
        if (Sub.ItemId == OrbId) {
          // Found sub-orb in a cluster on the board
          FVector AnchorWorld;
          if (GetItemVisualLocation(Item.Id, AnchorWorld)) {
            const float Tile = TileSizeUU;
            const EHexOrientation Ori = GridOrientation;

            // Items on belt don't rotate their clusters (yet), so we just add
            // world offset as if unrotated
            FVector2D Offset2D =
                HexCoord::AxialToXY(FHexAxial(Sub.dQ, Sub.dR), Tile, Ori);
            OutWorld = AnchorWorld + FVector(Offset2D.X, Offset2D.Y, 0.f);
            return true;
          }
        }
      }
    }
  }

  // 2. Check Arms (being held in simulation)
  for (int32 ArmIdx = 0; ArmIdx < Arms.Num(); ++ArmIdx) {
    const FArmState &A = Arms[ArmIdx];
    if (!A.bHolding)
      continue;

    if (A.Held.Id == OrbId) {
      OutWorld = HexCenterToWorld(GetArmGripHex_Sim(ArmIdx));
      return true;
    }
    if (A.Held.bIsCluster) {
      for (const FOrbData &Sub : A.Held.Orbs) {
        if (Sub.ItemId == OrbId) {
          const FHex Grip = GetArmGripHex_Sim(ArmIdx);
          const float Tile = TileSizeUU;
          const EHexOrientation Ori = GridOrientation;
          const uint8 ccw = GetHeldCCW_Steps_Sim(ArmIdx);
          FHex Local(Sub.dQ, Sub.dR);
          FHex Rot = Rot60CCW(Local, ccw);
          FVector2D Offset2D =
              HexCoord::AxialToXY(FHexAxial(Rot.Q, Rot.R), Tile, Ori);
          OutWorld =
              HexCenterToWorld(Grip) + FVector(Offset2D.X, Offset2D.Y, 0.f);
          return true;
        }
      }
    }
  }

  // 3. Check Walkers (being held in simulation)
  for (const FWalkerState &W : Walkers) {
    auto CheckFoot = [&](const FFootState &F) {
      if (!F.bHoldingItem)
        return false;
      if (F.HeldItem.Id == OrbId) {
        OutWorld = HexCenterToWorld(F.CurrentTile);
        return true;
      }
      if (F.HeldItem.bIsCluster) {
        for (const auto &Sub : F.HeldItem.Orbs) {
          if (Sub.ItemId == OrbId) {
            const float Tile = TileSizeUU;
            const EHexOrientation Ori = GridOrientation;
            FVector2D Offset2D =
                HexCoord::AxialToXY(FHexAxial(Sub.dQ, Sub.dR), Tile, Ori);
            OutWorld = HexCenterToWorld(F.CurrentTile) +
                       FVector(Offset2D.X, Offset2D.Y, 0.f);
            return true;
          }
        }
      }
      return false;
    };
    if (CheckFoot(W.DFoot))
      return true;
    if (CheckFoot(W.LFoot))
      return true;
    if (CheckFoot(W.RFoot))
      return true;
    if (CheckFoot(W.UFoot))
      return true;
  }

  return false;
}
// AddBoardOrbToHeldCluster() and AddBoardOrbToHeldItem() moved to
// SimWorldHex_Clusters.cpp CheckAndRemoveDisconnectedOrbsFromHeldClusters()
// moved to SimWorldHex_Clusters.cpp
// CheckAndRemoveDisconnectedOrbsFromHeldClusters() moved to
// SimWorldHex_Clusters.cpp
void USimWorldHex::DrawBondDebugLines() {
  // Draw bond cylinders when DebugSpheres is enabled
  if (!bDebugSpheres || Bonds.Num() == 0) {
    return;
  }

  UWorld *World = GetWorld();
  if (!World) {
    return;
  }

  // Get driver for coordinate conversion
  AOrbSimDriver *Driver = nullptr;
  for (TActorIterator<AOrbSimDriver> It(World); It; ++It) {
    Driver = *It;
    break;
  }

  if (!Driver) {
    return;
  }

  const AActor *Anchor = Driver->GetGridAnchor();
  const FVector AnchorLoc =
      Anchor ? Anchor->GetActorLocation() : FVector::ZeroVector;
  const float Tile = Driver->GetTileSize();
  const EHexOrientation DriverGridOrientation = Driver->GetGridOrientation();
  const float GridYaw = Driver->GetGridYaw();

  // Helper lambda to convert hex to world position
  auto HexToWorld = [&](const FHex &H) -> FVector {
    const FVector2D LocalXY =
        HexCoord::AxialToXY(FHexAxial(H.Q, H.R), Tile, DriverGridOrientation);
    const FVector2D Rotated = HexCoord::Rotate2D(LocalXY, GridYaw);
    const float Z = (H.Level == 0) ? OrbZOffset : -OrbZOffset;
    return AnchorLoc + FVector(Rotated.X, Rotated.Y, Z);
  };

  // Build lookup map for board items once
  TMap<int32, FHex> BoardItemLookup;
  BoardItemLookup.Reserve(ItemMap.Num());
  for (const auto &ItemKV : ItemMap) {
    if (ItemKV.Value.Id != 0) {
      BoardItemLookup.Add(ItemKV.Value.Id, ItemKV.Key);
    }
  }

  // Draw debug lines for all active bonds
  for (const FBond &Bond : Bonds) {
    if (!Bond.bActive) {
      continue;
    }

    // Find the actual positions of the bonded items
    FVector PosA = FVector::ZeroVector;
    FVector PosB = FVector::ZeroVector;
    bool bFoundA = false, bFoundB = false;

    // Check if items are on the board using lookup
    if (const FHex *HexA = BoardItemLookup.Find(Bond.ItemA)) {
      PosA = HexToWorld(*HexA);
      bFoundA = true;
    }
    if (const FHex *HexB = BoardItemLookup.Find(Bond.ItemB)) {
      PosB = HexToWorld(*HexB);
      bFoundB = true;
    }

    // Check if items are held by arms (for held clusters)
    if (!bFoundA || !bFoundB) {
      for (int32 ArmIndex = 0; ArmIndex < Arms.Num(); ++ArmIndex) {
        const FArmState &Arm = Arms[ArmIndex];
        if (!Arm.bHolding)
          continue;

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

        if (Arm.Held.bIsCluster) {
          for (const FOrbData &Sub : Arm.Held.Orbs) {
            if ((Sub.ItemId == Bond.ItemA && !bFoundA) ||
                (Sub.ItemId == Bond.ItemB && !bFoundB)) {
              FHex Local(Sub.dQ, Sub.dR);
              FHex Rot = Rot60(Local, ccw);
              FHex WorldHex(GripHex.Q + Rot.Q, GripHex.R + Rot.R);
              const FVector Pos = HexToWorld(WorldHex);
              if (Sub.ItemId == Bond.ItemA) {
                PosA = Pos;
                bFoundA = true;
              } else {
                PosB = Pos;
                bFoundB = true;
              }
            }
          }
        } else {
          if (Arm.Held.Id == Bond.ItemA && !bFoundA) {
            PosA = HexToWorld(GripHex);
            bFoundA = true;
          }
          if (Arm.Held.Id == Bond.ItemB && !bFoundB) {
            PosB = HexToWorld(GripHex);
            bFoundB = true;
          }
        }
      }
    }

    // Only draw if both positions were found
    if (!bFoundA || !bFoundB) {
      continue;
    }

    // Add some height offset to make bonds visible above the ground
    PosA.Z += 15.f;
    PosB.Z += 15.f;

    // Calculate bond direction and distance
    FVector BondDirection = (PosB - PosA).GetSafeNormal();
    float BondLength = FVector::Dist(PosA, PosB);
    FVector BondMidpoint = (PosA + PosB) * 0.5f;

    // Draw cylinder to represent the bond
    if (BondLength > 0.f) {
      // Calculate rotation to align cylinder with bond direction
      FVector Up = FVector::UpVector;
      FVector Right = FVector::CrossProduct(BondDirection, Up).GetSafeNormal();
      if (Right.IsNearlyZero()) {
        Right = FVector::RightVector;
      }
      FVector Forward =
          FVector::CrossProduct(Right, BondDirection).GetSafeNormal();

      // Create rotation matrix from the three axes
      FMatrix RotationMatrix =
          FMatrix(Forward, Right, BondDirection, FVector::ZeroVector);
      FRotator BondRotation = FQuat(RotationMatrix).Rotator();

      // Draw the cylinder (persistent for 1 second to avoid flickering)
      DrawDebugCylinder(World, PosA, PosB, 2.f, 8, FColor::Cyan, false, 1.0f, 0,
                        1.f);

      // Draw small spheres at bond endpoints (persistent for 1 second)
      // DrawDebugSphere(World, PosA, 4.f, 8, FColor::Cyan, false, 1.0f,
      // 0, 1.f); DrawDebugSphere(World, PosB, 4.f, 8, FColor::Cyan,
      // false, 1.0f, 0, 1.f);

      // Draw bond ID text at midpoint (persistent for 1 second)
      FVector TextPosition = BondMidpoint;
      TextPosition.Z += 10.f;
      DrawDebugString(
          World, TextPosition,
          FString::Printf(TEXT("Bond: %d-%d"), Bond.ItemA, Bond.ItemB), nullptr,
          FColor::Cyan, 1.0f, true, 0.8f);
    }
  }
}

FHex USimWorldHex::GetArmGripHex_Sim(int32 ArmIndex) const {
  if (ArmIndex < 0 || ArmIndex >= Arms.Num())
    return FHex(0, 0);
  const FArmState &A = Arms[ArmIndex];
  const uint8 SimFace =
      (SimFacingByArm.Contains(ArmIndex) ? SimFacingByArm[ArmIndex]
                                         : uint8(A.Facing));
  const int32 SimLen =
      (SimArmLenByArm.Contains(ArmIndex) ? SimArmLenByArm[ArmIndex] : A.ArmLen);
  FHex Grip = A.BaseHex;
  for (int i = 0; i < SimLen; ++i)
    Grip = HexUtil::Neighbor(Grip, (EHexDir)(SimFace % 6));
  return Grip;
}

uint8 USimWorldHex::GetHeldCCW_Steps_Sim(int32 ArmIndex) const {
  if (ArmIndex < 0 || ArmIndex >= Arms.Num())
    return 0;
  const FArmState &A = Arms[ArmIndex];
  const uint8 cur =
      (SimFacingByArm.Contains(ArmIndex) ? SimFacingByArm[ArmIndex]
                                         : uint8(A.Facing)) %
      6;

  int8 simGrab = int8(-1);
  if (const int8 *P = SimGrabFacingByArm.Find(ArmIndex)) {
    simGrab = *P;
  }
  // If sim grab is invalid, fall back to the current held grab (if any)
  const int8 grab = (simGrab >= 0)
                        ? simGrab
                        : (A.Held.bIsCluster ? A.Held.GrabFacing : int8(-1));

  const uint8 facingDelta =
      (grab >= 0) ? (uint8)(((int32)cur - (int32)(grab % 6) + 6) % 6) : cur;

  return (uint8)((6 - facingDelta) % 6); // CCW steps to convert local->world
}

FHex USimWorldHex::Rot60CCW(FHex D, uint8 StepsCCW) {
  for (uint8 i = 0; i < (StepsCCW % 6); ++i) {
    const int32 q = D.Q;
    const int32 r = D.R;
    D.Q = -r;
    D.R = q + r;
  }
  return D;
}

FHex USimWorldHex::Rot60CW(FHex D, uint8 StepsCW) {
  return Rot60CCW(D, (uint8)((6 - (StepsCW % 6)) % 6));
}

void USimWorldHex::ProcessPendingBondFormations(float DeltaTime) {
  // DEBUG: Log total pending formations (commented out for production)
  // if (PendingBondFormations.Num() > 0)
  // {
  //	UE_LOG(LogAGSim, Warning, TEXT("Processing %d pending bond formations"),
  // PendingBondFormations.Num());
  // }

  for (int32 i = PendingBondFormations.Num() - 1; i >= 0; i--) {
    FPendingBondFormation &Pending = PendingBondFormations[i];

    // Check if the arm is still valid and holding the item
    if (Pending.ArmIndex < 0 || Pending.ArmIndex >= Arms.Num()) {
      PendingBondFormations.RemoveAt(i);
      continue;
    }

    const FArmState &Arm = Arms[Pending.ArmIndex];
    if (!Arm.bHolding) {
      // Arm is no longer holding anything, cancel the pending formation
      PendingBondFormations.RemoveAt(i);
      continue;
    }

    // Check if the board item still exists
    const FItem *BoardItem = ItemMap.Find(Pending.BoardItemHex);
    if (!BoardItem || BoardItem->Id != Pending.BoardItemId) {
      // Board item no longer exists, cancel the pending formation
      PendingBondFormations.RemoveAt(i);
      continue;
    }

    // Decrease time remaining
    Pending.TimeRemaining -= DeltaTime;

    if (Pending.TimeRemaining <= 0.f) {
      // Time to form the bond - use the current held item ID
      if (bDebugLogs && bDevDebugBonds && GEngine) {
        GEngine->AddOnScreenDebugMessage(
            -1, 1.0f, FColor::Green,
            FString::Printf(
                TEXT("Processing pending bond: board orb %d -> held item %d"),
                Pending.BoardItemId, Arm.Held.Id));
      }

      AddBoardOrbToHeldItem(Arm.Held.Id, Pending.BoardItemId,
                            Pending.BoardItemHex);
      PendingBondFormations.RemoveAt(i);
    }
  }
}

void USimWorldHex::OnArmRotationComplete(int32 /*ArmIndex*/) {
  // No-op for bonding/merges with sim-delayed facing.
  // Bonds and held-held merges are resolved in StepBonds using committed sim
  // state.
}

void USimWorldHex::OnArmRotationFullyComplete(int32 ArmIndex) {
  // Visual event only: record state for UX/other subsystems if desired.
  // Do not commit sim-facing here to avoid coupling sim to render frame timing.
  if (ArmIndex >= 0 && ArmIndex < Arms.Num()) {
    FArmState &S = Arms[ArmIndex];
    S.bRotationCompleted = true;
  }
}

void USimWorldHex::CancelPendingBondFormationsForArm(int32 ArmIndex) {
  // Cancel any pending bond formations for this arm
  for (int32 i = PendingBondFormations.Num() - 1; i >= 0; i--) {
    FPendingBondFormation &Pending = PendingBondFormations[i];

    if (Pending.ArmIndex == ArmIndex) {
      if (bDebugLogs && bDevDebugBonds && GEngine) {
        GEngine->AddOnScreenDebugMessage(
            -1, 1.0f, FColor::Yellow,
            FString::Printf(TEXT("Canceling pending bond formation for arm %d "
                                 "(arm dropped item)"),
                            ArmIndex));
      }

      PendingBondFormations.RemoveAt(i);
    }
  }
}

// --------------------------------------------------

// TriggerTile signal management
void USimWorldHex::SetBeltTriggerSignal(const FHex &TriggerHex,
                                        const FHex &BeltOwnerHex,
                                        bool bPositive) {
  TMap<FHex, bool> &Signals = BeltTriggerSignals.FindOrAdd(BeltOwnerHex);
  Signals.Add(TriggerHex, bPositive);
}

void USimWorldHex::ClearBeltTriggerSignal(const FHex &TriggerHex,
                                          const FHex &BeltOwnerHex) {
  if (TMap<FHex, bool> *Signals = BeltTriggerSignals.Find(BeltOwnerHex)) {
    Signals->Remove(TriggerHex);
    if (Signals->Num() == 0) {
      BeltTriggerSignals.Remove(BeltOwnerHex);
    }
  }
}

bool USimWorldHex::IsBeltEnabled(const FHex &BeltHex) const {
  const TMap<FHex, bool> *Signals = BeltTriggerSignals.Find(BeltHex);
  if (!Signals || Signals->Num() == 0) {
    return true; // No triggers = enabled by default
  }

  // Logical AND: All associated trigger tiles must have a positive signal
  for (const auto &KV : *Signals) {
    if (!KV.Value) {
      return false; // Any negative signal disables the belt
    }
  }

  return true; // All signals are positive
}

void USimWorldHex::SetArmTriggerSignal(const FHex &TriggerHex,
                                       const FHex &ArmBaseHex, bool bPositive) {
  TMap<FHex, bool> &Signals = ArmTriggerSignals.FindOrAdd(ArmBaseHex);

  // Check if signal actually changed before logging
  bool bSignalChanged = true;
  if (const bool *OldSignal = Signals.Find(TriggerHex)) {
    bSignalChanged = (*OldSignal != bPositive);
  }

  Signals.Add(TriggerHex, bPositive);

  // Only log on actual state changes to reduce spam
  if (bDebugLogs && bSignalChanged) {
    UE_LOG(LogTemp, Log,
           TEXT("Sim: Arm at (%d,%d) signal CHANGED from (%d,%d): %s. Total "
                "triggers: %d"),
           ArmBaseHex.Q, ArmBaseHex.R, TriggerHex.Q, TriggerHex.R,
           bPositive ? TEXT("POSITIVE") : TEXT("NEGATIVE"), Signals.Num());
  }
}

void USimWorldHex::ClearArmTriggerSignal(const FHex &TriggerHex,
                                         const FHex &ArmBaseHex) {
  if (TMap<FHex, bool> *Signals = ArmTriggerSignals.Find(ArmBaseHex)) {
    Signals->Remove(TriggerHex);
    if (Signals->Num() == 0) {
      ArmTriggerSignals.Remove(ArmBaseHex);
    }
  }
  // Don't clear activation - it persists until reset command
}

bool USimWorldHex::IsArmEnabled(const FHex &ArmBaseHex) const {
  const TMap<FHex, bool> *Signals = ArmTriggerSignals.Find(ArmBaseHex);
  if (!Signals || Signals->Num() == 0) {
    return true; // No triggers = enabled by default
  }

  // Logical AND: All associated trigger tiles must have a positive signal
  for (const auto &KV : *Signals) {
    if (!KV.Value) {
      if (bDebugLogs) {
        UE_LOG(LogTemp, Log,
               TEXT("Sim: Arm at (%d,%d) DISABLED by trigger at (%d,%d)"),
               ArmBaseHex.Q, ArmBaseHex.R, KV.Key.Q, KV.Key.R);
      }
      return false; // Any negative signal disables the arm
    }
  }

  return true; // All signals are positive
}

TArray<FHex>
USimWorldHex::FindClusterInputChain(const FHex &StartHex,
                                    const TSet<FHex> &ExcludeHexes) const {
  TArray<FHex> Chain;
  TArray<FHex> ToVisit;
  TSet<FHex> Visited;

  ToVisit.Add(StartHex);
  Visited.Add(StartHex);

  while (ToVisit.Num() > 0) {
    FHex Current = ToVisit.Pop();
    Chain.Add(Current);

    for (int i = 0; i < 6; ++i) {
      FHex Neighbor = HexUtil::Neighbor(Current, (EHexDir)i);
      if (Visited.Contains(Neighbor) || ExcludeHexes.Contains(Neighbor)) {
        continue;
      }

      const FTile *Tile = TileMap.Find(Neighbor);
      if (Tile && (Tile->Kind == ETileKind::ClusterInput ||
                   Tile->Kind == ETileKind::DiscoveryClusterInput)) {
        Visited.Add(Neighbor);
        ToVisit.Add(Neighbor);
      }
    }
  }

  return Chain;
}
EOrbType USimWorldHex::GetOrbTypeAtLocation(const FHex &H) const {
  // 1. Check direct board items (singles)
  if (const FItem *Item = ItemMap.Find(H)) {
    if (!Item->bIsCluster) {
      return Item->Single.Type;
    }
  }

  // 2. Check cluster members on board
  for (const auto &KV : ItemMap) {
    const FItem &Item = KV.Value;
    if (Item.bIsCluster) {
      for (const FOrbData &Orb : Item.Orbs) {
        FHex OrbPos(KV.Key.Q + Orb.dQ, KV.Key.R + Orb.dR, KV.Key.Level);
        if (OrbPos == H)
          return (EOrbType)Orb.Type;
      }
    }
  }

  // 3. Check items held by arms
  for (int32 i = 0; i < Arms.Num(); ++i) {
    const FArmState &A = Arms[i];
    if (!A.bHolding)
      continue;

    // Calculate grip hex using the most current sim position (Pending -> Sim ->
    // Base)
    uint8 Facing = static_cast<uint8>(A.Facing);
    if (const uint8 *PF = PendingFacingByArm.Find(i))
      Facing = *PF;
    else if (const uint8 *SF = SimFacingByArm.Find(i))
      Facing = *SF;

    int32 ArmLen = A.ArmLen;
    if (const int32 *PL = PendingArmLenByArm.Find(i))
      ArmLen = *PL;
    else if (const int32 *SL = SimArmLenByArm.Find(i))
      ArmLen = *SL;

    FHex GripHex = A.BaseHex;
    for (int j = 0; j < ArmLen; ++j)
      GripHex = HexUtil::Neighbor(GripHex, (EHexDir)Facing);

    if (!A.Held.bIsCluster) {
      if (GripHex == H)
        return A.Held.Single.Type;
    } else {
      const uint8 cur = Facing % 6;

      int8 grab = A.Held.GrabFacing;
      if (const int8 *PG = PendingGrabFacingByArm.Find(i))
        grab = *PG;
      else if (const int8 *SG = SimGrabFacingByArm.Find(i))
        grab = *SG;

      const uint8 facingDelta =
          (grab >= 0) ? (uint8)(((int32)cur - (int32)(grab % 6) + 6) % 6) : cur;
      const uint8 ccw = (uint8)((6 - facingDelta) % 6);

      for (const FOrbData &Sub : A.Held.Orbs) {
        FHex Local(Sub.dQ, Sub.dR, 0); // Local delta is at Level 0
        FHex Rot = Local.RotateCCW(ccw);
        FHex WorldPos(GripHex.Q + Rot.Q, GripHex.R + Rot.R, GripHex.Level);
        if (WorldPos == H)
          return (EOrbType)Sub.Type;
      }
    }
  }

  return EOrbType::None;
}

// Helper: get the orb type of a board item by its ID
static EOrbType GetOrbTypeById(const TMap<FHex, FItem> &ItemMap, int32 ItemId) {
  for (const auto &KV : ItemMap) {
    const FItem &Item = KV.Value;
    if (Item.Id == ItemId) {
      if (!Item.bIsCluster)
        return Item.Single.Type;
      if (Item.Orbs.Num() > 0)
        return (EOrbType)Item.Orbs[0].Type;
    }
    // Check sub-orbs in clusters
    if (Item.bIsCluster) {
      for (const FOrbData &Sub : Item.Orbs) {
        if (Sub.ItemId == ItemId)
          return (EOrbType)Sub.Type;
      }
    }
  }
  return EOrbType::None;
}

// Helper: collect the types of all bonded neighbors for a given item ID
static void
GetBondedNeighborTypes(int32 ItemId, const TArray<USimWorldHex::FBond> &Bonds,
                       const TMap<int32, TArray<int32>> &ItemToBonds,
                       const TMap<FHex, FItem> &ItemMap,
                       TArray<EOrbType> &OutTypes) {
  const TArray<int32> *BondList = ItemToBonds.Find(ItemId);
  if (!BondList)
    return;
  for (int32 BondIdx : *BondList) {
    if (!Bonds.IsValidIndex(BondIdx))
      continue;
    const USimWorldHex::FBond &B = Bonds[BondIdx];
    if (!B.bActive)
      continue;
    int32 OtherId = (B.ItemA == ItemId) ? B.ItemB : B.ItemA;
    EOrbType OtherType = GetOrbTypeById(ItemMap, OtherId);
    if (OtherType != EOrbType::None)
      OutTypes.Add(OtherType);
  }
}

// Compute net stability delta for an orb given its bonded neighbor types
// Returns: positive = bonus ticks, negative = penalty ticks
// Sets bCatalystImmune to true if any neighbor is CatalystX
static int32
ComputeBondSynergyDelta(EOrbType MyType, const TArray<EOrbType> &NeighborTypes,
                        bool &bCatalystImmune, int32 SameTypeBondBonus,
                        int32 MetalBondBonus, int32 VoidBondPenalty,
                        bool bCatalystGrantsInfiniteLife) {
  int32 Delta = 0;
  bCatalystImmune = false;

  for (EOrbType NeighborType : NeighborTypes) {
    // CatalystX grants infinite lifetime
    if (NeighborType == EOrbType::CatalystX && bCatalystGrantsInfiniteLife) {
      bCatalystImmune = true;
      return 0; // No need to compute more — this orb is immune
    }

    // Void neighbor corrodes typed orbs
    if (NeighborType == EOrbType::Void && MyType != EOrbType::Void) {
      Delta -= VoidBondPenalty;
      continue;
    }

    // MetalA neighbor stabilizes
    if (NeighborType == EOrbType::MetalA && MyType != EOrbType::MetalA) {
      Delta += MetalBondBonus;
      continue;
    }

    // Same-type bond bonus
    if (NeighborType == MyType && MyType != EOrbType::Void &&
        MyType != EOrbType::None) {
      Delta += SameTypeBondBonus;
      continue;
    }
  }

  return Delta;
}

void USimWorldHex::StepOrbStability() {
  if (!bOrbLifetimeEnabled || DefaultOrbLifetimeTicks <= 0)
    return;

  // Lambda: get lifetime for a given orb type
  auto GetLifetimeForType = [&](uint8 OrbType) -> int32 {
    if (const int32 *Override = OrbLifetimeByType.Find(OrbType))
      return *Override;
    return DefaultOrbLifetimeTicks;
  };

  // Lambda: apply decay to a single orb, considering bond synergies
  // Returns true if the orb became Void this tick
  auto DecayOrb = [&](EOrbType &Type, EOrbColor &Color, int32 &Stability,
                      int32 ItemId) -> bool {
    if (Type == EOrbType::Void || Type == EOrbType::None)
      return false; // Void and None don't decay

    // CatalystX never decays
    if (Type == EOrbType::CatalystX)
      return false;

    // Bond synergy check
    if (bBondSynergiesEnabled && ItemId > 0) {
      TArray<EOrbType> NeighborTypes;
      GetBondedNeighborTypes(ItemId, Bonds, ItemToBonds, ItemMap,
                             NeighborTypes);

      bool bImmune = false;
      int32 SynergyDelta = ComputeBondSynergyDelta(
          Type, NeighborTypes, bImmune, SameTypeBondBonus, MetalBondBonus,
          VoidBondPenalty, bCatalystGrantsInfiniteLife);

      if (bImmune)
        return false; // CatalystX neighbor → skip decay entirely

      // Apply synergy: net decay = 1 - bonus (minimum 0 = no decay, can go
      // negative for extra drain) A positive SynergyDelta reduces decay, a
      // negative one increases it
      int32 NetDecay = 1 - SynergyDelta; // base decay is 1 per tick
      if (NetDecay < 0)
        NetDecay = 0; // Can't gain stability from synergy alone (floor at 0
                      // decay)
      // Actually, let's allow negative decay (net gain) to let strong synergies
      // extend lifetime But cap at the type's max lifetime
      int32 MaxLife = GetLifetimeForType((uint8)Type);

      Stability -= (1 - SynergyDelta); // Apply net decay
      if (Stability > MaxLife)
        Stability = MaxLife; // Cap at max
    } else {
      // No synergy — simple decay
      Stability--;
    }

    if (Stability <= 0) {
      Type = EOrbType::Void;
      Color = EOrbColor::None;
      // Reset stability to Void's lifetime
      Stability = GetLifetimeForType((uint8)EOrbType::Void);
      return true;
    }
    return false;
  };

  // --- Board items ---
  for (auto &KV : ItemMap) {
    FHex H = KV.Key;
    FItem &Item = KV.Value;
    bool bBecameVoid = false;

    if (Item.bIsCluster) {
      for (FOrbData &Orb : Item.Orbs) {
        EOrbType OrbType = (EOrbType)Orb.Type;
        EOrbColor OrbColor = (EOrbColor)Orb.Color;
        if (DecayOrb(OrbType, OrbColor, Orb.Stability, Orb.ItemId)) {
          bBecameVoid = true;
        }
        Orb.Type = (uint8)OrbType;
        Orb.Color = (uint8)OrbColor;
      }
    } else {
      if (DecayOrb(Item.Single.Type, Item.Single.Color, Item.Single.Stability,
                   Item.Id)) {
        bBecameVoid = true;
      }
    }

    if (bBecameVoid) {
      OrbAt.Add(H, KindFromItem(Item));
    }
  }

  // --- Held items (arms) ---
  for (FArmState &Arm : Arms) {
    if (!Arm.bHolding)
      continue;

    if (Arm.Held.bIsCluster) {
      for (FOrbData &Orb : Arm.Held.Orbs) {
        EOrbType OrbType = (EOrbType)Orb.Type;
        EOrbColor OrbColor = (EOrbColor)Orb.Color;
        DecayOrb(OrbType, OrbColor, Orb.Stability, Orb.ItemId);
        Orb.Type = (uint8)OrbType;
        Orb.Color = (uint8)OrbColor;
      }
    } else {
      DecayOrb(Arm.Held.Single.Type, Arm.Held.Single.Color,
               Arm.Held.Single.Stability, Arm.Held.Id);
    }
  }

  // --- Held items (walker feet) ---
  for (FWalkerState &W : Walkers) {
    auto DecayFootHeld = [&](FFootState &Foot) {
      if (!Foot.bHoldingItem)
        return;
      if (Foot.HeldItem.bIsCluster) {
        for (FOrbData &Orb : Foot.HeldItem.Orbs) {
          EOrbType OrbType = (EOrbType)Orb.Type;
          EOrbColor OrbColor = (EOrbColor)Orb.Color;
          DecayOrb(OrbType, OrbColor, Orb.Stability, Orb.ItemId);
          Orb.Type = (uint8)OrbType;
          Orb.Color = (uint8)OrbColor;
        }
      } else {
        DecayOrb(Foot.HeldItem.Single.Type, Foot.HeldItem.Single.Color,
                 Foot.HeldItem.Single.Stability, Foot.HeldItem.Id);
      }
    };
    DecayFootHeld(W.DFoot);
    DecayFootHeld(W.LFoot);
    DecayFootHeld(W.RFoot);
    DecayFootHeld(W.UFoot);
  }
}

// ============================================================
// PHASE 2: Automatic CatalystX Transmutation
// Detects 7-Void filled hex ring and converts center to CatalystX.
// Checks both board items AND held clusters (arms + walker feet).
// ============================================================
void USimWorldHex::StepTransmutations() {
  // Clear previous tick's consumed IDs
  TransmutedItemIds.Empty();

  // Build a lookup: ItemId -> (Hex, OrbType) for ALL orbs in the simulation
  TMap<int32, FHex> ItemToHex;
  TMap<int32, EOrbType> ItemToType;

  // --- Index board items ---
  for (const auto &KV : ItemMap) {
    const FItem &Item = KV.Value;
    if (!Item.bIsCluster) {
      ItemToHex.Add(Item.Id, KV.Key);
      ItemToType.Add(Item.Id, Item.Single.Type);
    }
    if (Item.bIsCluster) {
      for (const FOrbData &Sub : Item.Orbs) {
        if (Sub.ItemId > 0) {
          FHex SubHex(KV.Key.Q + Sub.dQ, KV.Key.R + Sub.dR, KV.Key.Level);
          ItemToHex.Add(Sub.ItemId, SubHex);
          ItemToType.Add(Sub.ItemId, (EOrbType)Sub.Type);
        }
      }
    }
  }

  // --- Index held items (arms) ---
  for (int32 ArmIndex = 0; ArmIndex < Arms.Num(); ++ArmIndex) {
    const FArmState &Arm = Arms[ArmIndex];
    if (!Arm.bHolding)
      continue;

    const FHex GripHex = GetArmGripHex_Sim(ArmIndex);
    const uint8 CCW = GetHeldCCW_Steps_Sim(ArmIndex);

    if (Arm.Held.bIsCluster) {
      for (const FOrbData &Sub : Arm.Held.Orbs) {
        if (Sub.ItemId > 0) {
          FHex LocalHex(Sub.dQ, Sub.dR);
          FHex RotHex = Rot60CCW(LocalHex, CCW);
          FHex WorldHex(GripHex.Q + RotHex.Q, GripHex.R + RotHex.R,
                        GripHex.Level);
          ItemToHex.Add(Sub.ItemId, WorldHex);
          ItemToType.Add(Sub.ItemId, (EOrbType)Sub.Type);
        }
      }
    } else {
      ItemToHex.Add(Arm.Held.Id, GripHex);
      ItemToType.Add(Arm.Held.Id, Arm.Held.Single.Type);
    }
  }

  // --- Index held items (walker feet) ---
  auto IndexFootHeld = [&](const FFootState &Foot) {
    if (!Foot.bHoldingItem)
      return;
    if (Foot.HeldItem.bIsCluster) {
      for (const FOrbData &Sub : Foot.HeldItem.Orbs) {
        if (Sub.ItemId > 0) {
          FHex WorldHex(Foot.CurrentTile.Q + Sub.dQ,
                        Foot.CurrentTile.R + Sub.dR, Foot.CurrentTile.Level);
          ItemToHex.Add(Sub.ItemId, WorldHex);
          ItemToType.Add(Sub.ItemId, (EOrbType)Sub.Type);
        }
      }
    } else {
      ItemToHex.Add(Foot.HeldItem.Id, Foot.CurrentTile);
      ItemToType.Add(Foot.HeldItem.Id, Foot.HeldItem.Single.Type);
    }
  };
  for (const FWalkerState &W : Walkers) {
    IndexFootHeld(W.DFoot);
    IndexFootHeld(W.LFoot);
    IndexFootHeld(W.RFoot);
    IndexFootHeld(W.UFoot);
  }

  // --- Now scan ALL indexed Void orbs as potential ring centers ---
  // Each transmutation: center Void + 6 bonded Void ring -> center becomes CatalystX, ring consumed
  struct FTransmutationCandidate {
    int32 CenterId;
    FHex CenterHex;
    TArray<int32> RingIds;
    TArray<FHex> RingHexes;
  };
  TArray<FTransmutationCandidate> Candidates;
  TSet<int32> AlreadyClaimed; // Prevent overlapping transmutations

  for (const auto &TypeKV : ItemToType) {
    if (TypeKV.Value != EOrbType::Void)
      continue;

    const int32 CenterId = TypeKV.Key;
    if (AlreadyClaimed.Contains(CenterId))
      continue;

    const FHex *CenterHexPtr = ItemToHex.Find(CenterId);
    if (!CenterHexPtr)
      continue;
    const FHex CenterHex = *CenterHexPtr;

    // Check that center has bonds to at least 6 neighbors
    const TArray<int32> *CenterBonds = ItemToBonds.Find(CenterId);
    if (!CenterBonds || CenterBonds->Num() < 6)
      continue;

    // Collect bonded Void neighbors that are at hex distance 1
    TArray<int32> RingIds;
    TArray<FHex> RingHexes;

    for (int32 BondIdx : *CenterBonds) {
      if (!Bonds.IsValidIndex(BondIdx))
        continue;
      const FBond &B = Bonds[BondIdx];
      if (!B.bActive)
        continue;

      int32 NeighborId = (B.ItemA == CenterId) ? B.ItemB : B.ItemA;
      if (AlreadyClaimed.Contains(NeighborId))
        continue;

      const FHex *NeighborHex = ItemToHex.Find(NeighborId);
      const EOrbType *NeighborType = ItemToType.Find(NeighborId);

      if (!NeighborHex || !NeighborType)
        continue;
      if (*NeighborType != EOrbType::Void)
        continue;
      if (HexUtil::Dist(CenterHex, *NeighborHex) != 1)
        continue;

      RingIds.Add(NeighborId);
      RingHexes.Add(*NeighborHex);
    }

    if (RingIds.Num() != 6)
      continue;

    // All checks passed: 1 Void center bonded to 6 Void neighbors at distance 1
    FTransmutationCandidate Candidate;
    Candidate.CenterId = CenterId;
    Candidate.CenterHex = CenterHex;
    Candidate.RingIds = RingIds;
    Candidate.RingHexes = RingHexes;
    Candidates.Add(Candidate);

    // Claim all 7 orbs to prevent overlapping transmutations
    AlreadyClaimed.Add(CenterId);
    for (int32 RId : RingIds)
      AlreadyClaimed.Add(RId);
  }

  // --- Apply transmutations ---
  for (const FTransmutationCandidate &Cand : Candidates) {
    // ========================================
    // 1. Transmute center Void -> CatalystX
    // ========================================
    bool bCenterApplied = false;

    // Board items (single orbs)
    for (auto &KV : ItemMap) {
      FItem &Item = KV.Value;
      if (!Item.bIsCluster && Item.Id == Cand.CenterId) {
        Item.Single.Type = EOrbType::CatalystX;
        Item.Single.Color = EOrbColor::Magenta;
        Item.Single.Stability = 999999;
        OrbAt.Add(KV.Key, (uint8)EOrbType::CatalystX);
        bCenterApplied = true;
        break;
      }
      if (Item.bIsCluster) {
        for (FOrbData &Sub : Item.Orbs) {
          if (Sub.ItemId == Cand.CenterId) {
            Sub.Type = (uint8)EOrbType::CatalystX;
            Sub.Color = (uint8)EOrbColor::Magenta;
            Sub.Stability = 999999;
            bCenterApplied = true;
            break;
          }
        }
        if (bCenterApplied)
          break;
      }
    }

    // Held items (arms)
    if (!bCenterApplied) {
      for (FArmState &Arm : Arms) {
        if (!Arm.bHolding)
          continue;
        if (Arm.Held.bIsCluster) {
          for (FOrbData &Sub : Arm.Held.Orbs) {
            if (Sub.ItemId == Cand.CenterId) {
              Sub.Type = (uint8)EOrbType::CatalystX;
              Sub.Color = (uint8)EOrbColor::Magenta;
              Sub.Stability = 999999;
              bCenterApplied = true;
              break;
            }
          }
        } else if (Arm.Held.Id == Cand.CenterId) {
          Arm.Held.Single.Type = EOrbType::CatalystX;
          Arm.Held.Single.Color = EOrbColor::Magenta;
          Arm.Held.Single.Stability = 999999;
          bCenterApplied = true;
        }
        if (bCenterApplied)
          break;
      }
    }

    // Held items (walker feet)
    if (!bCenterApplied) {
      auto TryTransmuteFootHeld = [&](FFootState &Foot) -> bool {
        if (!Foot.bHoldingItem)
          return false;
        if (Foot.HeldItem.bIsCluster) {
          for (FOrbData &Sub : Foot.HeldItem.Orbs) {
            if (Sub.ItemId == Cand.CenterId) {
              Sub.Type = (uint8)EOrbType::CatalystX;
              Sub.Color = (uint8)EOrbColor::Magenta;
              Sub.Stability = 999999;
              return true;
            }
          }
        } else if (Foot.HeldItem.Id == Cand.CenterId) {
          Foot.HeldItem.Single.Type = EOrbType::CatalystX;
          Foot.HeldItem.Single.Color = EOrbColor::Magenta;
          Foot.HeldItem.Single.Stability = 999999;
          return true;
        }
        return false;
      };
      for (FWalkerState &W : Walkers) {
        if (TryTransmuteFootHeld(W.DFoot) || TryTransmuteFootHeld(W.LFoot) ||
            TryTransmuteFootHeld(W.RFoot) || TryTransmuteFootHeld(W.UFoot))
          break;
      }
    }

    // ========================================
    // 2. Consume the 6 ring Void orbs
    // ========================================
    FTransmutationAnimation TransAnim;
    TransAnim.CenterHex = Cand.CenterHex;
    TransAnim.CenterItemId = Cand.CenterId;
    TransAnim.StartStep = StepCounter;
    TransAnim.CompletionStep = StepCounter + TRANSMUTATION_CONSUME_TICKS;
    TransAnim.bActive = true;

    for (int32 ri = 0; ri < Cand.RingIds.Num(); ++ri) {
      int32 RingItemId = Cand.RingIds[ri];
      FHex RingHex = Cand.RingHexes[ri];

      TransAnim.ConsumedItemIds.Add(RingItemId);
      TransAnim.ConsumedHexes.Add(RingHex);
      TransmutedItemIds.Add(RingItemId);

      // Break ALL bonds for this ring item
      if (const TArray<int32> *BondList = ItemToBonds.Find(RingItemId)) {
        TArray<int32> BondsCopy = *BondList;
        for (int32 BondIdx : BondsCopy) {
          RemoveBondIndex(BondIdx);
        }
      }
      ItemToBonds.Remove(RingItemId);

      // Build item data for the merge animation (stored in TransAnim, not AnimatedOrbs)
      FItem ConsumedItem;
      bool bRemovedFromBoard = false;

      // Try board single orb
      for (auto It = ItemMap.CreateIterator(); It; ++It) {
        FItem &Item = It.Value();
        if (!Item.bIsCluster && Item.Id == RingItemId) {
          ConsumedItem = Item;
          OrbAt.Remove(It.Key());
          It.RemoveCurrent();
          bRemovedFromBoard = true;
          break;
        }
      }

      // Try board cluster sub-orb (remove from cluster)
      if (!bRemovedFromBoard) {
        for (auto &KV : ItemMap) {
          FItem &Item = KV.Value;
          if (Item.bIsCluster) {
            for (int32 si = Item.Orbs.Num() - 1; si >= 0; --si) {
              if (Item.Orbs[si].ItemId == RingItemId) {
                ConsumedItem.Id = RingItemId;
                ConsumedItem.bIsCluster = false;
                ConsumedItem.Single.Type = (EOrbType)Item.Orbs[si].Type;
                ConsumedItem.Single.Color = (EOrbColor)Item.Orbs[si].Color;
                ConsumedItem.Single.Stability = Item.Orbs[si].Stability;
                Item.Orbs.RemoveAt(si);
                bRemovedFromBoard = true;
                break;
              }
            }
            if (bRemovedFromBoard)
              break;
          }
        }
      }

      // Try held by arm
      if (!bRemovedFromBoard) {
        for (int32 ai = 0; ai < Arms.Num(); ++ai) {
          FArmState &Arm = Arms[ai];
          if (!Arm.bHolding)
            continue;

          if (!Arm.Held.bIsCluster && Arm.Held.Id == RingItemId) {
            ConsumedItem.Id = RingItemId;
            ConsumedItem.bIsCluster = false;
            ConsumedItem.Single = Arm.Held.Single;
            // The arm will be reset by the post-transmutation arm check
            Arm.bHolding = false;
            Arm.Held = FItem();
            bRemovedFromBoard = true;
            break;
          }

          if (Arm.Held.bIsCluster) {
            for (int32 si = Arm.Held.Orbs.Num() - 1; si >= 0; --si) {
              if (Arm.Held.Orbs[si].ItemId == RingItemId) {
                ConsumedItem.Id = RingItemId;
                ConsumedItem.bIsCluster = false;
                ConsumedItem.Single.Type = (EOrbType)Arm.Held.Orbs[si].Type;
                ConsumedItem.Single.Color = (EOrbColor)Arm.Held.Orbs[si].Color;
                ConsumedItem.Single.Stability = Arm.Held.Orbs[si].Stability;
                Arm.Held.Orbs.RemoveAt(si);
                if (Arm.Held.Orbs.Num() == 0) {
                  Arm.bHolding = false;
                  Arm.Held = FItem();
                }
                bRemovedFromBoard = true;
                break;
              }
            }
            if (bRemovedFromBoard)
              break;
          }
        }
      }

      // Try held by walker foot
      if (!bRemovedFromBoard) {
        auto TryRemoveFromFoot = [&](FFootState &Foot) -> bool {
          if (!Foot.bHoldingItem)
            return false;
          if (!Foot.HeldItem.bIsCluster && Foot.HeldItem.Id == RingItemId) {
            ConsumedItem.Id = RingItemId;
            ConsumedItem.bIsCluster = false;
            ConsumedItem.Single = Foot.HeldItem.Single;
            Foot.bHoldingItem = false;
            Foot.HeldItem = FItem();
            return true;
          }
          if (Foot.HeldItem.bIsCluster) {
            for (int32 si = Foot.HeldItem.Orbs.Num() - 1; si >= 0; --si) {
              if (Foot.HeldItem.Orbs[si].ItemId == RingItemId) {
                ConsumedItem.Id = RingItemId;
                ConsumedItem.bIsCluster = false;
                ConsumedItem.Single.Type = (EOrbType)Foot.HeldItem.Orbs[si].Type;
                ConsumedItem.Single.Color =
                    (EOrbColor)Foot.HeldItem.Orbs[si].Color;
                ConsumedItem.Single.Stability = Foot.HeldItem.Orbs[si].Stability;
                Foot.HeldItem.Orbs.RemoveAt(si);
                if (Foot.HeldItem.Orbs.Num() == 0) {
                  Foot.bHoldingItem = false;
                  Foot.HeldItem = FItem();
                }
                return true;
              }
            }
          }
          return false;
        };
        for (FWalkerState &W : Walkers) {
          if (TryRemoveFromFoot(W.DFoot) || TryRemoveFromFoot(W.LFoot) ||
              TryRemoveFromFoot(W.RFoot) || TryRemoveFromFoot(W.UFoot)) {
            bRemovedFromBoard = true;
            break;
          }
        }
      }

      if (bRemovedFromBoard) {
        TransAnim.ConsumedItems.Add(ConsumedItem);
      }
    }

    // Also break bonds between center and the consumed ring (they no longer exist)
    if (const TArray<int32> *CenterBondList = ItemToBonds.Find(Cand.CenterId)) {
      TArray<int32> BondsCopy = *CenterBondList;
      for (int32 BondIdx : BondsCopy) {
        if (!Bonds.IsValidIndex(BondIdx))
          continue;
        const FBond &B = Bonds[BondIdx];
        int32 OtherId = (B.ItemA == Cand.CenterId) ? B.ItemB : B.ItemA;
        // Only break bonds to the consumed ring items
        if (Cand.RingIds.Contains(OtherId)) {
          RemoveBondIndex(BondIdx);
        }
      }
    }

    // Return mass for the 6 consumed orbs
    ReturnMassToSources(6);

    TransmutationAnimations.Add(TransAnim);

    UE_LOG(LogTemp, Log,
           TEXT("TRANSMUTATION: 7 Void → CatalystX at (%d,%d). 6 ring orbs consumed."),
           Cand.CenterHex.Q, Cand.CenterHex.R);
  }

  // ========================================
  // 3. Reset arms that lost their held item to transmutation
  // ========================================
  if (TransmutedItemIds.Num() > 0) {
    for (int32 ArmIndex = 0; ArmIndex < Arms.Num(); ++ArmIndex) {
      FArmState &S = Arms[ArmIndex];
      if (!S.bHolding)
        continue;

      bool bLostHeldItem = false;

      if (!S.Held.bIsCluster) {
        // Single orb — check if it was consumed
        if (TransmutedItemIds.Contains(S.Held.Id)) {
          bLostHeldItem = true;
        }
      } else {
        // Cluster — check if ALL sub-orbs were consumed (empty cluster)
        if (S.Held.Orbs.Num() == 0) {
          bLostHeldItem = true;
        }
      }

      if (bLostHeldItem) {
        // Force drop: clear the held item
        S.bHolding = false;
        S.Held = FItem();
        S.DirectlyHeldOrbId = 0;
        S.bPausedForBonding = false;
        S.bWaitingForBond = false;
        S.WaitForBondInitialCount = 0;

        // Reset PC to 0 and generate reset commands to return home
        S.PC = 0;
        S.WaitCounter = 0;

        // Insert Reset command at current PC to navigate back to home orientation
        TArray<FOp> ResetOps;

        // Calculate rotation back to home
        const uint8 CurrentRotation = static_cast<uint8>(S.Facing);
        const uint8 TargetRotation = static_cast<uint8>(S.HomeFacing);
        if (CurrentRotation != TargetRotation) {
          const int32 StepsClockwise =
              (CurrentRotation - TargetRotation + 6) % 6;
          const int32 StepsCounterClockwise =
              (TargetRotation - CurrentRotation + 6) % 6;
          if (StepsClockwise <= StepsCounterClockwise) {
            for (int32 i = 0; i < StepsClockwise; ++i)
              ResetOps.Add({ECmd::RotateR, 1});
          } else {
            for (int32 i = 0; i < StepsCounterClockwise; ++i)
              ResetOps.Add({ECmd::RotateL, 1});
          }
        }

        // Adjust arm length back to home
        const int32 ReachDiff = S.HomeArmLen - S.ArmLen;
        if (ReachDiff > 0) {
          for (int32 i = 0; i < ReachDiff; ++i)
            ResetOps.Add({ECmd::Extend, 1});
        } else if (ReachDiff < 0) {
          for (int32 i = 0; i < -ReachDiff; ++i)
            ResetOps.Add({ECmd::Retract, 1});
        }

        if (ResetOps.Num() > 0) {
          S.Tape.Insert(ResetOps, S.PC);
        }

        UE_LOG(LogTemp, Warning,
               TEXT("Arm [%d] lost held item to transmutation — resetting to home."),
               ArmIndex);
      }
    }
  }
}

// ============================================================
// Transmutation Animation Update (called from rendering tick)
// ============================================================
void USimWorldHex::UpdateTransmutationAnimations(float DeltaTime) {
  const int32 CurrentStep = GetStepCounter();

  for (int32 i = TransmutationAnimations.Num() - 1; i >= 0; --i) {
    FTransmutationAnimation &Anim = TransmutationAnimations[i];
    if (!Anim.bActive)
      continue;

    if (CurrentStep >= Anim.CompletionStep) {
      Anim.bActive = false;
      TransmutationAnimations.RemoveAt(i);
    }
  }
}

// ============================================================
// PHASE 5: Mass Budget — Return mass to source groups
// ============================================================
void USimWorldHex::ReturnMassToSources(int32 OrbCount) {
  if (OrbCount <= 0)
    return;

  // Distribute returned mass evenly across all source groups that have budgets
  int32 GroupsWithBudget = 0;
  for (const auto &KV : SourceGroups) {
    if (KV.Value.MassBudget > 0)
      GroupsWithBudget++;
  }

  if (GroupsWithBudget == 0)
    return; // No budgeted source groups

  int32 MassPerGroup = FMath::Max(1, OrbCount / GroupsWithBudget);
  int32 Remainder = OrbCount - (MassPerGroup * GroupsWithBudget);

  for (auto &KV : SourceGroups) {
    FSourceGroup &G = KV.Value;
    if (G.MassBudget <= 0)
      continue; // Infinite budget, skip

    int32 ToReturn = MassPerGroup + (Remainder > 0 ? 1 : 0);
    if (Remainder > 0)
      Remainder--;

    G.MassRemaining = FMath::Min(G.MassRemaining + ToReturn, G.MassBudget);
  }
}
