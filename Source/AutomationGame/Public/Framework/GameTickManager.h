#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Framework/FixedStepListener.h"
#include "GameTickManager.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FFixedTickSignature, float, StepSeconds);

/** Central fixed-timestep driver. Place one in the level. */
UCLASS(BlueprintType)
class AUTOMATIONGAME_API AGameTickManager : public AActor
{
    GENERATED_BODY()
public:
    AGameTickManager();

    /** Static accessor: find the first manager in the world. */
    UFUNCTION(BlueprintCallable, Category="FixedStep", meta=(WorldContext="WorldContextObject"))
    static AGameTickManager* Get(UObject* WorldContextObject);

    /** Steps per second for the simulation (when playing). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="FixedStep")
    float StepsPerSecond = 10.f;

    /** If true, accumulate World Delta Seconds; otherwise use StepCountPerFrame when playing. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="FixedStep")
    bool bUseWorldDelta = true;

    /** If !bUseWorldDelta, execute this many steps per frame while playing. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="FixedStep", meta=(EditCondition="!bUseWorldDelta"))
    int32 StepCountPerFrame = 1;

    /** Auto-discover listeners at BeginPlay (actors implementing UFixedStepListener). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="FixedStep")
    bool bAutoDiscoverListeners = true;

    /** Optional explicit listeners (actors that implement UFixedStepListener). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="FixedStep")
    TArray<TObjectPtr<AActor>> ExplicitListeners;

    /** Begin ticking the fixed-step loop. */
    UFUNCTION(BlueprintCallable, Category="FixedStep")
    void Play();

    /** Pause the fixed-step loop. */
    UFUNCTION(BlueprintCallable, Category="FixedStep")
    void Pause();

    /** Stop (pause + reset accumulator). */
    UFUNCTION(BlueprintCallable, Category="FixedStep")
    void Stop();

    /** Execute exactly N steps immediately (even if paused). */
    UFUNCTION(BlueprintCallable, Category="FixedStep")
    void StepTicks(int32 NumSteps);

    /** Broadcast every fixed step. */
    UPROPERTY(BlueprintAssignable, Category="FixedStep")
    FFixedTickSignature OnFixedTick;

protected:
    virtual void BeginPlay() override;
    virtual void Tick(float DeltaSeconds) override;

private:
    bool bPlaying = true;
    float Accum = 0.f;
    float StepSeconds = 0.1f;

    /** Cached discovered listeners (actors implementing the interface). */
    UPROPERTY() TArray<TWeakObjectPtr<AActor>> Listeners;

    void DoOneStep();
    void DiscoverListeners();
    static bool ImplementsFixedStep(AActor* A);
};
