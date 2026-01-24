#pragma once

#include "Components/SkeletalMeshComponent.h"
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Sim/ArmTypes.h" // FOp, ECmd
#include "Sim/HexCoord.h" // Axial<->XY helpers
#include "Sim/HexTypes.h" // FHex
#include "Sim/OrbTypes.h" // FOrb
// .generate.h must be last
#include "BasicArm.generated.h"

class AOrbSimDriver;
class AGameTickManager;
class UStaticMeshComponent;
class USimWorldHex;
class UArmTimelineWidget;

/** Discrete arm instructions (Opus Magnum–style, kept simple to start) */
UENUM(BlueprintType)
enum class EArmCmd : uint8 {
  NoOp UMETA(DisplayName = "NoOp"),
  Grab UMETA(DisplayName = "Grab"),
  Drop UMETA(DisplayName = "Drop"),
  RotateCW UMETA(DisplayName = "Rotate CW (60° * Arg)"),
  RotateCCW UMETA(DisplayName = "Rotate CCW (60° * Arg)"),
  Wait UMETA(DisplayName = "Wait (ticks = Arg)"),
  Reset UMETA(DisplayName = "Reset to Start (macro)"),
  Extend UMETA(DisplayName = "Extend (+Arg, default 1)"),           // NEW
  Retract UMETA(DisplayName = "Retract (-Arg, default 1)"),         // NEW
  RotateHeldCW UMETA(DisplayName = "Rotate Held CW (60° * Arg)"),   // NEW
  RotateHeldCCW UMETA(DisplayName = "Rotate Held CCW (60° * Arg)"), // NEW
  WaitForBond UMETA(DisplayName = "Wait For Bond")                  // NEW
};

/** One instruction + small integer argument (dir steps, wait ticks, etc.) */
USTRUCT(BlueprintType)
struct FArmInstr {
  GENERATED_BODY()

  UPROPERTY(EditAnywhere, BlueprintReadWrite)
  EArmCmd Cmd = EArmCmd::NoOp;

  /** For Rotate/Wait, meaning depends on Cmd. Ignored otherwise. */
  UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = 0, UIMin = 0))
  int32 Arg = 0;

  // Added equality operators so TArray<FArmInstr>::operator== works
  bool operator==(const FArmInstr &Other) const {
    return Cmd == Other.Cmd && Arg == Other.Arg;
  }
  bool operator!=(const FArmInstr &Other) const { return !(*this == Other); }
};

UCLASS()
class AUTOMATIONGAME_API ABasicArm : public AActor {
  GENERATED_BODY()

public:
  ABasicArm();

  // Engine
  virtual void OnConstruction(const FTransform &Transform) override;
  virtual void BeginPlay() override;
  virtual void Tick(float DeltaSeconds) override;

  // --- Grid / placement ---
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid")
  AOrbSimDriver *Driver = nullptr;

  /** Axial tile where the arm’s pivot sits. */
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid")
  int32 Q = 0;

  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid")
  int32 R = 0;

  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid")
  int32 Level = 0;

  /** Facing dir (0..5), where +1 means +60° CW. */
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid",
            meta = (ClampMin = 0, ClampMax = 5))
  uint8 FacingDir = 0;

  /** Convenience: set grid placement & driver in one call. */
  UFUNCTION(BlueprintCallable, Category = "Grid")
  void InitializeOnGrid(AOrbSimDriver *InDriver, int32 InQ, int32 InR,
                        int32 InLevel, uint8 InFacing);

  // --- Visuals ---
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visual")
  USkeletalMesh *ArmSkeletalMesh = nullptr;

  // Animation Blueprint class for the arm
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visual")
  TSubclassOf<UAnimInstance> ArmAnimationBlueprintClass = nullptr;

  // Extension range for alpha calculation
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Animation")
  float MinExtensionReach = 1.0f;

  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Animation")
  float MaxExtensionReach = 3.0f;

  // Rapid command response settings
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Animation")
  float RapidCommandSpeedMultiplier =
      3.0f; // Speed multiplier for rapid command changes

  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Animation")
  float TargetChangeThreshold =
      0.1f; // Threshold for detecting significant changes

  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Animation")
  bool bUseInstantResponse =
      false; // If true, skip interpolation for instant response

  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visual")
  UStaticMesh *HandMesh = nullptr;

  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visual")
  float BaseZ = 10.f;

  /** Tile-based reach: 1 = adjacent target tile center. */
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visual",
            meta = (ClampMin = 1))
  int32 ReachTiles = 1;

  /** Starting reach distance for this arm's commands. Used to initialize
   * ReachTiles. */
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Program",
            meta = (ClampMin = 1))
  int32 StartingReach = 1;

  /** Starting rotation (facing direction) for this arm's commands. Used to
   * initialize FacingDir. */
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Program",
            meta = (ClampMin = 0, ClampMax = 5))
  uint8 StartingRotation = 0;

  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visual")
  FVector BaseScale = FVector(1.f);

  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visual")
  FVector HandScale = FVector(0.4f);

  /** Bone names for skeletal mesh control */
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visual")
  FName RotatingBase2BoneName = TEXT("RotatingBase2");

  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visual")
  FName BaseGear7BoneName = TEXT("BaseGear7");

  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visual")
  FName HoloscopicGear3BoneName = TEXT("HoloscopicGear3");

  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visual")
  FName Base1BoneName = TEXT("Base1");

  // Telescopic arm bone names (largest to smallest)
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visual")
  FName ArmBase4BoneName = TEXT("ArmBase4");

  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visual")
  FName ArmMid5BoneName = TEXT("ArmMid5");

  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visual")
  FName ArmTip6BoneName = TEXT("ArmTip6");

  /** Bone rotation offsets to compensate for initial skeletal mesh orientations
   */
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visual|BoneOffsets")
  FRotator RotatingBase2RotationOffset = FRotator::ZeroRotator;

  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visual|BoneOffsets")
  FRotator BaseGear7RotationOffset = FRotator::ZeroRotator;

  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visual|BoneOffsets")
  FRotator HoloscopicGear3RotationOffset = FRotator::ZeroRotator;

  /** Additive yaw (degrees) applied to the base mesh only, after grid yaw +
   * facing. */
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visual")
  float BaseYawOffsetDeg = 0.f;

  // Optional held-orb preview (purely visual)
  // Note: Orb meshes and materials are now managed by UOrbMaterialManager

  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visual")
  FVector HeldOrbScale = FVector(0.2f);
  float EffectiveReachStart = 1.f;
  float EffectiveReachEnd = 1.f;
  float LastExtendSegmentMultiplier = 1.0f;
  /**
   * Multiplier applied ONLY to each additional segment beyond the first when
   * computing visual distance. Effective visual reach = 1 + (ReachTiles - 1) *
   * ExtensionSegmentMultiplier. ReachTiles == 1 always yields exactly 1 (no
   * scaling).
   */
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visual",
            meta = (ClampMin = 0.01))
  float ExtensionSegmentMultiplier = 1.5f;
  float ComputeEffectiveReach(int32 Tiles) const {
    return (Tiles <= 1) ? 1.f
                        : 1.f + float(Tiles - 1) * ExtensionSegmentMultiplier;
  }
  // --- Program control (Blueprint-friendly) ---
  /** Program editable in BP (looped if bLoopProgram). */
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Program")
  TArray<FArmInstr> Program;

  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Program")
  bool bLoopProgram = true;

  /** Replace Program at runtime. */
  UFUNCTION(BlueprintCallable, Category = "Program")
  void SetProgram(const TArray<FArmInstr> &InProgram, bool bLoop = true);

  /** Control */
  UFUNCTION(BlueprintCallable, Category = "Program")
  void Start();

  UFUNCTION(BlueprintCallable, Category = "Program")
  void Stop();

  UFUNCTION(BlueprintCallable, Category = "Program")
  void ResetProgram();
  UFUNCTION(BlueprintCallable, Category = "Program")
  void SyncFromSim(const FArmState &SimState); // also sets SimArmIndex

  UFUNCTION(BlueprintCallable, Category = "Program")
  void SyncFromCurrentSim(); // sync from current sim state

  UFUNCTION(BlueprintCallable, Category = "Program")
  void UpdateHeldClusterOnly(); // update only the held cluster visual

  /** Check if the arm is currently animating (rotating, etc.) */
  UFUNCTION(BlueprintPure, Category = "Program")
  bool IsAnimating() const { return StepAlpha < 1.0f; }

  /** Get the current step alpha for debugging */
  UFUNCTION(BlueprintPure, Category = "Program")
  float GetStepAlpha() const { return StepAlpha; }

  /** Get whether the arm is currently holding something */
  UFUNCTION(BlueprintPure, Category = "Program")
  bool IsHolding() const { return bHolding; }

  /** Get the starting facing direction */
  UFUNCTION(BlueprintPure, Category = "Program")
  uint8 GetStartFacingDir() const { return StartFacingDir; }

  /** Get the current visual yaw (interpolated during rotation) */
  float GetVisualYawDeg() const;

  /** For debugging: execute current instruction immediately (ignores Wait). */
  UFUNCTION(BlueprintCallable, Category = "Program")
  void ExecuteCurrent();

  /** Sync debug settings from the OrbSimDriver */
  UFUNCTION(BlueprintCallable, Category = "Debug")
  void SyncDebugSettingsFromDriver();
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sim")
  int32 SimArmIndex;

  // Effective (scaled) visual reach in tile units (not world units).
  UFUNCTION(BlueprintPure, Category = "Visual")
  float GetEffectiveReach() const {
    return ReachTiles <= 1
               ? 1.f
               : 1.f + float(ReachTiles - 1) * ExtensionSegmentMultiplier;
  }

public:
  void PushProgramToSim();
  void PushRuntimePropertiesToSim();
  TArray<FOp> BuildOpsFromProgram(const TArray<FArmInstr> &InProgram) const;
  TArray<FArmInstr> ConvertTapeToArmInstructions(const TArray<FOp> &Tape) const;
  /** If true, cluster sub-orb offsets are multiplied by TileSize (grid units ->
   * UU). */
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Orb Visuals")
  bool bScaleClusterOffsetsByTile = true;

  /** Extra scalar applied after (optional) tile scaling. */
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Orb Visuals",
            meta = (ClampMin = 0.01))
  float ClusterOffsetScale = 1.f;

  /** Show the container/static mesh for a cluster (sphere) in addition to
   * sub-orbs. Usually false. */
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Orb Visuals")
  bool bShowClusterContainerMesh = false;
  UFUNCTION(BlueprintCallable, Category = "Program")
  void RebuildTapeFromProgram(); // runtime/editor safe manual push

  /** Update StartingReach and sync to sim/visuals */
  UFUNCTION(BlueprintCallable, Category = "Program")
  void SetStartingReach(int32 NewReach);

  /** Update StartingRotation and sync to sim/visuals */
  UFUNCTION(BlueprintCallable, Category = "Program")
  void SetStartingRotation(int32 NewRotation);

#if WITH_EDITOR
  virtual void PostEditChangeProperty(FPropertyChangedEvent &Event) override;
#endif

protected:
  // Fixed tick hookup
  UFUNCTION()
  void HandleFixedTickSeconds(float StepSeconds);

private:
  // Components
  UPROPERTY(Transient) USkeletalMeshComponent *BaseComp = nullptr;

  // Animation Blueprint instance for telescopic bone control
  UPROPERTY(Transient) UAnimInstance *ArmAnimInstance = nullptr;

  UPROPERTY(Transient) UStaticMeshComponent *HandComp = nullptr;
  UPROPERTY(Transient) UStaticMeshComponent *HeldOrbComp = nullptr;

  // --- Reach interpolation (visual only) ---
  // Reach at start/end of current fixed step for smooth Extend/Retract visual
  // tween
  float ReachVisualStart = 1.f;
  float ReachVisualEnd = 1.f; // target logical reach for this step

  // Track last applied multiplier to update interpolation smoothly
  float LastExtensionSegmentMultiplier = 1.0f;

  // Animation Blueprint lerp variables (like rotation system)
  float LastExtendValue = 0.0f;    // Start position for interpolation
  float CurrExtendValue = 0.0f;    // Target position for interpolation
  float CurrentExtendValue = 0.0f; // Current interpolated position
  float ExtendLerpSpeed = 5.0f;    // How fast to lerp to target

  // Animation Blueprint update function
  void UpdateAnimationBlueprintExtend(float DeltaSeconds);

  // Ensure Animation Blueprint instance is available
  void EnsureAnimationBlueprintInstance();

  // --- Held cluster visual support ---
  UPROPERTY(Transient)
  TArray<UStaticMeshComponent *> HeldClusterComps; // One per sub-orb
  TArray<FVector2D>
      HeldClusterLocalOffsets;     // Raw (unrotated) 2D offsets from FOrbData
  void RebuildHeldClusterVisual(); // Build from HeldOrb
  void ClearHeldClusterVisual(bool bKeepParent = true);
  void UpdateHeldClusterTransforms(
      float CurrentYawDeg); // Called each tick after RefreshPose

  // Sim ref
  UPROPERTY(Transient) USimWorldHex *Sim = nullptr;

  // State
  bool bRunning = true;
  bool bHolding = false; // holding an orb from Sim
  FItem HeldOrb;         // Holds the actual item (single orb or cluster)
  int32 PC = 0;          // program counter
  int32 WaitLeft = 0;    // ticks left for a Wait

  // Smooth yaw animation across fixed steps
  float StepSecondsCur = 0.f;
  float StepAlpha = 0.f;  // 0..1 inside current fixed step (local)
  float LastYawDeg = 0.f; // snapshot at step start
  float CurrYawDeg = 0.f; // target yaw derived from FacingDir

  // Previous hand position for smooth interpolation
  FVector LastHandWorld = FVector::ZeroVector;
  FHex LastTargetHex = FHex(0, 0);

  // Remember where Reset should point us
  uint8 StartFacingDir = 0; // captured at BeginPlay

  // Track extension state for BaseGear7 rotation direction
  bool bIsExtending =
      false; // true when arm is extending, false when retracting

  // Track when executing Extend/Retract commands for HoloscopicGear3
  bool bExecutingExtendRetract =
      false; // true when executing Extend or Retract commands

  // HoloscopicGear3 rotation tracking for continuous spinning
  float HoloscopicGear3CurrYaw = 0.f; // accumulated Yaw rotation (never resets)

  int32 LastDisplayLevel = -1;

  // --- Orb mesh/material mapping ---

  // Helpers
  void EnsureComps();
  void RefreshMeshes();
  void RefreshPose(float InAlpha); // update transforms
  void
  UpdateBoneRotations(float InAlpha); // update skeletal mesh bone rotations

  FVector HexToWorld(const FHex &H) const; // grid → world
  float DirToYaw(uint8 Dir) const;         // grid yaw + 60° steps

  FHex DirStep(uint8 Dir) const;
  FHex ComputeTargetHex() const;

  void StepProgram();                       // advance one instruction (or wait)
  void ApplyRotateSteps(int32 StepsSigned); // +CW, -CCW
  void ExpandResetAtPC(); // turn Reset into Drop(if holding)+Rotate

  // Visual held-orb toggle (purely cosmetic)
  void ShowHeldOrb(bool bShow);
  void AttachHeldOrb();
  void DetachHeldOrb();
  UStaticMesh *GetMeshForOrbType(uint8 OrbType) const;
  UMaterialInterface *GetMaterialForOrbColor(uint8 OrbColor) const;

  // NEW: track previous held item to know when to rebuild visual
  int32 LastHeldItemId = 0;
  int32 LastHeldOrbCount = 0;

  // NEW helper: update (attach/detach) held orb visual when state changes
  void UpdateHeldOrbVisual();

  UStaticMesh *ResolveOrbMesh(const FItem &Item) const;            // NEW
  UMaterialInterface *ResolveOrbMaterial(const FItem &Item) const; // NEW

  // Bonded orb mesh generation
  void GenerateBondedOrbMeshes();

  // Helper to get bonded orb properties from OrbRenderer
  UStaticMesh *GetBondedOrbMesh() const;
  UMaterialInterface *GetBondedOrbMaterial() const;
  float GetBondedOrbScale() const;

  // --- Add near other private members (after LastHeldOrbCount for example)

  // Held cluster rotation (visual interpolation for RotateHeld commands)
  float HeldClusterFacingStartDeg = 0.f;
  float HeldClusterFacingEndDeg = 0.f;
  int32 LastHeldGrabFacing = -1; // last seen HeldOrb.GrabFacing from sim (0..5)
  bool bHeldClusterRotating = false;

  // Raw (unrotated) 2D offsets per sub‑orb in local cluster space (tile -> UU
  // already scaled)
  TArray<FVector2D> HeldClusterRawOffsets;

  // Helper returns current interpolated cluster yaw in degrees (0..360)
  float GetCurrentClusterFacingDeg() const {
    if (!bHeldClusterRotating)
      return HeldClusterFacingEndDeg;
    // Use same StepAlpha that drives arm interpolation for consistency
    return FMath::Lerp(HeldClusterFacingStartDeg, HeldClusterFacingEndDeg,
                       StepAlpha);
  }

public:
  // If > 0 overrides ReachTiles * TileSize for grip distance (UU)
  UPROPERTY(EditAnywhere, Category = "Arm|Grip")
  float GripDistanceOverride = 0.f;

  // Vertical (Z) offset for the grip pivot relative to base component
  UPROPERTY(EditAnywhere, Category = "Arm|Grip")
  float GripZOffset = 0.f;

  // Extra lateral (Y) offset if your mesh forward axis is not centered
  UPROPERTY(EditAnywhere, Category = "Arm|Grip")
  float GripSideOffset = 0.f;

  // Additional offset for fine-tuning grip position (in world space)
  UPROPERTY(EditAnywhere, Category = "Arm|Grip")
  FVector GripLocationOffset = FVector::ZeroVector;

  // If true we still try to use a socket; if false we force procedural pivot
  // usage
  UPROPERTY(EditAnywhere, Category = "Arm|Grip")
  bool bUseGripSocket = false;

  /** If the authored arm mesh forward is flipped, invert cluster visual layout
   * (does not affect logic). */
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Orb Visuals")
  bool bInvertClusterVisualFacing = true; // RESTORED

  /** Local rotation applied to the procedural grip pivot after its location
   * update. */
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Arm|Grip")
  FRotator GripPivotLocalRotation = FRotator::ZeroRotator; // RESTORED

  // ------------ Timeline Widget ------------
  // NOTE: Timeline settings are controlled by GameManager, not individual arms
  // Use GameManager's "Arm Timeline" category to configure all timeline
  // properties

  /** Timeline widget component for displaying command sequence (controlled by
   * GameManager) */
  UArmTimelineWidget *TimelineWidget = nullptr;

  /** Whether to show the timeline widget (controlled by GameManager) */
  bool bShowTimeline = false;

  /** Initialize timeline widget */
  UFUNCTION(BlueprintCallable, Category = "Timeline")
  void InitializeTimelineWidget();

  /** Update timeline widget with current command */
  UFUNCTION(BlueprintCallable, Category = "Timeline")
  void UpdateTimelineWidget();

  /** Set timeline visibility */
  UFUNCTION(BlueprintCallable, Category = "Timeline")
  void SetTimelineVisible(bool bVisible);

  /** Get the arm's current world position based on hex coordinates */
  UFUNCTION(BlueprintPure, Category = "Timeline")
  FVector GetArmWorldPosition() const;

protected:
  UPROPERTY(Transient)
  USceneComponent *GripPivotComp = nullptr;

  FVector ComputeGripLocalOffset() const;

public:
  UPROPERTY(EditAnywhere, Category = "Debug")
  bool bDebugHandLocation = false;

  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
  bool bDebugLogs = false;

  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
  bool bDebugSpheres = false;

  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
  bool bDebugLines = false;

  // Bonded orb mesh properties (get from OrbRenderer)
  UPROPERTY(EditAnywhere, Category = "Bonded Orbs")
  bool bShowBondedOrbs = true;
};
