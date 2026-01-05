#pragma once

#include "CoreMinimal.h"
#include "TriggerTypes.generated.h"

// Owner type enum
UENUM(BlueprintType)
enum class ETriggerTileOwnerType : uint8
{
	Belt,
	Arm
};

// Trigger tile variant
UENUM(BlueprintType)
enum class ETriggerVariant : uint8
{
	Positive,
	Negative
};
