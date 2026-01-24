#pragma once

#include "AutomationGameMinimal.h"
#include "Containers/Map.h"
#include "Containers/Set.h"
#include "Sim/ArmTypes.h"
#include "Sim/HexCoord.h"
#include "Sim/HexTypes.h"
#include "Sim/OrbTypes.h"
#include "Sim/Tiles.h"
#include "Sim/WalkerTypes.h"

#include "Templates/Tuple.h"
#include "UObject/Object.h"
// .generated.h must be last
#include "SimWorldHex.generated.h"

/** Authoritative deterministic hex simulation world. */
UCLASS(BlueprintType)
class AUTOMATIONGAME_API USimWorldHex : public UObject {
  GENERATED_BODY()
  friend class AHexTopDownPawn;

public:
  // Constructor and destructor
  USimWorldHex();
  virtual ~USimWorldHex();
  // Config
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sim")
  float TileSizeUU = 120.f;

  /** Vertical offset for orbs on different grid levels. */
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sim")
  float OrbZOffset = 50.f;

  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sim")
  float FixedStepSec = 0.20f; // 5 Hz

  void StepBonds(); // Made public for BondResolver

  // Mark arm base cache as needing rebuild (call when arms are added/removed)
  void MarkArmBaseCacheDirty() { bArmBaseCacheDirty = true; }
  // Debug toggles
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
  bool bDebugLogs = false;
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
  bool bDebugSpheres = false;
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
  bool bDebugLines = false;
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
  bool bDebugBonds = false;

  // Collision debug tracking
  struct FCollisionDebugInfo {
    int32 ArmIndex = -1;
    FHex BlockedHex;
    FString Reason;
    FHex ArmBaseHex;
    FHex ArmGripHex;
  };

  // Collision debug data (cleared each frame, populated during arm stepping)
  TArray<FCollisionDebugInfo> CollisionDebugData;

  // Accessor for collision debug data
  const TArray<FCollisionDebugInfo> &GetCollisionDebugData() const {
    return CollisionDebugData;
  }

  // Predictive collision debug info struct (declared before use)
  struct FPredictiveCollisionInfo {
    int32 ArmIndexA = -1;
    int32 ArmIndexB = -1;
    FHex CollisionHex;
    FString Reason;
  };

  // Predictive collision debug data (cleared each frame, populated during
  // predictive collision check)
  TArray<FPredictiveCollisionInfo> PredictiveCollisionDebugData;
  // Accessor for predictive collision debug data
  const TArray<FPredictiveCollisionInfo> &
  GetPredictiveCollisionDebugData() const {
    return PredictiveCollisionDebugData;
  }
  // ---- Arm <-> Item API ----
  struct FBond {
    int32 ItemA = 0;
    int32 ItemB = 0;
    FHex Root;
    int8 dQ = 0;
    int8 dR = 0;
    bool bActive = true;
  };

  UFUNCTION(BlueprintCallable, Category = "Sim|Arm")
  bool TryArmPickItem(const FHex &From, FItem &OutItem);
  UFUNCTION(BlueprintCallable, Category = "Sim|Arm")
  bool TryArmPlaceItem(const FHex &To, const FItem &InItem);
  UFUNCTION(BlueprintCallable, Category = "Sim|Arm")
  bool PeekItemAt(const FHex &H, FItem &OutItem) const;

  TArray<FBond> Bonds;
  TMap<int32, TArray<int32>> ItemToBonds; // ItemId -> list of bond indices

  // ---- Orbs overlay ----
  UFUNCTION(BlueprintCallable, Category = "Sim|Orbs")
  bool TryPlaceOrbAt(const FHex &Hex, uint8 Kind);
  UFUNCTION(BlueprintCallable, Category = "Sim|Orbs")
  bool TryPickupOrbAt(const FHex &Hex, uint8 &OutKind);
  UFUNCTION(BlueprintCallable, Category = "Sim|Arm")
  bool HasOrbAt(const FHex &H) const;
  UFUNCTION(BlueprintPure, Category = "Sim|Orbs")
  bool GetOrbKindAt(const FHex &Hex, uint8 &OutKind) const;

  /** Returns the type of orb at the given location, checking board items and
   * held items. */
  UFUNCTION(BlueprintPure, Category = "Sim|Orbs")
  EOrbType GetOrbTypeAtLocation(const FHex &H) const;

  // Maps
  UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Sim")
  TMap<FHex, FTile> TileMap;
  UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Sim")
  TMap<FHex, FItem> ItemMap;
  UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Sim")
  TArray<FArmState> Arms;
  UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Sim")
  TArray<FWalkerState> Walkers;

  UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Sim")
  int32 OutputCount = 0;
  UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Sim")
  int32 TileRevision = 0;

  // World lifecycle
  UFUNCTION(BlueprintCallable, Category = "Sim")
  void Initialize(int32 Radius = 20);
  UFUNCTION(BlueprintCallable, Category = "Sim") void StepSim();

  // Build API
  UFUNCTION(BlueprintCallable, Category = "Build")
  bool PlaceTile(const FHex &H, const FTile &T);
  UFUNCTION(BlueprintCallable, Category = "Build")
  int32 AddArm(const FArmState &Arm);
  UFUNCTION(BlueprintCallable, Category = "Build")
  int32 AddWalker(const FWalkerState &Walker);

  // Walker request API
  UFUNCTION(BlueprintCallable, Category = "Sim|Walker")
  int32 FindNearestAvailableWalker(const FHex &FromHex);
  UFUNCTION(BlueprintCallable, Category = "Sim|Walker")
  bool RequestWalkerMoveTo(int32 WalkerIndex, const FHex &TargetHex,
                           int32 Priority = 0);

  // TriggerTile signal API - controls belt enable/disable based on trigger
  // signals
  void SetBeltTriggerSignal(const FHex &TriggerHex, const FHex &BeltOwnerHex,
                            bool bPositive);
  void ClearBeltTriggerSignal(const FHex &TriggerHex, const FHex &BeltOwnerHex);
  bool IsBeltEnabled(const FHex &BeltHex) const;

  // TriggerTile signal API - controls arm enable/disable based on trigger
  // signals
  void SetArmTriggerSignal(const FHex &TriggerHex, const FHex &ArmBaseHex,
                           bool bPositive);
  void ClearArmTriggerSignal(const FHex &TriggerHex, const FHex &ArmBaseHex);
  bool IsArmEnabled(const FHex &ArmBaseHex) const;

  // Cluster template API (C++ only - TPair is not Blueprint-compatible)
  // Cluster template structure (must be public for external access)
  struct FClusterTemplate {
    // Orbs in the cluster with their relative positions
    TArray<FOrbData> Orbs;

    // Bond connections between orbs (pairs of indices into Orbs array)
    TArray<TPair<int32, int32>> Bonds;

    // Name/ID for this template
    FString Name;

    // Whether this template should auto-rotate to match output orientation
    bool bAutoRotate = false;
  };

  void RegisterClusterTemplate(uint8 TemplateId, const TArray<FOrbData> &Orbs,
                               const TArray<TPair<int32, int32>> &BondPairs,
                               const FString &Name);
  bool ValidateClusterAgainstTemplate(const FItem &Cluster,
                                      uint8 TemplateId) const;

  // Auto-generate a cluster template for two adjacent output tiles
  uint8 AutoGenerateClusterTemplateForOutputs(const FHex &Hex1,
                                              const FHex &Hex2, uint8 OrbType1,
                                              uint8 OrbType2);

  // Expand an existing cluster template to include additional tiles
  uint8 ExpandClusterTemplate(uint8 ExistingTemplateId, const FHex &OldBaseHex,
                              const FHex &NewBaseHex,
                              const TArray<FHex> &AllClusterHexes,
                              const TMap<FHex, uint8> &HexToOrbType);

  // Generate a new unique template ID
  uint8 GenerateNewTemplateId() { return NextAutoTemplateId++; }

  // Find all tiles that are part of a cluster (connected via IOBondPartners)
  TArray<FHex> FindAllTilesInCluster(const FHex &StartHex) const;

  // Helper function to find all connected ClusterInput tiles in a chain
  TArray<FHex>
  FindClusterInputChain(const FHex &StartHex,
                        const TSet<FHex> &ExcludeHexes = TSet<FHex>()) const;

  // Get cluster templates map for template ID lookup
  const TMap<uint8, FClusterTemplate> &GetClusterTemplates() const {
    return ClusterTemplates;
  }

  // Get IO bond partners map (for ClusterOutput partnerships)
  const TMap<FHex, FHex> &GetIOBondPartners() const { return IOBondPartners; }
  TMap<FHex, FHex> &GetIOBondPartnersMutable() { return IOBondPartners; }

  // Item helpers
  bool TryPutItem(const FHex &H, const FItem &I);
  bool TryTakeItem(const FHex &H, FItem &Out);

  FVector HexCenterToWorld(const FHex &H) const;
  // ----- Visual interpolation (belt smoothing) -----
  void SetFrameAlpha(float InAlpha) {
    FrameAlpha = FMath::Clamp(InAlpha, 0.f, 1.f);
  }
  float GetFrameAlpha() const { return FrameAlpha; }

  bool GetItemVisualLocation(int32 ItemId, FVector &OutWorld) const;

  int32 GetStepCounter() const { return StepCounter; }

  // External world transform (optional)
  void SetWorldTransform(const FVector &InAnchor, float InGridYawDeg,
                         EHexOrientation InOrientation) {
    WorldAnchor = InAnchor;
    GridYawDeg = InGridYawDeg;
    GridOrientation = InOrientation;
  }

  /** Get the bonds array for rendering purposes */
  const TArray<FBond> &GetBonds() const { return Bonds; }

  /** Get belt progress for an item (for sub-tile interpolation in renderer) */
  float GetBeltProgress(int32 ItemId) const {
    if (const float *Progress = BeltProgress.Find(ItemId)) {
      return *Progress;
    }
    return 0.f;
  }

  /** Animated orb structure for input/output animations */
  struct FAnimatedOrb {
    FItem Item;
    FHex Hex;
    int32 StartStep = 0;      // Step counter when animation started
    int32 CompletionStep = 0; // Step counter when animation should complete

    bool bIsInput =
        false; // true for input (ascending), false for output (descending)
    bool bActive = true;
    bool bIsBonded =
        false; // true if this animated orb should show a bond to a partner
    FHex PartnerHex; // hex of the bonded partner (if bIsBonded is true)
    int32 PartnerItemId =
        0; // item ID of the bonded partner (if bIsBonded is true)
  };

  /** Get animated orbs for rendering */
  const TArray<FAnimatedOrb> &GetAnimatedOrbs() const { return AnimatedOrbs; }

  /** Update animated orbs (call from renderer) */
  void UpdateAnimatedOrbs(float DeltaTime);

  /** Process pending bond formations (call from renderer) */
  void ProcessPendingBondFormations(float DeltaTime);

  /** Notify that an arm has finished its visual rotation */
  void OnArmRotationComplete(int32 ArmIndex);

  /** Notify that an arm has fully completed its visual rotation (100%) */
  void OnArmRotationFullyComplete(int32 ArmIndex);

  /** Cancel pending bond formations for a specific arm (when arm drops item) */
  void CancelPendingBondFormationsForArm(int32 ArmIndex);

  // Sim-state accessors (used by StepBonds and other sim-only logic)
  FORCEINLINE uint8 GetSimFacing(int32 ArmIndex, const FArmState &A) const {
    if (const uint8 *V = SimFacingByArm.Find(ArmIndex))
      return *V;
    return static_cast<uint8>(A.Facing);
  }
  FORCEINLINE int32 GetSimArmLen(int32 ArmIndex, const FArmState &A) const {
    if (const int32 *V = SimArmLenByArm.Find(ArmIndex))
      return *V;
    return A.ArmLen;
  }
  FORCEINLINE int8 GetSimGrabFacing(int32 ArmIndex, const FArmState &A) const {
    if (const int8 *V = SimGrabFacingByArm.Find(ArmIndex))
      return *V;
    return A.Held.bIsCluster ? A.Held.GrabFacing : int8(-1);
  }

  /** Set of items that have been rejected by ClusterOutput validation */
  UPROPERTY(Transient)
  TSet<int32> ItemsRejectedByClusterOutput;

  /** Per-tile orb types for Discovery tiles (since FTile only stores TemplateId
   * in Arg) */
  UPROPERTY(Transient)
  TMap<FHex, uint8> DiscoveryTileOrbTypes;

private:
  /** Helper function to check if an item matches an output's expected orb type
   */
  bool DoesItemMatchOutputType(const FItem &Item,
                               const FTile &OutputTile) const;
  bool bDevDebugArms = true; // set true to see arm/hand debug messages
  float FrameAlpha = 0.f;
  int32 StepCounter = 0;
  int32 MAX_ARM_LEN = 6;
  // Cached world transform
  FVector WorldAnchor = FVector::ZeroVector;
  float GridYawDeg = 0.f;
  EHexOrientation GridOrientation = EHexOrientation::PointyTop;
  int OUTPUT_DESCEND_TICKS = 100;
  // One-frame skip sets
  TSet<FHex> SkipInputThisFrame;
  TSet<FHex> SkipOutputThisFrame;

  // Track newly placed items that should wait a tick before being moved by
  // belts
  TSet<int32> NewlyPlacedItems;

  // Track arms that are ready for unbond processing (rotation complete)
  // Maps arm index to the facing direction when rotation completed
  TMap<int32, uint8> ArmsReadyForUnbond;

  // Track arms that need unbond processing after a 1-tick delay
  // Maps arm index to the facing direction when rotation completed
  TMap<int32, uint8> PendingUnbonds;

  // Track orbs recently dropped by arms to prevent immediate re-grabbing
  // Maps item ID to arm index that dropped it
  TMap<int32, int32> RecentlyDroppedByArm;

  // Hexes where orbs were newly moved to an output tile this tick
  TSet<FHex> OrbsNewlyMovedToOutput;

  TSet<int32> OrbsNewlyMovedByBelts;

  // Item IDs of orbs that were moved in the PREVIOUS tick
  // Grab logic must check this to wait for visual movement to finish
  TSet<int32> OrbsMovedLastTick;

  // Output tile gates - tracks whether output tiles are open for new orbs
  // True = open for new orbs, False = blocked (incorrect type orb on tile)
  TMap<FHex, bool> OutputTileGates;

  // TriggerTile signals - tracks whether belts are enabled based on TriggerTile
  // signals Outer Key = belt hex location (owner), Inner Key = trigger tile
  // hex, Value = true if positive signal If a belt has no entries for any
  // trigger, it is enabled by default
  TMap<FHex, TMap<FHex, bool>> BeltTriggerSignals;

  /** Track the ID of the last item/cluster moved by a belt at a specific
   * location. Used to give priority to belts that haven't pushed their current
   * occupant yet. */
  TMap<FHex, int32> BeltLastMovedItemId;

  // TriggerTile signals - tracks whether arms are enabled based on TriggerTile
  // signals Outer Key = arm base hex location, Inner Key = trigger tile hex,
  // Value = true if positive signal If an arm has no entries for any trigger,
  // it is enabled by default
  TMap<FHex, TMap<FHex, bool>> ArmTriggerSignals;

  // UI overlay
  UPROPERTY() TMap<FHex, uint8> OrbAt;

  // Animated orbs for input/output effects
  TArray<FAnimatedOrb> AnimatedOrbs;

  // Held items last board position (for future bond persistence if needed)
  TMap<int32, FHex> HeldItemLastHex;

  // Input/Output bond tiles - maps tile hex to partner hex
  // For InputBond tiles: both input tiles spawn orbs pre-bonded
  // For OutputBond tiles: both output tiles require bonded orbs
  TMap<FHex, FHex> IOBondPartners;

  // Cluster templates mapped by template ID (Tile.Arg stores the template ID)
  TMap<uint8, FClusterTemplate> ClusterTemplates;

  // Track which cluster output tiles have accepted clusters (for gating)
  TMap<FHex, bool> ClusterOutputGates;

  // Counter for auto-generated cluster template IDs (starts at 128 to avoid
  // conflicts with manual templates)
  uint8 NextAutoTemplateId = 128;

  // Pending bond formations waiting for arm rotation to complete
  struct FPendingBondFormation {
    int32 HeldItemId = 0;
    int32 BoardItemId = 0;
    FHex BoardItemHex;
    int32 ArmIndex = -1;
    float TimeRemaining = 0.f; // Time to wait before forming bond
  };
  TArray<FPendingBondFormation> PendingBondFormations;

  // Pending grab tracking to prevent multiple arms from grabbing the same
  // cluster
  struct FPendingGrab {
    int32 ArmIndex = -1;
    FHex TargetHex;
    int32 ClusterId = 0;
  };
  TArray<FPendingGrab> PendingGrabs;

  // ============================================================
  // OCCUPANCY CACHE - computed once per tick, shared across steps
  // ============================================================
  // Hexes occupied by held orbs (arms holding items)
  TSet<FHex> CachedHeldOrbHexes;
  // Hexes where a walker foot is physically on the ground (Planted/Lowering)
  TSet<FHex> CachedBlockingFootHexes;
  // Hexes where a walker foot intends to be (Current + Target) - for foot
  // assignment
  TSet<FHex> CachedIntendedFootHexes;
  // Hexes occupied by arm bases (static until user adds/removes arms)
  TSet<FHex> CachedArmBaseHexes;
  // Flag to indicate if arm base cache needs rebuild (set true when arms
  // added/removed)
  bool bArmBaseCacheDirty = true;

  // Rebuild occupancy cache for held orbs (call once per tick after StepArms)
  void UpdateHeldOrbCache();
  // Rebuild occupancy cache for walker feet (call once per tick)
  void UpdateWalkerFootCache();
  // Rebuild arm base cache (only when arms added/removed)
  void UpdateArmBaseCache();

  // Bonded arm pair tracking for cross-arm cluster management
  struct FBondedArmPair {
    int32 ArmIndexA = -1;
    int32 ArmIndexB = -1;
    int32 BondIndex = -1;
    bool bActive = false;
  };
  TArray<FBondedArmPair> BondedArmPairs;

  // Pending held-to-held cluster merges (processed when arms finish rotating)
  struct FPendingHeldMerge {
    int32 ReceiverArmIndex = -1; // Arm that will receive the items
    int32 DonorArmIndex = -1;    // Arm that will lose its items
    int32 ItemIdA = -1;          // Item in receiver arm
    int32 ItemIdB = -1;          // Item in donor arm
  };
  TArray<FPendingHeldMerge> PendingHeldMerges;

  // Item Id generator
  int32 NextItemId = 1;

  // Core step pieces
  void StepArms();
  void StepArmsSecondPass();  // Second pass for immediate grab after unbonding
  void StepBondsSecondPass(); // Second pass for bond formation after rotations
                              // complete
  void StepBelts();
  void StepWalkers();

  // Opus Magnum-style bond prediction for rotation obstruction
  TMap<FHex, int32> GetPredictedBondFormations() const;
  void StepGlyphs();

  static uint8 KindFromItem(const FItem &Item) {
    if (Item.bIsCluster && Item.Orbs.Num() > 0) {
      return (uint8)Item.Orbs[0].Type;
    } else {
      return (uint8)Item.Single.Type;
    }
  }

  // Belt progress
  UPROPERTY()
  TMap<int32, float> BeltProgress; // ItemId -> progress [0,1)

  float BeltBlockedClamp = 0.98f;

  void TryFormBondAt(const FHex &Root, uint8 Dir, const TSet<int32> &HeldIds,
                     TSet<int32> &NewlyBondedItems);
  void CleanupBrokenBonds(const TSet<int32> &HeldIds);
  void RemoveBondIndex(int32 Index);

  // Helpers for multi-bond bookkeeping
  void AddItemBondLink(int32 ItemId, int32 BondIndex);
  void RemoveItemBondLink(int32 ItemId, int32 BondIndex);

  // Walker helpers
  bool IsHexOccupied(const FHex &H, const TSet<FHex> &HeldOrbHexes,
                     const TSet<FHex> &ArmBaseHexes,
                     const TSet<FHex> &HoldingFootHexes = TSet<FHex>()) const;
  TArray<struct FFootState *> GetAllFeet(struct FWalkerState &W) const;
  bool AreFeetAdjacent(int32 FootIndexA, int32 FootIndexB) const;
  FString GetFixedQuadrantFromHex(const FHex &BodyHex, const FHex &TestHex,
                                  float RotationOffsetDeg) const;
  float CalculateSimWalkerRotation(const struct FWalkerState &W) const;
  FHex GetQuadrantCenterHex(const FHex &BodyHex, int32 FootIndex,
                            int32 ReachDistance, float MinReach,
                            float RotationOffsetDeg) const;

  // Walker simulation components
  void ProcessWalkerRequestAssignment(
      const TArray<class ARequestBeacon *> &AllBeacons,
      const TMap<FHex, class ARequestBeacon *> &BeaconCache);
  void ProcessWalkerTick(int32 WalkerIndex, const TSet<FHex> &HeldOrbHexes,
                         const TSet<FHex> &ArmBaseHexes,
                         const TMap<FHex, class ARequestBeacon *> &BeaconCache);
  void HandleFootStateProcessing(
      struct FWalkerState &W, int32 WalkerIndex, const TSet<FHex> &HeldOrbHexes,
      const TSet<FHex> &ArmBaseHexes,
      const TMap<FHex, class ARequestBeacon *> &BeaconCache,
      const TSet<FHex> &HoldingFootHexes, bool bIsPivoting);
  void HandleFootPlanted(struct FWalkerState &W, struct FFootState &Foot,
                         int32 WalkerIndex, int32 FootIndex,
                         const TSet<FHex> &HeldOrbHexes,
                         const TSet<FHex> &ArmBaseHexes,
                         const TSet<FHex> &HoldingFootHexes,
                         const TMap<FHex, class ARequestBeacon *> &BeaconCache,
                         bool bIsPivoting, int32 &RaisedCount,
                         TArray<int32> &RaisedIndices);
  void HandleFootMoving(struct FWalkerState &W, struct FFootState &Foot,
                        int32 WalkerIndex, int32 FootIndex,
                        const TSet<FHex> &HeldOrbHexes,
                        const TSet<FHex> &ArmBaseHexes,
                        const TSet<FHex> &HoldingFootHexes,
                        const TMap<FHex, class ARequestBeacon *> &BeaconCache,
                        bool bIsPivoting);
  void UpdateFootHorizontalTarget(
      struct FWalkerState &W, struct FFootState &Foot, int32 FootIndex,
      const TSet<FHex> &HeldOrbHexes, const TSet<FHex> &ArmBaseHexes,
      const TSet<FHex> &HoldingFootHexes,
      const TMap<FHex, class ARequestBeacon *> &BeaconCache, bool bIsPivoting);
  void TryWalkerPickUp(struct FWalkerState &W, struct FFootState &Foot,
                       int32 WalkerIndex, int32 FootIndex,
                       const TMap<FHex, class ARequestBeacon *> &BeaconCache,
                       const TArray<struct FFootState *> &AllFeet);
  bool TryWalkerDropOff(FWalkerState &W, FFootState &Foot, int32 WalkerIndex,
                        int32 FootIndex, const TSet<FHex> &HeldOrbHexes,
                        const TSet<FHex> &ArmBaseHexes,
                        const TSet<FHex> &HoldingFootHexes,
                        const TMap<FHex, ARequestBeacon *> &BeaconCache);
  void UpdateWalkerBodyPosition(struct FWalkerState &W, int32 WalkerIndex);
  void ProcessWalkerInstructions(struct FWalkerState &W, int32 WalkerIndex);

  void AddBoardOrbToHeldCluster(int32 HeldOrbId, int32 BoardOrbId,
                                const FHex &BoardOrbHex);
  void AddBoardOrbToHeldItem(int32 HeldOrbId, int32 BoardOrbId,
                             const FHex &BoardOrbHex);

  // Check for disconnected orbs in held clusters and remove them
  void CheckAndRemoveDisconnectedOrbsFromHeldClusters();

  // Process unbonds for a specific arm's held cluster (called when arm rotation
  // completes)
  void ProcessUnbondsForHeldCluster(int32 ArmIndex);

  // Process held cluster unbonds for arms that completed rotation
  void ProcessHeldClusterUnbonds();

  // Process held-to-held cluster merge
  void ProcessHeldToHeldMerge(int32 ReceiverArmIndex, int32 DonorArmIndex);

  // Helper to calculate arm grip position
  FHex CalculateArmGripPosition(const FArmState &ArmState) const;

  // Debug visualization
  void DrawBondDebugLines();

  UPROPERTY(EditAnywhere, Category = "Debug")
  bool bDevDebugBonds = false;

  // === SIMPLIFIED SPATIAL SYSTEM ===

  /** Spatial manager - single source of truth for orb positions */
  UPROPERTY(VisibleAnywhere, Category = "Spatial")
  TObjectPtr<class UOrbSpatialManager> SpatialManager;

  /** Bond resolver - unified bond formation system */
  UPROPERTY(VisibleAnywhere, Category = "Spatial")
  TObjectPtr<class UBondResolver> BondResolver;

  // Track items moved by belts this step (for spatial system bond timing)
  TSet<int32> ItemsMovedLastStep;

  // ---- Bond control (glyphs will call these later) ----
  UFUNCTION(BlueprintCallable, Category = "Sim|Bond")
  bool BreakBondBetween(int32 ItemA, int32 ItemB);

  UFUNCTION(BlueprintCallable, Category = "Sim|Bond")
  int32 BreakAllBondsForItem(int32 ItemId);

  bool IsItemBonded(int32 ItemId) const {
    if (const TArray<int32> *L = ItemToBonds.Find(ItemId)) {
      // Check if any of the bonds are actually active
      for (int32 BondIdx : *L) {
        if (Bonds.IsValidIndex(BondIdx) && Bonds[BondIdx].bActive) {
          return true;
        }
      }
    }
    return false;
  }

  TMap<int32, FHex> PrevItemHex;

  void SpawnInputs();
  void ProcessOutputs();
  void SpawnInputsAfterBelts();
  void MoveBondedClusterOnBelt(int32 SeedItemId, const FHex &FromHex,
                               const FHex &ToHex, const FTile &BeltTile,
                               TMap<int32, FHex> &ItemHex);
  void MoveBondedItemRecursive(int32 ItemId, const FHex &FromHex, int32 dQ,
                               int32 dR, TMap<int32, FHex> &ItemHex);

  // Bonded arm management functions
  void CheckForNewBondedArmPairs();
  void PauseBondedArms();
  void ResumeArmsWhenBondsBroken();
  bool IsArmBonded(int32 ArmIndex) const;

  // Committed simulation properties (consumed by StepBonds)
  // Values update only at fixed-tick boundaries to avoid mid-anim bonds.
  TMap<int32, uint8> SimFacingByArm;    // 0..5
  TMap<int32, int32> SimArmLenByArm;    // reach
  TMap<int32, int8> SimGrabFacingByArm; // -1 for single held

  // Pending values to commit next fixed tick (written by
  // Rotate*/Extend/Retract)
  TMap<int32, uint8> PendingFacingByArm;
  TMap<int32, int32> PendingArmLenByArm;
  TMap<int32, int8> PendingGrabFacingByArm;

  // --- NEW: sim-space helpers for held clusters ---
  FHex GetArmGripHex_Sim(int32 ArmIndex) const;
  uint8 GetHeldCCW_Steps_Sim(int32 ArmIndex) const;
  static FHex Rot60CCW(FHex D, uint8 StepsCCW);
  static FHex Rot60CW(FHex D, uint8 StepsCW);

  // --- Predictive collision detection for simultaneous arm movements ---
  struct FPredictiveArmState {
    FHex BaseHex;
    uint8 Facing;
    int32 ArmLen;
    bool bHolding;
    FItem Held;
    uint8 WaitCounter;
    int32 PC;
    bool bRotationCompleted;

    FPredictiveArmState()
        : BaseHex(0, 0), Facing(0), ArmLen(1), bHolding(false), WaitCounter(0),
          PC(0), bRotationCompleted(false) {}
    FPredictiveArmState(const FArmState &ArmState)
        : BaseHex(ArmState.BaseHex),
          Facing(static_cast<uint8>(ArmState.Facing)), ArmLen(ArmState.ArmLen),
          bHolding(ArmState.bHolding), Held(ArmState.Held),
          WaitCounter(ArmState.WaitCounter), PC(ArmState.PC),
          bRotationCompleted(ArmState.bRotationCompleted) {}
  };

  // Check if executing all arms' current commands simultaneously would cause
  // held cluster overlap
  bool WouldCauseHeldClusterOverlap();

  // Simulate what would happen if all arms executed their current commands (one
  // tick ahead only)
  TArray<FPredictiveArmState> SimulateAllArmsExecution();

  // Check if two held clusters would overlap given their predicted positions
  bool WouldHeldClustersOverlap(const FPredictiveArmState &ArmA,
                                const FPredictiveArmState &ArmB,
                                const FArmState &CurrentA,
                                const FArmState &CurrentB);

  // Get all positions that would be occupied by a held cluster in a given state
  TArray<FHex> GetHeldClusterPositions(const FPredictiveArmState &ArmState);

  // Check if two arms' rotation sweeps would cause held clusters to overlap
  bool WouldRotationSweepsOverlap(const FArmState &ArmA, const FArmState &ArmB,
                                  const FPredictiveArmState &PredictedA,
                                  const FPredictiveArmState &PredictedB);

  // Check if a rotating arm's sweep would overlap with a static arm's held
  // cluster
  bool
  WouldRotationSweepOverlapStatic(const FArmState &RotatingArm,
                                  const FPredictiveArmState &PredictedRotating,
                                  const FArmState &StaticArm);

  // Check if an arm's movement (rotation sweep or extend/retract) would overlap
  // with any of the given tiles
  bool DoesArmSweepOverlapTiles(const FArmState &Arm,
                                const FPredictiveArmState &Predicted,
                                const TSet<FHex> &Tiles);

  // Safety and validation methods
  UFUNCTION(BlueprintCallable, Category = "Sim|Debug")
  bool ValidateDataIntegrity() const;

  UFUNCTION(BlueprintCallable, Category = "Sim|Debug")
  void CleanupCorruptedData();
};
