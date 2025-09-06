#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Sim/HexCoord.h"
#include "Sim/HexTypes.h"
#include "HexFactoryActor.generated.h"

class UInstancedStaticMeshComponent;
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
	virtual void Tick(float DeltaSeconds) override;

	UFUNCTION(BlueprintCallable, Category = "Build")
	void Rebuild();

	// ----- Shared / Grid -----
	UPROPERTY(EditAnywhere, Category = "Grid") bool bAutoRefresh = true;

	AActor* GetAnchor() const;

	// ----- Visual Assets -----
	UPROPERTY(EditAnywhere, Category = "Visual|Tiles") UStaticMesh* TileMesh = nullptr;
	UPROPERTY(EditAnywhere, Category = "Visual|Tiles") UMaterialInterface* TileMaterial = nullptr;

	UPROPERTY(EditAnywhere, Category = "Visual|Belts") UStaticMesh* BeltMesh = nullptr;
	UPROPERTY(EditAnywhere, Category = "Visual|Belts") UMaterialInterface* BeltMaterial = nullptr;
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

	// Optional debug arrows for bonds
	UPROPERTY(EditAnywhere, Category = "Visual|Bonds") bool bDrawBondDebugArrows = false;

protected:
	// Components
	UPROPERTY(Transient) UInstancedStaticMeshComponent* TileISM = nullptr;
	UPROPERTY(Transient) UInstancedStaticMeshComponent* BeltISM = nullptr;
	UPROPERTY(Transient) UInstancedStaticMeshComponent* BondISM = nullptr;

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
	void RebuildBonds();          // NEW
	void DrawBeltArrow(const FVector& Center, float TileRadiusUU, float YawDeg);
	void DrawBondArrow(const FVector& A, const FVector& B);

private:
	FVector HexToWorld(const FHex& H) const;
};
