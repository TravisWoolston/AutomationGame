#include "Actors/RequestBeacon.h"
#include "Components/StaticMeshComponent.h"
#include "Components/DecalComponent.h"
#include "Actors/OrbSimDriver.h"
#include "Sim/SimWorldHex.h"
#include "Sim/HexTypes.h"
#include "Framework/GameManager.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "EngineUtils.h"

ARequestBeacon::ARequestBeacon()
{
	PrimaryActorTick.bCanEverTick = true;

	RootComp = CreateDefaultSubobject<USceneComponent>(TEXT("RootComp"));
	RootComponent = RootComp;

	MeshComp = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("MeshComp"));
	MeshComp->SetupAttachment(RootComp);
	MeshComp->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	DecalComp = CreateDefaultSubobject<UDecalComponent>(TEXT("DecalComp"));
	DecalComp->SetupAttachment(MeshComp);
	// Default decal rotation to face up (pitch -90)
	DecalComp->SetRelativeRotation(FRotator(-90.0f, 0.0f, 0.0f));
}

void ARequestBeacon::BeginPlay()
{
	Super::BeginPlay();
	// Find Driver and Sim
	if (UWorld* World = GetWorld())
	{
		Driver = AOrbSimDriver::Find(World);
		if (Driver)
		{
			Sim = Driver->GetSim();
		}
		
		// Initialize IsGridVisible from GameManager's current state
		for (TActorIterator<AGameManager> It(World); It; ++It)
		{
			AGameManager* GM = *It;
			if (GM)
			{
				if (UInstancedStaticMeshComponent* ISM = GM->FindComponentByClass<UInstancedStaticMeshComponent>())
				{
					SetIsGridVisible(ISM->IsVisible());
				}
				break;
			}
		}
	}
	UpdateVisuals();
}

void ARequestBeacon::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	CheckReadiness();
}

void ARequestBeacon::CheckReadiness()
{
	// Only check for Pickup beacons
	if (BeaconRole != EBeaconRole::PickUp) return;

	// If in progress, don't override (assume walker handling it)
	if (BeaconState == EBeaconState::InProgress) return;

	// Need Sim
	if (!Sim && !Driver)
	{
		// Try finding again
		if (UWorld* World = GetWorld())
		{
			Driver = AOrbSimDriver::Find(World);
			if (Driver) Sim = Driver->GetSim();
		}
	}

	if (!Sim) return;

	// Check tile content at Q, R
	FHex Location(Q, R);
	bool bOccupied = false;

	// Check Sim Grid for ItemId
	// Sim->ItemMap maps Hex -> FItem
	if (Sim->ItemMap.Contains(Location))
	{
		// If there's an entry in ItemMap, the tile is occupied
		// FItem contains Id, Type, etc.
		const FItem* Item = Sim->ItemMap.Find(Location);
		if (Item && Item->Id != 0)
		{
			bOccupied = true;
		}
	}

	// Update State
	EBeaconState DesiredState = bOccupied ? EBeaconState::Ready : EBeaconState::NotReady;
	if (BeaconState != DesiredState)
	{
		SetState(DesiredState);
	}
}

void ARequestBeacon::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	UpdateVisuals();
}

void ARequestBeacon::SetState(EBeaconState NewState)
{
	BeaconState = NewState;
	UpdateVisuals();
}

void ARequestBeacon::SetIsGridVisible(bool bVisible)
{
	if (IsGridVisible != bVisible)
	{
		IsGridVisible = bVisible;
		OnGridVisibilityChanged.Broadcast(bVisible);
	}
}

void ARequestBeacon::UpdateVisuals()
{
	// Update Mesh Material based on State
	if (MeshComp)
	{
		UMaterialInterface* MaterialToUse = nullptr;
		switch (BeaconState)
		{
		case EBeaconState::Ready:
			MaterialToUse = ReadyMaterial;
			break;
		case EBeaconState::InProgress:
			MaterialToUse = InProgressMaterial;
			break;
		case EBeaconState::NotReady:
			MaterialToUse = NotReadyMaterial;
			break;
		}

		if (MaterialToUse)
		{
			MeshComp->SetMaterial(0, MaterialToUse);
		}
	}

	// Update Decal Material based on Role/Type
	if (DecalComp)
	{
		UMaterialInterface* DecalMat = nullptr;
		switch (BeaconType)
		{
		case EBeaconType::Relocate:
			if (BeaconRole == EBeaconRole::PickUp)
			{
				DecalMat = PickupDecalMaterial;
			}
			else if (BeaconRole == EBeaconRole::DropOff)
			{
				DecalMat = DropOffDecalMaterial;
			}
			break;
		}

		if (DecalMat)
		{
			DecalComp->SetDecalMaterial(DecalMat);
		}
	}
}

void ARequestBeacon::Destroyed()
{
	Super::Destroyed();

	// If we have a paired beacon, destroy it too
	if (PairedBeacon && IsValid(PairedBeacon))
	{
		// Avoid infinite recursion by clearing the pointer on the partner first
		PairedBeacon->PairedBeacon = nullptr;
		PairedBeacon->Destroy();
		PairedBeacon = nullptr;
	}
}
