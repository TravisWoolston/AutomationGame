#include "Actors/GridTestHarness.h"
#include "Actors/OrbSimDriver.h"
#include "Components/HexPlacementComponent.h"
#include "Rendering/OrbRenderer.h"
#include "GameFramework/PlayerController.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Components/SceneComponent.h"

AGridTestHarness::AGridTestHarness()
{
	PrimaryActorTick.bCanEverTick = false;

	USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);

	// NOTE: UHexPlacementComponent and UOrbRenderer derive from UActorComponent (not USceneComponent),
	// so we do NOT call SetupAttachment on them.
	Placement = CreateDefaultSubobject<UHexPlacementComponent>(TEXT("HexPlacement"));
	Placement->bUseInputPolling = true;
	Placement->PlaceKind = EPlaceKind::Input;
	Placement->bDebug = true;

	Renderer = CreateDefaultSubobject<UOrbRenderer>(TEXT("OrbRenderer"));
}

void AGridTestHarness::BeginPlay()
{
	Super::BeginPlay();

	UWorld* W = GetWorld();
	if (!W) return;

	AOrbSimDriver* Driver = AOrbSimDriver::Find(W);
	if (!Driver && bAutoSpawnDriver)
	{
		FActorSpawnParameters SP; SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		Driver = W->SpawnActor<AOrbSimDriver>(GetActorLocation(), FRotator::ZeroRotator, SP);
	}
	if (Driver && bUseSelfAsAnchorIfUnset && !Driver->GetGridAnchor())
	{
		//Driver->GridAnchor = this;
	}

	if (bEnableMouse)
	{
		if (APlayerController* PC = W->GetFirstPlayerController())
		{
			PC->bShowMouseCursor = true;
			PC->bEnableClickEvents = true;
			PC->bEnableMouseOverEvents = true;
		}
	}
}
