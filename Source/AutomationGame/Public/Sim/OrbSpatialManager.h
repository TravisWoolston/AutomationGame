#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "Sim/HexTypes.h"
#include "Sim/OrbTypes.h"
#include "OrbSpatialManager.generated.h"

class USimWorldHex;

UCLASS()
class AUTOMATIONGAME_API UOrbSpatialManager : public UObject
{
	GENERATED_BODY()

public:
	void Initialize(USimWorldHex* InSimWorld);

	// Simplified interface for now
	bool IsArmWaiting(int32 ArmIndex) const;
	bool IsArmRotating(int32 ArmIndex) const;

private:
	TWeakObjectPtr<USimWorldHex> SimWorld;
};
