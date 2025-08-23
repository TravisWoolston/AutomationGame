#pragma once
#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "FixedStepListener.generated.h"

UINTERFACE(BlueprintType)
class AUTOMATIONGAME_API UFixedStepListener : public UInterface
{
    GENERATED_BODY()
};

/** Any actor that wants fixed-step updates implements this. */
class AUTOMATIONGAME_API IFixedStepListener
{
    GENERATED_BODY()
public:
    /** Called exactly once per fixed-step by GameTickManager. */
    UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category="FixedStep")
    void OnFixedStep(float StepSeconds);
};