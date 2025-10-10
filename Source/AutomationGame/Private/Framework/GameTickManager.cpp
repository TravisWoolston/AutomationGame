#include "Framework/GameTickManager.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Engine/Engine.h"

AGameTickManager::AGameTickManager()
{
    PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.bStartWithTickEnabled = true;
}

void AGameTickManager::BeginPlay()
{
    Super::BeginPlay();
    RemainingDelayTicks = StartupTickDelay;
    if (bAutoPlay) Play();
}

void AGameTickManager::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);

    //UE_LOG(LogTemp, VeryVerbose, TEXT("GameTickManager::Tick: bPlaying=%d Accum=%.3f"), bPlaying ? 1 : 0, Accum);
    if (!bPlaying) return;

    Accum += double(DeltaSeconds);
    const float Step = GetStepSeconds();
    while (Accum >= Step)
    {
        Accum -= Step;
        
        // Check if we're still in the startup delay period
        if (RemainingDelayTicks > 0)
        {
            RemainingDelayTicks--;
            UE_LOG(LogTemp, Log, TEXT("GameTickManager: Startup delay tick consumed, %d remaining"), RemainingDelayTicks);
        }
        else
        {
            // Delay period is over, broadcast the actual simulation tick
            OnFixedTick.Broadcast(Step);
            GEngine->AddOnScreenDebugMessage(-1, 0.2f, FColor::White,
                FString::Printf(TEXT("TICK")));
            //UE_LOG(LogTemp, VeryVerbose, TEXT("GameTickManager: Broadcast OnFixedTick step=%.3f"), Step);
        }
    }
}

void AGameTickManager::Play()
{
    bPlaying = true;
    RemainingDelayTicks = StartupTickDelay;
    UE_LOG(LogTemp, Log, TEXT("GameTickManager: Play() - Startup delay set to %d ticks"), RemainingDelayTicks);
}

void AGameTickManager::Pause()
{
    bPlaying = false;
    UE_LOG(LogTemp, Log, TEXT("GameTickManager: Pause()"));
}

void AGameTickManager::Stop()
{
    bPlaying = false;
    Accum = 0.0;
    UE_LOG(LogTemp, Log, TEXT("GameTickManager: Stop()"));
}

void AGameTickManager::StepTicks(int32 NumTicks)
{
    const float Step = GetStepSeconds();
    for (int32 i = 0; i < FMath::Max(1, NumTicks); ++i)
    {
        OnFixedTick.Broadcast(Step);
        UE_LOG(LogTemp, Verbose, TEXT("GameTickManager: StepTicks broadcast %d/%d step=%.3f"), i+1, FMath::Max(1, NumTicks), Step);
    }
}

AGameTickManager* AGameTickManager::Get(const UObject* WorldContextObject)
{
    if (!WorldContextObject) return nullptr;
    if (const UWorld* W = WorldContextObject->GetWorld())
    {
        for (TActorIterator<AGameTickManager> It(W); It; ++It)
        {
            return *It;
        }
    }
    return nullptr;
}
