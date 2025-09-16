#include "Data/OrbMaterialManager.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "UObject/UnrealType.h"

UOrbMaterialManager::UOrbMaterialManager()
{
	PrimaryComponentTick.bCanEverTick = false;
	// Initialize with some default mappings if needed
	InitializeDefaultCustomNames();
}

void UOrbMaterialManager::BeginPlay()
{
	Super::BeginPlay();
	// Initialize default materials when the component starts
	InitializeDefaultMaterials();
}

UMaterialInterface* UOrbMaterialManager::GetMaterialForOrbType(EOrbType OrbType) const
{
	if (const UMaterialInterface* const* FoundMaterial = OrbTypeMaterials.Find(OrbType))
	{
		return const_cast<UMaterialInterface*>(*FoundMaterial);
	}
	return DefaultOrbMaterial;
}

FString UOrbMaterialManager::GetOrbTypeDisplayName(EOrbType OrbType) const
{
	if (const FString* FoundName = OrbTypeCustomNames.Find(OrbType))
	{
		if (!FoundName->IsEmpty())
		{
			return *FoundName;
		}
	}
	
	// Fallback to enum name
	return UEnum::GetValueAsString(OrbType);
}

UOrbMaterialManager* UOrbMaterialManager::FindInWorld(const UObject* WorldContextObject)
{
	if (!WorldContextObject)
	{
		return nullptr;
	}

	UWorld* World = WorldContextObject->GetWorld();
	if (!World)
	{
		return nullptr;
	}

	// Look for an actor with this component
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (UOrbMaterialManager* Manager = (*It)->FindComponentByClass<UOrbMaterialManager>())
		{
			return Manager;
		}
	}

	return nullptr;
}

void UOrbMaterialManager::InitializeDefaultCustomNames()
{
	// Set default custom names for all orb types
	OrbTypeCustomNames.Add(EOrbType::None, TEXT("None"));
	OrbTypeCustomNames.Add(EOrbType::BaseA, TEXT("Basic Element A"));
	OrbTypeCustomNames.Add(EOrbType::BaseB, TEXT("Basic Element B"));
	OrbTypeCustomNames.Add(EOrbType::BaseC, TEXT("Basic Element C"));
	OrbTypeCustomNames.Add(EOrbType::MetalA, TEXT("Metal Fragment"));
	OrbTypeCustomNames.Add(EOrbType::CatalystX, TEXT("Catalyst Crystal"));
}

void UOrbMaterialManager::InitializeDefaultMaterials()
{
	// Initialize default materials for each orb type
	// This should be called from BeginPlay or when materials are assigned
	if (DefaultOrbMaterial)
	{
		// If we have a default material, use it for all orb types that don't have specific materials
		TArray<EOrbType> AllOrbTypes = {
			EOrbType::None,
			EOrbType::BaseA,
			EOrbType::BaseB,
			EOrbType::BaseC,
			EOrbType::MetalA,
			EOrbType::CatalystX
		};
		
		for (EOrbType OrbType : AllOrbTypes)
		{
			if (!OrbTypeMaterials.Contains(OrbType))
			{
				OrbTypeMaterials.Add(OrbType, DefaultOrbMaterial);
			}
		}
	}
}
