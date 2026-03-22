#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Sim/HexTypes.h"
#include "Sim/Tiles.h"
#include "Framework/TestLevelDirector.h" // For FScenarioSpec and FTutorialStep
#include "TutorialDirector.generated.h"

class AGameTickManager;
class AOrbSimDriver;
class UStaticMeshComponent;
class UInstancedStaticMeshComponent;
class UMaterialInterface;

UCLASS()
class AUTOMATIONGAME_API ATutorialDirector : public AActor {
    GENERATED_BODY()
public:
    ATutorialDirector();
    virtual void BeginPlay() override;
    virtual void Tick(float DeltaTime) override;

    UPROPERTY(EditAnywhere, Category = "Refs")
    AGameTickManager *TickManager = nullptr;
    
    UFUNCTION(BlueprintCallable, Category = "Tutorial")
    static bool IsTutorialActive(UObject* WorldContextObject);
    
    UPROPERTY(EditAnywhere, Category = "Refs")
    AOrbSimDriver *Driver = nullptr;

    // The primary logic director that actually stores our scenario state
    // Leave blank to auto-find the first TestLevelDirector in the level
    UPROPERTY(EditAnywhere, Category = "Tutorial")
    ATestLevelDirector* TargetDirector = nullptr;

    UPROPERTY(BlueprintReadOnly, Transient, Category = "Tutorial")
    int32 CurrentStepIndex = 0;

    UPROPERTY(BlueprintReadOnly, Transient, Category = "Tutorial")
    float CurrentStepTimeRemaining = 0.0f;

    UPROPERTY(BlueprintReadOnly, Transient, Category = "Tutorial")
    float StepCompletionDelayRemaining = 0.0f;

    UPROPERTY(BlueprintReadOnly, Transient, Category = "Tutorial")
    bool bRequirementsMet = false;

    UFUNCTION(BlueprintCallable, Category = "Tutorial")
    void AdvanceStep();

    UFUNCTION(BlueprintCallable, Category = "Tutorial")
    void CheckCurrentStepRequirements();

    // --- Capture Glyphs Flow ---
    // (Moved to TestLevelDirector)



    // Editor-only visualization of the target hex
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Visuals")
    class UInstancedStaticMeshComponent* HighlightISMC;

    // Use a specific highlight mesh
    UPROPERTY(EditAnywhere, Category = "Visuals")
    UStaticMesh* HighlightStaticMesh;

    // Material to apply to the highlight mesh
    UPROPERTY(EditAnywhere, Category = "Visuals")
    UMaterialInterface* HighlightMaterial;

    UPROPERTY(Transient)
    TArray<class UInstancedStaticMeshComponent*> PreviewISMCs;

private:
    void AutoFindOrSpawn();
    void UpdateHighlight();
    class UInstancedStaticMeshComponent* GetPreviewISMC(class UStaticMesh* Mesh, bool bIsGreen);
    class UInstancedStaticMeshComponent* GetRingHighlightISMC(bool bIsGreen);
    ATestLevelDirector* GetOrSpawnTargetDirector();
};
