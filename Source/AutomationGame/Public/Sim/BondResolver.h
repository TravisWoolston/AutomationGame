#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "Sim/HexTypes.h"
#include "BondResolver.generated.h"

class USimWorldHex;
class UOrbSpatialManager;

UCLASS()
class AUTOMATIONGAME_API UBondResolver : public UObject
{
	GENERATED_BODY()

public:
	void Initialize(USimWorldHex* InSimWorld, UOrbSpatialManager* InSpatialManager);

	// Simplified interface for now
	void ProcessAllBonds();

private:
	TWeakObjectPtr<USimWorldHex> SimWorld;
	TWeakObjectPtr<UOrbSpatialManager> SpatialManager;
};
