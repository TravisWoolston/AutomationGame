#include "Framework/GameTickManager.h"
#include "Framework/FixedStepListener.h"
#include "Kismet/GameplayStatics.h"
#include "EngineUtils.h"

AGameTickManager::AGameTickManager()
{
    PrimaryActorTick.bCanEverTick = true;
}

AGameTickManager* AGameTickManager::Get(UObject* WorldContextObject)
{
    if (!WorldContextObject) return nullptr;
    UWorld* W = WorldContextObject->GetWorld();
    if (!W) return nullptr;
    for (TActorIterator<AGameTickManager> It(W); It; ++It)
    {
        return *It;
    }
    return nullptr;
}

void AGameTickManager::BeginPlay()
{
    Super::BeginPlay();
    StepSeconds = (StepsPerSecond > 0.f) ? (1.f / StepsPerSecond) : 0.1f;
    if (bAutoDiscoverListeners) DiscoverListeners();
    // Merge explicit listeners (Blueprint-friendly AActor array)
    for (AActor* A : ExplicitListeners)
    {
        if (A && ImplementsFixedStep(A)) Listeners.AddUnique(A);
    }
}

void AGameTickManager::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    if (!bPlaying || StepsPerSecond <= 0.f) return;

    if (bUseWorldDelta)
    {
        Accum += DeltaSeconds;
        while (Accum >= StepSeconds)
        {
            Accum -= StepSeconds;
            DoOneStep();
        }
    }
    else
    {
        for (int32 i=0; i<StepCountPerFrame; ++i)
        {
            DoOneStep();
        }
    }
}

void AGameTickManager::Play()
{
    bPlaying = true;
}

void AGameTickManager::Pause()
{
    bPlaying = false;
}

void AGameTickManager::Stop()
{
    bPlaying = false;
    Accum = 0.f;
}

void AGameTickManager::StepTicks(int32 NumSteps)
{
    if (NumSteps <= 0) return;
    for (int32 i=0; i<NumSteps; ++i) DoOneStep();
}

void AGameTickManager::DoOneStep()
{
    // Cull dead
    for (int32 i=Listeners.Num()-1; i>=0; --i)
    {
        if (!Listeners[i].IsValid())
        {
            Listeners.RemoveAtSwap(i);
        }
    }

    // Call interface on each live listener
    for (int32 i=0; i<Listeners.Num(); ++i)
    {
        if (AActor* A = Listeners[i].Get())
        {
            if (ImplementsFixedStep(A))
            {
                IFixedStepListener::Execute_OnFixedStep(A, StepSeconds);
            }
        }
    }

    // Editor/Blueprint hook
    OnFixedTick.Broadcast(StepSeconds);
}

bool AGameTickManager::ImplementsFixedStep(AActor* A)
{
    return A && A->GetClass()->ImplementsInterface(UFixedStepListener::StaticClass());
}

void AGameTickManager::DiscoverListeners()
{
    UWorld* World = GetWorld();
    if (!World) return;
    for (TActorIterator<AActor> It(World); It; ++It)
    {
        AActor* A = *It;
        if (ImplementsFixedStep(A))
        {
            Listeners.AddUnique(A);
        }
    }
}
