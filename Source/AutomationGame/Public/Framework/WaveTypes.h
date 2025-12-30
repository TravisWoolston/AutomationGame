#pragma once

#include "CoreMinimal.h"
#include "WaveTypes.generated.h"

/** 
 * Represents a single active wave instance on the grid.
 * Renamed from FWaveInstance to FGridWaveInstance to avoid conflict with Engine's FWaveInstance.
 */
USTRUCT(BlueprintType)
struct FGridWaveInstance
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wave")
	FVector WorldPosition = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wave")
	float StartTime = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wave")
	float LifeTime = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wave")
	float MaxHeight = 30.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wave")
	float Radius = 100.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wave")
	float Falloff = 50.0f;
};
