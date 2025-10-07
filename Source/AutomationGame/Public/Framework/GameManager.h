#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Actors/BasicArm.h"
#include "GameManager.generated.h"

class AGameTickManager;
class UInstancedStaticMeshComponent;
class UStaticMesh;
class UMaterialInterface;
class AOrbSimDriver;
class UGoldbergGrid; // NEW
/** Lightweight coordinator that also renders a simple hex or Goldberg grid via ISM. */
UCLASS(BlueprintType)
class AUTOMATIONGAME_API AGameManager : public AActor
{
	GENERATED_BODY()
public:
	AGameManager();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Factory|Arms")
	TSubclassOf<ABasicArm> BasicArmClass;

	/** Optional default program to use when placing from input. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Factory|Arms")
	TArray<FArmInstr> DefaultArmProgram;

	UFUNCTION(BlueprintCallable, Category = "Factory|Arms")
	ABasicArm* SpawnBasicArmAt(const FHex& Hex, uint8 FacingDir, const TArray<FArmInstr>& Program, bool bLoop = true);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Refs")
	AGameTickManager* TickManager = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Refs")
	AOrbSimDriver* OrbSimDriver = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Refs")
	TSubclassOf<AOrbSimDriver> OrbSimDriverClass;

	// ------------ Startup ------------
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Startup")
	FString InitialMapToLoad;

	// ------------ Grid Visual ------------
	/** Center-to-center spacing for hexes (was TileSizeUU). */
	UPROPERTY(EditAnywhere, Category = "Grid", meta = (ClampMin = "1.0"))
	float TileSizeUU = 120.f; // Legacy name kept for serialization

	/** Alias for TileSizeUU for clarity when working in sphere mode. */
	UFUNCTION(BlueprintPure, Category="Grid") FORCEINLINE float GetTileSpacing() const { return TileSizeUU; }

	/** PointyTop vs FlatTop orientation of the hex grid. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid")
	EHexOrientation GridOrientation = EHexOrientation::PointyTop;

	/** World yaw (deg) for the grid around the anchor. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid", meta = (ClampMin = -360.0, ClampMax = 360.0))
	float GridYawDeg = 0.f;

	/** Radius in hexes (axial distance). */
	UPROPERTY(EditAnywhere, Category = "Grid", meta = (ClampMin = "1", UIMin = "1"))
	int32 Radius = 12;

	/** Optional anchor actor for the grid; if null uses this actor. */
	UPROPERTY(EditAnywhere, Category = "Grid")
	AActor* GridAnchor = nullptr;

	UPROPERTY(EditAnywhere, Category = "Grid")
	UStaticMesh* TileMesh = nullptr;

	UPROPERTY(EditAnywhere, Category = "Grid")
	UMaterialInterface* TileMaterial = nullptr;

	/** Auto-rebuild when moved farther than this threshold. */
	UPROPERTY(EditAnywhere, Category = "Grid")
	bool bAutoExpandAtRuntime = true;

	UPROPERTY(EditAnywhere, Category = "Grid", meta = (EditCondition = "bAutoExpandAtRuntime"))
	float RecenterThresholdUU = 800.f;

	/** If true, map hex grid onto a sphere surface instead of a plane. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Sphere")
	bool bSphereGrid = false;

	/** Multiplier for automatic sphere radius: AutoSphereRadius = TileSizeUU * Radius * SphereRadiusScale. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Sphere", meta=(EditCondition="bSphereGrid", ClampMin="0.1"))
	float SphereRadiusScale = 1.0f;

	/** If true, sphere radius derives from TileSpacing*Radius*SphereRadiusScale; otherwise uses SphereRadiusOverrideUU. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Sphere", meta=(EditCondition="bSphereGrid"))
	bool bAutoSphereRadius = true;

	/** Explicit sphere radius to use when bAutoSphereRadius is false. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Sphere", meta=(EditCondition="bSphereGrid && !bAutoSphereRadius", ClampMin="10"))
	float SphereRadiusOverrideUU = 3000.f;

	/** Use Goldberg (geodesic) sphere instead of projected hex disc. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Sphere", meta=(EditCondition="bSphereGrid"))
	bool bUseGoldbergSphere = false; // NEW

	/** Subdivision frequency for Goldberg sphere (>=1). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Sphere", meta=(EditCondition="bSphereGrid && bUseGoldbergSphere", ClampMin="1", UIMin="1"))
	int32 GoldbergFrequency = 3; // NEW

	/** Current effective sphere radius (auto or override). */
	UFUNCTION(BlueprintPure, Category="Grid|Sphere")
	float GetEffectiveSphereRadius() const { return bAutoSphereRadius ? (TileSizeUU * Radius * SphereRadiusScale) : SphereRadiusOverrideUU; }

	// ------------ Editor / Control ------------
	UFUNCTION(BlueprintCallable, Category = "Grid")
	void SetGridRadius(int32 NewRadius);

	UFUNCTION(BlueprintCallable, Category = "Grid")
	void SetTileMeshAndMaterial(UStaticMesh* Mesh, UMaterialInterface* Mat);

	UFUNCTION(CallInEditor, BlueprintCallable, Category = "Grid")
	void Rebuild();

	UFUNCTION(BlueprintCallable, Category="Grid")
	FVector GetWorldForHex(const FHex& H) const;

protected:
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
#if WITH_EDITOR
	virtual void OnConstruction(const FTransform& Transform) override;
#endif

private:
	void AutoFindRefs();
	void TryLoadInitial();
	void EnsureISM();
	void BuildGridInstances();
	void SyncOrbSimDriverParameters(); // Sync OrbSimDriver to match GameManager parameters

	// state
	UPROPERTY(Transient) UInstancedStaticMeshComponent* ISM = nullptr;
	FVector LastGridCenter = FVector::ZeroVector;
	UPROPERTY(Transient) UGoldbergGrid* GoldbergGrid = nullptr; // NEW
};
