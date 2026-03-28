#pragma once
#include "AutomationGameMinimal.h"
#include "Components/ActorComponent.h"
#include "Sim/ArmTypes.h"
#include "Sim/HexCoord.h" // <-- Needed for EHexOrientation
#include "Sim/HexTypes.h"
#include "Sim/OrbTypes.h" // <-- Needed for EOrbType
#include "OrbRenderer.generated.h"

class UInstancedStaticMeshComponent;
class USimWorldHex;
class AOrbSimDriver;
class UStaticMesh;
class UMaterialInterface;

/**
 * Renders orbs from the sim via an ISM. Robust to load order:
 * - Reacquires the driver/sim automatically if missing.
 * - Creates ISM in OnRegister so it works when the component is already placed
 * in the level.
 */
UCLASS(ClassGroup = (Custom), meta = (BlueprintSpawnableComponent))
class AUTOMATIONGAME_API UOrbRenderer : public UActorComponent {
  GENERATED_BODY()

public:
  /** Rebuild the current position cache for all items (used to prevent
   * flickering during bond formations) */
  void RebuildCurrPos();
  UOrbRenderer();

  /** Create the ISM as soon as the component registers */
  virtual void OnRegister() override;
  virtual void BeginPlay() override;
  virtual void
  TickComponent(float DeltaTime, enum ELevelTick TickType,
                FActorComponentTickFunction *ThisTickFunction) override;

  /** Optional override; otherwise we read anchor from the driver */
  UPROPERTY(EditAnywhere, Category = "Grid") bool bOverrideAnchor = false;
  UPROPERTY(EditAnywhere, Category = "Grid",
            meta = (EditCondition = "bOverrideAnchor"))
  AActor *AnchorOverride = nullptr;

  /** Auto repair references if driver/sim is not found at BeginPlay (eg.
   * spawned later) */
  UPROPERTY(EditAnywhere, Category = "Debug") bool bAutoReacquire = true;

  /** Debug: Make orbs invisible when false */
  UPROPERTY(EditAnywhere, Category = "Debug") bool bShowOrbs = true;

  /** Debug: Show alpha interpolation dial on screen */
  UPROPERTY(EditAnywhere, Category = "Debug") bool bShowAlphaDial = true;

  /** Basic visuals */
  // Note: OrbMesh and OrbMaterial are now managed by UOrbMaterialManager
  UPROPERTY(EditAnywhere, Category = "Visual") float OrbScale = 0.30f;

  /** Bond connection visuals */
  UPROPERTY(EditAnywhere, Category = "Bonds")
  UStaticMesh *BondedOrbMesh = nullptr;
  UPROPERTY(EditAnywhere, Category = "Bonds")
  UMaterialInterface *BondedOrbMaterial = nullptr;
  UPROPERTY(EditAnywhere, Category = "Bonds") float BondedOrbScale = 0.15f;

private:
  void EnsureISM();

  void RebuildInstances();
  void RebuildBondedOrbs();
  void DrawDebugAlphaDial(float Alpha, int32 Step);
  AActor *GetAnchor() const;
  UFUNCTION()
  void HandleSimStepComplete(float StepSeconds);
  void EnsureStepBinding();

  UPROPERTY(Transient)
  TMap<EOrbType, UInstancedStaticMeshComponent *> OrbTypeISMs;
  UPROPERTY(Transient) UInstancedStaticMeshComponent *BondedOrbISM = nullptr;
  UPROPERTY(Transient) AOrbSimDriver *Driver = nullptr;
  UPROPERTY(Transient) USimWorldHex *Sim = nullptr;

  TMap<int32, FVector> PrevPos, CurrPos;
  TMap<int32, FRotator> PrevRot, CurrRot;
  TMap<int32, EOrbType> OrbTypes; // Optimized lookup: ItemId -> OrbType
  float PrevAlpha = 1.f;
  float DirToYaw(uint8 Dir) const;

  int32 LastSeenStepCounter = -1;
  int32 LastSeenTileRevision = -1;

  // Track which instance index corresponds to which orb ID per ISM type
  // Key: OrbType, Value: Map of OrbId -> InstanceIndex
  TMap<EOrbType, TMap<int32, int32>> OrbIdToInstanceIndex;

  // Track animated orb instances separately (inputs/outputs)
  // Key: OrbType, Value: Map of AnimatedOrbIndex -> InstanceIndex
  TMap<EOrbType, TMap<int32, int32>> AnimatedOrbToInstanceIndex;

  // Track transmutation merge orb instances
  // Key: OrbType, Value: Map of unique merge orb key -> InstanceIndex  
  TMap<EOrbType, TMap<int32, int32>> TransmutationOrbToInstanceIndex;

  // Flag to trigger full ISM rebuild (set when simulation step changes)
  bool bNeedFullRebuild = true;

protected:
  // New: arm facing history for arc interpolation
  TMap<int32, uint8> PrevArmFacing;
  TMap<int32, uint8> CurrArmFacing;
  virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
  // Helper for rotational interpolation of held items
  FVector ComputeArmGripAtYaw(const FArmState &S, float YawDeg,
                              const AActor *Anchor, float Tile, float GridYaw,
                              EHexOrientation Ori) const;

  // Render animated orbs for input/output effects
  void RenderAnimatedOrbs();

  // Render transmutation merge orbs (ring orbs converging toward center)
  void RenderTransmutationOrbs();

  // Track what we are currently bound to (to avoid duplicates and to unbind
  // safely)
  TWeakObjectPtr<class AOrbSimDriver> BoundDriver;
  bool bStepBound = false;
};