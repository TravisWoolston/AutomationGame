#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "GameManager.generated.h"

class AGameTickManager;
class UInstancedStaticMeshComponent;
class UStaticMesh;
class UMaterialInterface;

/** Lightweight coordinator that also renders a simple hex grid via ISM. */
UCLASS(BlueprintType)
class AUTOMATIONGAME_API AGameManager : public AActor
{
	GENERATED_BODY()
public:
	AGameManager();

	// ------------ References ------------
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Refs")
	AGameTickManager* TickManager = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Refs")
	AActor* OrbSimDriver = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Refs")
	TSubclassOf<AActor> OrbSimDriverClass;

	// ------------ Startup ------------
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Startup")
	FString InitialMapToLoad;

	// ------------ Grid Visual ------------
	/** Center-to-center spacing in uu. */
	UPROPERTY(EditAnywhere, Category = "Grid", meta=(ClampMin="1.0"))
	float TileSizeUU = 120.f;

	/** Radius in hexes (axial distance). */
	UPROPERTY(EditAnywhere, Category = "Grid", meta=(ClampMin="1", UIMin="1"))
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

	// ------------ Editor / Control ------------
	UFUNCTION(BlueprintCallable, Category = "Grid")
	void SetGridRadius(int32 NewRadius);

	UFUNCTION(BlueprintCallable, Category = "Grid")
	void SetTileMeshAndMaterial(UStaticMesh* Mesh, UMaterialInterface* Mat);

	UFUNCTION(CallInEditor, BlueprintCallable, Category = "Grid")
	void Rebuild();

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

	// state
	UPROPERTY(Transient) UInstancedStaticMeshComponent* ISM = nullptr;
	FVector LastGridCenter = FVector::ZeroVector;
};
