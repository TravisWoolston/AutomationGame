#include "Framework/TutorialDirector.h"
#include "Framework/GameTickManager.h"
#include "Actors/OrbSimDriver.h"
#include "Sim/SimWorldHex.h"
#include "Actors/HexTopDownPawn.h"
#include "Engine/World.h"
#include "Components/StaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Materials/MaterialInterface.h"
#include "EngineUtils.h"

#if WITH_EDITOR
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "Editor/UnrealEdEngine.h"
#endif

ATutorialDirector::ATutorialDirector() {
    PrimaryActorTick.bCanEverTick = true;

    HighlightISMC = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("HighlightISMC"));
    RootComponent = HighlightISMC;
    HighlightISMC->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    HighlightISMC->SetVisibility(false);
}

void ATutorialDirector::BeginPlay() {
    Super::BeginPlay();
    AutoFindOrSpawn();

    CurrentStepIndex = 0;
    
    ATestLevelDirector* TLD = GetOrSpawnTargetDirector();
    if (TLD && TLD->Scenarios.IsValidIndex(TLD->ActiveScenarioIndex)) {
        if (TLD->Scenarios[TLD->ActiveScenarioIndex].TutorialSteps.Num() > 0) {
            FTutorialStep& FirstStep = TLD->Scenarios[TLD->ActiveScenarioIndex].TutorialSteps[0];
            CurrentStepTimeRemaining = FirstStep.TextTimer;
            
            if (FirstStep.bShouldPauseGTM && TickManager) {
                TickManager->Pause();
            }
            
            if (FirstStep.bHasFocusTile && Driver) {
                FVector TargetLoc = Driver->MapHexToWorld(FirstStep.FocusTile);
                if (APlayerController* PC = GetWorld()->GetFirstPlayerController()) {
                    if (APawn* Pawn = PC->GetPawn()) {
                        FVector CurrentLoc = Pawn->GetActorLocation();
                        TargetLoc.Z = CurrentLoc.Z;
                        if (AHexTopDownPawn* HexPawn = Cast<AHexTopDownPawn>(Pawn)) {
                            HexPawn->CameraTargetLocation = TargetLoc;
                        } else {
                            Pawn->SetActorLocation(TargetLoc);
                        }
                    }
                }
            }
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
        
        if (Step.bShouldPauseGTM && TickManager) {
            TickManager->Pause();
        }
        
        if (Step.bHasFocusTile && Driver) {
            FVector TargetLoc = Driver->MapHexToWorld(Step.FocusTile);
            if (APlayerController* PC = GetWorld()->GetFirstPlayerController()) {
                if (APawn* Pawn = PC->GetPawn()) {
                    FVector CurrentLoc = Pawn->GetActorLocation();
                    TargetLoc.Z = CurrentLoc.Z;
                    if (AHexTopDownPawn* HexPawn = Cast<AHexTopDownPawn>(Pawn)) {
                        HexPawn->CameraTargetLocation = TargetLoc;
                    } else {
                        Pawn->SetActorLocation(TargetLoc);
                    }
                }
            }
        }
        
        UpdateHighlight();
    } else {
        UE_LOG(LogTemp, Log, TEXT("Tutorial Completed!"));
        CurrentStepIndex = Scenario.TutorialSteps.Num(); // done
        CurrentStepTimeRemaining = 0.0f;
        if (HighlightISMC) {
            HighlightISMC->SetVisibility(false);
        }
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

    bool bArrayTilesMet = true;
    for (const FPlacedTileSpec& ReqTile : CurrentStep.RequiredTiles) {
        if (const FTile* Tile = Sim->GetTiles().Find(ReqTile.Hex)) {
            if (Tile->Kind != ReqTile.Kind || Tile->Arg != ReqTile.Dir) {
                bArrayTilesMet = false; break;
            }
        } else { bArrayTilesMet = false; break; }
    }

    bool bArrayArmsMet = true;
    for (const FArmSpawnSpec& ReqArm : CurrentStep.RequiredArms) {
        bool bFound = false;
        for (const FArmState& Arm : Sim->GetArms()) {
            if (Arm.BaseHex.Q == ReqArm.Q && Arm.BaseHex.R == ReqArm.R && Arm.BaseHex.Level == ReqArm.Level) {
                if (static_cast<uint8>(Arm.Facing) == ReqArm.Facing) {
                    bFound = true; break;
                }
            }
        }
        if (!bFound) { bArrayArmsMet = false; break; }
    }

    bool bNeedsAction = CurrentStep.bRequiresTilePlacement || CurrentStep.bRequiresArmPlacement || CurrentStep.bShouldPauseGTM || (CurrentStep.RequiredTiles.Num() > 0) || (CurrentStep.RequiredArms.Num() > 0);
    bool bGtmConditionMet = !CurrentStep.bShouldPauseGTM || (TickManager && TickManager->IsPlaying());

    if (bTileConditionMet && bArmConditionMet && bArrayTilesMet && bArrayArmsMet && bGtmConditionMet && bNeedsAction) {
        AdvanceStep();
    }
}



void ATutorialDirector::UpdateHighlight() {
    ATestLevelDirector* TLD = GetOrSpawnTargetDirector();
    if (!TLD || !TLD->Scenarios.IsValidIndex(TLD->ActiveScenarioIndex)) {
        HighlightISMC->SetVisibility(false);
        return;
    }

    FScenarioSpec& Scenario = TLD->Scenarios[TLD->ActiveScenarioIndex];
    if (CurrentStepIndex >= Scenario.TutorialSteps.Num()) {
        HighlightISMC->SetVisibility(false);
        return;
    }

    FTutorialStep& CurrentStep = Scenario.TutorialSteps[CurrentStepIndex];
    HighlightISMC->ClearInstances();
    
    if (HighlightStaticMesh) {
        HighlightISMC->SetStaticMesh(HighlightStaticMesh);
    }
    if (HighlightMaterial) {
        HighlightISMC->SetMaterial(0, HighlightMaterial);
    }

    if (Driver) {
        bool bAnyHighlights = false;
        
        // Helper lambda to add an instance for a hex + direction
        auto AddHighlight = [&](FHex Hex, int32 Dir) {
            FVector Pos = Driver->MapHexToWorld(Hex);
            FRotator Rot(0.0f, Dir * 60.0f, 0.0f);
            HighlightISMC->AddInstance(FTransform(Rot, Pos, FVector(1.0f)));
            bAnyHighlights = true;
        };

        if (CurrentStep.bRequiresTilePlacement || CurrentStep.bRequiresArmPlacement) {
            int32 Dir = CurrentStep.bRequiresDirection ? CurrentStep.RequiredDirection : 0;
            if (CurrentStep.bRequiresArmPlacement) Dir = CurrentStep.RequiredArmFacing;
            AddHighlight(CurrentStep.TargetHex, Dir);
        }

        for (const FPlacedTileSpec& ReqTile : CurrentStep.RequiredTiles) {
            AddHighlight(ReqTile.Hex, ReqTile.Dir);
        }

        for (const FArmSpawnSpec& ReqArm : CurrentStep.RequiredArms) {
            AddHighlight(FHex(ReqArm.Q, ReqArm.R, ReqArm.Level), ReqArm.Facing);
        }

        HighlightISMC->SetVisibility(bAnyHighlights);
    } else {
        HighlightISMC->SetVisibility(false);
    }
}
