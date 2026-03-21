#include "Framework/TutorialDirector.h"
#include "Framework/GameTickManager.h"
#include "Actors/OrbSimDriver.h"
#include "Sim/SimWorldHex.h"
#include "Engine/World.h"
#include "Components/StaticMeshComponent.h"
#include "Materials/MaterialInterface.h"
#include "EngineUtils.h"

#if WITH_EDITOR
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "Editor/UnrealEdEngine.h"
#endif

ATutorialDirector::ATutorialDirector() {
    PrimaryActorTick.bCanEverTick = true;

    HighlightMeshComponent = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("HighlightMeshComponent"));
    RootComponent = HighlightMeshComponent;
    HighlightMeshComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    HighlightMeshComponent->SetVisibility(false);
}

void ATutorialDirector::BeginPlay() {
    Super::BeginPlay();
    AutoFindOrSpawn();

    CurrentStepIndex = 0;
    
    ATestLevelDirector* TLD = GetOrSpawnTargetDirector();
    if (TLD && TLD->Scenarios.IsValidIndex(TLD->ActiveScenarioIndex)) {
        if (TLD->Scenarios[TLD->ActiveScenarioIndex].TutorialSteps.Num() > 0) {
            CurrentStepTimeRemaining = TLD->Scenarios[TLD->ActiveScenarioIndex].TutorialSteps[0].TextTimer;
        }
    }
    
    UpdateHighlight();
}

void ATutorialDirector::Tick(float DeltaTime) {
    Super::Tick(DeltaTime);

    ATestLevelDirector* TLD = GetOrSpawnTargetDirector();
    if (Driver && Driver->GetSim() && TLD && TLD->Scenarios.IsValidIndex(TLD->ActiveScenarioIndex)) {
        if (CurrentStepIndex < TLD->Scenarios[TLD->ActiveScenarioIndex].TutorialSteps.Num()) {
            
            if (CurrentStepTimeRemaining > 0.0f) {
                CurrentStepTimeRemaining -= DeltaTime;
                if (CurrentStepTimeRemaining <= 0.0f) {
                    AdvanceStep();
                }
            } else {
                FTutorialStep& CurrentStep = TLD->Scenarios[TLD->ActiveScenarioIndex].TutorialSteps[CurrentStepIndex];
                if (CurrentStep.TextTimer <= 0.0f) {
                    CheckCurrentStepRequirements();
                }
            }
        }
    }
}

ATestLevelDirector* ATutorialDirector::GetOrSpawnTargetDirector() {
    if (TargetDirector && IsValid(TargetDirector)) {
        return TargetDirector;
    }

    UWorld* W = GetWorld();
    if (!W) return nullptr;

    // See if there's already one
    for (TActorIterator<ATestLevelDirector> It(W); It; ++It) {
        TargetDirector = *It;
        break;
    }

    // If not, spawn it
    if (!TargetDirector) {
        TargetDirector = W->SpawnActor<ATestLevelDirector>();
    }

    return TargetDirector;
}

void ATutorialDirector::AutoFindOrSpawn() {
    UWorld *W = GetWorld();
    if (!W) return;

    if (!TickManager) {
        for (TActorIterator<AGameTickManager> It(W); It; ++It) {
            TickManager = *It;
            break;
        }
    }

    if (!Driver) {
        for (TActorIterator<AOrbSimDriver> It(W); It; ++It) {
            Driver = *It;
            break;
        }
    }
}

void ATutorialDirector::AdvanceStep() {
    ATestLevelDirector* TLD = GetOrSpawnTargetDirector();
    if (!TLD || !TLD->Scenarios.IsValidIndex(TLD->ActiveScenarioIndex)) return;

    FScenarioSpec& Scenario = TLD->Scenarios[TLD->ActiveScenarioIndex];
    if (CurrentStepIndex < Scenario.TutorialSteps.Num() - 1) {
        CurrentStepIndex++;
        
        FTutorialStep& Step = Scenario.TutorialSteps[CurrentStepIndex];
        CurrentStepTimeRemaining = Step.TextTimer;
        
        UE_LOG(LogTemp, Log, TEXT("Tutorial Advance to Step %d: %s"), CurrentStepIndex, *Step.Message);
        
        UpdateHighlight();
    } else {
        UE_LOG(LogTemp, Log, TEXT("Tutorial Completed!"));
        CurrentStepIndex = Scenario.TutorialSteps.Num(); // done
        CurrentStepTimeRemaining = 0.0f;
        HighlightMeshComponent->SetVisibility(false);
    }
}

void ATutorialDirector::CheckCurrentStepRequirements() {
    ATestLevelDirector* TLD = GetOrSpawnTargetDirector();
    if (!Driver || !Driver->GetSim() || !TLD || !TLD->Scenarios.IsValidIndex(TLD->ActiveScenarioIndex)) return;

    FScenarioSpec& Scenario = TLD->Scenarios[TLD->ActiveScenarioIndex];
    if (CurrentStepIndex >= Scenario.TutorialSteps.Num()) return;

    FTutorialStep& CurrentStep = Scenario.TutorialSteps[CurrentStepIndex];
    bool bTileConditionMet = !CurrentStep.bRequiresTilePlacement;
    bool bArmConditionMet = !CurrentStep.bRequiresArmPlacement;

    USimWorldHex* Sim = Driver->GetSim();

    if (CurrentStep.bRequiresTilePlacement) {
        if (const FTile* Tile = Sim->GetTiles().Find(CurrentStep.TargetHex)) {
            if (Tile->Kind == CurrentStep.RequiredTileKind) {
                if (CurrentStep.bRequiresDirection) {
                    if (Tile->Arg == CurrentStep.RequiredDirection) {
                        bTileConditionMet = true;
                    }
                } else {
                    bTileConditionMet = true;
                }
            }
        }
    }

    if (CurrentStep.bRequiresArmPlacement) {
        for (const FArmState& Arm : Sim->GetArms()) {
            if (Arm.BaseHex.Q == CurrentStep.TargetHex.Q && 
                Arm.BaseHex.R == CurrentStep.TargetHex.R && 
                Arm.BaseHex.Level == CurrentStep.TargetHex.Level) {
                
                if (static_cast<uint8>(Arm.Facing) == CurrentStep.RequiredArmFacing) {
                    bArmConditionMet = true;
                    break;
                }
            }
        }
    }

    if (bTileConditionMet && bArmConditionMet && (CurrentStep.bRequiresTilePlacement || CurrentStep.bRequiresArmPlacement)) {
        AdvanceStep();
    }
}

void ATutorialDirector::CaptureGlyphs() {
    ATestLevelDirector* TLD = GetOrSpawnTargetDirector();
    if (bIsCapturingGlyphs || !TLD || !TLD->Scenarios.IsValidIndex(TLD->ActiveScenarioIndex)) return;
    
    // Force save the current state so we know our baseline
    TLD->CommitPreviewEdits();
    
    CaptureBaseScenario = TLD->Scenarios[TLD->ActiveScenarioIndex];
    bIsCapturingGlyphs = true;

    UE_LOG(LogTemp, Log, TEXT("Started capturing glyphs..."));
}

void ATutorialDirector::EndCapture() {
    ATestLevelDirector* TLD = GetOrSpawnTargetDirector();
    if (!bIsCapturingGlyphs || !TLD || !TLD->Scenarios.IsValidIndex(TLD->ActiveScenarioIndex)) return;
    
    // Save current world into active scenario temporarily
    TLD->CommitPreviewEdits();
    
    FScenarioSpec NewSpec = TLD->Scenarios[TLD->ActiveScenarioIndex];
    
    // Diff tiles
    TMap<FHex, FPlacedTileSpec> OldTiles;
    for (const FPlacedTileSpec& OldT : CaptureBaseScenario.Tiles) {
        OldTiles.Add(OldT.Hex, OldT);
    }
    
    for (const FPlacedTileSpec& NewT : NewSpec.Tiles) {
        bool bIsNewOrChanged = false;
        if (!OldTiles.Contains(NewT.Hex)) {
            bIsNewOrChanged = true;
        } else if (OldTiles[NewT.Hex].Kind != NewT.Kind || OldTiles[NewT.Hex].Dir != NewT.Dir) {
            bIsNewOrChanged = true;
        }
        
        if (bIsNewOrChanged) {
            FTutorialStep NewStep;
            NewStep.Message = FString::Printf(TEXT("Place the missing tile at %d, %d"), NewT.Hex.Q, NewT.Hex.R);
            NewStep.bRequiresTilePlacement = true;
            NewStep.TargetHex = NewT.Hex;
            NewStep.RequiredTileKind = NewT.Kind;
            
            if (NewT.Kind == ETileKind::Belt || NewT.Kind == ETileKind::Bond || NewT.Kind == ETileKind::Unbond) {
                NewStep.bRequiresDirection = true;
                NewStep.RequiredDirection = NewT.Dir;
            }
            
            CaptureBaseScenario.TutorialSteps.Add(NewStep);
        }
    }
    
    // Diff arms
    TMap<FHex, FArmSpawnSpec> OldArms;
    for (const FArmSpawnSpec& OldA : CaptureBaseScenario.Arms) {
        OldArms.Add(FHex(OldA.Q, OldA.R, OldA.Level), OldA);
    }
    
    for (const FArmSpawnSpec& NewA : NewSpec.Arms) {
        FHex ArmHex = FHex(NewA.Q, NewA.R, NewA.Level);
        bool bIsNewOrChanged = false;
        
        if (!OldArms.Contains(ArmHex)) {
            bIsNewOrChanged = true;
        } else if (OldArms[ArmHex].Facing != NewA.Facing) {
            bIsNewOrChanged = true;
        }
        
        if (bIsNewOrChanged) {
            FTutorialStep NewStep;
            NewStep.Message = FString::Printf(TEXT("Place an arm at %d, %d facing direction %d"), NewA.Q, NewA.R, NewA.Facing);
            NewStep.bRequiresArmPlacement = true;
            NewStep.TargetHex = ArmHex;
            NewStep.RequiredArmFacing = NewA.Facing;
            CaptureBaseScenario.TutorialSteps.Add(NewStep);
        }
    }
    
    // Revert the level back to the state BEFORE capture, but importantly keep the new TutorialSteps we just generated
    TLD->Scenarios[TLD->ActiveScenarioIndex] = CaptureBaseScenario;
    TLD->BuildActiveScenario();
    
    bIsCapturingGlyphs = false;
    UE_LOG(LogTemp, Log, TEXT("Finished capturing glyphs. Captured steps added and simulated tiles removed."));

#if WITH_EDITOR
    // Ensure TLD executes its saving logic to explicitly mirror if necessary,
    // though BuildActiveScenario combined with package dirty handles most of it.
    if (UPackage *Pkg = TLD->GetOutermost()) {
        Pkg->SetDirtyFlag(true);
    }
#endif
}

void ATutorialDirector::ForceSaveTutorialBaseScenario() {
    ATestLevelDirector* TLD = GetOrSpawnTargetDirector();
    if (!TLD || !TLD->Scenarios.IsValidIndex(TLD->ActiveScenarioIndex)) return;
    
    TLD->CommitPreviewEdits();
    
#if WITH_EDITOR
    if (GetWorld() && GetWorld()->IsPlayInEditor()) {
        UEditorEngine *EE = Cast<UEditorEngine>(GEngine);
        if (EE) {
            UWorld *EditorWorld = EE->GetEditorWorldContext().World();
            if (EditorWorld) {
                for (TActorIterator<ATestLevelDirector> It(EditorWorld); It; ++It) {
                    if (It->Scenarios.IsValidIndex(TLD->ActiveScenarioIndex)) {
                        It->Scenarios[TLD->ActiveScenarioIndex] = TLD->Scenarios[TLD->ActiveScenarioIndex];
                        It->MarkActiveScenarioDirty();
                        UE_LOG(LogTemp, Log, TEXT("Force-saved tutorial scenario back to Editor World."));
                    }
                    break;
                }
            }
        }
    } else {
        UE_LOG(LogTemp, Log, TEXT("Saved tutorial base scenario (already in editor)."));
    }
    
    if (UPackage *Pkg = TLD->GetOutermost()) {
        Pkg->SetDirtyFlag(true);
    }
#endif
}

void ATutorialDirector::UpdateHighlight() {
    ATestLevelDirector* TLD = GetOrSpawnTargetDirector();
    if (!TLD || !TLD->Scenarios.IsValidIndex(TLD->ActiveScenarioIndex)) {
        HighlightMeshComponent->SetVisibility(false);
        return;
    }

    FScenarioSpec& Scenario = TLD->Scenarios[TLD->ActiveScenarioIndex];
    if (CurrentStepIndex >= Scenario.TutorialSteps.Num()) {
        HighlightMeshComponent->SetVisibility(false);
        return;
    }

    FTutorialStep& CurrentStep = Scenario.TutorialSteps[CurrentStepIndex];
    if (Driver && (CurrentStep.bRequiresTilePlacement || CurrentStep.bRequiresArmPlacement)) {
        FVector StepLocation = Driver->MapHexToWorld(CurrentStep.TargetHex);
        HighlightMeshComponent->SetWorldLocation(StepLocation);
        HighlightMeshComponent->SetVisibility(true);
        if (HighlightStaticMesh) {
            HighlightMeshComponent->SetStaticMesh(HighlightStaticMesh);
        }
        if (HighlightMaterial) {
            HighlightMeshComponent->SetMaterial(0, HighlightMaterial);
        }
    } else {
        HighlightMeshComponent->SetVisibility(false);
    }
}
