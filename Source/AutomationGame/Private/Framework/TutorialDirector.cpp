#include "Framework/TutorialDirector.h"
#include "Framework/GameTickManager.h"
#include "Actors/OrbSimDriver.h"
#include "Sim/SimWorldHex.h"
#include "Sim/HexTypes.h"
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
    HighlightISMC->SetRenderCustomDepth(true);
    HighlightISMC->SetCustomDepthStencilValue(1);
    HighlightISMC->SetRenderInMainPass(false); // Only render the outline
    HighlightISMC->SetVisibility(false);
}

void ATutorialDirector::BeginPlay() {
    Super::BeginPlay();
    AutoFindOrSpawn();

    // Bind to GTM tick events
    if (TickManager) {
        TickManager->OnFixedTick.AddDynamic(this, &ATutorialDirector::OnGTMTick);
    }

    CurrentStepIndex = 0;
    
    ATestLevelDirector* TLD = GetOrSpawnTargetDirector();
    if (TLD && TLD->Scenarios.IsValidIndex(TLD->ActiveScenarioIndex)) {
        if (TLD->Scenarios[TLD->ActiveScenarioIndex].TutorialSteps.Num() > 0) {
            FTutorialStep& FirstStep = TLD->Scenarios[TLD->ActiveScenarioIndex].TutorialSteps[0];
            CurrentStepTimeRemaining = FirstStep.TextTimer;
            CurrentStepGTMStepsRemaining = FirstStep.GTMStepTimer;
            
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
            
            if (bRequirementsMet) {
                StepCompletionDelayRemaining -= DeltaTime;
                if (StepCompletionDelayRemaining <= 0.0f) {
                    bRequirementsMet = false;
                    AdvanceStep();
                }
            } else {
                FTutorialStep& CurrentStep = TLD->Scenarios[TLD->ActiveScenarioIndex].TutorialSteps[CurrentStepIndex];

                // Count down real-time timer
                if (CurrentStepTimeRemaining > 0.0f) {
                    CurrentStepTimeRemaining -= DeltaTime;
                }

                // Determine if each timer minimum has been met
                bool bHasTextTimer = CurrentStep.TextTimer > 0.0f;
                bool bHasGTMTimer  = CurrentStep.GTMStepTimer > 0;
                bool bTextTimerDone = !bHasTextTimer || CurrentStepTimeRemaining <= 0.0f;
                bool bGTMTimerDone  = !bHasGTMTimer  || CurrentStepGTMStepsRemaining <= 0;
                bool bTimersDone    = bTextTimerDone && bGTMTimerDone;
                bool bHasAnyTimer   = bHasTextTimer || bHasGTMTimer;

                bool bHasRequirements = CurrentStep.bRequiresTilePlacement || CurrentStep.bRequiresArmPlacement ||
                                        CurrentStep.bShouldPauseGTM || CurrentStep.RequiredTiles.Num() > 0 ||
                                        CurrentStep.RequiredArms.Num() > 0;

                if (bHasAnyTimer && !bTimersDone) {
                    // Still waiting on timer minimums
                    // If there are also requirements, keep checking them so highlights update
                    if (bHasRequirements) {
                        CheckCurrentStepRequirements();
                        UpdateHighlight();
                        // But block advancement even if requirements are met - timer not done
                        bRequirementsMet = false;
                    }
                } else if (bHasRequirements) {
                    // Timers satisfied (or none set), check requirements
                    CheckCurrentStepRequirements();
                    UpdateHighlight();
                } else if (bHasAnyTimer && bTimersDone) {
                    // Timers satisfied, no requirements - advance
                    AdvanceStep();
                }
                // else: no timers, no requirements - wait for external advance
            }
        }
    }
}

void ATutorialDirector::OnGTMTick(float StepSeconds) {
    if (CurrentStepGTMStepsRemaining > 0) {
        CurrentStepGTMStepsRemaining--;
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
        bRequirementsMet = false;
        
        FTutorialStep& Step = Scenario.TutorialSteps[CurrentStepIndex];
        CurrentStepTimeRemaining = Step.TextTimer;
        CurrentStepGTMStepsRemaining = Step.GTMStepTimer;
        
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
        if (!bRequirementsMet) {
            bRequirementsMet = true;
            StepCompletionDelayRemaining = 1.0f; // 1 second delay
        }
    }
}



#include "Actors/HexFactoryActor.h"

// ... existing includes ...

UInstancedStaticMeshComponent* ATutorialDirector::GetPreviewISMC(UStaticMesh* Mesh, bool bIsGreen) {
    if (!Mesh) return nullptr;
    
    int32 StencilValue = bIsGreen ? 3 : 2;
    FString CompName = FString::Printf(TEXT("PreviewISMC_%s_%s"), *Mesh->GetName(), bIsGreen ? TEXT("Green") : TEXT("Red"));
    
    // Find existing
    for (UInstancedStaticMeshComponent* Comp : PreviewISMCs) {
        if (Comp && Comp->GetStaticMesh() == Mesh && Comp->CustomDepthStencilValue == StencilValue) {
            return Comp;
        }
    }
    
    UInstancedStaticMeshComponent* NewISMC = NewObject<UInstancedStaticMeshComponent>(this, FName(*CompName));
    NewISMC->SetupAttachment(RootComponent);
    NewISMC->SetStaticMesh(Mesh);
    
    // Attempt to use a translucent version of the material or default highlight material
    if (HighlightMaterial) {
        NewISMC->SetMaterial(0, HighlightMaterial);
    }
    
    NewISMC->SetRenderCustomDepth(true);
    NewISMC->SetCustomDepthStencilValue(StencilValue);
    NewISMC->SetRenderInMainPass(false); // Only render the outline
    NewISMC->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    NewISMC->RegisterComponent();
    
    PreviewISMCs.Add(NewISMC);
    return NewISMC;
}

UInstancedStaticMeshComponent* ATutorialDirector::GetRingHighlightISMC(bool bIsGreen) {
    if (!HighlightStaticMesh) return nullptr;
    
    int32 StencilValue = bIsGreen ? 3 : 2;
    
    // Check if the base HighlightISMC matches (first time use)
    if (HighlightISMC->GetStaticMesh() == HighlightStaticMesh && HighlightISMC->CustomDepthStencilValue == StencilValue) {
        return HighlightISMC;
    }

    // Search additional ring comps or create
    return GetPreviewISMC(HighlightStaticMesh, bIsGreen);
}

void ATutorialDirector::UpdateHighlight() {
    ATestLevelDirector* TLD = GetOrSpawnTargetDirector();
    if (!TLD) return;

    // 1. Force the simulation to sync
    if (!Driver) {
        for (TActorIterator<AOrbSimDriver> It(GetWorld()); It; ++It) {
            Driver = *It;
            break;
        }
    }
    USimWorldHex* Sim = Driver ? Driver->GetSim() : nullptr;

    if (!TLD->Scenarios.IsValidIndex(TLD->ActiveScenarioIndex)) return;
    FScenarioSpec& Scenario = TLD->Scenarios[TLD->ActiveScenarioIndex];
    if (CurrentStepIndex >= Scenario.TutorialSteps.Num()) {
        HighlightISMC->ClearInstances();
        for (auto* Comp : PreviewISMCs) if (Comp) Comp->ClearInstances();
        return;
    }

    FTutorialStep& CurrentStep = Scenario.TutorialSteps[CurrentStepIndex];
    
    // Clear EVERYTHING before drawing this frame
    HighlightISMC->ClearInstances();
    for (auto* Comp : PreviewISMCs) if (Comp) Comp->ClearInstances();

    if (!Driver || !Sim) {
        // This is a major error case - we need to see this in your log!
        UE_LOG(LogTemp, Error, TEXT("TUTORIAL DIRECTOR: Simulation Driver or Sim is missing!"));
        return;
    }

    AHexFactoryActor* Factory = AHexFactoryActor::FindInWorld(GetWorld());
    
    auto AddISMCInstance = [&](UInstancedStaticMeshComponent* ISMC, FHex Hex, int32 Dir, FVector MeshScale = FVector(1.0f), bool bAtMidpoint = false) {
        if (!ISMC) return;
        FVector PosA = Driver->MapHexToWorld(Hex);
        FHex Neighbor = HexUtil::Neighbor(Hex, (EHexDir)(Dir % 6));
        FVector PosB = Driver->MapHexToWorld(Neighbor);
        FVector DirVec = (PosB - PosA).GetSafeNormal();
        float Yaw = DirVec.IsNearlyZero() ? 0.f : FMath::RadiansToDegrees(FMath::Atan2(DirVec.Y, DirVec.X));
        
        FVector FinalPos = bAtMidpoint ? (PosA + PosB) * 0.5f : PosA;
        ISMC->AddInstance(FTransform(FRotator(0.0f, Yaw, 0.0f), FinalPos, MeshScale));
        ISMC->SetVisibility(true);
    };

    // 2. REQUIRED TILES CHECK
    for (const FPlacedTileSpec& ReqTile : CurrentStep.RequiredTiles) {
        const FTile* ActualTile = Sim->GetTiles().Find(ReqTile.Hex);
        
        bool bIsCorrect = false;
        if (ActualTile) {
            if (ActualTile->Kind == ReqTile.Kind) {
                // If it's a belt/bond, we require direction match. 
                // Empty tiles or simple tiles might not.
                if (ReqTile.Kind == ETileKind::Empty || ActualTile->Arg == ReqTile.Dir) {
                    bIsCorrect = true;
                } else {
                    // LOG THE MISMATCH
                    UE_LOG(LogTemp, Warning, TEXT("TUTORIAL MISMATCH at (%d,%d): Placed Dir %d, Required Dir %d"), 
                           ReqTile.Hex.Q, ReqTile.Hex.R, (int32)ActualTile->Arg, (int32)ReqTile.Dir);
                }
            } else {
                 UE_LOG(LogTemp, Warning, TEXT("TUTORIAL KIND MISMATCH at (%d,%d): Placed Kind %d, Required Kind %d"), 
                        ReqTile.Hex.Q, ReqTile.Hex.R, (int32)ActualTile->Kind, (int32)ReqTile.Kind);
            }
        } 
        // else {
        //     // LOG MISSING TILE
        //     UE_LOG(LogTemp, Log, TEXT("TUTORIAL: Missing tile required at (%d,%d) Level %d"), ReqTile.Hex.Q, ReqTile.Hex.R, ReqTile.Hex.Level);
        // }

        UStaticMesh* MeshToUse = HighlightStaticMesh;
        FVector Scale = FVector(1.0f);
        if (Factory) {
            if (ReqTile.Kind == ETileKind::Belt) { MeshToUse = Factory->BeltMesh; Scale = Factory->BeltScale; }
            else if (ReqTile.Kind == ETileKind::Bond) { MeshToUse = Factory->BondMesh; Scale = Factory->BondScale; }
            else if (ReqTile.Kind == ETileKind::Unbond) { MeshToUse = Factory->UnbondMesh; Scale = Factory->UnbondScale; }
            else { MeshToUse = Factory->TileMesh; }
        }

        // Always show the outline/preview using the actual mesh kind
        UInstancedStaticMeshComponent* PreviewISMC = GetPreviewISMC(MeshToUse, bIsCorrect);
        if (PreviewISMC) {
            bool bMidpoint = (ReqTile.Kind == ETileKind::Bond || ReqTile.Kind == ETileKind::Unbond);
            AddISMCInstance(PreviewISMC, ReqTile.Hex, ReqTile.Dir, Scale, bMidpoint);
        }
    }

    // 3. TARGET HEX (Main instruction)
    if (CurrentStep.bRequiresTilePlacement || CurrentStep.bRequiresArmPlacement) {
        bool bIsCorrect = false;
        int32 Dir = CurrentStep.bRequiresDirection ? CurrentStep.RequiredDirection : 0;
        
        if (CurrentStep.bRequiresTilePlacement) {
            if (const FTile* Tile = Sim->GetTiles().Find(CurrentStep.TargetHex)) {
                if (Tile->Kind == CurrentStep.RequiredTileKind) {
                    if (!CurrentStep.bRequiresDirection || Tile->Arg == CurrentStep.RequiredDirection) {
                        bIsCorrect = true;
                    }
                }
            }
        } else if (CurrentStep.bRequiresArmPlacement) {
            Dir = CurrentStep.RequiredArmFacing;
            for (const FArmState& Arm : Sim->GetArms()) {
                if (Arm.BaseHex == CurrentStep.TargetHex && static_cast<uint8>(Arm.Facing) == CurrentStep.RequiredArmFacing) {
                    bIsCorrect = true;
                    break;
                }
            }
        }

        UInstancedStaticMeshComponent* RingISMC = GetRingHighlightISMC(bIsCorrect);
        bool bGoalMidpoint = (CurrentStep.bRequiresTilePlacement && (CurrentStep.RequiredTileKind == ETileKind::Bond || CurrentStep.RequiredTileKind == ETileKind::Unbond));
        AddISMCInstance(RingISMC, CurrentStep.TargetHex, Dir, FVector(1.0f), bGoalMidpoint);
    }
}

bool ATutorialDirector::IsTutorialActive(UObject* WorldContextObject) {
    if (!WorldContextObject) return false;
    UWorld* World = WorldContextObject->GetWorld();
    if (!World) return false;

    for (TActorIterator<ATutorialDirector> It(World); It; ++It) {
        if (ATestLevelDirector* TLD = (*It)->GetOrSpawnTargetDirector()) {
            if (TLD->Scenarios.IsValidIndex(TLD->ActiveScenarioIndex)) {
                return true;
            }
        }
    }
    return false;
}
