#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Sim/HexTypes.h"
#include "Sim/Tiles.h"
#include "Actors/BasicArm.h"
#include "TestLevelDirector.generated.h"

class AGameTickManager;
class AOrbSimDriver;
class USimWorldHex;

USTRUCT(BlueprintType)
struct FPlacedTileSpec
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite) FHex Hex { 0, 0 };
	UPROPERTY(EditAnywhere, BlueprintReadWrite) ETileKind Kind = ETileKind::Empty;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = 0, ClampMax = 5)) uint8 Dir = 0;
};

USTRUCT(BlueprintType)
struct FArmSpawnSpec
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite) int32 Q = 0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) int32 R = 0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = 0, ClampMax = 5)) uint8 Facing = 0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) TArray<FArmInstr> Program;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) bool bLoopProgram = true;
	UPROPERTY(Transient) ABasicArm* Spawned = nullptr;
};

USTRUCT(BlueprintType)
struct FScenarioSpec
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite) FName Name = "Scenario";
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = 8)) float TileSizeUU = 120.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = 1)) int32 GridRadius = 8;

	UPROPERTY(EditAnywhere, BlueprintReadWrite) TArray<FPlacedTileSpec> Tiles;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) TArray<FArmSpawnSpec> Arms;
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

	UPROPERTY(EditAnywhere, Category = "Scenarios") TArray<FScenarioSpec> Scenarios;
	UPROPERTY(EditAnywhere, Category = "Scenarios", meta = (ClampMin = 0)) int32 ActiveScenarioIndex = 0;

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
	UFUNCTION(BlueprintCallable, Category = "Scenario|Runtime")
	bool RemoveTileSpec(const FHex& Hex);
	UFUNCTION(BlueprintCallable, Category = "Scenario|Runtime")
	void UpsertArmSpec(int32 Q, int32 R, uint8 Facing, const TArray<FArmInstr>& Program, bool bLoop);
	UFUNCTION(BlueprintCallable, Category = "Scenario|Runtime")
	bool RemoveArmSpec(int32 Q, int32 R);

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
	void ResetSimForActiveScenario();          // NEW: clears sim before applying scenario
	void DelayedScenarioApplication();         // NEW: applies scenarios after GameManager initialization

	void SyncPreviewEditsIntoScenarioFiltered(FScenarioSpec& Scenario); // filtered version

#if WITH_EDITOR
	void HandlePreBeginPIE(bool bIsSimulating);
	void HandleEndPIE(bool bIsSimulating);
	void MirrorScenarioEditsToEditor();       // live mirroring during PIE
	void SyncAllRuntimeArmEdits();            // harvest live arm program/facing into specs
#endif
};
