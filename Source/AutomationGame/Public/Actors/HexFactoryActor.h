#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Actors/HexBoardActor.h"      // <-- Ensure UHT knows this UCLASS (required for UPROPERTY pointer)
#include "HexFactoryActor.generated.h"

class UInstancedStaticMeshComponent;
class USimWorldHex;
class AOrbSimDriver;
class UStaticMesh;
class UMaterialInterface;

UCLASS()
class AUTOMATIONGAME_API AHexFactoryActor : public AActor
{
    GENERATED_BODY()
public:
    AHexFactoryActor();

    virtual void BeginPlay() override;
    virtual void Tick(float DeltaSeconds) override;

    UFUNCTION(BlueprintCallable) void Rebuild();

    /** Visuals */
    UPROPERTY(EditAnywhere, Category="Visual|Tiles") UStaticMesh* TileMesh = nullptr;
    UPROPERTY(EditAnywhere, Category="Visual|Tiles") UMaterialInterface* TileMaterial = nullptr;
    UPROPERTY(EditAnywhere, Category="Visual|Belts") UStaticMesh* BeltMesh = nullptr;
    UPROPERTY(EditAnywhere, Category="Visual|Belts") UMaterialInterface* BeltMaterial = nullptr;
    UPROPERTY(EditAnywhere, Category="Visual|Belts") float BeltZOffset = 10.f;
    UPROPERTY(EditAnywhere, Category="Visual|Belts") FVector BeltScale = FVector(1.f, 1.f, 1.f);

    /** Debug */
    UPROPERTY(EditAnywhere, Category="Debug") bool bAutoRefresh = true;
    UPROPERTY(EditAnywhere, Category="Debug") bool bAlwaysDrawBeltArrows = false;
    UPROPERTY(EditAnywhere, Category="Debug") float DebugArrowLenScale = 0.6f;
    UPROPERTY(EditAnywhere, Category="Debug") float DebugArrowThickness = 6.f;

    /** Board reference (optional). If null, the actor will auto-find it. */
    UPROPERTY(EditAnywhere, Category="Board") AHexBoardActor* Board = nullptr;

    /** Anchor override (optional). */
    UPROPERTY(EditAnywhere, Category="Grid") bool bOverrideAnchor = false;
    UPROPERTY(EditAnywhere, Category="Grid", meta=(EditCondition="bOverrideAnchor")) AActor* AnchorOverride = nullptr;

private:
    void EnsureComponents();
    void RebuildInternal();
    void RebuildTiles();
    void RebuildBelts();
    void DrawBeltArrow(const FVector& Center, float TileRadiusUU, float YawDeg);
    AActor* GetAnchor() const;

    UPROPERTY(Transient) UInstancedStaticMeshComponent* TileISM = nullptr;
    UPROPERTY(Transient) UInstancedStaticMeshComponent* BeltISM = nullptr;
    UPROPERTY(Transient) USimWorldHex* Sim = nullptr;
    UPROPERTY(Transient) AOrbSimDriver* Driver = nullptr;
    int32 LastSeenRevision = -1;
    int32 LastSeenBoardRevision = -1;
};
