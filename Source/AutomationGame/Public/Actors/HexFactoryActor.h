#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Sim/HexCoord.h"
#include "Sim/HexTypes.h"
#include "Sim/OrbTypes.h"
#include "Data/OrbMaterialManager.h"
#include "HexFactoryActor.generated.h"

class UInstancedStaticMeshComponent;
class UProceduralMeshComponent;
class USimWorldHex;
class AOrbSimDriver;
class AHexBoardActor;

UCLASS()
class AUTOMATIONGAME_API AHexFactoryActor : public AActor
{
	GENERATED_BODY()
public:
	AHexFactoryActor();

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void Tick(float DeltaSeconds) override;

	UFUNCTION(BlueprintCallable, Category = "Build")
	void Rebuild();
	
	UFUNCTION()
	void HandleSimStepComplete(float StepSeconds);

	// ----- Shared / Grid -----
	UPROPERTY(EditAnywhere, Category = "Grid") bool bAutoRefresh = true;

	AActor* GetAnchor() const;

	// ----- Visual Assets -----
	UPROPERTY(EditAnywhere, Category = "Visual|Tiles") UStaticMesh* TileMesh = nullptr;
	UPROPERTY(EditAnywhere, Category = "Visual|Tiles") UMaterialInterface* TileMaterial = nullptr;
	
	// Different materials for different tile types
	UPROPERTY(EditAnywhere, Category = "Visual|Tiles") UMaterialInterface* InputTileMaterial = nullptr;
	UPROPERTY(EditAnywhere, Category = "Visual|Tiles") UMaterialInterface* OutputTileMaterial = nullptr;
	UPROPERTY(EditAnywhere, Category = "Visual|Tiles") UMaterialInterface* BeltTileMaterial = nullptr;
	UPROPERTY(EditAnywhere, Category = "Visual|Tiles") UMaterialInterface* BondTileMaterial = nullptr;
	

	UPROPERTY(EditAnywhere, Category = "Visual|Belts") UStaticMesh* BeltMesh = nullptr;
	UPROPERTY(EditAnywhere, Category = "Visual|Belts") UMaterialInterface* BeltMaterial = nullptr;
	UPROPERTY(EditAnywhere, Category = "Visual|Belts") UMaterialInterface* BeltReadyMaterial = nullptr;
	UPROPERTY(EditAnywhere, Category = "Visual|Belts") UMaterialInterface* BeltWaitingMaterial = nullptr;
	UPROPERTY(EditAnywhere, Category = "Visual|Belts") FVector BeltScale = FVector(1.f);
	UPROPERTY(EditAnywhere, Category = "Visual|Belts") float BeltZOffset = 8.f;
	UPROPERTY(EditAnywhere, Category = "Visual|Belts") bool bAlwaysDrawBeltArrows = false;
	UPROPERTY(EditAnywhere, Category = "Visual|Belts") float DebugArrowLenScale = 0.55f;
	UPROPERTY(EditAnywhere, Category = "Visual|Belts") float DebugArrowThickness = 4.f;

	// ----- NEW: Bond visuals (handled like belts) -----
	UPROPERTY(EditAnywhere, Category = "Visual|Bonds") UStaticMesh* BondMesh = nullptr;
	UPROPERTY(EditAnywhere, Category = "Visual|Bonds") UMaterialInterface* BondMaterial = nullptr;
	UPROPERTY(EditAnywhere, Category = "Visual|Bonds") FVector BondScale = FVector(1.f, 1.f, 1.f);
	UPROPERTY(EditAnywhere, Category = "Visual|Bonds") float BondZOffset = 10.f;

	// Unbond visuals (separate from bonds)
	UPROPERTY(EditAnywhere, Category = "Visual|Unbonds") UStaticMesh* UnbondMesh = nullptr;
	UPROPERTY(EditAnywhere, Category = "Visual|Unbonds") UMaterialInterface* UnbondMaterial = nullptr;
	UPROPERTY(EditAnywhere, Category = "Visual|Unbonds") FVector UnbondScale = FVector(1.f, 1.f, 1.f);
	UPROPERTY(EditAnywhere, Category = "Visual|Unbonds") float UnbondZOffset = 10.f;

	// Optional debug arrows for bonds
	UPROPERTY(EditAnywhere, Category = "Visual|Bonds") bool bDrawBondDebugArrows = false;

protected:
	// Components
	UPROPERTY(Transient) UInstancedStaticMeshComponent* TileISM = nullptr;
	UPROPERTY(Transient) UInstancedStaticMeshComponent* InputTileISM = nullptr;
	UPROPERTY(Transient) UInstancedStaticMeshComponent* OutputTileISM = nullptr;
	UPROPERTY(Transient) UInstancedStaticMeshComponent* BeltTileISM = nullptr;
	UPROPERTY(Transient) UInstancedStaticMeshComponent* BondTileISM = nullptr;
	UPROPERTY(Transient) UInstancedStaticMeshComponent* UnbondTileISM = nullptr;
	UPROPERTY(Transient) UInstancedStaticMeshComponent* BeltISM = nullptr;
	UPROPERTY(Transient) UInstancedStaticMeshComponent* BeltReadyISM = nullptr;
	UPROPERTY(Transient) UInstancedStaticMeshComponent* BeltWaitingISM = nullptr;
	UPROPERTY(Transient) UInstancedStaticMeshComponent* BondISM = nullptr;
	UPROPERTY(Transient) UInstancedStaticMeshComponent* UnbondISM = nullptr;
	
	// Type-specific output ISM components
	UPROPERTY(Transient) UInstancedStaticMeshComponent* OutputNoneISM = nullptr;
	UPROPERTY(Transient) UInstancedStaticMeshComponent* OutputBaseAISM = nullptr;
	UPROPERTY(Transient) UInstancedStaticMeshComponent* OutputBaseBISM = nullptr;
	UPROPERTY(Transient) UInstancedStaticMeshComponent* OutputBaseCISM = nullptr;
	UPROPERTY(Transient) UInstancedStaticMeshComponent* OutputMetalAISM = nullptr;
	UPROPERTY(Transient) UInstancedStaticMeshComponent* OutputCatalystXISM = nullptr;
	
	// Type-specific input ISM components
	UPROPERTY(Transient) UInstancedStaticMeshComponent* InputNoneISM = nullptr;
	UPROPERTY(Transient) UInstancedStaticMeshComponent* InputBaseAISM = nullptr;
	UPROPERTY(Transient) UInstancedStaticMeshComponent* InputBaseBISM = nullptr;
	UPROPERTY(Transient) UInstancedStaticMeshComponent* InputBaseCISM = nullptr;
	UPROPERTY(Transient) UInstancedStaticMeshComponent* InputMetalAISM = nullptr;
	UPROPERTY(Transient) UInstancedStaticMeshComponent* InputCatalystXISM = nullptr;
	
	// Procedural mesh for merged cluster outputs
	UPROPERTY(Transient) UProceduralMeshComponent* MergedOutputMesh = nullptr;
	
	// Procedural mesh for Input/ClusterInput tiles with cutouts
	UPROPERTY(Transient) UProceduralMeshComponent* InputTilesMesh = nullptr;

	// References
	UPROPERTY(Transient) USimWorldHex* Sim = nullptr;
	UPROPERTY(Transient) AOrbSimDriver* Driver = nullptr;
	UPROPERTY(Transient) AHexBoardActor* Board = nullptr;

	// Revisions
	int32 LastSeenRevision = -1;
	int32 LastSeenBoardRevision = -1;

	void EnsureComponents();
	void RebuildInternal();
	void RebuildTiles();
	void RebuildBelts();
	void UpdateBeltMaterials();   // Update belt materials based on current item positions
	void RebuildBonds();          // NEW
	void RebuildOutputs();        // NEW: Type-specific output rendering
	void RebuildInputTiles();     // Render Input/ClusterInput tiles using ISM with orb-type materials
	void DrawBeltArrow(const FVector& Center, float TileRadiusUU, float YawDeg);
	void DrawBondArrow(const FVector& A, const FVector& B);
	
	// Helper functions for output type management
	UInstancedStaticMeshComponent* GetOutputISMForType(EOrbType OrbType);
	
	// Helper functions for input type management
	UInstancedStaticMeshComponent* GetInputISMForType(EOrbType OrbType);
	
	// Generate a merged hex mesh between two adjacent hexagons
	void GenerateMergedHexMesh(const FVector& Hex1Center, const FVector& Hex2Center, float HexRadius, 
		UMaterialInterface* Mat1, UMaterialInterface* Mat2, int32 SectionIndex);
	
	// Generate a hexagon mesh with a circular hole cut out (for Input/ClusterInput tiles)
	void GenerateHexWithHole(const FVector& Center, float HexRadius, float HoleRadius, 
		UMaterialInterface* Material, int32 SectionIndex);

private:
	FVector HexToWorld(const FHex& H) const;
};
