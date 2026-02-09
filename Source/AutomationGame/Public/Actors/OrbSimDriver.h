#pragma once

#include "AutomationGameMinimal.h"
#include "GameFramework/Actor.h"
#include "Sim/HexCoord.h"
// .generated must be last
#include "OrbSimDriver.generated.h"
class USimWorldHex;
class AGameManager;
class UGoldbergGrid;

/** Broadcasts after simulation has stepped */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnSimStepComplete, float,
                                            StepSeconds);

/**
 * Drives the hex-world simulation.
 * IMPORTANT: Simulation steps ONLY when GameTickManager broadcasts OnFixedTick.
 * Tick() is used only to update an interpolation alpha for rendering.
 */
UCLASS()
class AUTOMATIONGAME_API AOrbSimDriver : public AActor {
  GENERATED_BODY()

public:
  AOrbSimDriver();

  virtual void BeginPlay() override;
  virtual void Tick(float DeltaSeconds) override;
#if WITH_EDITOR
  virtual void
  PostEditChangeProperty(FPropertyChangedEvent &PropertyChangedEvent) override;
#endif

  /** Called by GameTickManager each fixed step. */
  UFUNCTION() void HandleFixedTickSeconds(float StepSeconds);

  /** Fired after simulation has stepped - useful for updating visuals */
  UPROPERTY(BlueprintAssignable, Category = "Simulation")
  FOnSimStepComplete OnSimStepComplete;

  /** Update trigger tile signals immediately after arms move */
  void HandleMidStepTriggerUpdate();

  // ---------------------------------------------------------------------
  // Grid / environment knobs (consumed by placement & rendering)
  // ---------------------------------------------------------------------

  /** Centralized grid anchor (used by placement & picking). */
  UPROPERTY(EditAnywhere, Category = "Grid")
  AActor *GridAnchor = nullptr;

  /** If true, use GridAnchor's Z for the mouse hit plane. */
  UPROPERTY(EditAnywhere, Category = "Grid")
  bool bUseAnchorZForPicking = true;

  /** Visual conventions for belts */
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Belts")
  int32 BeltVisualDirOffset = 0;

  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Belts")
  bool bBeltYawClockwise = true;

  /** Debug toggles */
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
  bool bDebugLogs = false;

  /** Draw debug spheres at all orb simulation locations every frame.
   * Shows: Board orbs (colored by type), held orbs (yellow/orange), cluster
   * sub-orbs with offsets */
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
  bool bDebugSpheres = false;

  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
  bool bDebugLines = false;

  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
  bool bDebugBonds = false;

  /** Draw red spheres on tiles that are in the way of arm commands */
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
  bool DebugCollision = false;

  /** Size of debug spheres for all orb types */
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug",
            meta = (ClampMin = "1.0", ClampMax = "50.0"))
  float DebugSphereSize = 8.0f;

  /** Optional reference to a manager if you want to sync knobs externally (not
   * required). */
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Framework")
  AGameManager *GM = nullptr;

  /** When true, Level 0 objects are displayed at Level 1's Z position and vice
   * versa. This allows viewing the "underside" grid without moving the camera.
   * The simulation Level values remain unchanged - only display positions swap.
   */
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid")
  bool bDisplayFlipped = false;

  /** Set the display flip state (swaps Level 0 and Level 1 display positions)
   */
  UFUNCTION(BlueprintCallable, Category = "Grid")
  void SetDisplayFlipped(bool bFlipped) { bDisplayFlipped = bFlipped; }

  /** Get whether display is currently flipped */
  UFUNCTION(BlueprintPure, Category = "Grid")
  bool IsDisplayFlipped() const { return bDisplayFlipped; }

  // ---------------------------------------------------------------------
  // Accessors used across the project (kept public so other systems compile)
  // ---------------------------------------------------------------------

  UFUNCTION(BlueprintCallable, Category = "Sim")
  USimWorldHex *GetSim() const { return Sim; }

  /** Interp alpha in [0..1) within the current fixed step (for render lerp). */
  UFUNCTION(BlueprintCallable, Category = "Sim")
  float GetStepAlpha() const { return StepAlpha; }

  UFUNCTION(BlueprintCallable, Category = "Grid")
  AActor *GetGridAnchor() const { return GridAnchor; }
  UFUNCTION(BlueprintCallable, Category = "Grid")
  float GetGridPlaneZ(const AActor *FallbackOwner) const;
  UFUNCTION(BlueprintCallable, Category = "Grid")
  EHexOrientation GetGridOrientation() const { return GridOrientation; }
  UFUNCTION(BlueprintCallable, Category = "Grid") float GetGridYaw() const {
    return GridYawDeg;
  }
  UFUNCTION(BlueprintCallable, Category = "Grid") float GetTileSize() const {
    return TileSizeUU;
  }

  /** Set grid parameters from GameManager (source of truth) */
  UFUNCTION(BlueprintCallable, Category = "Grid")
  void SetGridParameters(EHexOrientation InOrientation, float InYawDeg,
                         float InTileSizeUU, float InOrbZOffset,
                         AActor *InAnchor);

  // Set Goldberg mode and grid reference (called by GameManager)
  void SetGoldbergMode(bool bEnable, UGoldbergGrid *InGrid) {
    bGoldbergMode = bEnable;
    CachedGoldbergGrid = InGrid;
  }
  bool IsGoldbergMode() const { return bGoldbergMode; }
  UGoldbergGrid *GetGoldbergGrid() const { return CachedGoldbergGrid; }

  // Backward compatibility: IsSphereEnabled now returns whether Goldberg mode
  // is active (Goldberg grid is a curved surface like the old sphere mode)
  bool IsSphereEnabled() const { return bGoldbergMode; }

  /** Sync debug settings to all BasicArm actors in the world */
  UFUNCTION(BlueprintCallable, Category = "Debug")
  void SyncDebugSettingsToArms();

  /** Force sync debug settings to arms and sim (useful for runtime changes) */
  UFUNCTION(BlueprintCallable, Category = "Debug")
  void ForceSyncDebugSettings();

  /** Map a simulation hex coordinate to world space (handles sphere mapping if
   * enabled). */
  UFUNCTION(BlueprintCallable, Category = "Grid")
  FVector MapHexToWorld(const FHex &H) const;

  /** Inverse of MapHexToWorld (best-effort for sphere mapping). Returns true if
   * success. */
  UFUNCTION(BlueprintCallable, Category = "Grid")
  bool WorldToHex(const FVector &World, FHex &OutHex) const;

  /** Draw debug spheres for all orbs in the simulation.
   * Called every frame in Tick() when bDebugSpheres is enabled.
   * Shows the current simulation state (target positions) without
   * interpolation. Draws spheres for:
   * - Board items (single orbs and clusters) with type-based colors
   * - Held items by arms (yellow/orange) with proper arm rotation
   * - Cluster sub-orbs with hex coordinate offsets
   * - Debug text showing item IDs and cluster information */
  void DrawAllOrbDebugSpheres();

  /** Draw red spheres on tiles that are blocking arm commands.
   * Called every frame in Tick() when DebugCollision is enabled.
   * Shows collision detection for arm movements and rotations. */
  void DrawCollisionDebugSpheres();

  /** Get debug color for orb type */
  FColor GetDebugColorForOrbType(uint8 OrbType) const;

  /** Get debug sphere size (uses DebugSphereSize variable for all orbs) */
  float GetDebugSizeForOrbType(uint8 OrbType, bool bIsSubOrb) const;

  UFUNCTION(BlueprintCallable, Category = "Belts")
  int32 GetBeltVisualOffset() const { return BeltVisualDirOffset; }
  UFUNCTION(BlueprintCallable, Category = "Belts")
  bool IsBeltYawClockwise() const { return bBeltYawClockwise; }

  /** Utility to find the first driver in the world. */
  static AOrbSimDriver *Find(UWorld *World);

private:
  UPROPERTY(Transient)
  USimWorldHex *Sim = nullptr;

  /** Interpolation factor [0..1]; reset to 0 each fixed step. */
  float StepAlpha = 0.f;

  bool bSubscribed = false;

  /** Grid parameters (set by GameManager - source of truth) */
  EHexOrientation GridOrientation = EHexOrientation::PointyTop;
  float GridYawDeg = 0.f;
  float TileSizeUU = 120.f;
  float OrbZOffset = 50.f;

  // Goldberg mode state
  bool bGoldbergMode = false;

  UPROPERTY(Transient)
  UGoldbergGrid *CachedGoldbergGrid = nullptr;
};
