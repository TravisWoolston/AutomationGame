#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "TutorialWidget.generated.h"

class UTextBlock;
class ATutorialDirector;

/**
 * Native logic class for the Tutorial UI
 * Automatically polls the TutorialDirector and updates the bound text block.
 */
UCLASS(BlueprintType, Blueprintable)
class AUTOMATIONGAME_API UTutorialWidget : public UUserWidget
{
    GENERATED_BODY()

public:
    virtual void NativeConstruct() override;
    virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

protected:
    /** The actual text component in your UMG hierarchy that displays the tutorial message */
    UPROPERTY(BlueprintReadOnly, meta = (BindWidget))
    UTextBlock* MessageText = nullptr;

private:
    UPROPERTY(Transient)
    ATutorialDirector* CachedDirector = nullptr;
    
    float FindDirectorTimer = 0.0f;
};
