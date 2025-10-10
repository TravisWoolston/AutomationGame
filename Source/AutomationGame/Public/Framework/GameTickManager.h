#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "GameTickManager.generated.h"

/** Broadcasts a fixed-step "simulation tick" in seconds. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FFixedStepEvent, float, StepSeconds);

/**
 * Centralized fixed-step tick manager.
 * - Call Play/Pause/Stop/StepTicks from code or Blueprints.
 * - Bind to OnFixedTick to advance gameplay/sim in lock-step.
 */
UCLASS(BlueprintType)
class AUTOMATIONGAME_API AGameTickManager : public AActor
{
	GENERATED_BODY()
public:
	AGameTickManager();

	// -- Controls --
	UFUNCTION(BlueprintCallable, Category = "Tick") void Play();
	UFUNCTION(BlueprintCallable, Category = "Tick") void Pause();
	UFUNCTION(BlueprintCallable, Category = "Tick") void Stop();
	UFUNCTION(BlueprintCallable, Category = "Tick") void StepTicks(int32 NumTicks = 1);
	UFUNCTION(BlueprintCallable, Category = "Tick") bool IsPlaying() const { return bPlaying; }
	UFUNCTION(BlueprintCallable, Category = "Tick") bool IsInStartupDelay() const { return RemainingDelayTicks > 0; }
	UFUNCTION(BlueprintCallable, Category = "Tick") int32 GetRemainingDelayTicks() const { return RemainingDelayTicks; }

	/** Helper to find a manager in the current world. */
	UFUNCTION(BlueprintCallable, Category = "Tick", meta = (WorldContext = "WorldContextObject"))
	static AGameTickManager* Get(const UObject* WorldContextObject);

	/** Fired every fixed step while playing (or when StepTicks is called). */
	UPROPERTY(BlueprintAssignable, Category = "Tick")
	FFixedStepEvent OnFixedTick;

	/** Steps per real-time second when playing. This value can be changed in the editor, so don't assume the sim will be running at this rate in real world time. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tick", meta = (ClampMin = "0.01", ClampMax = "240.0"))
	float TicksPerSecond = 2.0f;

	/** Auto-start playback on BeginPlay. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tick")
	bool bAutoPlay = false;

	/** Number of ticks to delay before starting simulation. Useful for setup/initialization. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tick", meta = (ClampMin = "0"))
	int32 StartupTickDelay = 0;

protected:
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

private:
	bool bPlaying = false;
	double Accum = 0.0;
	int32 RemainingDelayTicks = 0;

	FORCEINLINE float GetStepSeconds() const { return 1.0f / FMath::Max(0.01f, TicksPerSecond); }
};