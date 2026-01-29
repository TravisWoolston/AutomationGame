#pragma once
#include "CoreMinimal.h"
#include "Data/OrbMaterialManager.h"
#include "GameFramework/Actor.h"

#include "Sim/HexCoord.h"
#include "Sim/HexTypes.h"
#include "Sim/OrbTypes.h"
//.generated.h must be the last include
#include "HexFactoryActor.generated.h"

class UInstancedStaticMeshComponent;
class UProceduralMeshComponent;
class UBoxComponent;
class USimWorldHex;
class AOrbSimDriver;
class AHexBoardActor;

UCLASS()
class AUTOMATIONGAME_API AHexFactoryActor : public AActor {
  GENERATED_BODY()
public:
  AHexFactoryActor();

  virtual void BeginPlay() override;
  virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
  virtual void Tick(float DeltaSeconds) override;

  UFUNCTION(BlueprintCallable, Category = "Build")
  void Rebuild();

  UFUNCTION(BlueprintCallable, Category = "Visual")
  void SetInputHighlight(bool bEnable);
  UFUNCTION(BlueprintCallable, Category = "Visual")
  void SetOutputHighlight(bool bEnable);
  UFUNCTION(BlueprintCallable, Category = "Visual")
  void SetBeltHighlight(bool bEnable);
  UFUNCTION(BlueprintCallable, Category = "Visual")
  void SetBondHighlight(bool bEnable);
  UFUNCTION(BlueprintCallable, Category = "Visual")
  void SetUnbondHighlight(bool bEnable);

  UFUNCTION(BlueprintCallable, Category = "Visual")
  void UpdateGlobalHighlight(EPlaceKind Kind, bool bEnable, int32 Level,
                             EOrbType OrbType);

  UFUNCTION(BlueprintCallable, Category = "Visual")
  void SetViewMode(EViewMode Mode);

  /** Utility to find the first factory actor in the world. */
  static AHexFactoryActor *FindInWorld(UWorld *World);

  UFUNCTION()
  void HandleSimStepComplete(float StepSeconds);

  // ----- Shared / Grid -----
  UPROPERTY(EditAnywhere, Category = "Grid") bool bAutoRefresh = true;

  AActor *GetAnchor() const;

  // ----- Visual Assets -----
  UPROPERTY(EditAnywhere, Category = "Visual|Tiles")
  UStaticMesh *TileMesh = nullptr;
  UPROPERTY(EditAnywhere, Category = "Visual|Tiles")
  UMaterialInterface *TileMaterial = nullptr;

  // Different materials for different tile types
  UPROPERTY(EditAnywhere, Category = "Visual|Tiles")
  UMaterialInterface *InputTileMaterial = nullptr;
  UPROPERTY(EditAnywhere, Category = "Visual|Tiles")
  UMaterialInterface *OutputTileMaterial = nullptr;
  UPROPERTY(EditAnywhere, Category = "Visual|Tiles")
  UMaterialInterface *BeltTileMaterial = nullptr;
  UPROPERTY(EditAnywhere, Category = "Visual|Tiles")
  UMaterialInterface *BondTileMaterial = nullptr;

  UPROPERTY(EditAnywhere, Category = "Visual|Belts")
  UStaticMesh *BeltMesh = nullptr;
  UPROPERTY(EditAnywhere, Category = "Visual|Belts")
  UMaterialInterface *BeltMaterial = nullptr;
  UPROPERTY(EditAnywhere, Category = "Visual|Belts")
  UMaterialInterface *BeltReadyMaterial = nullptr;
  UPROPERTY(EditAnywhere, Category = "Visual|Belts")
  UMaterialInterface *BeltWaitingMaterial = nullptr;
  UPROPERTY(EditAnywhere, Category = "Visual|Belts")
  FVector BeltScale = FVector(1.f);
  UPROPERTY(EditAnywhere, Category = "Visual|Belts") float BeltZOffset = 8.f;
  UPROPERTY(EditAnywhere, Category = "Visual|Belts")
  bool bAlwaysDrawBeltArrows = false;
  UPROPERTY(EditAnywhere, Category = "Visual|Belts")
  float DebugArrowLenScale = 0.55f;
  UPROPERTY(EditAnywhere, Category = "Visual|Belts")
  float DebugArrowThickness = 4.f;

  // ----- NEW: Bond visuals (handled like belts) -----
  UPROPERTY(EditAnywhere, Category = "Visual|Bonds")
  UStaticMesh *BondMesh = nullptr;
  UPROPERTY(EditAnywhere, Category = "Visual|Bonds")
  UMaterialInterface *BondMaterial = nullptr;
  UPROPERTY(EditAnywhere, Category = "Visual|Bonds")
  FVector BondScale = FVector(1.f, 1.f, 1.f);
  UPROPERTY(EditAnywhere, Category = "Visual|Bonds") float BondZOffset = 10.f;

  // Unbond visuals (separate from bonds)
  UPROPERTY(EditAnywhere, Category = "Visual|Unbonds")
  UStaticMesh *UnbondMesh = nullptr;
  UPROPERTY(EditAnywhere, Category = "Visual|Unbonds")
  UMaterialInterface *UnbondMaterial = nullptr;
  UPROPERTY(EditAnywhere, Category = "Visual|Unbonds")
  FVector UnbondScale = FVector(1.f, 1.f, 1.f);
  UPROPERTY(EditAnywhere, Category = "Visual|Unbonds")
  float UnbondZOffset = 10.f;

  // Optional debug arrows for bonds
  UPROPERTY(EditAnywhere, Category = "Visual|Bonds")
  bool bDrawBondDebugArrows = false;

  // ----- Arms -----
  UPROPERTY(EditAnywhere, Category = "Visual|Arms")
  USkeletalMesh *UpsideDownArmMesh = nullptr;

  // ----- Helix Output -----
  UPROPERTY(EditAnywhere, Category = "Visual|Helix")
  UStaticMesh *HelixTileMesh = nullptr;

  UPROPERTY(EditAnywhere, Category = "Visual|Helix")
  UMaterialInterface *HelixTileMaterial = nullptr;

  UPROPERTY(EditAnywhere, Category = "Visual|Helix")
  UStaticMesh *HelixOrbMesh = nullptr;

  UPROPERTY(EditAnywhere, Category = "Visual|Helix")
  UMaterialInterface *HelixOrbMaterial = nullptr;

  UPROPERTY(EditAnywhere, Category = "Visual|Helix")
  UStaticMesh *HelixBondMesh = nullptr;

  UPROPERTY(EditAnywhere, Category = "Visual|Helix")
  UMaterialInterface *HelixBondMaterial = nullptr;

  UPROPERTY(EditAnywhere, Category = "Visual|Helix")
  FVector HelixOrbScale = FVector(0.3f);

  UPROPERTY(EditAnywhere, Category = "Visual|Helix")
  FVector HelixBondScale = FVector(1.0f, 0.15f, 0.15f);

  UPROPERTY(EditAnywhere, Category = "Visual|Helix")
  float HelixIdleRotationSpeed = 10.f; // Degrees per second

protected:
  // Components
  UPROPERTY(Transient) UInstancedStaticMeshComponent *TileISM = nullptr;
  UPROPERTY(Transient) UInstancedStaticMeshComponent *InputTileISM = nullptr;
  UPROPERTY(Transient) UInstancedStaticMeshComponent *OutputTileISM = nullptr;
  UPROPERTY(Transient) UInstancedStaticMeshComponent *BeltTileISM = nullptr;
  UPROPERTY(Transient) UInstancedStaticMeshComponent *BondTileISM = nullptr;
  UPROPERTY(Transient) UInstancedStaticMeshComponent *UnbondTileISM = nullptr;
  UPROPERTY(Transient) UInstancedStaticMeshComponent *BeltISM = nullptr;
  UPROPERTY(Transient) UInstancedStaticMeshComponent *BeltReadyISM = nullptr;
  UPROPERTY(Transient) UInstancedStaticMeshComponent *BeltWaitingISM = nullptr;
  UPROPERTY(Transient) UInstancedStaticMeshComponent *BondISM = nullptr;
  UPROPERTY(Transient) UInstancedStaticMeshComponent *UnbondISM = nullptr;

  // Type-specific output ISM components
  UPROPERTY(Transient) UInstancedStaticMeshComponent *OutputNoneISM = nullptr;
  UPROPERTY(Transient) UInstancedStaticMeshComponent *OutputBaseAISM = nullptr;
  UPROPERTY(Transient) UInstancedStaticMeshComponent *OutputBaseBISM = nullptr;
  UPROPERTY(Transient) UInstancedStaticMeshComponent *OutputBaseCISM = nullptr;
  UPROPERTY(Transient) UInstancedStaticMeshComponent *OutputMetalAISM = nullptr;
  UPROPERTY(Transient)
  UInstancedStaticMeshComponent *OutputCatalystXISM = nullptr;
  UPROPERTY(Transient) UInstancedStaticMeshComponent *OutputVoidISM = nullptr;

  // Type-specific input ISM components
  UPROPERTY(Transient) UInstancedStaticMeshComponent *InputNoneISM = nullptr;
  UPROPERTY(Transient) UInstancedStaticMeshComponent *InputBaseAISM = nullptr;
  UPROPERTY(Transient) UInstancedStaticMeshComponent *InputBaseBISM = nullptr;
  UPROPERTY(Transient) UInstancedStaticMeshComponent *InputBaseCISM = nullptr;
  UPROPERTY(Transient) UInstancedStaticMeshComponent *InputMetalAISM = nullptr;
  UPROPERTY(Transient)
  UInstancedStaticMeshComponent *InputCatalystXISM = nullptr;
  UPROPERTY(Transient) UInstancedStaticMeshComponent *InputVoidISM = nullptr;

  // Highlight ISMs for global category highlighting
  UPROPERTY(Transient)
  UInstancedStaticMeshComponent *HighlightTileISM = nullptr;
  UPROPERTY(Transient)
  UInstancedStaticMeshComponent *HighlightBeltISM = nullptr;
  UPROPERTY(Transient)
  UInstancedStaticMeshComponent *HighlightBondISM = nullptr;
  UPROPERTY(Transient)
  UInstancedStaticMeshComponent *HighlightUnbondISM = nullptr;

  // Helix output ISMs
  UPROPERTY(Transient) UInstancedStaticMeshComponent *HelixTileISM = nullptr;
  UPROPERTY(Transient) UInstancedStaticMeshComponent *HelixOrbISM = nullptr;
  UPROPERTY(Transient) UInstancedStaticMeshComponent *HelixBondISM = nullptr;

  // Collision plane for line traces (more performant than individual tile
  // meshes)
  UPROPERTY(Transient) UBoxComponent *GridCollisionPlane = nullptr;

  // References
  UPROPERTY(Transient) USimWorldHex *Sim = nullptr;
  UPROPERTY(Transient) AOrbSimDriver *Driver = nullptr;
  UPROPERTY(Transient) AHexBoardActor *Board = nullptr;

  // Revisions
  int32 LastSeenRevision = -1;
  int32 LastSeenBoardRevision = -1;

  // Global highlight state
  EPlaceKind CurrentHighlightKind = (EPlaceKind)255;
  bool bHighlightEnabled = false;
  int32 HighlightLevel = 0;
  EOrbType HighlightOrbType = EOrbType::None;

  void EnsureComponents();
  void RebuildInternal();
  void RebuildTiles();
  void RebuildBelts();
  void UpdateBeltMaterials(); // Update belt materials based on current item
                              // positions
  void RebuildBonds();        // NEW
  void RebuildOutputs();      // NEW: Type-specific output rendering
  void RebuildInputTiles();   // Render Input/ClusterInput tiles using ISM with
                              // orb-type materials
  void DrawBeltArrow(const FVector &Center, float TileRadiusUU, float YawDeg);
  void DrawBondArrow(const FVector &A, const FVector &B);
  void RebuildHelixOutput(); // NEW: Helix tile and rung rendering
  void
  UpdateHelixAnimation(float DeltaSeconds); // NEW: Helix continuous animation

  // Helper functions for output type management
  UInstancedStaticMeshComponent *GetOutputISMForType(EOrbType OrbType);

  // Helper functions for input type management
  UInstancedStaticMeshComponent *GetInputISMForType(EOrbType OrbType);

  // Helper functions for helix orb type management
  UInstancedStaticMeshComponent *GetHelixOrbISMForType(EOrbType OrbType);

  // Per-type helix orb ISMs (use orb mesh with per-type materials)
  UPROPERTY(Transient) UInstancedStaticMeshComponent *HelixOrbNoneISM = nullptr;
  UPROPERTY(Transient)
  UInstancedStaticMeshComponent *HelixOrbBaseAISM = nullptr;
  UPROPERTY(Transient)
  UInstancedStaticMeshComponent *HelixOrbBaseBISM = nullptr;
  UPROPERTY(Transient)
  UInstancedStaticMeshComponent *HelixOrbBaseCISM = nullptr;
  UPROPERTY(Transient)
  UInstancedStaticMeshComponent *HelixOrbMetalAISM = nullptr;
  UPROPERTY(Transient)
  UInstancedStaticMeshComponent *HelixOrbCatalystXISM = nullptr;
  UPROPERTY(Transient) UInstancedStaticMeshComponent *HelixOrbVoidISM = nullptr;

  // Update the grid collision plane position to match the grid Z level
  void UpdateGridCollisionPlane();

  /** Helper to swap display level when bDisplayFlipped is enabled */
  int32 GetDisplayLevel(int32 Level) const;

private:
  FVector HexToWorld(const FHex &H) const;
};
