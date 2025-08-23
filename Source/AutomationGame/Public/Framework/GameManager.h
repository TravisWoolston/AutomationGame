#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Sim/HexTypes.h"
#include "GameManager.generated.h"

class UInstancedStaticMeshComponent;
class USceneComponent;
class UStaticMesh;
class UMaterialInterface;
class AGameTickManager;

/**
 * Lightweight coordinator for world systems.
 * Owns the visible hex grid (ISM) so we don't need a separate HexGridActor.
 */
UCLASS(BlueprintType)
class AUTOMATIONGAME_API AGameManager : public AActor
{
	GENERATED_BODY()
public:
	AGameManager();

	// --- Lifecycle ---
	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

	// --- References ---
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Refs")
	AGameTickManager* TickManager = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Refs")
	AActor* OrbSimDriver = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Refs")
	TSubclassOf<AActor> OrbSimDriverClass;

	// --- Startup ---
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Startup")
	FString InitialMapToLoad;

	// --- Grid (visual & placement) ---
	/** Single knob for hex spacing used by placement/renderer/sim. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid", meta=(ClampMin="2.0"))
	float TileSizeUU = 120.f;

	/** Radius in hexes (axial distance from center). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid", meta=(ClampMin="0", UIMin="0"))
	int32 GridRadius = 12;

	/** Mesh + material for each tile instance. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid")
	UStaticMesh* TileMesh = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid")
	UMaterialInterface* TileMaterial = nullptr;

	/** If true, grid rebuilds when the anchor moves farther than this threshold. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid")
	bool bAutoExpandAtRuntime = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid", meta = (EditCondition = "bAutoExpandAtRuntime", ClampMin="0.0"))
	float RecenterThresholdUU = 800.f;

	/** Optional anchor to position the grid; falls back to this actor's transform. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Grid")
	USceneComponent* GridAnchor = nullptr;

	/** Expose a simple editor button. */
	UFUNCTION(CallInEditor, Category="Grid")
	void Rebuild();

	/** Setters for BP/C++ convenience. */
	UFUNCTION(BlueprintCallable, Category="Grid")
	void SetGridRadius(int32 Radius);

	UFUNCTION(BlueprintCallable, Category="Grid")
	void SetTileMeshAndMaterial(UStaticMesh* Mesh, UMaterialInterface* Mat);

	// --- Belts visual config (kept here for future expansion) ---
	UPROPERTY(EditAnywhere, Category = "Visual|Belts") UStaticMesh* BeltStraightMesh = nullptr;
	UPROPERTY(EditAnywhere, Category = "Visual|Belts") UStaticMesh* BeltTurnMesh = nullptr;
	UPROPERTY(EditAnywhere, Category = "Visual|Belts") UMaterialInterface* BeltMaterial = nullptr;
	UPROPERTY(EditAnywhere, Category = "Visual|Belts") FVector BeltScale = FVector(1.f,1.f,1.f);
	UPROPERTY(EditAnywhere, Category = "Visual|Belts") float BeltZOffset = 10.f;

	// Quick getters (BlueprintPure)
	UFUNCTION(BlueprintPure, Category="Grid") float GetTileSize() const { return TileSizeUU; }
	UFUNCTION(BlueprintPure, Category="Visual|Belts") UStaticMesh* GetBeltStraightMesh() const { return BeltStraightMesh; }
	UFUNCTION(BlueprintPure, Category="Visual|Belts") UStaticMesh* GetBeltTurnMesh() const { return BeltTurnMesh; }
	UFUNCTION(BlueprintPure, Category="Visual|Belts") UMaterialInterface* GetBeltMaterial() const { return BeltMaterial; }
	UFUNCTION(BlueprintPure, Category="Visual|Belts") FVector GetBeltScale() const { return BeltScale; }
	UFUNCTION(BlueprintPure, Category="Visual|Belts") float GetBeltZOffset() const { return BeltZOffset; }

	/** Utility to find the first manager in world (for C++/BP). */
	UFUNCTION(BlueprintCallable, Category="Framework", meta=(WorldContext="WorldContextObject"))
	static AGameManager* FindInWorld(const UObject* WorldContextObject);

protected:
	// where the grid was last built around
	FVector LastGridCenter = FVector::ZeroVector;

private:
	// Components
	UPROPERTY(VisibleAnywhere, Category="Grid", meta=(AllowPrivateAccess="true"))
	UInstancedStaticMeshComponent* GridISM = nullptr;

	// Terrain representation (filled radius)
	TSet<FHex> Terrain;

	// impl
	void AutoFindRefs();
	void TryLoadInitial();

	void EnsureGridComponents();
	void GenerateFilledRadius(int32 Radius);
	void BuildGridFromTerrain();
	FVector GetGridOrigin() const;
};
