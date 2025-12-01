#include "Data/OrbMaterialManager.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "UObject/UnrealType.h"
#include "Math/Color.h"

UOrbMaterialManager::UOrbMaterialManager()
{
	PrimaryComponentTick.bCanEverTick = false;
	// Initialize with some default mappings if needed
	InitializeDefaultCustomNames();
	InitializeDefaultUIColors();
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
		// Only log occasionally to reduce spam
		static int32 LogCounter = 0;
		// if (++LogCounter % 100 == 0)
		// {
		// 	UE_LOG(LogTemp, Log, TEXT("MaterialManager: Found material for type %d: %s"), 
		// 		(int32)OrbType, *(*FoundMaterial)->GetName());
		// }
		return const_cast<UMaterialInterface*>(*FoundMaterial);
	}
	
	// Only log when using default material (this is more important to track)
	UE_LOG(LogTemp, Log, TEXT("MaterialManager: Using default material for type %d: %s"), 
		(int32)OrbType, DefaultOrbMaterial ? *DefaultOrbMaterial->GetName() : TEXT("NULL"));
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

FLinearColor UOrbMaterialManager::GetOrbTypeUIColor(EOrbType OrbType) const
{
	if (const FLinearColor* FoundColor = OrbTypeUIColors.Find(OrbType))
	{
		return *FoundColor;
	}
	
	// Fallback to a default color (white)
	return FLinearColor::White;
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
	OrbTypeCustomNames.Add(EOrbType::Void, TEXT("Void Essence"));
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
			EOrbType::CatalystX,
			EOrbType::Void
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

void UOrbMaterialManager::InitializeDefaultUIColors()
{
	// Set default UI colors for all orb types
	OrbTypeUIColors.Add(EOrbType::None, FLinearColor(0.5f, 0.5f, 0.5f, 1.0f));           // Gray
	OrbTypeUIColors.Add(EOrbType::BaseA, FLinearColor(0.0f, 0.8f, 1.0f, 1.0f));         // Light Blue
	OrbTypeUIColors.Add(EOrbType::BaseB, FLinearColor(0.0f, 1.0f, 0.5f, 1.0f));         // Green
	OrbTypeUIColors.Add(EOrbType::BaseC, FLinearColor(1.0f, 0.8f, 0.0f, 1.0f));         // Yellow
	OrbTypeUIColors.Add(EOrbType::MetalA, FLinearColor(0.7f, 0.7f, 0.8f, 1.0f));        // Silver
	OrbTypeUIColors.Add(EOrbType::CatalystX, FLinearColor(1.0f, 0.0f, 1.0f, 1.0f));    // Magenta
	OrbTypeUIColors.Add(EOrbType::Void, FLinearColor(0.1f, 0.0f, 0.2f, 1.0f));         // Dark Purple
}
