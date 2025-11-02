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
	StepAlpha = 0.f;
	if (bAutoPlay) Play();
}

void AGameTickManager::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (!bPlaying) return;

	const float Step = GetStepSeconds();

	// Accumulate time and update alpha before processing steps
	Accum += double(DeltaSeconds);
	if (RemainingDelayTicks > 0)
	{
		StepAlpha = 0.f; // freeze alpha during startup delay
	}
	else
	{
		StepAlpha = FMath::Clamp(float(Accum) / Step, 0.f, 1.f);
	}

	while (Accum >= Step)
	{
		Accum -= Step;

		if (RemainingDelayTicks > 0)
		{
			RemainingDelayTicks--;
			StepAlpha = 0.f; // still in delay, keep alpha at 0
			UE_LOG(LogTemp, Log, TEXT("GameTickManager: Startup delay tick consumed, %d remaining"), RemainingDelayTicks);
		}
		else
		{
			// Start of a new fixed step: reset alpha and broadcast
			StepAlpha = 0.f;
			OnFixedTick.Broadcast(Step);

			// Optional lightweight on-screen mark
			if (GEngine && bShowTickDebug)
			{
				GEngine->AddOnScreenDebugMessage(-1, 0.2f, FColor::White, TEXT("TICK"));
			}
		}
	}

	// Update alpha for any partial frame remaining after stepping
	if (RemainingDelayTicks > 0)
	{
		StepAlpha = 0.f;
	}
	else
	{
		StepAlpha = FMath::Clamp(float(Accum) / Step, 0.f, 1.f);
	}
}

void AGameTickManager::Play()
{
	bPlaying = true;
	RemainingDelayTicks = StartupTickDelay;
	StepAlpha = 0.f;
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
	StepAlpha = 0.f;
	UE_LOG(LogTemp, Log, TEXT("GameTickManager: Stop()"));
}

void AGameTickManager::StepTicks(int32 NumTicks)
{
	const float Step = GetStepSeconds();
	for (int32 i = 0; i < FMath::Max(1, NumTicks); ++i)
	{
		// Emit a tick and reset alpha back to start of new step
		OnFixedTick.Broadcast(Step);
		StepAlpha = 0.f;
		UE_LOG(LogTemp, Verbose, TEXT("GameTickManager: StepTicks broadcast %d/%d step=%.3f"), i + 1, FMath::Max(1, NumTicks), Step);
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