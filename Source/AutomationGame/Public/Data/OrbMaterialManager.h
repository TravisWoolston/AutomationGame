#pragma once
#include "AutomationGameMinimal.h"
#include "Components/ActorComponent.h"
#include "Sim/OrbTypes.h"
#include "OrbMaterialManager.generated.h"

/** Centralized manager for all orb materials and meshes */
UCLASS(BlueprintType, ClassGroup=(Custom), meta=(BlueprintSpawnableComponent))
class AUTOMATIONGAME_API UOrbMaterialManager : public UActorComponent
{
	GENERATED_BODY()

public:
	UOrbMaterialManager();

	virtual void BeginPlay() override;

	/** Get the mesh for any orb type (all orbs use the same mesh) */
	UFUNCTION(BlueprintPure, Category = "Orb Materials")
	UStaticMesh* GetOrbMesh() const { return OrbMesh; }

	/** Get the material for a specific orb type */
	UFUNCTION(BlueprintPure, Category = "Orb Materials")
	UMaterialInterface* GetMaterialForOrbType(EOrbType OrbType) const;

	/** Get the default orb material (fallback) */
	UFUNCTION(BlueprintPure, Category = "Orb Materials")
	UMaterialInterface* GetDefaultOrbMaterial() const { return DefaultOrbMaterial; }

	/** Get the custom name for an orb type, or fallback to enum name */
	UFUNCTION(BlueprintPure, Category = "Orb Materials")
	FString GetOrbTypeDisplayName(EOrbType OrbType) const;

	/** Find the material manager in the world */
	UFUNCTION(BlueprintCallable, Category = "Orb Materials", meta = (WorldContext = "WorldContextObject"))
	static UOrbMaterialManager* FindInWorld(const UObject* WorldContextObject);

	/** Initialize default custom names for all orb types */
	UFUNCTION(BlueprintCallable, Category = "Orb Materials")
	void InitializeDefaultCustomNames();

	/** Initialize default materials for all orb types */
	UFUNCTION(BlueprintCallable, Category = "Orb Materials")
	void InitializeDefaultMaterials();

public:
	/** The mesh used for all orbs */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Orb Materials")
	UStaticMesh* OrbMesh = nullptr;

	/** Default orb material used as fallback */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Orb Materials")
	UMaterialInterface* DefaultOrbMaterial = nullptr;

	/** Material mapping for each orb type */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Orb Materials")
	TMap<EOrbType, UMaterialInterface*> OrbTypeMaterials;

	/** Custom names for each orb type (editable in the editor) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Orb Materials")
	TMap<EOrbType, FString> OrbTypeCustomNames;
};
