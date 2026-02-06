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

class ATestLevelDirector;
class AHexTopDownPawn;
class UOrbRenderer;
class UBondResolver;
class UOrbSpatialManager;
class AHexFactoryActor;
class UHexPlacementComponent;
class AOrbSimDriver;
class AGameManager;

/** Authoritative deterministic hex simulation world. */
UCLASS(BlueprintType)
class AUTOMATIONGAME_API USimWorldHex : public UObject {
  GENERATED_BODY()
  friend class AHexTopDownPawn;
  friend class ATestLevelDirector;
  friend class UOrbRenderer;
  friend class UBondResolver;
  friend class UOrbSpatialManager;
  friend class AHexFactoryActor;
  friend class UHexPlacementComponent;
  friend class AOrbSimDriver;
  friend class AGameManager;
  friend class ARequestBeacon;
  friend class ABasicArm;
  friend class ABasicWalker;
  friend class UArmTimelineUMGWidget;
  friend struct FSimWorldHexSaveLoad;

public:
  /** Representation of a physical bond between two items on the board. */
  struct FBond {
    int32 ItemA = 0;
    int32 ItemB = 0;
    /** Visual root of the bond (location of Item A). */
    FHex Root;
    /** Relative axial offset from Item A to Item B. */
    int32 dQ = 0;
    int32 dR = 0;
    bool bActive = true;
  };

  /** Animated appearance of an orb moving between states. */
  struct FAnimatedOrb {
    FItem Item;
    FHex Hex;
    int32 StartStep = 0;
    int32 CompletionStep = 0;
    bool bIsInput = false;
    bool bActive = true;
    bool bIsBonded = false;
    FHex PartnerHex;
    int32 PartnerItemId = 0;
  };

  /** Bond formation that is visually animating before becoming a simulation
   * bond. */
  struct FPendingBondFormation {
    int32 ArmIndex = -1;
    FHex BoardItemHex;
    int32 BoardItemId = 0;
    float TimeRemaining = 0.f;
  };

  /** Debug information for simulated collisions. */
  struct FCollisionDebugInfo {
    int32 ArmIndex = -1;
    FHex ArmBaseHex;
    FHex ArmGripHex;
    FHex BlockedHex;
    FString Reason;
  };

  /** Information about a predictive collision between two arms or an arm and
   * the world. */
  struct FPredictiveCollisionInfo {
    int32 ArmIndexA = -1;
    int32 ArmIndexB = -1;
    FString Reason;
    FHex CollisionHex;
  };

  /** Tracks a pair of arms that are physically bonded via the items they hold.
   */
  struct FBondedArmPair {
    int32 ArmIndexA = -1;
    int32 ArmIndexB = -1;
    int32 BondIndex = -1;
    bool bActive = true;
  };

  /** A single rung in the helix - represents one processed cluster */
  struct FHelixRung {
    TArray<FOrbData> Orbs;
    TArray<TPair<int32, int32>> Bonds;
    TArray<TPair<int32, int32>> BondsToPrevious;
    TArray<TPair<int32, int32>> BondsToBoard;
    float SpawnTime = 0.f;
    int32 SpawnTick = 0;
    float ZOffset = 0.f;
    float TargetZOffset = 0.f;
    float RotationDegrees = 0.f;
    uint8 EntryDirection =
        0; // Direction items were pushed INTO the helix (for portal exit)
  };

  /** A group of adjacent HelixOutput tiles that form a single helix column */
  struct FHelixOutputGroup {
    TArray<FHex> MemberTiles;
    FHex CenterHex = FHex(0, 0, 0);
    FVector CenterWorld = FVector::ZeroVector;
    TArray<FHelixRung> Rungs;
    int64 TotalClustersProcessed = 0;
    float HelixRotationOffset = 0.f;

    TArray<FHex> PairedInputTiles;
    FHex PairedInputCenter = FHex(0, 0, 0);
    FVector PairedInputCenterWorld = FVector::ZeroVector;
    bool bHasPairedInput = false;

    float DegreesPerRung = 36.f;
    float RungSpacing = 100.f;
    int32 MaxVisibleRungs = 20;
    int32 LastProcessedTick = -1;

    int32 LastEnteredItemId = 0;
    int32 LastEnteredItemTick = -1;
    int32 RungTransitTime = 20;

    static constexpr int32 MinPairDistance = 3;
  };

  // Constructor and destructor
  USimWorldHex();
  virtual ~USimWorldHex();

  void Initialize(int32 Radius);

  // Config
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sim")
  float TileSizeUU = 120.f;

  /** Vertical offset for orbs on different grid levels. */
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sim")
  float OrbZOffset = 50.f;

  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sim")
  float FixedStepSec = 0.20f; // 5 Hz

  void StepBonds();
  void ProcessUnbondsForHeldClusters();

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

  void StepSim();

  // World state accessors
  const TMap<FHex, FItem> &GetItems() const { return ItemMap; }
  TMap<FHex, FItem> &GetItemsMutable() { return ItemMap; }

  const TArray<FArmState> &GetArms() const { return Arms; }
  TArray<FArmState> &GetArmsMutable() { return Arms; }

  const TArray<FWalkerState> &GetWalkers() const { return Walkers; }
  TArray<FWalkerState> &GetWalkersMutable() { return Walkers; }

  const TMap<FHex, FTile> &GetTiles() const { return TileMap; }
  TMap<FHex, FTile> &GetTilesMutable() { return TileMap; }

  const TMap<FHex, uint8> &GetOrbAt() const { return OrbAt; }

  const TMap<FHex, FHex> &GetIOBondPartners() const { return IOBondPartners; }
  TMap<FHex, FHex> &GetIOBondPartnersMutable() { return IOBondPartners; }

  /** Register a helix output tile - will auto-group with adjacent helix tiles
   */
  void RegisterHelixOutputTile(const FHex &Hex);

  /** Unregister a helix output tile */
  void UnregisterHelixOutputTile(const FHex &Hex);

  /** Register a helix input tile - pairs with an existing output group */
  void RegisterHelixInputTile(const FHex &Hex, int32 OutputGroupIndex);

  /** Unregister a helix input tile */
  void UnregisterHelixInputTile(const FHex &Hex);

  /** Explicitly pair a helix output group with an input layout */
  void PairHelixInput(int32 OutputGroupIndex, const TArray<FHex> &InputTiles,
                      const FHex &Center);

  /** Check if a hex position is valid for placing paired helix input tiles */
  bool CanPlaceHelixInputAt(const FHex &Hex, int32 OutputGroupIndex) const;

  /** Get the output group index that a hex would be paired with, or -1 if none
   */
  int32 GetHelixOutputGroupIndexAt(const FHex &Hex) const;

  /** Find which helix group a tile belongs to */
  FHelixOutputGroup *FindHelixGroupContaining(const FHex &Hex);
  const FHelixOutputGroup *FindHelixGroupContaining(const FHex &Hex) const;

  /** Find which helix group's INPUT tiles contain this hex */
  FHelixOutputGroup *FindHelixGroupWithInputContaining(const FHex &Hex);
  const FHelixOutputGroup *
  FindHelixGroupWithInputContaining(const FHex &Hex) const;

  /** Get all helix groups for rendering */
  const TArray<FHelixOutputGroup> &GetHelixGroups() const {
    return HelixGroups;
  }

  /** Check if an item (by ID) is currently within the helix output system */
  bool IsItemInHelix(int32 ItemId) const;

  /** Try to process helix output */
  UFUNCTION(BlueprintCallable, Category = "Sim|Helix")
  bool TryProcessHelixCluster(FHelixOutputGroup &Group);

  /** Try to eject a mature cluster from the helix */
  UFUNCTION(BlueprintCallable, Category = "Sim|Helix")
  bool TryEjectHelixCluster(FHelixOutputGroup &Group,
                            bool bIsForcedByPush = false);

  /** Update current portal-locked status (DNA strand anchoring) */
  void UpdateLockedItems();

  /** Update helix animation */
  void UpdateHelixGroups(float DeltaTime);

  /** Get world position of a helix center */
  FVector GetHelixCenterWorld(int32 GroupIndex) const;

  /** Revision counter for tile map changes to trigger visual updates. */
  UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Sim")
  int32 TileRevision = 0;

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

  // TriggerTile signals
  void SetBeltTriggerSignal(const FHex &TriggerHex, const FHex &BeltOwnerHex,
                            bool bPositive);
  void ClearBeltTriggerSignal(const FHex &TriggerHex, const FHex &BeltOwnerHex);
  bool IsBeltEnabled(const FHex &BeltHex) const;

  void SetArmTriggerSignal(const FHex &TriggerHex, const FHex &ArmBaseHex,
                           bool bPositive);
  void ClearArmTriggerSignal(const FHex &TriggerHex, const FHex &ArmBaseHex);
  bool IsArmEnabled(const FHex &ArmBaseHex) const;

  // Cluster template API
  struct FClusterTemplate {
    TArray<FOrbData> Orbs;
    TArray<TPair<int32, int32>> Bonds;
    FString Name;
    bool bAutoRotate = false;
  };

  void RegisterClusterTemplate(uint8 TemplateId, const TArray<FOrbData> &Orbs,
                               const TArray<TPair<int32, int32>> &BondPairs,
                               const FString &Name);
  bool ValidateClusterAgainstTemplate(const FItem &Cluster,
                                      uint8 TemplateId) const;

  uint8 AutoGenerateClusterTemplateForOutputs(const FHex &Hex1,
                                              const FHex &Hex2, uint8 OrbType1,
                                              uint8 OrbType2);

  uint8 ExpandClusterTemplate(uint8 ExistingTemplateId, const FHex &OldBaseHex,
                              const FHex &NewBaseHex,
                              const TArray<FHex> &AllClusterHexes,
                              const TMap<FHex, uint8> &HexToOrbType);

  uint8 GenerateNewTemplateId() { return NextAutoTemplateId++; }

  TArray<FHex> FindAllTilesInCluster(const FHex &StartHex) const;
  TArray<FHex>
  FindClusterInputChain(const FHex &StartHex,
                        const TSet<FHex> &ExcludeHexes = TSet<FHex>()) const;

  const TMap<uint8, FClusterTemplate> &GetClusterTemplates() const {
    return ClusterTemplates;
  }

  // Item helpers
  bool TryPutItem(const FHex &H, const FItem &I);
  bool TryTakeItem(const FHex &H, FItem &Out);

  /** Convert hex coordinate to world position */
  FVector HexCenterToWorld(const FHex &H) const;

  // Visual interpolation
  void SetFrameAlpha(float InAlpha) {
    FrameAlpha = FMath::Clamp(InAlpha, 0.f, 1.f);
  }
  float GetFrameAlpha() const { return FrameAlpha; }

  bool GetItemVisualLocation(int32 ItemId, FVector &OutWorld) const;
  bool GetOrbVisualLocation(int32 OrbId, FVector &OutWorld) const;

  int32 GetStepCounter() const { return StepCounter; }

  void SetWorldTransform(const FVector &InAnchor, float InGridYawDeg,
                         EHexOrientation InOrientation) {
    WorldAnchor = InAnchor;
    GridYawDeg = InGridYawDeg;
    GridOrientation = InOrientation;
  }

  const TArray<FBond> &GetBonds() const { return Bonds; }

  float GetBeltProgress(int32 ItemId) const {
    if (const float *Progress = BeltProgress.Find(ItemId)) {
      return *Progress;
    }
    return 0.f;
  }

  const TArray<FAnimatedOrb> &GetAnimatedOrbs() const { return AnimatedOrbs; }
  void UpdateAnimatedOrbs(float DeltaTime);
  void ProcessPendingBondFormations(float DeltaTime);

  void OnArmRotationComplete(int32 ArmIndex);
  void OnArmRotationFullyComplete(int32 ArmIndex);
  void CancelPendingBondFormationsForArm(int32 ArmIndex);

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

  UPROPERTY(Transient)
  TSet<int32> ItemsRejectedByClusterOutput;

  void UpdateArmBaseCache();
  void UpdateHeldOrbCache();
  void UpdateWalkerFootCache();
  bool PeekItemAt(const FHex &H, FItem &OutItem) const;
  EOrbType GetOrbTypeAtLocation(const FHex &H) const;
  bool TryPlaceOrbAt(const FHex &Hex, uint8 Kind);
  bool TryPickupOrbAt(const FHex &Hex, uint8 &OutKind);
  bool GetOrbKindAt(const FHex &Hex, uint8 &OutKind) const;

  const TArray<FCollisionDebugInfo> &GetCollisionDebugData() const {
    return CollisionDebugData;
  }
  const TArray<FPredictiveCollisionInfo> &
  GetPredictiveCollisionDebugData() const {
    return PredictiveCollisionDebugData;
  }

  UPROPERTY(Transient)
  TMap<FHex, uint8> DiscoveryTileOrbTypes;

  UPROPERTY(VisibleAnywhere, Category = "Spatial")
  TObjectPtr<class UOrbSpatialManager> SpatialManager;

  UPROPERTY(VisibleAnywhere, Category = "Spatial")
  TObjectPtr<class UBondResolver> BondResolver;

  // Track items moved by belts this step
  TSet<int32> ItemsMovedLastStep;

private:
  bool DoesItemMatchOutputType(const FItem &Item,
                               const FTile &OutputTile) const;
  bool bDevDebugArms = true;
  float FrameAlpha = 0.f;
  int32 StepCounter = 0;
  int32 MAX_ARM_LEN = 6;
  FVector WorldAnchor = FVector::ZeroVector;
  float GridYawDeg = 0.f;
  EHexOrientation GridOrientation = EHexOrientation::PointyTop;
  int OUTPUT_DESCEND_TICKS = 100;
  int32 OutputCount = 0;

  TSet<FHex> SkipInputThisFrame;
  TSet<FHex> SkipOutputThisFrame;
  TSet<int32> NewlyPlacedItems;

  struct FEjectedItemInfo {
    FVector Center;
    float StartZ;
  };
  TMap<int32, FEjectedItemInfo> EjectedItems;

  TMap<int32, uint8> ArmsReadyForUnbond;
  TMap<int32, uint8> PendingUnbonds;
  TMap<int32, int32> RecentlyDroppedByArm;
  TSet<FHex> OrbsNewlyMovedToOutput;
  TSet<int32> OrbsNewlyMovedByBelts;
  TSet<int32> OrbsMovedLastTick;
  TMap<FHex, bool> OutputTileGates;
  TMap<FHex, TMap<FHex, bool>> BeltTriggerSignals;
  TMap<FHex, int32> BeltLastMovedItemId;
  TMap<FHex, TMap<FHex, bool>> ArmTriggerSignals;

  UPROPERTY() TMap<FHex, uint8> OrbAt;
  TArray<FAnimatedOrb> AnimatedOrbs;
  TMap<int32, FHex> HeldItemLastHex;
  TMap<FHex, FHex> IOBondPartners;
  TMap<uint8, FClusterTemplate> ClusterTemplates;
  TMap<FHex, bool> ClusterOutputGates;
  uint8 NextAutoTemplateId = 128;

  UPROPERTY()
  TArray<FHelixOutputGroup> HelixGroups;

  TMap<FHex, FTile> TileMap;
  TMap<FHex, FItem> ItemMap;
  TArray<FArmState> Arms;
  TArray<FWalkerState> Walkers;
  TArray<FBond> Bonds;
  TMap<int32, TArray<int32>> ItemToBonds;

  TSet<FHex> CachedArmBaseHexes;
  bool bArmBaseCacheDirty = true;
  TSet<FHex> CachedHeldOrbHexes;
  TSet<FHex> CachedBlockingFootHexes;
  TSet<FHex> CachedIntendedFootHexes;

  bool HasOrbAt(const FHex &H) const;

  uint8 KindFromOrbAt(const FHex &H) const {
    if (const FItem *Item = ItemMap.Find(H))
      return KindFromItem(*Item);
    return 0;
  }

  struct FPendingHeldMerge {
    int32 ReceiverArmIndex = -1;
    int32 DonorArmIndex = -1;
    int32 ItemIdA = -1;
    int32 ItemIdB = -1;
  };
  TArray<FPendingHeldMerge> PendingHeldMerges;

  int32 NextItemId = 1;

  void StepArms();
  void StepArmsSecondPass();
  void StepBondsSecondPass();
  void StepBelts();
  void StepWalkers();

  TMap<FHex, int32> GetPredictedBondFormations() const;
  void StepGlyphs();

  static uint8 KindFromItem(const FItem &Item) {
    if (Item.bIsCluster && Item.Orbs.Num() > 0)
      return (uint8)Item.Orbs[0].Type;
    return (uint8)Item.Single.Type;
  }

  UPROPERTY()
  TMap<int32, float> BeltProgress;
  float BeltBlockedClamp = 0.98f;

  void TryFormBondAt(const FHex &Root, uint8 Dir, const TSet<int32> &HeldIds,
                     TSet<int32> &NewlyBondedItems);
  void CleanupBrokenBonds(const TSet<int32> &HeldIds);
  void RemoveBondIndex(int32 Index);
  void AddItemBondLink(int32 ItemId, int32 BondIndex);
  void RemoveItemBondLink(int32 ItemId, int32 BondIndex);

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

  void CheckAndRemoveDisconnectedOrbsFromHeldClusters();
  void ProcessUnbondsForHeldCluster(int32 ArmIndex);
  void ProcessHeldClusterUnbonds();
  void ProcessHeldToHeldMerge(int32 ReceiverArmIndex, int32 DonorArmIndex);
  FHex CalculateArmGripPosition(const FArmState &ArmState) const;

  void DrawBondDebugLines();

  UPROPERTY(EditAnywhere, Category = "Debug")
  bool bDevDebugBonds = false;

  UFUNCTION(BlueprintCallable, Category = "Sim|Bond")
  bool BreakBondBetween(int32 ItemA, int32 ItemB);

  UFUNCTION(BlueprintCallable, Category = "Sim|Bond")
  int32 BreakAllBondsForItem(int32 ItemId);

  bool IsItemBonded(int32 ItemId) const {
    if (const TArray<int32> *L = ItemToBonds.Find(ItemId)) {
      for (int32 BondIdx : *L) {
        if (Bonds.IsValidIndex(BondIdx) && Bonds[BondIdx].bActive)
          return true;
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

  void CheckForNewBondedArmPairs();
  void PauseBondedArms();
  void ResumeArmsWhenBondsBroken();
  bool IsArmBonded(int32 ArmIndex) const;

  TMap<int32, uint8> SimFacingByArm;
  TMap<int32, int32> SimArmLenByArm;
  TMap<int32, int8> SimGrabFacingByArm;

  TMap<int32, uint8> PendingFacingByArm;
  TMap<int32, int32> PendingArmLenByArm;
  TMap<int32, int8> PendingGrabFacingByArm;

  FHex GetArmGripHex_Sim(int32 ArmIndex) const;
  uint8 GetHeldCCW_Steps_Sim(int32 ArmIndex) const;
  static FHex Rot60CCW(FHex D, uint8 StepsCCW);
  static FHex Rot60CW(FHex D, uint8 StepsCW);

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
    FPredictiveArmState(const FArmState &S)
        : BaseHex(S.BaseHex), Facing(static_cast<uint8>(S.Facing)),
          ArmLen(S.ArmLen), bHolding(S.bHolding), Held(S.Held),
          WaitCounter(S.WaitCounter), PC(S.PC),
          bRotationCompleted(S.bRotationCompleted) {}
  };

  bool WouldCauseHeldClusterOverlap();
  TArray<FPredictiveArmState> SimulateAllArmsExecution();
  bool WouldHeldClustersOverlap(const FPredictiveArmState &A,
                                const FPredictiveArmState &B,
                                const FArmState &CurrA, const FArmState &CurrB);
  TArray<FHex> GetHeldClusterPositions(const FPredictiveArmState &S);
  bool WouldRotationSweepsOverlap(const FArmState &A, const FArmState &B,
                                  const FPredictiveArmState &PredA,
                                  const FPredictiveArmState &PredB);
  bool WouldRotationSweepOverlapStatic(const FArmState &RotArm,
                                       const FPredictiveArmState &PredRot,
                                       const FArmState &StaticArm);
  bool DoesArmSweepOverlapTiles(const FArmState &A,
                                const FPredictiveArmState &P,
                                const TSet<FHex> &Tiles);

  UFUNCTION(BlueprintCallable, Category = "Sim|Debug")
  bool ValidateDataIntegrity() const;

  UFUNCTION(BlueprintCallable, Category = "Sim|Debug")
  void CleanupCorruptedData();

  bool TryArmPickItem(const FHex &From, FItem &OutItem);
  bool TryArmPlaceItem(const FHex &To, const FItem &Item);

  TArray<FPendingBondFormation> PendingBondFormations;
  TArray<FBondedArmPair> BondedArmPairs;
  TArray<FCollisionDebugInfo> CollisionDebugData;
  TArray<FPredictiveCollisionInfo> PredictiveCollisionDebugData;

  struct FPendingGrab {
    int32 ArmIndex;
    FHex Target;
  };
  TArray<FPendingGrab> PendingGrabs;
};
