#include "Actors/TriggerTile.h"
#include "Components/StaticMeshComponent.h"
#include "Actors/OrbSimDriver.h"
#include "Sim/SimWorldHex.h"
#include "Sim/HexTypes.h"
#include "Sim/Tiles.h"
#include "Framework/GameManager.h"
#include "Data/OrbMaterialManager.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "EngineUtils.h"

ATriggerTile::ATriggerTile()
{
	PrimaryActorTick.bCanEverTick = true;

	RootComp = CreateDefaultSubobject<USceneComponent>(TEXT("RootComp"));
	RootComponent = RootComp;

	MeshComp = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("MeshComp"));
	MeshComp->SetupAttachment(RootComp);
	MeshComp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
}

void ATriggerTile::BeginPlay()
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

void ATriggerTile::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	// Only check trigger condition if we have an owner
	if (bHasOwner)
	{
		CheckTriggerCondition();
	}
}

void ATriggerTile::CheckTriggerCondition()
{
	// Ensure we have Sim reference
	if (!Sim && !Driver)
	{
		if (UWorld* World = GetWorld())
		{
			Driver = AOrbSimDriver::Find(World);
			if (Driver)
			{
				Sim = Driver->GetSim();
			}
		}
	}

	if (!Sim) return;

	// Check tile content at Q, R
	FHex Location(Q, R);
	bool bMatchingOrb = false;

	// Check Sim Grid for ItemId
	if (Sim->ItemMap.Contains(Location))
	{
		const FItem* Item = Sim->ItemMap.Find(Location);
		if (Item && Item->Id != 0)
		{
			// There's an orb here - check if it matches our trigger type
			if (TriggerOrbType == EOrbType::Void)
			{
				// Void type: any orb triggers positive signal
				bMatchingOrb = true;
			}
			else if (!Item->bIsCluster)
			{
				// Single orb - check if type matches
				bMatchingOrb = (Item->Single.Type == TriggerOrbType);
			}
			else
			{
				// Cluster - check if any orb in the cluster matches
				// For now, check the first orb in the cluster at this position
				for (const FOrbData& OrbData : Item->Orbs)
				{
					if (OrbData.dQ == 0 && OrbData.dR == 0)
					{
						// This is the orb at the cluster's root position
						bMatchingOrb = ((EOrbType)OrbData.Type == TriggerOrbType);
						break;
					}
				}
			}
		}
	}

	// Update signal state if changed
	SetSignal(bMatchingOrb);
}

void ATriggerTile::SetSignal(bool bNewSignal)
{
	if (bCurrentSignal != bNewSignal)
	{
		bCurrentSignal = bNewSignal;
		OnSignalChanged.Broadcast(bCurrentSignal);

		// Update the simulation's belt trigger signal
		if (bHasOwner && Sim)
		{
			FHex OwnerHex(OwnerQ, OwnerR);
			Sim->SetBeltTriggerSignal(OwnerHex, bCurrentSignal);
			
			UE_LOG(LogTemp, Log, TEXT("TriggerTile at (%d,%d) signal changed to %s - belt at (%d,%d) now %s"),
				Q, R, bCurrentSignal ? TEXT("POSITIVE") : TEXT("NEGATIVE"),
				OwnerQ, OwnerR, bCurrentSignal ? TEXT("ENABLED") : TEXT("DISABLED"));
		}

		// Optionally update visuals to reflect signal state
		UpdateVisuals();
	}
}

void ATriggerTile::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	UpdateVisuals();
}

void ATriggerTile::SetIsGridVisible(bool bVisible)
{
	if (bIsGridVisible != bVisible)
	{
		bIsGridVisible = bVisible;
		OnGridVisibilityChanged.Broadcast(bVisible);
	}
}

void ATriggerTile::SetOwnerBelt(int32 InOwnerQ, int32 InOwnerR)
{
	OwnerQ = InOwnerQ;
	OwnerR = InOwnerR;
	bHasOwner = true;

	UE_LOG(LogTemp, Log, TEXT("TriggerTile at (%d,%d) now owned by belt at (%d,%d)"), Q, R, OwnerQ, OwnerR);
	
	// Initialize the simulation with the current signal state
	// Start with negative signal (no orb present yet) so belt starts disabled
	if (Sim)
	{
		FHex OwnerHex(OwnerQ, OwnerR);
		Sim->SetBeltTriggerSignal(OwnerHex, bCurrentSignal);
	}
}

void ATriggerTile::UpdateVisuals()
{
	if (!MeshComp) return;

	// Priority 1: Use explicit signal state materials if provided
	if (bCurrentSignal && PositiveSignalMaterial)
	{
		MeshComp->SetMaterial(0, PositiveSignalMaterial);
		return;
	}
	else if (!bCurrentSignal && NegativeSignalMaterial)
	{
		MeshComp->SetMaterial(0, NegativeSignalMaterial);
		return;
	}

	// Priority 2: Use TranslucentBaseMaterial with orb type color tint
	if (TranslucentBaseMaterial)
	{
		UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(TranslucentBaseMaterial, this);
		if (MID)
		{
			// Get the orb type color from the material manager
			FLinearColor OrbColor = FLinearColor::White;
			if (UWorld* World = GetWorld())
			{
				UOrbMaterialManager* MaterialManager = UOrbMaterialManager::FindInWorld(this);
				if (MaterialManager)
				{
					// Try to get color from the orb type material
					UMaterialInterface* TypeMaterial = MaterialManager->GetMaterialForOrbType(TriggerOrbType);
					if (TypeMaterial)
					{
						// Try to extract base color from the material
						// Use material-specific colors based on orb type
						switch (TriggerOrbType)
						{
						case EOrbType::BaseA:   OrbColor = FLinearColor(0.8f, 0.2f, 0.2f); break; // Red
						case EOrbType::BaseB:   OrbColor = FLinearColor(0.2f, 0.8f, 0.2f); break; // Green
						case EOrbType::BaseC:   OrbColor = FLinearColor(0.2f, 0.2f, 0.8f); break; // Blue
						case EOrbType::MetalA:  OrbColor = FLinearColor(0.7f, 0.7f, 0.7f); break; // Silver
						case EOrbType::CatalystX: OrbColor = FLinearColor(0.8f, 0.6f, 0.2f); break; // Gold
						case EOrbType::Void:    OrbColor = FLinearColor(0.5f, 0.0f, 0.8f); break; // Purple (any orb)
						default:                OrbColor = FLinearColor::White; break;
						}
					}
				}
			}

			// Apply color and opacity to the translucent material
			MID->SetVectorParameterValue(FName("BaseColor"), OrbColor);
			MID->SetVectorParameterValue(FName("Color"), OrbColor); // Alternative parameter name
			MID->SetVectorParameterValue(FName("Tint"), OrbColor); // Another common name
			MID->SetScalarParameterValue(FName("Opacity"), TriggerTileOpacity);
			
			MeshComp->SetMaterial(0, MID);
			return;
		}
	}

	// Priority 3: Fallback - create translucent version of orb material
	if (UWorld* World = GetWorld())
	{
		UOrbMaterialManager* MaterialManager = UOrbMaterialManager::FindInWorld(this);
		if (MaterialManager)
		{
			UMaterialInterface* TypeMaterial = MaterialManager->GetMaterialForOrbType(TriggerOrbType);
			if (TypeMaterial)
			{
				UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(TypeMaterial, this);
				if (MID)
				{
					// Try to set opacity (material must support this parameter)
					MID->SetScalarParameterValue(FName("Opacity"), TriggerTileOpacity);
					MeshComp->SetMaterial(0, MID);
					return;
				}
			}
		}
	}
}

void ATriggerTile::Destroyed()
{
	Super::Destroyed();

	// Clean up the belt trigger signal in simulation
	if (bHasOwner && Sim)
	{
		FHex OwnerHex(OwnerQ, OwnerR);
		Sim->ClearBeltTriggerSignal(OwnerHex);
		UE_LOG(LogTemp, Log, TEXT("TriggerTile at (%d,%d) destroyed - belt at (%d,%d) re-enabled"), Q, R, OwnerQ, OwnerR);
	}
	else
	{
		UE_LOG(LogTemp, Log, TEXT("TriggerTile at (%d,%d) destroyed"), Q, R);
	}
}

