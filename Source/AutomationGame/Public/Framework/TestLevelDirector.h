#pragma once

#include "Actors/BasicArm.h"
#include "Actors/BasicWalker.h"
#include "Actors/RequestBeacon.h"
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Sim/HexTypes.h"
#include "Sim/Tiles.h"
#include "Sim/TriggerTypes.h"
#include "Sim/WalkerTypes.h"
#include "TestLevelDirector.generated.h"

class ATriggerTile;

class AGameTickManager;
class AOrbSimDriver;
class USimWorldHex;

USTRUCT(BlueprintType)
struct FPlacedTileSpec {
  GENERATED_BODY()

  UPROPERTY(EditAnywhere, BlueprintReadWrite) FHex Hex { 0, 0 };
  UPROPERTY(EditAnywhere, BlueprintReadWrite) ETileKind Kind = ETileKind::Empty;
  /**
   * Multi-purpose field based on tile kind:
   * - Belt/Bond/Unbond tiles: Direction (0-5)
   * - Input/Output tiles: Orb Type (EOrbType enum value)
   * - ClusterOutput tiles: Template ID (uint8, can be 0-255)
   */
  UPROPERTY(EditAnywhere, BlueprintReadWrite) uint8 Dir = 0;

  /**
   * Optional metadata for ClusterOutput tiles: the actual orb type for this
   * tile position. This preserves the original Output tile's orb type after
   * conversion to ClusterOutput.
   */
  UPROPERTY(EditAnywhere, BlueprintReadWrite) uint8 OrbType = 0;
};

USTRUCT(BlueprintType)
struct FArmSpawnSpec {
  GENERATED_BODY()

  UPROPERTY(EditAnywhere, BlueprintReadWrite) int32 Q = 0;
  UPROPERTY(EditAnywhere, BlueprintReadWrite) int32 R = 0;
  UPROPERTY(EditAnywhere, BlueprintReadWrite,
            meta = (ClampMin = 0, ClampMax = 5))
  uint8 Facing = 0;
  UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = 1))
  int32 StartingReach = 1;
  UPROPERTY(EditAnywhere, BlueprintReadWrite,
            meta = (ClampMin = 0, ClampMax = 5))
  uint8 StartingRotation = 0;
  UPROPERTY(EditAnywhere, BlueprintReadWrite) TArray<FArmInstr> Program;
  UPROPERTY(EditAnywhere, BlueprintReadWrite) bool bLoopProgram = true;
  UPROPERTY(EditAnywhere, BlueprintReadWrite) int32 Level = 0;
  UPROPERTY(Transient) ABasicArm *Spawned = nullptr;
};

USTRUCT(BlueprintType)
struct FWalkerSpawnSpec {
  GENERATED_BODY()

  UPROPERTY(EditAnywhere, BlueprintReadWrite) int32 Q = 0;
  UPROPERTY(EditAnywhere, BlueprintReadWrite) int32 R = 0;
  UPROPERTY(EditAnywhere, BlueprintReadWrite)
  FHex StartingLocation; // Set when placed
  UPROPERTY(EditAnywhere, BlueprintReadWrite) TArray<FWalkerInstr> Program;
  UPROPERTY(EditAnywhere, BlueprintReadWrite) bool bLoopProgram = true;
  UPROPERTY(EditAnywhere, BlueprintReadWrite) int32 Level = 0;
  UPROPERTY(Transient) ABasicWalker *Spawned = nullptr;
};

USTRUCT(BlueprintType)
struct FRequestBeaconSpec {
  GENERATED_BODY()

  UPROPERTY(EditAnywhere, BlueprintReadWrite) int32 Q = 0;
  UPROPERTY(EditAnywhere, BlueprintReadWrite) int32 R = 0;
  UPROPERTY(EditAnywhere, BlueprintReadWrite)
  EBeaconType BeaconType = EBeaconType::Relocate;
  UPROPERTY(EditAnywhere, BlueprintReadWrite)
  EBeaconRole BeaconRole = EBeaconRole::None;
  UPROPERTY(EditAnywhere, BlueprintReadWrite)
  float Rotation = 0.f; // Yaw rotation for DropOff beacons
  UPROPERTY(EditAnywhere, BlueprintReadWrite) int32 PairedBeaconQ = 0;
  UPROPERTY(EditAnywhere, BlueprintReadWrite) int32 PairedBeaconR = 0;
  UPROPERTY(EditAnywhere, BlueprintReadWrite)
  EBeaconRole PairedBeaconRole = EBeaconRole::None;
  UPROPERTY(EditAnywhere, BlueprintReadWrite) int32 Level = 0;
  UPROPERTY(EditAnywhere, BlueprintReadWrite) int32 Radius = 10;
  UPROPERTY(EditAnywhere, BlueprintReadWrite) uint8 FilterOrbType = 0;
  UPROPERTY(EditAnywhere, BlueprintReadWrite) int32 MaxGatherCount = 10;
  UPROPERTY(Transient) ARequestBeacon *Spawned = nullptr;
};

USTRUCT(BlueprintType)
struct FTriggerTileSpec {
  GENERATED_BODY()

  UPROPERTY(EditAnywhere, BlueprintReadWrite) int32 Q = 0;
  UPROPERTY(EditAnywhere, BlueprintReadWrite) int32 R = 0;
  UPROPERTY(EditAnywhere, BlueprintReadWrite)
  EOrbType TriggerOrbType = EOrbType::BaseA;
  UPROPERTY(EditAnywhere, BlueprintReadWrite)
  ETriggerVariant Variant = ETriggerVariant::Positive;

  /** List of owners for this trigger signal */
  UPROPERTY(EditAnywhere, BlueprintReadWrite) TArray<FTriggerOwner> Owners;

  UPROPERTY(EditAnywhere, BlueprintReadWrite) float Rotation = 0.f;
  UPROPERTY(EditAnywhere, BlueprintReadWrite) int32 Level = 0;
  UPROPERTY(Transient) ATriggerTile *Spawned = nullptr;
};

USTRUCT(BlueprintType)
struct FHelixPairSpec {
  GENERATED_BODY()

  UPROPERTY(EditAnywhere, BlueprintReadWrite) TArray<FHex> OutputTiles;
  UPROPERTY(EditAnywhere, BlueprintReadWrite) TArray<FHex> InputTiles;
  UPROPERTY(EditAnywhere, BlueprintReadWrite) FHex OutputCenter;
  UPROPERTY(EditAnywhere, BlueprintReadWrite) FHex InputCenter;
  UPROPERTY(EditAnywhere, BlueprintReadWrite) int32 TransitTime = 5;
};

USTRUCT(BlueprintType)
struct FTutorialStep {
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(MultiLine=true))
    FString Message;

    // Time to display this message before proceeding. If > 0, it advances automatically after this time. If 0, it waits for the placement conditions below.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(ClampMin=0.0f))
    float TextTimer = 0.0f;

    // Should GameTickManager be paused when this step begins?
    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    bool bShouldPauseGTM = false;

    // Is there a tile we want to focus the camera on?
    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    bool bHasFocusTile = false;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(EditCondition="bHasFocusTile"))
    FHex FocusTile;

    // Is there a tile requirement to proceed?
    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    bool bRequiresTilePlacement = false;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(EditCondition="bRequiresTilePlacement"))
    ETileKind RequiredTileKind = ETileKind::Empty;

    // Is there a direction requirement (e.g. for Belts, Bonds, Unbonds)?
    UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(EditCondition="bRequiresTilePlacement"))
    bool bRequiresDirection = false;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(EditCondition="bRequiresTilePlacement && bRequiresDirection"))
    uint8 RequiredDirection = 0;

    // Is there an arm placement requirement?
    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    bool bRequiresArmPlacement = false;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(EditCondition="bRequiresArmPlacement || bRequiresTilePlacement"))
    FHex TargetHex; // Shared with tile placement

    UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(EditCondition="bRequiresArmPlacement"))
    uint8 RequiredArmFacing = 0;

    // Optional arrays for multi-tile/arm requirements
    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    TArray<FPlacedTileSpec> RequiredTiles;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    TArray<FArmSpawnSpec> RequiredArms;
};

USTRUCT(BlueprintType)
struct FScenarioSpec {
  GENERATED_BODY()

  UPROPERTY(EditAnywhere, BlueprintReadWrite) FName Name = "Scenario";
  UPROPERTY(EditAnywhere, BlueprintReadWrite) bool bIsDiscovery = false;

  UPROPERTY(EditAnywhere, BlueprintReadWrite) TArray<FPlacedTileSpec> Tiles;
  UPROPERTY(EditAnywhere, BlueprintReadWrite) TArray<FArmSpawnSpec> Arms;
  UPROPERTY(EditAnywhere, BlueprintReadWrite) TArray<FWalkerSpawnSpec> Walkers;
  UPROPERTY(EditAnywhere, BlueprintReadWrite)
  TArray<FRequestBeaconSpec> Beacons;

  UPROPERTY(EditAnywhere, BlueprintReadWrite)
  TArray<FTriggerTileSpec> TriggerTiles;
  
  UPROPERTY(EditAnywhere, BlueprintReadWrite)
  TArray<FHelixPairSpec> HelixPairs;
  
  UPROPERTY(EditAnywhere, BlueprintReadWrite)
  TArray<FTutorialStep> TutorialSteps;
  
  UPROPERTY(EditAnywhere, BlueprintReadWrite) bool bAutoPlay = true;

  UPROPERTY(EditAnywhere, BlueprintReadWrite) float DefaultOrbLifetime = -1.0f;
};

UCLASS()
class AUTOMATIONGAME_API ATestLevelDirector : public AActor {
  GENERATED_BODY()
public:
  ATestLevelDirector();
  virtual void BeginPlay() override;
  virtual void BeginDestroy() override;

  UPROPERTY(EditAnywhere, Category = "Refs")
  AGameTickManager *TickManager = nullptr;
  UPROPERTY(EditAnywhere, Category = "Refs") AOrbSimDriver *Driver = nullptr;

  UPROPERTY(EditAnywhere, Category = "Refs")
  TSubclassOf<AGameTickManager> TickManagerClass;
  UPROPERTY(EditAnywhere, Category = "Refs")
  TSubclassOf<AOrbSimDriver> DriverClass;
  UPROPERTY(EditAnywhere, Category = "Refs") TSubclassOf<ABasicArm> ArmClass;
  UPROPERTY(EditAnywhere, Category = "Refs")
  TSubclassOf<ABasicWalker> WalkerClass;
  UPROPERTY(EditAnywhere, Category = "Refs")
  TSubclassOf<ARequestBeacon> BeaconClass;
  UPROPERTY(EditAnywhere, Category = "Refs")
  TSubclassOf<ATriggerTile> TriggerTileClass;

  UPROPERTY(EditAnywhere, Category = "Scenarios")
  TArray<FScenarioSpec> Scenarios;
  UPROPERTY(EditAnywhere, Category = "Scenarios", meta = (ClampMin = 0))
  int32 ActiveScenarioIndex = 0;

  // --- Tutorial Editor Settings ---
  UPROPERTY(EditAnywhere, Category = "Tutorial Editor")
  int32 EditTutorialScenarioIndex = 0;

  UPROPERTY(EditAnywhere, Category = "Tutorial Editor")
  int32 EditTutorialStepIndex = 0;

  UFUNCTION(CallInEditor, Category = "Tutorial Editor")
  void CaptureSelectedHexAsFocusTile();

  UFUNCTION(CallInEditor, Category = "Tutorial Editor")
  void CaptureGlyphsBase();

  UFUNCTION(CallInEditor, Category = "Tutorial Editor")
  void CaptureGlyphsDiffIntoStep();

  UPROPERTY(Transient)
  FScenarioSpec CaptureBaseScenario;

  UPROPERTY(Transient)
  bool bWaitingForContextTileClick = false;

  // --- Discovery Mode Settings ---
  UPROPERTY(EditAnywhere, Category = "Discovery Mode")
  int32 StartingGridRadius = 40;

  UPROPERTY(EditAnywhere, Category = "Discovery Mode")
  int32 StartingDifficulty = 4;

  // --- DEPRECATED: No longer used by new discovery mode ---
  int32 DifficultyScale_DEPRECATED = 1;
  int32 GeneratedRadius_DEPRECATED = 400;
  int32 MinimumUnlockRadiusPerOutput_DEPRECATED = 10;
  float GeneratedInputPerOutputRatio_DEPRECATED = 0.25f;
  float GeneratedInputOutputSpacingScale_DEPRECATED = 1.0f;

  UFUNCTION(CallInEditor, BlueprintCallable, Category = "Discovery Mode")
  void GenerateDiscoveryScenario();

  UFUNCTION(CallInEditor, BlueprintCallable, Category = "Scenarios")
  void BuildActiveScenario();
  UFUNCTION(CallInEditor, BlueprintCallable, Category = "Scenarios")
  void ClearArms();
  UFUNCTION(CallInEditor, Category = "Scenario")
  void SpawnScenarioPreview();
  UFUNCTION(BlueprintCallable, Category = "Scenario")
  FVector GetWorldPositionForHex(int32 Q, int32 R) const;

  // Runtime + editor API to change scenario (auto rebuild)
  UFUNCTION(BlueprintCallable, Category = "Scenarios")
  void SetActiveScenarioIndex(int32 NewIndex);

  UPROPERTY(EditAnywhere, Category = "Scenarios|Editing")
  bool bAutoPatchSpawnedArms = true;

  UFUNCTION(CallInEditor, BlueprintCallable, Category = "Scenarios")
  void CommitPreviewEdits();
  UFUNCTION(CallInEditor, BlueprintCallable, Category = "Scenarios")
  void ReapplyActiveScenario();

  // Runtime mutation API (used by placement component)
  UFUNCTION(BlueprintCallable, Category = "Scenario|Runtime")
  void UpsertTileSpec(const FHex &Hex, ETileKind Kind, uint8 Dir);

  // Specialized version for ClusterOutput tiles that includes orb type
  UFUNCTION(BlueprintCallable, Category = "Scenario|Runtime")
  void UpsertClusterOutputTileSpec(const FHex &Hex, uint8 TemplateId,
                                   uint8 OrbType);

  // Specialized version for Belt tiles that includes orb type filter
  UFUNCTION(BlueprintCallable, Category = "Scenario|Runtime")
  void UpsertBeltTileSpec(const FHex &Hex, uint8 Dir, uint8 OrbType);

  // Scan the simulation and sync all ClusterOutput tiles with their orb types
  UFUNCTION(BlueprintCallable, Category = "Scenario|Runtime")
  void SyncClusterOutputTiles();

  UFUNCTION(BlueprintCallable, Category = "Scenario|Runtime")
  bool RemoveTileSpec(const FHex &Hex);
  UFUNCTION(BlueprintCallable, Category = "Scenario|Runtime")
  void UpsertArmSpec(int32 Q, int32 R, uint8 Facing,
                     const TArray<FArmInstr> &Program, bool bLoop, int32 Level);
  UFUNCTION(BlueprintCallable, Category = "Scenario|Runtime")
  bool RemoveArmSpec(int32 Q, int32 R, int32 Level);
  UFUNCTION(BlueprintCallable, Category = "Scenario|Runtime")
  void UpsertWalkerSpec(int32 Q, int32 R, const FHex &StartingLocation,
                        const TArray<FWalkerInstr> &Program, bool bLoop,
                        int32 Level);
  UFUNCTION(BlueprintCallable, Category = "Scenario|Runtime")
  bool RemoveWalkerSpec(int32 Q, int32 R, int32 Level);
  UFUNCTION(BlueprintCallable, Category = "Scenario|Runtime")
  void UpsertBeaconSpec(int32 Q, int32 R, EBeaconType BeaconType,
                        EBeaconRole BeaconRole, float Rotation, int32 Level,
                        int32 Radius = 10, uint8 FilterOrbType = 0,
                        int32 MaxGatherCount = 10);
  UFUNCTION(BlueprintCallable, Category = "Scenario|Runtime")
  bool RemoveBeaconSpec(int32 Q, int32 R, int32 Level);
  UFUNCTION(BlueprintCallable, Category = "Scenario|Runtime")
  void UpsertTriggerTileSpec(int32 Q, int32 R, EOrbType TriggerOrbType,
                             ETriggerVariant Variant, float Rotation,
                             int32 Level);

  UFUNCTION(BlueprintCallable, Category = "Scenario|Runtime")
  void ToggleTriggerTileOwnerSpec(int32 Q, int32 R, int32 Level, int32 OwnerQ,
                                  int32 OwnerR, int32 OwnerLevel,
                                  ETriggerTileOwnerType OwnerType);
  UFUNCTION(BlueprintCallable, Category = "Scenario|Runtime")
  bool RemoveTriggerTileSpec(int32 Q, int32 R, int32 Level);

  // Control what runtime edits get written back into scenario specs.
  UPROPERTY(EditAnywhere, Category = "Scenarios|Runtime Capture")
  bool bCaptureRuntimeProgramEdits = true;

  UPROPERTY(EditAnywhere, Category = "Scenarios|Runtime Capture")
  bool bCaptureRuntimeFacingEdits =
      false; // NEW: default off so placing arms won't overwrite original facing

  // Manually capture current facing (if bCaptureRuntimeFacingEdits is false)
  UFUNCTION(CallInEditor, BlueprintCallable,
            Category = "Scenarios|Runtime Capture")
  void CommitRuntimeFacingEdits();

#if WITH_EDITOR
  virtual void PostEditChangeProperty(FPropertyChangedEvent &Event) override;
#endif

private:
  void AutoFindOrSpawn();
  void ApplyScenario(const FScenarioSpec &S);
  void RefreshSpawnedArmsFromScenario(int32 ScenarioIndex);

public:
  void MarkActiveScenarioDirty();

private:
  void ResetSimForActiveScenario(
      bool bSyncFromWorld = true);   // NEW: clears sim before applying scenario
  void DelayedScenarioApplication(); // NEW: applies scenarios after GameManager
                                     // initialization

  void SyncPreviewEditsIntoScenarioFiltered(
      FScenarioSpec &Scenario); // filtered version
  void InvalidateHelixPairingAt(FScenarioSpec &Scenario, const FHex &Hex);

#if WITH_EDITOR
  void HandlePreBeginPIE(bool bIsSimulating);
  void HandleEndPIE(bool bIsSimulating);
public:
  void MirrorScenarioEditsToEditor(); // live mirroring during PIE
private:
  void SyncAllRuntimeArmEdits(); // harvest live arm program/facing into specs
#endif
};
