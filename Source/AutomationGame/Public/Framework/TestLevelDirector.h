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
	/** Meaningful for belts (0..5). Ignored for non-directional tiles. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = 0, ClampMax = 5)) uint8 Dir = 0;
};

USTRUCT(BlueprintType)
struct FArmSpawnSpec
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite) int32 Q = 0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) int32 R = 0;
	/** 0..5 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = 0, ClampMax = 5)) uint8 Facing = 0;

	/** Program to load into the arm; will loop if bLoopProgram is true. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite) TArray<FArmInstr> Program;

	/** Loop the arm’s program after the end. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite) bool bLoopProgram = true;

	/** (Runtime) spawned arm reference. */
	UPROPERTY(Transient) ABasicArm* Spawned = nullptr;
};

USTRUCT(BlueprintType)
struct FScenarioSpec
{
	GENERATED_BODY()

	/** Friendly name for the picker. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite) FName Name = "Scenario";

	/** Sim tile size for this scenario. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = 8)) float TileSizeUU = 120.f;

	/** Optional: how far you want to generate tiles elsewhere (purely visual is handled elsewhere). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = 1)) int32 GridRadius = 8;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	TArray<FPlacedTileSpec> Tiles;
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	TArray<FArmSpawnSpec> Arms;

	/** Should the tick manager start playing after setup? */
	UPROPERTY(EditAnywhere, BlueprintReadWrite) bool bAutoPlay = true;
};

UCLASS()
class AUTOMATIONGAME_API ATestLevelDirector : public AActor
{
	GENERATED_BODY()
public:
	ATestLevelDirector();
	virtual void BeginPlay() override;

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

	UPROPERTY(EditAnywhere, Category = "Scenarios|Editing")
	bool bAutoPatchSpawnedArms = true;

	UFUNCTION(CallInEditor, BlueprintCallable, Category = "Scenarios")
	void CommitPreviewEdits();

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& Event) override;
#endif

private:
	void AutoFindOrSpawn();
	void ApplyScenario(const FScenarioSpec& S);
	void RefreshSpawnedArmsFromScenario(int32 ScenarioIndex);
#if WITH_EDITOR
	void HandlePreBeginPIE(bool bIsSimulating);
#endif
};
