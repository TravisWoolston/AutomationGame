#include "Actors/OrbMaterialManagerActor.h"
#include "Components/SceneComponent.h"

AOrbMaterialManagerActor::AOrbMaterialManagerActor()
{
	PrimaryActorTick.bCanEverTick = false;

	// Create root component
	USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);

	// Create the material manager component
	MaterialManager = CreateDefaultSubobject<UOrbMaterialManager>(TEXT("OrbMaterialManager"));
}
