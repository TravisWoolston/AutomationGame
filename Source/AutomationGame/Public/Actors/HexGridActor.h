#pragma once
#include "AutomationGameMinimal.h"
#include "GameFramework/Actor.h"
#include "HexGridActor.generated.h"

class UInstancedStaticMeshComponent;
class USimWorldHex;

/** Editor-friendly grid that populates a ring of hex tiles via an InstancedStaticMesh. */
UCLASS()
class AUTOMATIONGAME_API AHexGridActor : public AActor
{
    GENERATED_BODY()
public:
    AHexGridActor();

    /** Generate/refresh in editor too */
    virtual void OnConstruction(const FTransform& Transform) override;

    /** Rebuild the grid instances */
    UFUNCTION(CallInEditor, Category="Grid") void Rebuild();

    /** Center-to-center spacing in uu; if 0, will try to read from Sim on BeginPlay */
    UPROPERTY(EditAnywhere, Category="Grid") float TileSizeUU = 120.f;

    /** Radius in hexes (axial distance) */
    UPROPERTY(EditAnywhere, Category="Grid", meta=(ClampMin="1", UIMin="1")) int32 Radius = 12;

    /** Mesh + material for each tile instance */
    UPROPERTY(EditAnywhere, Category="Grid") UStaticMesh* TileMesh = nullptr;
    UPROPERTY(EditAnywhere, Category="Grid") UMaterialInterface* TileMaterial = nullptr;

    /** If true, will refresh when moved farther than this threshold */
    UPROPERTY(EditAnywhere, Category="Grid") bool bAutoExpandAtRuntime = true;
    UPROPERTY(EditAnywhere, Category="Grid", meta=(EditCondition="bAutoExpandAtRuntime")) float RecenterThresholdUU = 800.f;

protected:
    virtual void BeginPlay() override;
    virtual void Tick(float DeltaSeconds) override;

private:
    UInstancedStaticMeshComponent* ISM = nullptr;
    FVector LastCenter = FVector::ZeroVector;
    void EnsureISM();
    void BuildRing(int32 InRadius);
};
