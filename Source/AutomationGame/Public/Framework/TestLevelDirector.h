#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Sim/HexTypes.h"
#include "Sim/Tiles.h"
#include "Actors/BasicArm.h"
#include "Actors/BasicWalker.h"
#include "Sim/WalkerTypes.h"
#include "Actors/RequestBeacon.h"
#include "Sim/TriggerTypes.h"
#include "TestLevelDirector.generated.h"

class ATriggerTile;

class AGameTickManager;
class AOrbSimDriver;
class USimWorldHex;

USTRUCT(BlueprintType)
struct FPlacedTileSpec
{
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
	 * Optional metadata for ClusterOutput tiles: the actual orb type for this tile position.
	 * This preserves the original Output tile's orb type after conversion to ClusterOutput.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite) uint8 OrbType = 0;
};

USTRUCT(BlueprintType)
struct FArmSpawnSpec
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite) int32 Q = 0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) int32 R = 0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = 0, ClampMax = 5)) uint8 Facing = 0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = 1)) int32 StartingReach = 1;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = 0, ClampMax = 5)) uint8 StartingRotation = 0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) TArray<FArmInstr> Program;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) bool bLoopProgram = true;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) int32 Level = 0;
	UPROPERTY(Transient) ABasicArm* Spawned = nullptr;
};

USTRUCT(BlueprintType)
struct FWalkerSpawnSpec
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite) int32 Q = 0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) int32 R = 0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) FHex StartingLocation;  // Set when placed
	UPROPERTY(EditAnywhere, BlueprintReadWrite) TArray<FWalkerInstr> Program;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) bool bLoopProgram = true;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) int32 Level = 0;
	UPROPERTY(Transient) ABasicWalker* Spawned = nullptr;
};

USTRUCT(BlueprintType)
struct FRequestBeaconSpec
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite) int32 Q = 0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) int32 R = 0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) EBeaconType BeaconType = EBeaconType::Relocate;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) EBeaconRole BeaconRole = EBeaconRole::None;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) float Rotation = 0.f; // Yaw rotation for DropOff beacons
	UPROPERTY(EditAnywhere, BlueprintReadWrite) int32 PairedBeaconQ = 0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) int32 PairedBeaconR = 0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) EBeaconRole PairedBeaconRole = EBeaconRole::None;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) int32 Level = 0;
	UPROPERTY(Transient) ARequestBeacon* Spawned = nullptr;
};

USTRUCT(BlueprintType)
struct FTriggerTileSpec
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite) int32 Q = 0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) int32 R = 0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) EOrbType TriggerOrbType = EOrbType::BaseA;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) ETriggerVariant Variant = ETriggerVariant::Positive;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) int32 OwnerQ = 0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) int32 OwnerR = 0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) ETriggerTileOwnerType OwnerType = ETriggerTileOwnerType::Belt;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) float Rotation = 0.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) int32 Level = 0;
	UPROPERTY(Transient) ATriggerTile* Spawned = nullptr;
};

USTRUCT(BlueprintType)
struct FScenarioSpec
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite) FName Name = "Scenario";
	UPROPERTY(EditAnywhere, BlueprintReadWrite) bool bIsDiscovery = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite) TArray<FPlacedTileSpec> Tiles;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) TArray<FArmSpawnSpec> Arms;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) TArray<FWalkerSpawnSpec> Walkers;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) TArray<FRequestBeaconSpec> Beacons;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) TArray<FTriggerTileSpec> TriggerTiles;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) bool bAutoPlay = true;
};

UCLASS()
class AUTOMATIONGAME_API ATestLevelDirector : public AActor
{
	GENERATED_BODY()
public:
	ATestLevelDirector();
	virtual void BeginPlay() override;
	virtual void BeginDestroy() override;

	UPROPERTY(EditAnywhere, Category = "Refs") AGameTickManager* TickManager = nullptr;
	UPROPERTY(EditAnywhere, Category = "Refs") AOrbSimDriver* Driver = nullptr;

	UPROPERTY(EditAnywhere, Category = "Refs") TSubclassOf<AGameTickManager> TickManagerClass;
	UPROPERTY(EditAnywhere, Category = "Refs") TSubclassOf<AOrbSimDriver>   DriverClass;
	UPROPERTY(EditAnywhere, Category = "Refs") TSubclassOf<ABasicArm> ArmClass;
	UPROPERTY(EditAnywhere, Category = "Refs") TSubclassOf<ABasicWalker> WalkerClass;
	UPROPERTY(EditAnywhere, Category = "Refs") TSubclassOf<ARequestBeacon> BeaconClass;
	UPROPERTY(EditAnywhere, Category = "Refs") TSubclassOf<ATriggerTile> TriggerTileClass;

	UPROPERTY(EditAnywhere, Category = "Scenarios") TArray<FScenarioSpec> Scenarios;
	UPROPERTY(EditAnywhere, Category = "Scenarios", meta = (ClampMin = 0)) int32 ActiveScenarioIndex = 0;

	// --- Discovery Mode Settings ---
	UPROPERTY(EditAnywhere, Category = "Discovery Mode")
	int32 StartingGridRadius = 40;

	UPROPERTY(EditAnywhere, Category = "Discovery Mode")
	int32 StartingDifficulty = 4;

	UPROPERTY(EditAnywhere, Category = "Discovery Mode")
	int32 DifficultyScale = 1;

	UPROPERTY(EditAnywhere, Category = "Discovery Mode")
	int32 GeneratedRadius = 400;

	UPROPERTY(EditAnywhere, Category = "Discovery Mode")
	int32 MinimumUnlockRadiusPerOutput = 10;

	UPROPERTY(EditAnywhere, Category = "Discovery Mode")
	float GeneratedInputPerOutputRatio = 0.25f;

	/** Controls how much spacing increases with distance from center. Higher = more spread out at edges. */
	UPROPERTY(EditAnywhere, Category = "Discovery Mode", meta = (ClampMin = "0.1", ClampMax = "5.0"))
	float GeneratedInputOutputSpacingScale = 1.0f;

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
	void UpsertTileSpec(const FHex& Hex, ETileKind Kind, uint8 Dir);
	
	// Specialized version for ClusterOutput tiles that includes orb type
	UFUNCTION(BlueprintCallable, Category = "Scenario|Runtime")
	void UpsertClusterOutputTileSpec(const FHex& Hex, uint8 TemplateId, uint8 OrbType);
	
	// Scan the simulation and sync all ClusterOutput tiles with their orb types
	UFUNCTION(BlueprintCallable, Category = "Scenario|Runtime")
	void SyncClusterOutputTiles();
	
	UFUNCTION(BlueprintCallable, Category = "Scenario|Runtime")
	bool RemoveTileSpec(const FHex& Hex);
	UFUNCTION(BlueprintCallable, Category = "Scenario|Runtime")
	void UpsertArmSpec(int32 Q, int32 R, uint8 Facing, const TArray<FArmInstr>& Program, bool bLoop, int32 Level);
	UFUNCTION(BlueprintCallable, Category = "Scenario|Runtime")
	bool RemoveArmSpec(int32 Q, int32 R, int32 Level);
	UFUNCTION(BlueprintCallable, Category = "Scenario|Runtime")
	void UpsertWalkerSpec(int32 Q, int32 R, const FHex& StartingLocation, const TArray<FWalkerInstr>& Program, bool bLoop, int32 Level);
	UFUNCTION(BlueprintCallable, Category = "Scenario|Runtime")
	bool RemoveWalkerSpec(int32 Q, int32 R, int32 Level);
	UFUNCTION(BlueprintCallable, Category = "Scenario|Runtime")
	void UpsertBeaconSpec(int32 Q, int32 R, EBeaconType BeaconType, EBeaconRole BeaconRole, float Rotation, int32 Level);
	UFUNCTION(BlueprintCallable, Category = "Scenario|Runtime")
	bool RemoveBeaconSpec(int32 Q, int32 R, int32 Level);
	UFUNCTION(BlueprintCallable, Category = "Scenario|Runtime")
	void UpsertTriggerTileSpec(int32 Q, int32 R, EOrbType TriggerOrbType, ETriggerVariant Variant, int32 OwnerQ, int32 OwnerR, ETriggerTileOwnerType OwnerType, float Rotation, int32 Level);
	UFUNCTION(BlueprintCallable, Category = "Scenario|Runtime")
	bool RemoveTriggerTileSpec(int32 Q, int32 R, int32 Level);

	// Control what runtime edits get written back into scenario specs.
	UPROPERTY(EditAnywhere, Category = "Scenarios|Runtime Capture")
	bool bCaptureRuntimeProgramEdits = true;

	UPROPERTY(EditAnywhere, Category = "Scenarios|Runtime Capture")
	bool bCaptureRuntimeFacingEdits = false;   // NEW: default off so placing arms won't overwrite original facing

	// Manually capture current facing (if bCaptureRuntimeFacingEdits is false)
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "Scenarios|Runtime Capture")
	void CommitRuntimeFacingEdits();

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& Event) override;
#endif

private:
	void AutoFindOrSpawn();
	void ApplyScenario(const FScenarioSpec& S);
	void RefreshSpawnedArmsFromScenario(int32 ScenarioIndex);
	void MarkActiveScenarioDirty();
	void ResetSimForActiveScenario(bool bSyncFromWorld = true);          // NEW: clears sim before applying scenario
	void DelayedScenarioApplication();         // NEW: applies scenarios after GameManager initialization

	void SyncPreviewEditsIntoScenarioFiltered(FScenarioSpec& Scenario); // filtered version

#if WITH_EDITOR
	void HandlePreBeginPIE(bool bIsSimulating);
	void HandleEndPIE(bool bIsSimulating);
	void MirrorScenarioEditsToEditor();       // live mirroring during PIE
	void SyncAllRuntimeArmEdits();            // harvest live arm program/facing into specs
#endif
};
