#include "UI/TutorialWidget.h"
#include "Components/TextBlock.h"
#include "Framework/TutorialDirector.h"
#include "Framework/TestLevelDirector.h"
#include "EngineUtils.h"

void UTutorialWidget::NativeConstruct() {
    Super::NativeConstruct();

    if (UWorld* World = GetWorld()) {
        for (TActorIterator<ATutorialDirector> It(World); It; ++It) {
            CachedDirector = *It;
            break;
        }
    }
}

void UTutorialWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime) {
    Super::NativeTick(MyGeometry, InDeltaTime);

    // Re-find the director periodically if it was spawned late or destroyed
    if (!CachedDirector || !IsValid(CachedDirector)) {
        FindDirectorTimer -= InDeltaTime;
        if (FindDirectorTimer <= 0.0f) {
            FindDirectorTimer = 1.0f; // Search once a second
            
            if (UWorld* World = GetWorld()) {
                for (TActorIterator<ATutorialDirector> It(World); It; ++It) {
                    CachedDirector = *It;
                    break;
                }
            }
        }
    }

    if (CachedDirector && IsValid(CachedDirector)) {
        ATestLevelDirector* TargetDir = CachedDirector->TargetDirector;

        if (TargetDir && IsValid(TargetDir) && TargetDir->Scenarios.IsValidIndex(TargetDir->ActiveScenarioIndex)) {
            const FScenarioSpec& Scenario = TargetDir->Scenarios[TargetDir->ActiveScenarioIndex];
            
            if (CachedDirector->CurrentStepIndex < Scenario.TutorialSteps.Num()) {
                const FTutorialStep& Step = Scenario.TutorialSteps[CachedDirector->CurrentStepIndex];
                
                // Update text block without allocating new FText unless it changed
                if (MessageText) {
                    FText NewText = FText::FromString(Step.Message);
                    if (!MessageText->GetText().EqualTo(NewText)) {
                        MessageText->SetText(NewText);
                    }
                }
                
                // Unhide widget since we have an active step
                if (GetVisibility() == ESlateVisibility::Collapsed || GetVisibility() == ESlateVisibility::Hidden) {
                    SetVisibility(ESlateVisibility::SelfHitTestInvisible);
                }
                return; 
            } else {
                if (MessageText) MessageText->SetText(FText::FromString(TEXT("Finished!")));
            }
        } else {
            if (MessageText) MessageText->SetText(FText::FromString(TEXT("TargetDir Invalid Scenario Index")));
        }
    } else {
        if (MessageText) MessageText->SetText(FText::FromString(TEXT("TargetDir or CachedDirector is Null")));
    }
    
    // Instead of completely hiding instantly, set text to empty after 1s, or leave diag
    // If not valid or completed
    /*if (GetVisibility() != ESlateVisibility::Collapsed) {
        SetVisibility(ESlateVisibility::Collapsed);
    }*/
}
