#pragma once
#include "AutomationGameMinimal.h"
#include "GameFramework/Actor.h"
#include "Data/OrbMaterialManager.h"
#include "OrbMaterialManagerActor.generated.h"

/** Actor that holds the centralized OrbMaterialManager component */
UCLASS(BlueprintType)
class AUTOMATIONGAME_API AOrbMaterialManagerActor : public AActor
{
	GENERATED_BODY()

public:
	AOrbMaterialManagerActor();

	/** Get the material manager component */
	UFUNCTION(BlueprintPure, Category = "Orb Materials")
	UOrbMaterialManager* GetMaterialManager() const { return MaterialManager; }

protected:
	/** The centralized material manager component */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Orb Materials")
	UOrbMaterialManager* MaterialManager = nullptr;
};
