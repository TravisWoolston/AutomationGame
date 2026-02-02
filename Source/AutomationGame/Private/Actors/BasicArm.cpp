#include "Actors/BasicArm.h"
#include "AGLogChannels.h"
#include "Actors/HexFactoryActor.h"
#include "Actors/OrbSimDriver.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Data/OrbMaterialManager.h"
#include "DrawDebugHelpers.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Framework/GameManager.h"
#include "Framework/GameTickManager.h"
#include "Rendering/OrbRenderer.h"
#include "Sim/SimWorldHex.h"
#include "UI/ArmTimelineUMGWidget.h"
#include "UI/ArmTimelineWidget.h"
#include "UObject/UnrealType.h"

static inline float NormDeg(float D) { return FMath::UnwindDegrees(D); }
namespace {
inline int32 Mod6(int32 V) { return (V % 6 + 6) % 6; }
} // namespace

static const TCHAR *CmdName(EArmCmd C) {
  switch (C) {
  case EArmCmd::NoOp:
    return TEXT("NoOp");
  case EArmCmd::Grab:
    return TEXT("Grab");
  case EArmCmd::Drop:
    return TEXT("Drop");
  case EArmCmd::RotateCW:
    return TEXT("RotateCW");
  case EArmCmd::RotateCCW:
    return TEXT("RotateCCW");
  case EArmCmd::Wait:
    return TEXT("Wait");
  case EArmCmd::Reset:
    return TEXT("Reset");
  case EArmCmd::RotateHeldCW:
    return TEXT("RotateHeldCW");
  case EArmCmd::RotateHeldCCW:
    return TEXT("RotateHeldCCW");
  case EArmCmd::WaitForBond:
    return TEXT("WaitForBond");
  default:
    return TEXT("<?>");
  }
}

ABasicArm::ABasicArm() {
  PrimaryActorTick.bCanEverTick = true; // we interpolate visuals in Tick

  SimArmIndex =
      -1; // Initialize to invalid index to prevent accidental overwrites
  LastHeldItemId = 0; // NEW

  USceneComponent *Root =
      CreateDefaultSubobject<USceneComponent>(TEXT("ArmRoot"));
  SetRootComponent(Root);

  BaseComp = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("ArmBase"));
  BaseComp->SetupAttachment(Root);
  BaseComp->SetMobility(EComponentMobility::Movable);

  HeldOrbComp =
      CreateDefaultSubobject<UStaticMeshComponent>(TEXT("HeldOrbPreview"));
  HeldOrbComp->SetupAttachment(BaseComp); // Attach directly to BaseComp
  HeldOrbComp->SetMobility(EComponentMobility::Movable);
  HeldOrbComp->SetVisibility(false, true);

  GripPivotComp = CreateDefaultSubobject<USceneComponent>(TEXT("GripPivot"));
  GripPivotComp->SetupAttachment(BaseComp);

  // Initialize timeline widget
  TimelineWidget =
      CreateDefaultSubobject<UArmTimelineWidget>(TEXT("TimelineWidget"));
  TimelineWidget->SetupAttachment(GetRootComponent());
}

void ABasicArm::OnConstruction(const FTransform &Transform) {
  Super::OnConstruction(Transform);
  EnsureComps();
  RefreshMeshes();

  CurrYawDeg = DirToYaw(FacingDir);
  LastYawDeg = CurrYawDeg; // lock start == target
  RefreshPose(1.f);

  EffectiveReachStart = EffectiveReachEnd = ComputeEffectiveReach(ReachTiles);
  LastExtensionSegmentMultiplier = ExtensionSegmentMultiplier;
}

void ABasicArm::BeginPlay() {
  Super::BeginPlay();

  EnsureComps();
  RefreshMeshes();

  StartFacingDir = FacingDir;

  // Initialize ReachTiles from StartingReach if not already set
  if (StartingReach > 0) {
    ReachTiles = FMath::Clamp(StartingReach, 1, 6);
  }

  // auto-find driver if not set
  if (!Driver) {
    if (UWorld *W = GetWorld()) {
      Driver = AOrbSimDriver::Find(W);
    }
  }
  Sim = (Driver ? Driver->GetSim() : nullptr);
  if (!Sim) {
    UE_LOG(LogAGArm, Warning,
           TEXT("BasicArm '%s': No Sim found; Grab/Drop will be visual only."),
           *GetName());
  }

  // hook shared fixed tick
  if (AGameTickManager *M = AGameTickManager::Get(this)) {
    M->OnFixedTick.RemoveDynamic(this, &ABasicArm::HandleFixedTickSeconds);
    M->OnFixedTick.AddDynamic(this, &ABasicArm::HandleFixedTickSeconds);

  } else {
    UE_LOG(LogAGArm, Warning,
           TEXT("BasicArm '%s': No GameTickManager; will not step."),
           *GetName());
  }

  UE_LOG(LogAGArm, Log,
         TEXT("BasicArm '%s' BeginPlay: Q=%d R=%d Facing=%d Driver=%s"),
         *GetName(), Q, R, (int32)FacingDir,
         Driver ? *Driver->GetName() : TEXT("null"));

  CurrYawDeg = DirToYaw(FacingDir);
  LastYawDeg = CurrYawDeg;
  RefreshPose(1.f);

  // Update skeletal mesh bone rotations
  UpdateBoneRotations(1.f);

  PushRuntimePropertiesToSim(); // NEW

  EffectiveReachStart = EffectiveReachEnd = ComputeEffectiveReach(ReachTiles);
  LastExtensionSegmentMultiplier = ExtensionSegmentMultiplier;

  // Initialize timeline widget
  InitializeTimelineWidget();
  // Ensure we have the Animation Blueprint instance
  EnsureAnimationBlueprintInstance();
}
void ABasicArm::Tick(float DeltaSeconds) {
  Super::Tick(DeltaSeconds);

  if (bDebugLogs) {
    UE_LOG(LogAGArm, Verbose, TEXT("Arm '%s': Tick called - DeltaSeconds=%.3f"),
           *GetName(), DeltaSeconds);
  }

  // Sync all state from simulation immediately
  if (Sim && SimArmIndex >= 0 && SimArmIndex < Sim->Arms.Num()) {
    const FArmState &SimState = Sim->Arms[SimArmIndex];

    // Update orientation and reach immediately
    FacingDir = static_cast<uint8>(SimState.Facing);
    CurrYawDeg = DirToYaw(FacingDir);

    const bool bHaveSim =
        (Sim && SimArmIndex >= 0 && SimArmIndex < Sim->Arms.Num());
    const int32 SimLen = bHaveSim ? Sim->Arms[SimArmIndex].ArmLen : ReachTiles;
    const bool bLenChanged = (SimLen != ReachTiles);
    const bool bMultiplierChanged = !FMath::IsNearlyEqual(
        ExtensionSegmentMultiplier, LastExtendSegmentMultiplier);

    // Set extension state based on current program instruction and execution
    // state
    if (PC < Program.Num()) {
      const FArmInstr &CurrentInstr = Program[PC];
      if (CurrentInstr.Cmd == EArmCmd::Extend) {
        bExecutingExtendRetract = true;
        bIsExtending = true;
        if (bDebugLogs) {
          UE_LOG(LogAGArm, Verbose,
                 TEXT("Arm '%s': Executing Extend command - bIsExtending=%d, "
                      "bExecutingExtendRetract=%d, PC=%d"),
                 *GetName(), bIsExtending ? 1 : 0,
                 bExecutingExtendRetract ? 1 : 0, PC);
        }
      } else if (CurrentInstr.Cmd == EArmCmd::Retract) {
        bExecutingExtendRetract = true;
        bIsExtending = false;
        if (bDebugLogs) {
          UE_LOG(LogAGArm, Verbose,
                 TEXT("Arm '%s': Executing Retract command - bIsExtending=%d, "
                      "bExecutingExtendRetract=%d, PC=%d"),
                 *GetName(), bIsExtending ? 1 : 0,
                 bExecutingExtendRetract ? 1 : 0, PC);
        }
      } else {
        // Not executing Extend/Retract command
        bExecutingExtendRetract = false;
        // Keep bIsExtending as is (don't reset it)
        if (bDebugLogs && (CurrentInstr.Cmd == EArmCmd::Reset ||
                           CurrentInstr.Cmd == EArmCmd::Wait)) {
          UE_LOG(LogAGArm, Verbose,
                 TEXT("Arm '%s': Non-Extend/Retract command - %s, "
                      "bExecutingExtendRetract=%d, PC=%d"),
                 *GetName(), CmdName(CurrentInstr.Cmd),
                 bExecutingExtendRetract ? 1 : 0, PC);
        }
      }
    } else {
      // No program or program finished
      bExecutingExtendRetract = false;
      // Keep bIsExtending as is (don't reset it)
    }

    // Also check if we're in the middle of an Extend/Retract operation based on
    // length changes This ensures we continue rotating during the entire visual
    // animation
    if (bLenChanged) {
      bExecutingExtendRetract = true;
      bIsExtending = (SimLen > ReachTiles);
      if (bDebugLogs) {
        UE_LOG(LogAGArm, Verbose,
               TEXT("Arm '%s': Length change detected - bIsExtending=%d, "
                    "bExecutingExtendRetract=%d (SimLen=%d, ReachTiles=%d)"),
               *GetName(), bIsExtending ? 1 : 0,
               bExecutingExtendRetract ? 1 : 0, SimLen, ReachTiles);
      }
    }

    // Extension state is now set directly based on program instruction above

    if (bHaveSim && (bLenChanged || bMultiplierChanged)) {
      const float CurrentEff =
          FMath::Lerp(EffectiveReachStart, EffectiveReachEnd, StepAlpha);
      EffectiveReachStart = CurrentEff;
      ReachTiles = SimLen;
      EffectiveReachEnd = ComputeEffectiveReach(ReachTiles);
      LastExtendSegmentMultiplier = ExtensionSegmentMultiplier;
    } else if (!bHaveSim && bMultiplierChanged) {
      const float CurrentEff =
          FMath::Lerp(EffectiveReachStart, EffectiveReachEnd, StepAlpha);
      EffectiveReachStart = CurrentEff;
      EffectiveReachEnd = ComputeEffectiveReach(ReachTiles);
      LastExtendSegmentMultiplier = ExtensionSegmentMultiplier;
    }

    // Sync wait state and program counter from simulation
    WaitLeft = SimState.WaitCounter;
    PC = SimState.PC;

    // Update holding state + item payload immediately when simulation state
    // changes
    bool bWasHolding = bHolding;
    bHolding = SimState.bHolding;

    if (bHolding) {
      HeldOrb = SimState.Held;
    } else {
      HeldOrb = FItem();
    }

    if (bWasHolding != bHolding ||
        (bHolding && HeldOrb.Orbs.Num() != LastHeldOrbCount)) {
      UpdateHeldOrbVisual();
      LastHeldOrbCount = bHolding ? HeldOrb.Orbs.Num() : 0;
    }
    if (bHolding && HeldOrb.bIsCluster) {
      const int32 NewGrabFacing = HeldOrb.GrabFacing;
      if (NewGrabFacing != LastHeldGrabFacing) {
        const float CurrentClusterDeg =
            NormDeg(HeldClusterFacingStartDeg +
                    FMath::FindDeltaAngleDegrees(HeldClusterFacingStartDeg,
                                                 HeldClusterFacingEndDeg) *
                        StepAlpha);
        HeldClusterFacingStartDeg = CurrentClusterDeg;
        HeldClusterFacingEndDeg = 60.f * float(NewGrabFacing % 6);
        LastHeldGrabFacing = NewGrabFacing;
      }
    } else {
      LastHeldGrabFacing = -1;
      HeldClusterFacingStartDeg = HeldClusterFacingEndDeg = 0.f;
    }
  }

  // Sync StepAlpha from GameTickManager (single source of truth)
  float PreviousStepAlpha = StepAlpha;
  if (AGameTickManager *TickManager = AGameTickManager::Get(this)) {
    StepAlpha = TickManager->GetStepAlpha();
  }

  // Rotation completion notifications (unchanged)
  if (StepAlpha >= 0.95f && PreviousStepAlpha < 0.95f) {
    if (Sim && SimArmIndex >= 0) {
      Sim->OnArmRotationComplete(SimArmIndex);
    }
  }
  if (StepAlpha >= 0.99f && PreviousStepAlpha < 0.99f) {
    if (Sim && SimArmIndex >= 0) {
      Sim->OnArmRotationFullyComplete(SimArmIndex);
    }
  }
  if (StepAlpha >= 1.0f && Sim && SimArmIndex >= 0) {
    Sim->OnArmRotationFullyComplete(SimArmIndex);
  }

  RefreshPose(StepAlpha);

  // Update Animation Blueprint CurrentExtend variable
  UpdateAnimationBlueprintExtend(DeltaSeconds);

  // Timeline widgets update on simulation steps for performance
}

void ABasicArm::UpdateAnimationBlueprintExtend(float DeltaSeconds) {
  // Calculate alpha value (0.0 to 1.0) for lerping between min and max
  // extension
  const float ClampedReach =
      FMath::Clamp(ReachTiles, MinExtensionReach, MaxExtensionReach);
  float NewTargetExtendValue =
      (ClampedReach - MinExtensionReach) /
      (MaxExtensionReach - MinExtensionReach); // Normalize to 0.0-1.0

  // Only update target during Extend/Retract commands
  if (bExecutingExtendRetract &&
      !FMath::IsNearlyEqual(CurrExtendValue, NewTargetExtendValue, 0.001f)) {
    // New target - start interpolation from current position
    LastExtendValue = CurrentExtendValue;
    CurrExtendValue = NewTargetExtendValue;
  }

  // Linear interpolation using the same StepAlpha as rotation system
  // Interpolate from last position to current target
  CurrentExtendValue = FMath::Lerp(LastExtendValue, CurrExtendValue, StepAlpha);

  // Update Animation Blueprint if available
  if (ArmAnimInstance) {
    // Set the CurrentExtend variable in the Animation Blueprint
    FProperty *CurrentExtendProperty =
        ArmAnimInstance->GetClass()->FindPropertyByName(TEXT("CurrentExtend"));
    FDoubleProperty *DoubleProperty =
        CastField<FDoubleProperty>(CurrentExtendProperty);

    DoubleProperty->SetPropertyValue_InContainer(ArmAnimInstance,
                                                 (double)EffectiveReachEnd);

    if (bDebugLogs) {
      UE_LOG(LogAGArm, Verbose,
             TEXT("Arm '%s': CurrentExtend: %.3f (Reach: %d)"), *GetName(),
             CurrentExtendValue, ReachTiles);
    }
  } else if (bDebugLogs) {
    UE_LOG(LogAGArm, Warning,
           TEXT("Arm '%s': Animation Blueprint instance not found"),
           *GetName());
  }
}

void ABasicArm::HandleFixedTickSeconds(float StepSeconds) {
  StepSecondsCur = StepSeconds;

  // FINALIZE current rendered yaw as starting point for new step
  const float RenderedYaw =
      NormDeg(LastYawDeg +
              FMath::FindDeltaAngleDegrees(LastYawDeg, CurrYawDeg) * StepAlpha);
  LastYawDeg = RenderedYaw;

  const float RenderedClusterDeg =
      NormDeg(HeldClusterFacingStartDeg +
              FMath::FindDeltaAngleDegrees(HeldClusterFacingStartDeg,
                                           HeldClusterFacingEndDeg) *
                  StepAlpha);
  HeldClusterFacingStartDeg = RenderedClusterDeg;

  // For reach, start new interpolation from current interpolated position
  // toward current logical target
  const float CurrentEff =
      FMath::Lerp(EffectiveReachStart, EffectiveReachEnd, StepAlpha);
  EffectiveReachStart = CurrentEff;
  EffectiveReachEnd = ComputeEffectiveReach(ReachTiles);

  // StepAlpha is now synced from OrbSimDriver in Tick(), not reset here
  // This ensures consistent timing across all systems
  RefreshPose(0.f);

  // Update timeline widget on simulation step for performance
  UpdateTimelineWidget();
}

void ABasicArm::EnsureComps() {
  if (!RootComponent) {
    USceneComponent *Root =
        NewObject<USceneComponent>(this, TEXT("ArmRoot_Runtime"));
    SetRootComponent(Root);
    Root->RegisterComponent();
  }
  if (!BaseComp) {
    BaseComp = NewObject<USkeletalMeshComponent>(this, TEXT("ArmBase"));
    BaseComp->SetupAttachment(GetRootComponent());
    BaseComp->RegisterComponent();

    // Set the skeletal mesh
    if (ArmSkeletalMesh) {
      BaseComp->SetSkeletalMesh(ArmSkeletalMesh);
    }

    // Set the Animation Blueprint class
    if (ArmAnimationBlueprintClass) {
      BaseComp->SetAnimInstanceClass(ArmAnimationBlueprintClass);
    }

    // Get the Animation Blueprint instance (may be null initially)
    ArmAnimInstance = BaseComp->GetAnimInstance();

    if (bDebugLogs) {
      UE_LOG(LogAGArm, Verbose,
             TEXT("Arm '%s': EnsureComps - ArmAnimInstance=%s, AnimClass=%s"),
             *GetName(), ArmAnimInstance ? TEXT("Found") : TEXT("NULL"),
             BaseComp->GetAnimClass() ? *BaseComp->GetAnimClass()->GetName()
                                      : TEXT("NULL"));
    }
  }
  if (!HeldOrbComp) {
    HeldOrbComp = NewObject<UStaticMeshComponent>(this, TEXT("HeldOrbPreview"));
    HeldOrbComp->SetupAttachment(BaseComp); // Attach directly to BaseComp
    HeldOrbComp->RegisterComponent();
    HeldOrbComp->SetVisibility(false, true);
  }
  if (!GripPivotComp) {
    GripPivotComp = NewObject<USceneComponent>(this, TEXT("GripPivot"));
    GripPivotComp->SetupAttachment(BaseComp);
    GripPivotComp->RegisterComponent();
  }
  // Set the local rotation offset once during initialization
  GripPivotComp->SetRelativeRotation(GripPivotLocalRotation);
}

void ABasicArm::RefreshMeshes() {
  if (BaseComp) {
    // Calculate display level based on flip state
    int32 DisplayLevel = Level;
    if (Driver) {
      DisplayLevel = Driver->IsDisplayFlipped() ? (1 - Level) : Level;
    }

    USkeletalMesh *TargetMesh = ArmSkeletalMesh;
    if (DisplayLevel == 1) { // Ceiling
      if (UpsideDownArmSkeletalMesh) {
        TargetMesh = UpsideDownArmSkeletalMesh;
      } else if (AHexFactoryActor *Factory =
                     AHexFactoryActor::FindInWorld(GetWorld())) {
        if (Factory->UpsideDownArmMesh) {
          TargetMesh = Factory->UpsideDownArmMesh;
        }
      }
    }

    BaseComp->SetSkinnedAssetAndUpdate(TargetMesh);

    FVector FinalScale = BaseScale;
    if (DisplayLevel == 1) {
      FinalScale *= CeilingVisualScale;
    }
    BaseComp->SetRelativeScale3D(FinalScale);
  }

  if (HeldOrbComp) {
    // Get mesh from centralized system
    UStaticMesh *CentralizedMesh = nullptr;
    if (UOrbMaterialManager *MaterialManager =
            UOrbMaterialManager::FindInWorld(this)) {
      CentralizedMesh = MaterialManager->GetOrbMesh();
    }

    HeldOrbComp->SetStaticMesh(CentralizedMesh);
    HeldOrbComp->SetRelativeScale3D(HeldOrbScale);
    HeldOrbComp->SetVisibility(bHolding && CentralizedMesh != nullptr, true);
  }
}

FVector ABasicArm::HexToWorld(const FHex &H) const {
  if (!Driver)
    return GetActorLocation();
  return Driver->MapHexToWorld(H);
}

float ABasicArm::DirToYaw(uint8 Dir) const {
  const float GridYaw = Driver ? Driver->GetGridYaw() : 0.f;
  const bool bFlat =
      (Driver && Driver->GetGridOrientation() == EHexOrientation::FlatTop);
  const float OriOffset =
      bFlat ? 30.f : 0.f; // mesh authored for pointy -> add 30 when flat
  return NormDeg(GridYaw - 60.f * float(Dir % 6) + OriOffset);
}

float ABasicArm::GetVisualYawDeg() const {
  // Calculate the interpolated yaw based on current animation state
  const float StartYaw = NormDeg(LastYawDeg);
  const float TargetYaw = NormDeg(CurrYawDeg);
  const float Delta = FMath::FindDeltaAngleDegrees(StartYaw, TargetYaw);
  return NormDeg(StartYaw + Delta * StepAlpha);
}

void ABasicArm::RefreshPose(float InAlpha) {
  if (bDebugLogs) {
    UE_LOG(LogAGArm, Verbose,
           TEXT("Arm '%s': RefreshPose called - InAlpha=%.2f"), *GetName(),
           InAlpha);
  }

  float EasedAlpha = InAlpha;

  // Calculate display level based on flip state
  int32 DisplayLevel = Level;
  if (Driver) {
    DisplayLevel = Driver->IsDisplayFlipped() ? (1 - Level) : Level;
  }

  FVector EffectiveVisualScale = FVector::OneVector;
  if (DisplayLevel == 1) {
    EffectiveVisualScale = CeilingVisualScale;
  }

  const float EffectiveBaseZ = BaseZ * EffectiveVisualScale.Z;
  const FVector BaseWorld =
      HexToWorld(FHex(Q, R, Level)) + FVector(0, 0, EffectiveBaseZ);

  // Apply different GripZOffset for ceiling arms since they reach 'up' to the
  // grid
  float EffectiveGripZ = GripZOffset * EffectiveVisualScale.Z;

  const float StartYaw = NormDeg(LastYawDeg);
  const float TargetYaw = NormDeg(CurrYawDeg);
  const float Delta = FMath::FindDeltaAngleDegrees(StartYaw, TargetYaw);
  const float Yaw = NormDeg(StartYaw + Delta * EasedAlpha);
  const float ReachInterp =
      FMath::Lerp(ReachVisualStart, ReachVisualEnd, EasedAlpha);

  const bool bFlat =
      (Driver && Driver->GetGridOrientation() == EHexOrientation::FlatTop);
  const float OriOffset = bFlat ? 30.f : 0.f;

  // For skeletal mesh, we only set the world location and base rotation
  // Individual bone rotations are handled in UpdateBoneRotations
  FRotator BaseRot(0.f, BaseYawOffsetDeg + OriOffset, 0.f);

  const bool bSphere = (Driver && Driver->IsSphereEnabled());
  if (bSphere) {
    const AActor *Anchor = Driver->GetGridAnchor();
    const FVector AnchorLoc =
        Anchor ? Anchor->GetActorLocation() : FVector::ZeroVector;
    FVector Up = (BaseWorld - AnchorLoc).GetSafeNormal();
    if (Up.IsNearlyZero())
      Up = FVector::UpVector;
    FHex FaceHex = FHex(Q, R);
    for (int32 i = 0; i < FMath::Max(1, ReachTiles); ++i) {
      FaceHex = HexUtil::Neighbor(FaceHex, static_cast<EHexDir>(FacingDir));
    }
    const FVector FaceWorld = HexToWorld(FaceHex) + FVector(0, 0, BaseZ);
    FVector Forward = (FaceWorld - BaseWorld).GetSafeNormal();
    if (Forward.IsNearlyZero()) {
      const float Rad = FMath::DegreesToRadians(Yaw);
      Forward = FVector(FMath::Cos(Rad), FMath::Sin(Rad), 0.f);
    }
    FVector Right = FVector::CrossProduct(Forward, Up).GetSafeNormal();
    if (Right.IsNearlyZero())
      Right = FVector::CrossProduct(Up, FVector::ForwardVector).GetSafeNormal();
    Forward = FVector::CrossProduct(Up, Right).GetSafeNormal();
    FMatrix M(Forward, Right, Up, FVector::ZeroVector);
    BaseRot = M.Rotator();
    BaseRot.Add(0.f, BaseYawOffsetDeg + OriOffset, 0.f);
  }
  if (BaseComp) {
    BaseComp->SetWorldLocation(BaseWorld);
    BaseComp->SetWorldRotation(BaseRot);

    FVector FinalScale = BaseScale;
    if (DisplayLevel == 1) {
      FinalScale *= CeilingVisualScale;
    }
    BaseComp->SetRelativeScale3D(FinalScale);
  }

  FVector InterpolatedHandWorld = BaseWorld;
  {
    FVector Forward;
    if (bSphere) {
      FHex TargetHex = ComputeTargetHex();
      const FVector TargetWorld = HexToWorld(TargetHex) + FVector(0, 0, BaseZ);
      Forward = (TargetWorld - BaseWorld).GetSafeNormal();
      if (Forward.IsNearlyZero())
        Forward = BaseRot.Vector();
    } else {
      const float GripYawRad = FMath::DegreesToRadians(Yaw);
      Forward = FVector(FMath::Cos(GripYawRad), FMath::Sin(GripYawRad), 0.f);
    }
    const float Tile = (Driver ? Driver->GetTileSize() : 300.f);
    const float ReachDistance =
        (Driver ? Driver->GetTileSize() : 300.f) *
        FMath::Lerp(EffectiveReachStart, EffectiveReachEnd, EasedAlpha);

    FVector Up =
        bSphere
            ? (BaseWorld - (Driver->GetGridAnchor()
                                ? Driver->GetGridAnchor()->GetActorLocation()
                                : FVector::ZeroVector))
                  .GetSafeNormal()
            : FVector::UpVector;
    if (Up.IsNearlyZero())
      Up = FVector::UpVector;
    FVector Right = FVector::CrossProduct(Forward, Up).GetSafeNormal();
    if (Right.IsNearlyZero())
      Right = FVector::CrossProduct(Up, Forward).GetSafeNormal();
    Forward = FVector::CrossProduct(Up, Right).GetSafeNormal();
    const FVector BaseGripPos =
        BaseWorld + Forward * ReachDistance +
        Right * (GripSideOffset * EffectiveVisualScale.Y) +
        FVector(0, 0, EffectiveGripZ);
    const FVector RelativeOffset =
        Forward * (GripLocationOffset.X * EffectiveVisualScale.X) +
        Right * (GripLocationOffset.Y * EffectiveVisualScale.Y) +
        Up * (GripLocationOffset.Z * EffectiveVisualScale.Z);
    const FVector FinalGripPos = BaseGripPos + RelativeOffset;
    if (bHolding && HeldOrb.bIsCluster && HeldOrb.Orbs.Num() > 0) {
      const FOrbData *GrabbedOrb = nullptr;
      for (const FOrbData &Orb : HeldOrb.Orbs) {
        if (Orb.dQ == 0 && Orb.dR == 0) {
          GrabbedOrb = &Orb;
          break;
        }
      }
      if (GrabbedOrb) {
        const EHexOrientation GridOrientation =
            (Driver ? Driver->GetGridOrientation()
                    : EHexOrientation::PointyTop);
        const float TileSize = (Driver ? Driver->GetTileSize() : 300.f);
        const FVector2D HexOffset2D =
            HexCoord::AxialToXY(FHexAxial(GrabbedOrb->dQ, GrabbedOrb->dR),
                                TileSize, GridOrientation) *
            ClusterOffsetScale;
        const float Inv = bInvertClusterVisualFacing ? -1.f : 1.f;
        InterpolatedHandWorld =
            FinalGripPos - (Forward * HexOffset2D.X * EffectiveVisualScale.X +
                            Right * HexOffset2D.Y * EffectiveVisualScale.Y) *
                               Inv;
      } else {
        InterpolatedHandWorld = FinalGripPos;
      }
    } else {
      InterpolatedHandWorld = FinalGripPos;
    }
  }
  if (GripPivotComp && !bUseGripSocket) {
    GripPivotComp->SetWorldLocation(InterpolatedHandWorld);

    // For skeletal mesh, use the actual bone rotation instead of BaseRot
    FRotator GripRotation = BaseRot + GripPivotLocalRotation;

    // If we have a skeletal mesh with RotatingBase2 bone, use its rotation for
    // grip
    if (BaseComp && BaseComp->GetSkinnedAsset() &&
        RotatingBase2BoneName != NAME_None) {
      const int32 BoneIndex = BaseComp->GetBoneIndex(RotatingBase2BoneName);
      if (BoneIndex != INDEX_NONE) {
        // Get the current bone transform and extract its rotation
        FTransform BoneTransform =
            BaseComp->GetBoneTransform(RotatingBase2BoneName);
        FRotator BoneRotation = BoneTransform.Rotator();

        // Use the bone's Y rotation for the grip, but preserve the base
        // rotation for other axes
        FRotator BaseRotation = BaseRot;
        BaseRotation.Yaw = BoneRotation.Yaw;
        GripRotation = BaseRotation + GripPivotLocalRotation;

        if (bDebugLogs) {
          UE_LOG(LogAGArm, Verbose,
                 TEXT("Arm '%s': Using bone rotation for grip - Bone Yaw=%.2f, "
                      "Grip Yaw=%.2f"),
                 *GetName(), BoneRotation.Yaw, GripRotation.Yaw);
        }
      }
    }

    GripPivotComp->SetWorldRotation(GripRotation);
  }
  // (retain existing debug + cluster code below unchanged)
  if (bDebugHandLocation && GetWorld()) {
    const FVector Start =
        BaseComp ? BaseComp->GetComponentLocation() : GetActorLocation();
    DrawDebugLine(GetWorld(), Start, InterpolatedHandWorld, FColor::Green,
                  false, 0.f, 0, 2.f);
    DrawDebugSphere(GetWorld(), InterpolatedHandWorld, 6.f, 12, FColor::Yellow,
                    false, 0.f);
  }

  // Debug lines for arm reach and target
  if (bDebugLines && GetWorld()) {
    const FHex TargetHex = ComputeTargetHex();
    const FVector TargetWorld = HexToWorld(TargetHex);
    DrawDebugLine(GetWorld(), BaseWorld, TargetWorld, FColor::Red, false, 0.f,
                  0, 2.f);
    DrawDebugLine(GetWorld(), BaseWorld, InterpolatedHandWorld, FColor::Orange,
                  false, 0.f, 0, 1.f);
    const float Tile = Driver ? Driver->GetTileSize() : 100.f;
    DrawDebugCircle(GetWorld(), TargetWorld, Tile * 0.5f, 6, FColor::Red, false,
                    0.f, 0, 2.f, FVector(1, 0, 0), FVector(0, 1, 0), false);
  }

  UpdateHeldClusterTransforms(EasedAlpha);

  // Update skeletal mesh bone rotations
  UpdateBoneRotations(EasedAlpha);

  if (bDebugSpheres && bHolding && HeldOrb.bIsCluster && GetWorld()) {
    const float Tile = (Driver ? Driver->GetTileSize() : 100.f);
    const EHexOrientation GridOrientation =
        (Driver ? Driver->GetGridOrientation() : EHexOrientation::PointyTop);
    const float GridYaw = Driver ? Driver->GetGridYaw() : 0.f;
    const FHex TargetHex = ComputeTargetHex();
    const FVector TargetWorld = HexToWorld(TargetHex);

    // Calculate the interpolated cluster rotation (same as
    // UpdateHeldClusterTransforms)
    const float StartDeg = NormDeg(HeldClusterFacingStartDeg);
    const float EndDeg = NormDeg(HeldClusterFacingEndDeg);
    const float DeltaDeg = FMath::FindDeltaAngleDegrees(StartDeg, EndDeg);
    const float CurrentClusterYawDeg = NormDeg(StartDeg + DeltaDeg * StepAlpha);

    for (int32 i = 0; i < HeldOrb.Orbs.Num(); i++) {
      const FOrbData &Sub = HeldOrb.Orbs[i];

      // Calculate base hex offset
      const FVector2D HexOffset =
          HexCoord::AxialToXY(FHexAxial(Sub.dQ, Sub.dR), Tile, GridOrientation);
      FVector2D Off2D = HexOffset;

      // Apply the interpolated cluster rotation
      const float YawRad = FMath::DegreesToRadians(CurrentClusterYawDeg);
      const float CosA = FMath::Cos(YawRad);
      const float SinA = FMath::Sin(YawRad);
      const float RotX = Off2D.X * CosA - Off2D.Y * SinA;
      const float RotY = Off2D.X * SinA + Off2D.Y * CosA;
      Off2D = FVector2D(RotX, RotY);

      // Apply grid rotation and convert to world position
      const FVector2D GridRotatedOffset = HexCoord::Rotate2D(Off2D, GridYaw);
      const FVector SubWorld =
          TargetWorld + FVector(GridRotatedOffset.X, GridRotatedOffset.Y, 0.f);

      const FColor DebugColor =
          (Sub.dQ == 0 && Sub.dR == 0) ? FColor::Green : FColor::Blue;
      const float SphereSize = (Sub.dQ == 0 && Sub.dR == 0) ? 8.f : 6.f;
      DrawDebugSphere(GetWorld(), SubWorld, SphereSize, 12, DebugColor, false,
                      0.f, 0, 2.f);
      DrawDebugLine(GetWorld(), BaseWorld, SubWorld, DebugColor, false, 0.f, 0,
                    1.f);
      DrawDebugString(GetWorld(), SubWorld + FVector(0, 0, 15.f),
                      FString::Printf(TEXT("dQ:%d dR:%d R:%.1f°"), Sub.dQ,
                                      Sub.dR, CurrentClusterYawDeg),
                      nullptr, DebugColor, 0.f, true, 1.2f);
    }
  }
}

void ABasicArm::UpdateBoneRotations(float InAlpha) {
  if (!BaseComp || !BaseComp->GetSkinnedAsset()) {
    return;
  }

  // Calculate the exact interpolated yaw value (same as rotation system)
  const float StartYaw = NormDeg(LastYawDeg);
  const float TargetYaw = NormDeg(CurrYawDeg);
  const float Delta = FMath::FindDeltaAngleDegrees(StartYaw, TargetYaw);
  const float Yaw = NormDeg(StartYaw + Delta * InAlpha);

  // Ensure we have the Animation Blueprint instance
  EnsureAnimationBlueprintInstance();

  // Pass the exact interpolated value to Animation Blueprint
  // This ensures no additional interpolation happens in the Blueprint
  if (ArmAnimInstance) {

    // Set the exact interpolated rotation value
    FProperty *YawProperty =
        ArmAnimInstance->GetClass()->FindPropertyByName(TEXT("CurrentYaw"));

    FDoubleProperty *DoubleProperty = CastField<FDoubleProperty>(YawProperty);

    DoubleProperty->SetPropertyValue_InContainer(ArmAnimInstance, (double)Yaw);

    // Set extension state for HoloscopicGear3 rotation
    FProperty *ExecutingExtendRetractProperty =
        ArmAnimInstance->GetClass()->FindPropertyByName(
            TEXT("bExecutingExtendRetract"));
    if (ExecutingExtendRetractProperty) {
      FBoolProperty *BoolProperty =
          CastField<FBoolProperty>(ExecutingExtendRetractProperty);
      if (BoolProperty) {
        BoolProperty->SetPropertyValue_InContainer(ArmAnimInstance,
                                                   bExecutingExtendRetract);
      }
    }

    FProperty *IsExtendingProperty =
        ArmAnimInstance->GetClass()->FindPropertyByName(TEXT("bIsExtending"));
    if (IsExtendingProperty) {
      FBoolProperty *BoolProperty =
          CastField<FBoolProperty>(IsExtendingProperty);
      if (BoolProperty) {
        BoolProperty->SetPropertyValue_InContainer(ArmAnimInstance,
                                                   bIsExtending);
      }
    }

    if (bDebugLogs) {
      UE_LOG(LogAGArm, Verbose,
             TEXT("Arm '%s': Updated Animation Blueprint - Yaw=%.2f, "
                  "ExtendRetract=%d, Extending=%d"),
             *GetName(), Yaw, bExecutingExtendRetract ? 1 : 0,
             bIsExtending ? 1 : 0);
    }
  } else if (bDebugLogs) {
    UE_LOG(LogAGArm, Warning,
           TEXT("Arm '%s': Animation Blueprint instance not found for bone "
                "rotations"),
           *GetName());
  }
}

void ABasicArm::EnsureAnimationBlueprintInstance() {
  // Try to get the Animation Blueprint instance if we don't have it
  if (!ArmAnimInstance && BaseComp) {
    // If no Animation Blueprint class is set, try to set it
    if (!BaseComp->GetAnimClass() && ArmAnimationBlueprintClass) {
      BaseComp->SetAnimInstanceClass(ArmAnimationBlueprintClass);

      if (bDebugLogs) {
        UE_LOG(LogAGArm, Verbose,
               TEXT("Arm '%s': Set Animation Blueprint class to %s"),
               *GetName(), *ArmAnimationBlueprintClass->GetName());
      }
    }

    ArmAnimInstance = BaseComp->GetAnimInstance();

    if (bDebugLogs) {
      UE_LOG(LogAGArm, Verbose,
             TEXT("Arm '%s': EnsureAnimationBlueprintInstance - "
                  "ArmAnimInstance=%s, AnimClass=%s, BaseComp=%s"),
             *GetName(), ArmAnimInstance ? TEXT("Found") : TEXT("Still NULL"),
             BaseComp->GetAnimClass() ? *BaseComp->GetAnimClass()->GetName()
                                      : TEXT("NULL"),
             BaseComp ? TEXT("Valid") : TEXT("NULL"));
    }
  }
}

void ABasicArm::SetProgram(const TArray<FArmInstr> &InProgram,
                           bool bLoop /*= true*/) {
  UE_LOG(
      LogAGArm, Warning,
      TEXT("SetProgram called on Arm '%s' (SimIndex=%d) with %d instructions"),
      *GetName(), SimArmIndex, InProgram.Num());

  Program = InProgram;
  bLoopProgram = bLoop;
  PC = 0;
  WaitLeft = 0;
  PushProgramToSim(); // NEW

  // Update timeline widget immediately to reflect the new program
  UpdateTimelineWidget();
}

TArray<FOp>
ABasicArm::BuildOpsFromProgram(const TArray<FArmInstr> &InProgram) const {
  TArray<FOp> Out;
  if (InProgram.Num() == 0) {
    Out.Add({ECmd::Wait, 1}); // harmless idle
    return Out;
  }
  for (const FArmInstr &I : InProgram) {
    switch (I.Cmd) {
    case EArmCmd::NoOp:
      Out.Add({ECmd::Wait, 1});
      break;
    case EArmCmd::Grab:
      Out.Add({ECmd::Grab, 0});
      break;
    case EArmCmd::Drop:
      Out.Add({ECmd::Drop, 0});
      break;
    case EArmCmd::RotateCW:
      Out.Add({ECmd::RotateR, (uint8)FMath::Max(1, I.Arg)});
      break;
    case EArmCmd::RotateCCW:
      Out.Add({ECmd::RotateL, (uint8)FMath::Max(1, I.Arg)});
      break;
    case EArmCmd::Wait:
      Out.Add({ECmd::Wait, (uint8)FMath::Clamp(I.Arg, 0, 255)});
      break;
    case EArmCmd::Reset:
      Out.Add({ECmd::Reset, 0});
      break;
    case EArmCmd::Extend:
      Out.Add(
          {ECmd::Extend, (uint8)FMath::Clamp(I.Arg == 0 ? 1 : I.Arg, 0, 255)});
      break; // NEW
    case EArmCmd::Retract:
      Out.Add(
          {ECmd::Retract, (uint8)FMath::Clamp(I.Arg == 0 ? 1 : I.Arg, 0, 255)});
      break; // NEW
    case EArmCmd::RotateHeldCW:
      Out.Add({ECmd::RotateHeldR, (uint8)FMath::Max(1, I.Arg)});
      break; // NEW
    case EArmCmd::RotateHeldCCW:
      Out.Add({ECmd::RotateHeldL, (uint8)FMath::Max(1, I.Arg)});
      break; // NEW
    case EArmCmd::WaitForBond:
      Out.Add({ECmd::WaitForBond, 0});
      break; // NEW
    default:
      Out.Add({ECmd::Wait, 1});
      break;
    }
  }
  return Out;
}

// NEW: push actor-side program / facing into sim
void ABasicArm::PushProgramToSim() {
  if (!Sim || SimArmIndex < 0 || SimArmIndex >= Sim->Arms.Num()) {
    UE_LOG(LogAGArm, Warning,
           TEXT("PushProgramToSim: Arm '%s' has invalid SimArmIndex=%d (Sim "
                "has %d arms)"),
           *GetName(), SimArmIndex, Sim ? Sim->Arms.Num() : -1);
    return;
  }

  FArmState &S = Sim->Arms[SimArmIndex];

  // Check if we're in the middle of executing temporary Reset operations
  // If the tape has more operations than what BuildOpsFromProgram would
  // generate, it means we have temporary operations that shouldn't be
  // overwritten
  TArray<FOp> NewTape = BuildOpsFromProgram(Program);
  if (S.Tape.Num() > NewTape.Num() && S.PC > 0) {
    // We're likely executing temporary Reset operations - don't overwrite!
    UE_LOG(LogAGArm, Warning,
           TEXT("PushProgramToSim: Arm '%s' (SimIndex=%d) skipping update - "
                "temporary ops in progress (Tape=%d, Expected=%d)"),
           *GetName(), SimArmIndex, S.Tape.Num(), NewTape.Num());
    return;
  }

  // Log what we're about to replace
  UE_LOG(LogAGArm, Warning,
         TEXT("PushProgramToSim: Arm '%s' (SimIndex=%d) updating tape from %d "
              "ops to %d ops"),
         *GetName(), SimArmIndex, S.Tape.Num(), NewTape.Num());

  S.Tape = NewTape;
  S.PC = 0;
  S.Facing = static_cast<EHexDir>(FacingDir);

  // Debug: Log the converted program
  UE_LOG(LogAGArm, Log,
         TEXT("Arm '%s': PushProgramToSim called - Converting %d instructions "
              "to %d ops"),
         *GetName(), Program.Num(), S.Tape.Num());

  // Always log tape contents after Reset expansion for debugging
  // UE_LOG(LogAGArm, Log, TEXT("Arm '%s': Program pushed (TapeLen=%d)"),
  // *GetName(), S.Tape.Num());
  for (int32 i = 0; i < S.Tape.Num(); i++) {
    const FOp &Op = S.Tape[i];
    FString CmdName = TEXT("Unknown");
    switch (Op.Cmd) {
    case ECmd::Wait:
      CmdName = TEXT("Wait");
      break;
    case ECmd::Grab:
      CmdName = TEXT("Grab");
      break;
    case ECmd::Drop:
      CmdName = TEXT("Drop");
      break;
    case ECmd::RotateR:
      CmdName = TEXT("RotateR");
      break;
    case ECmd::RotateL:
      CmdName = TEXT("RotateL");
      break;
    case ECmd::Extend:
      CmdName = TEXT("Extend");
      break;
    case ECmd::Retract:
      CmdName = TEXT("Retract");
      break;
    case ECmd::Reset:
      CmdName = TEXT("Reset");
      break;
    case ECmd::RotateHeldL:
      CmdName = TEXT("RotateHeldL");
      break;
    case ECmd::RotateHeldR:
      CmdName = TEXT("RotateHeldR");
      break;
    case ECmd::WaitForBond:
      CmdName = TEXT("WaitForBond");
      break;
    }
    UE_LOG(LogAGArm, Log, TEXT("  [%d]: %s %d"), i, *CmdName, Op.Arg);
  }

  // UE_LOG(LogAGArm, Verbose, TEXT("Arm '%s': Program pushed (TapeLen=%d
  // Holding(sim)=%d)"), 	*GetName(), S.Tape.Num(), S.bHolding ? 1 : 0);
}

void ABasicArm::PushRuntimePropertiesToSim() {
  if (!Sim || SimArmIndex < 0 || SimArmIndex >= Sim->Arms.Num())
    return;
  FArmState &S = Sim->Arms[SimArmIndex];
  S.Facing = static_cast<EHexDir>(FacingDir);
  S.ArmLen = FMath::Clamp(ReachTiles, 1, 3);
  S.HomeArmLen = FMath::Clamp(S.HomeArmLen, 1, 3); // keep bounds
  UE_LOG(LogAGArm, Verbose,
         TEXT("Arm '%s': Runtime props pushed (Facing=%d ArmLen=%d)"),
         *GetName(), (int32)S.Facing, S.ArmLen);
}

void ABasicArm::RebuildTapeFromProgram() { PushProgramToSim(); }

void ABasicArm::SetStartingReach(int32 NewReach) {
  StartingReach = FMath::Clamp(NewReach, 1, 6);
  ReachTiles = StartingReach;

  // Reset program and sync to sim
  if (Sim && SimArmIndex >= 0 && SimArmIndex < Sim->Arms.Num()) {
    FArmState &S = Sim->Arms[SimArmIndex];
    S.PC = 0;
    S.WaitCounter = 0;
    S.ArmLen = StartingReach;
    S.HomeArmLen = StartingReach;
  }

  PushRuntimePropertiesToSim();
  RefreshPose(1.f);

  UE_LOG(LogAGArm, Log, TEXT("Arm '%s': SetStartingReach %d"), *GetName(),
         StartingReach);
}

void ABasicArm::SetStartingRotation(int32 NewRotation) {
  StartingRotation = static_cast<uint8>(NewRotation % 6);
  FacingDir = StartingRotation;
  CurrYawDeg = DirToYaw(FacingDir);

  // Reset program and sync to sim
  if (Sim && SimArmIndex >= 0 && SimArmIndex < Sim->Arms.Num()) {
    FArmState &S = Sim->Arms[SimArmIndex];
    S.PC = 0;
    S.WaitCounter = 0;
    S.Facing = static_cast<EHexDir>(StartingRotation);
    S.HomeFacing = static_cast<EHexDir>(StartingRotation);
  }

  PushRuntimePropertiesToSim();
  RefreshPose(1.f);

  UE_LOG(LogAGArm, Log, TEXT("Arm '%s': SetStartingRotation %d"), *GetName(),
         StartingRotation);
}

#if WITH_EDITOR
void ABasicArm::PostEditChangeProperty(FPropertyChangedEvent &Event) {
  Super::PostEditChangeProperty(Event);

  if (HasAnyFlags(RF_ClassDefaultObject))
    return;
  if (!GIsPlayInEditorWorld)
    return; // only care while PIE is running

  const FName PropName =
      Event.Property ? Event.Property->GetFName() : NAME_None;
  const FName MemberName =
      Event.MemberProperty ? Event.MemberProperty->GetFName() : NAME_None;
  const bool bProgramMemberChanged =
      (MemberName == GET_MEMBER_NAME_CHECKED(ABasicArm, Program));

  const bool bProgramElementFieldChanged =
      (PropName == GET_MEMBER_NAME_CHECKED(FArmInstr, Cmd)) ||
      (PropName == GET_MEMBER_NAME_CHECKED(FArmInstr, Arg));

  const bool bLoopChanged =
      (MemberName == GET_MEMBER_NAME_CHECKED(ABasicArm, bLoopProgram));

  const bool bReachOrFacingChanged =
      (MemberName == GET_MEMBER_NAME_CHECKED(ABasicArm, ReachTiles)) ||
      (MemberName == GET_MEMBER_NAME_CHECKED(ABasicArm, FacingDir));

  const bool bStartingRotationChanged =
      (MemberName == GET_MEMBER_NAME_CHECKED(ABasicArm, StartingRotation));

  const bool bStartingReachChanged =
      (MemberName == GET_MEMBER_NAME_CHECKED(ABasicArm, StartingReach));

  // Any structural change to the array (add/remove/duplicate/clear) also fires
  // MemberProperty == Program
  const bool bArrayStructuralChange =
      bProgramMemberChanged &&
      (Event.ChangeType == EPropertyChangeType::ArrayAdd ||
       Event.ChangeType == EPropertyChangeType::ArrayRemove ||
       Event.ChangeType == EPropertyChangeType::ArrayClear ||
       Event.ChangeType == EPropertyChangeType::Duplicate ||
       Event.ChangeType == EPropertyChangeType::ValueSet);

  if (bProgramMemberChanged || bProgramElementFieldChanged ||
      bArrayStructuralChange) {
    PushProgramToSim();
    UE_LOG(
        LogAGArm, Verbose,
        TEXT("Arm '%s': Program edit -> tape rebuilt (Len=%d ChangeType=%d)"),
        *GetName(), Program.Num(), (int32)Event.ChangeType);
  } else if (bLoopChanged) {
    // If Sim supports a loop flag, set it here (pseudo:
    // Sim->Arms[SimArmIndex].bLoop = bLoopProgram).
    if (Sim && SimArmIndex >= 0 && SimArmIndex < Sim->Arms.Num()) {
      // If FArmState has something like bLoopTape, assign it here:
      // Sim->Arms[SimArmIndex].bLoopTape = bLoopProgram;
    }
    if (bDebugLogs) {
      UE_LOG(LogAGArm, Verbose, TEXT("Arm '%s': Loop flag changed=%d"),
             *GetName(), bLoopProgram ? 1 : 0);
    }
  } else if (bReachOrFacingChanged) {
    CurrYawDeg = DirToYaw(FacingDir);
    PushRuntimePropertiesToSim();
  } else if (bStartingRotationChanged || bStartingReachChanged) {
    // When starting values change, immediately update current values and
    // restart program
    if (bStartingRotationChanged) {
      FacingDir = StartingRotation % 6;
      CurrYawDeg = DirToYaw(FacingDir);
    }

    if (bStartingReachChanged) {
      ReachTiles = FMath::Clamp(StartingReach, 1, 6);
    }

    // Update simulation state to match new starting values
    PushRuntimePropertiesToSim();

    // Reset program counter to restart the program
    if (Sim && SimArmIndex >= 0 && SimArmIndex < Sim->Arms.Num()) {
      FArmState &S = Sim->Arms[SimArmIndex];
      S.PC = 0;          // Reset to beginning of program
      S.WaitCounter = 0; // Clear any wait state
    }

    // Refresh visuals immediately
    RefreshPose(1.f);

    UE_LOG(LogAGArm, Log,
           TEXT("Arm '%s': Starting values changed - Rotation=%d Reach=%d, "
                "program restarted"),
           *GetName(), StartingRotation, StartingReach);
  }

  // For safety, if Program became empty we still ensure a minimal idle op
  if (Program.Num() == 0 && Sim && SimArmIndex >= 0 &&
      SimArmIndex < Sim->Arms.Num()) {
    if (Sim->Arms[SimArmIndex].Tape.Num() == 0) {
      PushProgramToSim();
    }
  }

  if (GIsPlayInEditorWorld && !bUseGripSocket && GripPivotComp) {
    // PropName = Event.Property ? Event.Property->GetFName() : NAME_None;
    if (PropName ==
            GET_MEMBER_NAME_CHECKED(ABasicArm, GripPivotLocalRotation) ||
        PropName == GET_MEMBER_NAME_CHECKED(ABasicArm, GripDistanceOverride) ||
        PropName == GET_MEMBER_NAME_CHECKED(ABasicArm, GripSideOffset) ||
        PropName == GET_MEMBER_NAME_CHECKED(ABasicArm, GripZOffset)) {
      GripPivotComp->SetRelativeLocation(ComputeGripLocalOffset());
      GripPivotComp->SetRelativeRotation(GripPivotLocalRotation);
    }
  }
}
#endif

void ABasicArm::Start() { bRunning = true; }

void ABasicArm::Stop() { bRunning = false; }

void ABasicArm::ResetProgram() {
  PC = 0;
  WaitLeft = 0;
}

void ABasicArm::ExecuteCurrent() {
  const int32 SavedWait = WaitLeft;
  WaitLeft = 0;
  // StepProgram();
  WaitLeft = SavedWait;
}

void ABasicArm::SyncDebugSettingsFromDriver() {
  if (Driver) {
    bDebugLogs = Driver->bDebugLogs;
    bDebugSpheres = Driver->bDebugSpheres;
    bDebugLines = Driver->bDebugLines;

    // Temporary: Force enable debug logs for HoloscopicGear3 testing
    bDebugLogs = true;
    UE_LOG(LogAGArm, Log,
           TEXT("BasicArm '%s': Synced debug settings from driver - Logs=%d "
                "Spheres=%d Lines=%d"),
           *GetName(), bDebugLogs ? 1 : 0, bDebugSpheres ? 1 : 0,
           bDebugLines ? 1 : 0);
  } else {
    UE_LOG(LogAGArm, Warning,
           TEXT("BasicArm '%s': No driver found for debug sync"), *GetName());
  }
}

void ABasicArm::ApplyRotateSteps(int32 StepsSigned) {
  if (StepsSigned == 0)
    return;
  const uint8 Old = FacingDir;
  FacingDir = uint8(Mod6(int32(FacingDir) + StepsSigned));
  CurrYawDeg = DirToYaw(FacingDir);
  UE_LOG(LogAGArm, Verbose,
         TEXT("ApplyRotateSteps: steps=%d OldDir=%d NewDir=%d CurrYaw=%.2f"),
         StepsSigned, (int32)Old, (int32)FacingDir, CurrYawDeg);
}

FHex ABasicArm::DirStep(uint8 Dir) const {
  const uint8 D = Dir % 6;
  // Pointy-top neighbors (q,r): E(1,0), NE(1,-1), NW(0,-1), W(-1,0), SW(-1,1),
  // SE(0,1)
  static const FHex Pointy[6] = {FHex(1, 0),  FHex(1, -1), FHex(0, -1),
                                 FHex(-1, 0), FHex(-1, 1), FHex(0, 1)};
  // Flat-top (only used if you switch orientation)
  static const FHex Flat[6] = {FHex(1, -1), FHex(1, 0),  FHex(0, 1),
                               FHex(-1, 1), FHex(-1, 0), FHex(0, -1)};

  const bool bPointy =
      !Driver || Driver->GetGridOrientation() == EHexOrientation::PointyTop;
  return bPointy ? Pointy[D] : Flat[D];
}

// ABasicArm.cpp
FHex ABasicArm::ComputeTargetHex() const {
  FHex T(Q, R, Level);
  const int32 N = FMath::Max(1, ReachTiles);
  for (int32 i = 0; i < N; ++i)
    T = HexUtil::Neighbor(T, static_cast<EHexDir>(FacingDir));
  return T;
}

void ABasicArm::ExpandResetAtPC() {
  // Expand "Reset" into: (Drop if holding) + Reach adjustments + Rotation
  // adjustments back to starting values
  TArray<FArmInstr> Expansion;

  // Always drop first if holding
  if (bHolding) {
    Expansion.Add({EArmCmd::Drop, 0});
  }

  // Handle reach reset: calculate difference between current and starting reach
  const int32 CurrentReach = ReachTiles;
  const int32 TargetReach = FMath::Clamp(StartingReach, 1, 6);
  const int32 ReachDiff = TargetReach - CurrentReach;

  if (ReachDiff > 0) {
    // Need to extend
    for (int32 i = 0; i < ReachDiff; ++i) {
      Expansion.Add({EArmCmd::Extend, 1});
    }
  } else if (ReachDiff < 0) {
    // Need to retract
    for (int32 i = 0; i < -ReachDiff; ++i) {
      Expansion.Add({EArmCmd::Retract, 1});
    }
  }

  // Handle rotation reset: find shortest path to StartingRotation
  const uint8 CurrentRotation = FacingDir;
  const uint8 TargetRotation = StartingRotation % 6;
  const int32 DiffCW =
      Mod6(int32(TargetRotation) - int32(CurrentRotation)); // 0..5 steps CW
  const int32 DiffCCW =
      Mod6(int32(CurrentRotation) - int32(TargetRotation)); // 0..5 steps CCW
  const bool bUseCW = (DiffCW <= DiffCCW);
  const int32 RotationSteps = bUseCW ? DiffCW : DiffCCW;

  for (int32 i = 0; i < RotationSteps; ++i) {
    Expansion.Add({bUseCW ? EArmCmd::RotateCW : EArmCmd::RotateCCW, 1});
  }

  // Remove the Reset command and insert the expansion
  Program.RemoveAt(PC);
  if (Expansion.Num() > 0) {
    Program.Insert(Expansion, PC);
  }

  // Log the expansion for debugging
  UE_LOG(LogAGArm, Verbose,
         TEXT("BasicArm '%s' Reset expanded: Rotation(%d->%d) Reach(%d->%d) "
              "Holding=%d ExpansionSteps=%d"),
         *GetName(), int32(CurrentRotation), int32(TargetRotation),
         CurrentReach, TargetReach, bHolding ? 1 : 0, Expansion.Num());
}

void ABasicArm::StepProgram() {
  // Consume waiting ticks
  if (WaitLeft > 0) {
    --WaitLeft;
    return;
  }

  if (Program.Num() == 0)
    return;

  // Clamp/wrap PC
  if (PC >= Program.Num()) {
    if (bLoopProgram)
      PC = 0;
    else
      return;
  }

  const FArmInstr I = Program[PC];
}

void ABasicArm::ShowHeldOrb(bool bShow) {
  if (!HeldOrbComp) {
    UE_LOG(LogAGArm, Warning,
           TEXT("Arm '%s': HeldOrbComp is null, cannot show held orb."),
           *GetName());
    return;
  }

  // Remove previous orb meshes
  TArray<USceneComponent *> ChildComponents;
  HeldOrbComp->GetChildrenComponents(false, ChildComponents);
  // for (USceneComponent* Child : ChildComponents)
  //{
  //	Child->DestroyComponent();
  // }

  if (bShow && HeldOrb.Orbs.Num() > 0) {
    if (bDebugLogs) {
      UE_LOG(LogAGArm, Log,
             TEXT("Arm '%s': Showing held orb with %d sub-orbs."), *GetName(),
             HeldOrb.Orbs.Num());
    }
    HeldOrbComp->AttachToComponent(
        BaseComp, FAttachmentTransformRules::SnapToTargetIncludingScale,
        TEXT("GripSocket"));

    for (const FOrbData &Orb : HeldOrb.Orbs) {
      UStaticMeshComponent *OrbMesh = NewObject<UStaticMeshComponent>(this);
      OrbMesh->RegisterComponent();
      OrbMesh->AttachToComponent(
          HeldOrbComp, FAttachmentTransformRules::KeepRelativeTransform);
      OrbMesh->SetRelativeLocation(FVector(Orb.Offset, 0.f));
      OrbMesh->SetMaterial(0, GetMaterialForOrbColor(Orb.Type));
      OrbMesh->SetRelativeScale3D(HeldOrbScale);

      if (bDebugLogs) {
        UE_LOG(LogAGArm, Verbose,
               TEXT("Arm '%s': Created orb mesh for type=%d color=%d "
                    "offset=(%.2f, %.2f)"),
               *GetName(), Orb.Type, Orb.Color, Orb.Offset.X, Orb.Offset.Y);
      }
    }
    // HeldOrbComp->SetVisibility(true, true);
  } else {
    if (bShow) {
      UE_LOG(
          LogAGArm, Warning,
          TEXT("Arm '%s': Tried to show held orb, but HeldOrb.Orbs.Num() == 0"),
          *GetName());
    }
    HeldOrbComp->DetachFromComponent(
        FDetachmentTransformRules::KeepWorldTransform);
    // HeldOrbComp->SetVisibility(false, true);
  }
}

void ABasicArm::InitializeOnGrid(AOrbSimDriver *InDriver, int32 InQ, int32 InR,
                                 int32 InLevel, uint8 InFacing) {
  Driver = InDriver;
  Q = InQ;
  R = InR;
  Level = InLevel;
  FacingDir = InFacing % 6;
  StartFacingDir = FacingDir;

  // Initialize starting values for Reset command
  // These will be the values the arm resets to
  StartingRotation = InFacing % 6;
  StartingReach = ReachTiles;

  Sim = (Driver ? Driver->GetSim() : nullptr);

  // refresh visuals immediately
  CurrYawDeg = DirToYaw(FacingDir);
  LastYawDeg = CurrYawDeg;
  RefreshPose(1.f);
}

UStaticMesh *ABasicArm::GetMeshForOrbType(uint8 OrbType) const {
  // Use centralized material manager
  if (UOrbMaterialManager *MaterialManager =
          UOrbMaterialManager::FindInWorld(this)) {
    UStaticMesh *CentralizedMesh = MaterialManager->GetOrbMesh();
    if (CentralizedMesh) {
      return CentralizedMesh;
    }
  }

  // Final fallback to engine default
  return nullptr;
}

UMaterialInterface *ABasicArm::GetMaterialForOrbColor(uint8 OrbType) const {
  // Use centralized material manager
  if (UOrbMaterialManager *MaterialManager =
          UOrbMaterialManager::FindInWorld(this)) {
    // Use the orb type directly as the material key
    EOrbType EOrbTypeValue = (EOrbType)OrbType;
    UMaterialInterface *CentralizedMaterial =
        MaterialManager->GetMaterialForOrbType(EOrbTypeValue);
    if (CentralizedMaterial) {
      return CentralizedMaterial;
    }

    // Try the default material from the centralized system
    UMaterialInterface *DefaultMaterial =
        MaterialManager->GetDefaultOrbMaterial();
    if (DefaultMaterial) {
      return DefaultMaterial;
    }
  }

  // Final fallback to engine default
  return nullptr;
}

// Add helper functions near other helpers (e.g. after GetMaterialForOrbColor)
UStaticMesh *ABasicArm::ResolveOrbMesh(const FItem &Item) const {
  // Use centralized material manager
  if (UOrbMaterialManager *MaterialManager =
          UOrbMaterialManager::FindInWorld(this)) {
    // Get the correct orb type from the item
    EOrbType OrbType = EOrbType::BaseA; // Default
    if (Item.bIsCluster && Item.Orbs.Num() > 0) {
      OrbType = (EOrbType)Item.Orbs[0].Type;
    } else if (!Item.bIsCluster) {
      OrbType = Item.Single.Type;
    }

    // For now, all orb types use the same mesh, but this could be extended
    UStaticMesh *CentralizedMesh = MaterialManager->GetOrbMesh();
    if (CentralizedMesh) {
      return CentralizedMesh;
    }
  }

  // Final fallback to engine default
  return nullptr;
}

UMaterialInterface *ABasicArm::ResolveOrbMaterial(const FItem &Item) const {
  // Use centralized material manager
  if (UOrbMaterialManager *MaterialManager =
          UOrbMaterialManager::FindInWorld(this)) {
    EOrbType OrbType = EOrbType::BaseA; // Default
    if (Item.bIsCluster && Item.Orbs.Num() > 0) {
      OrbType = (EOrbType)Item.Orbs[0].Type;
    } else if (!Item.bIsCluster) {
      OrbType = Item.Single.Type;
    }

    UMaterialInterface *CentralizedMaterial =
        MaterialManager->GetMaterialForOrbType(OrbType);
    if (CentralizedMaterial) {
      return CentralizedMaterial;
    }

    // Try the default material from the centralized system
    UMaterialInterface *DefaultMaterial =
        MaterialManager->GetDefaultOrbMaterial();
    if (DefaultMaterial) {
      return DefaultMaterial;
    }
  }

  // Final fallback to engine default
  return nullptr;
}

// Replace AttachHeldOrb() with more robust version
void ABasicArm::AttachHeldOrb() {
  if (!bHolding) {
    DetachHeldOrb();
    return;
  }

  if (!HeldOrbComp) {
    HeldOrbComp = NewObject<UStaticMeshComponent>(this, TEXT("HeldOrb"));
    HeldOrbComp->RegisterComponent();
  }

  USceneComponent *ParentForHeld = nullptr;

  if (bUseGripSocket && BaseComp &&
      BaseComp->DoesSocketExist(TEXT("GripSocket"))) {
    ParentForHeld = BaseComp;
    HeldOrbComp->AttachToComponent(
        BaseComp, FAttachmentTransformRules::SnapToTargetIncludingScale,
        TEXT("GripSocket"));
  } else {
    // Fallback: procedural pivot
    ParentForHeld = GripPivotComp ? GripPivotComp : BaseComp;
    if (GripPivotComp && !bUseGripSocket) {
      GripPivotComp->SetRelativeLocation(ComputeGripLocalOffset());
    }
    HeldOrbComp->AttachToComponent(
        ParentForHeld, FAttachmentTransformRules::SnapToTargetIncludingScale);
  }

  HeldOrbComp->SetMobility(EComponentMobility::Movable);
  HeldOrbComp->SetVisibility(true, true);
  HeldOrbComp->SetRelativeScale3D(HeldOrbScale);

  // Clear old children
  TArray<USceneComponent *> ChildComps;
  HeldOrbComp->GetChildrenComponents(false, ChildComps);
  for (USceneComponent *C : ChildComps) {
    if (C)
      C->DestroyComponent();
  }

  const float Tile = (Driver ? Driver->GetTileSize() : 100.f);
  const float OffsetBaseScale =
      (bScaleClusterOffsetsByTile ? Tile : 1.f) * ClusterOffsetScale;

  if (HeldOrb.bIsCluster) {
    // Optional container mesh
    if (bShowClusterContainerMesh) {
      UStaticMesh *Container = ResolveOrbMesh(HeldOrb);
      HeldOrbComp->SetStaticMesh(Container);
    } else {
      HeldOrbComp->SetStaticMesh(nullptr);
    }

    if (!bShowClusterContainerMesh && !HeldOrbComp->GetStaticMesh()) {
      // Accept empty parent; just holds sub-orbs.
    }

    // Opus Magnum approach: position sub-orbs relative to the grabbed orb
    // The grabbed orb should be at the grab point, others positioned relative
    // to it UE_LOG(LogAGArm, Log, TEXT("Attaching cluster with %d sub-orbs,
    // GrabFacing=%d"), 	HeldOrb.Orbs.Num(), (int32)HeldOrb.GrabFacing);
    for (const FOrbData &Sub : HeldOrb.Orbs) {
      UStaticMeshComponent *SubMesh = NewObject<UStaticMeshComponent>(this);
      SubMesh->RegisterComponent();
      SubMesh->AttachToComponent(
          HeldOrbComp, FAttachmentTransformRules::KeepRelativeTransform);

      UStaticMesh *SM = GetMeshForOrbType(Sub.Type);
      SubMesh->SetStaticMesh(SM);

      UMaterialInterface *Mat = GetMaterialForOrbColor(Sub.Type);
      if (Mat) {
        SubMesh->SetMaterial(0, Mat);
        // Ensure the material renders as opaque (not transparent)
        SubMesh->SetRenderCustomDepth(false);
      }

      // Calculate visual offset from hex coordinates (dQ, dR)
      // Convert hex axial coordinates to world offset using the SAME logic as
      // simulation
      const EHexOrientation GridOrientation =
          (Driver ? Driver->GetGridOrientation() : EHexOrientation::PointyTop);
      const FVector2D HexOffset =
          HexCoord::AxialToXY(FHexAxial(Sub.dQ, Sub.dR), Tile, GridOrientation);
      FVector2D Off2D = HexOffset * ClusterOffsetScale;

      // Apply rotation based on GrabFacing to make offsets relative to arm
      // direction
      // if (HeldOrb.GrabFacing >= 0)
      //{
      //	const float GrabYawDeg = 60.f * (HeldOrb.GrabFacing % 6);
      //	const float YawRad = FMath::DegreesToRadians(GrabYawDeg);
      //	const float CosA = FMath::Cos(YawRad);
      //	const float SinA = FMath::Sin(YawRad);
      //	const float RotX = Off2D.X * CosA - Off2D.Y * SinA;
      //	const float RotY = Off2D.X * SinA + Off2D.Y * CosA;
      //	Off2D = FVector2D(RotX, RotY);
      //}

      if (bInvertClusterVisualFacing) {
        // 180° flip around Z so visual layout matches logical facing.
        Off2D *= -1.f;
      }

      // If on ceiling grid, clusters might need Z flip relative to the arm
      // attachment point to stay centered on the grid surface
      float SubZ = 0.f;
      FVector FinalLoc(Off2D.X, Off2D.Y, SubZ);

      SubMesh->SetRelativeLocation(FinalLoc);

      SubMesh->SetRelativeScale3D(HeldOrbScale);

      HeldClusterComps.Add(SubMesh);
    }

    // Preserve interpolation state when rebuilding cluster (e.g., during bond
    // formation)
    const int32 CurrentGrabFacing =
        (HeldOrb.GrabFacing >= 0 ? HeldOrb.GrabFacing % 6 : 0);

    // Check if we need to initialize new rotation interpolation
    if (CurrentGrabFacing != LastHeldGrabFacing && LastHeldGrabFacing >= 0) {
      // GrabFacing changed - this is a new RotateHeld command starting
      // Initialize interpolation from current visual angle to new target
      const float CurrentAngle =
          NormDeg(HeldClusterFacingStartDeg +
                  FMath::FindDeltaAngleDegrees(HeldClusterFacingStartDeg,
                                               HeldClusterFacingEndDeg) *
                      StepAlpha);
      HeldClusterFacingStartDeg = CurrentAngle;
      HeldClusterFacingEndDeg = 60.f * float(CurrentGrabFacing);
      LastHeldGrabFacing = CurrentGrabFacing;
      // Apply current interpolated position
      HeldOrbComp->SetRelativeRotation(FRotator(0.f, CurrentAngle, 0.f));
    } else if (LastHeldGrabFacing < 0 || StepAlpha >= 0.99f) {
      // First time setup or rotation complete - snap to current GrabFacing
      LastHeldGrabFacing = CurrentGrabFacing;
      HeldClusterFacingStartDeg = HeldClusterFacingEndDeg =
          60.f * float(CurrentGrabFacing);
      HeldOrbComp->SetRelativeRotation(
          FRotator(0.f, HeldClusterFacingEndDeg, 0.f));
    } else {
      // We're in the middle of a rotation and cluster composition changed (bond
      // formed) Preserve the current interpolated angle
      const float CurrentAngle =
          NormDeg(HeldClusterFacingStartDeg +
                  FMath::FindDeltaAngleDegrees(HeldClusterFacingStartDeg,
                                               HeldClusterFacingEndDeg) *
                      StepAlpha);
      HeldOrbComp->SetRelativeRotation(FRotator(0.f, CurrentAngle, 0.f));
      // Keep LastHeldGrabFacing and interpolation values unchanged
    }

    // Generate bonded orb meshes for held cluster
    if (bShowBondedOrbs && GetBondedOrbMesh() && Sim) {
      GenerateBondedOrbMeshes();
    }

  } else {
    // Handle single orbs the same way as clusters - create a sub-orb mesh
    // This ensures consistent material handling between single orbs and
    // clusters
    HeldOrbComp->SetStaticMesh(nullptr); // Clear the main component mesh

    // Create a sub-orb mesh for the single orb
    UStaticMeshComponent *SubMesh = NewObject<UStaticMeshComponent>(this);
    SubMesh->RegisterComponent();
    SubMesh->AttachToComponent(
        HeldOrbComp, FAttachmentTransformRules::KeepRelativeTransform);
    SubMesh->SetRelativeLocation(FVector::ZeroVector); // Single orb at center
    SubMesh->SetRelativeScale3D(HeldOrbScale);

    UStaticMesh *SM = GetMeshForOrbType((uint8)HeldOrb.Single.Type);
    SubMesh->SetStaticMesh(SM);

    UMaterialInterface *Mat =
        GetMaterialForOrbColor((uint8)HeldOrb.Single.Type);
    if (Mat) {
      SubMesh->SetMaterial(0, Mat);
      // Ensure the material renders as opaque (not transparent)
      SubMesh->SetRenderCustomDepth(false);
    }

    HeldClusterComps.Add(SubMesh);

    // Initialize for single orb too (harmless)
    LastHeldGrabFacing = (HeldOrb.GrabFacing >= 0 ? HeldOrb.GrabFacing % 6 : 0);
    HeldClusterFacingStartDeg = HeldClusterFacingEndDeg =
        60.f * float(LastHeldGrabFacing);
    HeldOrbComp->SetRelativeRotation(
        FRotator(0.f, HeldClusterFacingEndDeg, 0.f));
    if (bDebugLogs) {
      UE_LOG(LogAGArm, Verbose,
             TEXT("Arm '%s': Attached single orb Id=%d Type=%d"), *GetName(),
             HeldOrb.Id, (int32)HeldOrb.Single.Type);
    }
  }
}

// UpdateHeldOrbVisual(): relax Id requirement & add logging
void ABasicArm::UpdateHeldOrbVisual() {
  const int32 CurrentId = bHolding ? (HeldOrb.Id == 0 ? -1 : HeldOrb.Id)
                                   : 0; // -1 denotes held with no Id
  // Calculate current display level
  int32 DisplayLevel = Level;
  if (Driver) {
    DisplayLevel = Driver->IsDisplayFlipped() ? (1 - Level) : Level;
  }
  const int32 CurrentOrbCount = bHolding ? HeldOrb.Orbs.Num() : 0;

  // Check if anything changed that requires a visual rebuild
  bool bNeedsUpdate = (CurrentId != LastHeldItemId) ||
                      (CurrentOrbCount != LastHeldOrbCount) ||
                      (DisplayLevel != LastDisplayLevel);

  if (!bNeedsUpdate)
    return;

  // Swap skeletal mesh if needed
  if (DisplayLevel != LastDisplayLevel) {
    USkeletalMesh *TargetMesh = ArmSkeletalMesh;
    if (DisplayLevel == 1) { // Inactive/Ceiling
      if (UpsideDownArmSkeletalMesh) {
        TargetMesh = UpsideDownArmSkeletalMesh;
      } else if (AHexFactoryActor *Factory =
                     AHexFactoryActor::FindInWorld(GetWorld())) {
        if (Factory->UpsideDownArmMesh) {
          TargetMesh = Factory->UpsideDownArmMesh;
        }
      }
    }
    BaseComp->SetSkeletalMesh(TargetMesh);
  }

  if (!bHolding) {
    DetachHeldOrb();
    LastHeldItemId = 0;
    LastHeldOrbCount = 0;
    LastDisplayLevel = DisplayLevel; // sync state
    return;
  }

  AttachHeldOrb(); // always when holding
  LastHeldItemId = CurrentId;
  LastHeldOrbCount = CurrentOrbCount;
  LastDisplayLevel = DisplayLevel; // sync state
}

// --- Sync / detach implementations added ---

void ABasicArm::SyncFromSim(const FArmState &SimState) {
  // Update orientation
  FacingDir = static_cast<uint8>(SimState.Facing);
  CurrYawDeg = DirToYaw(FacingDir);

  // Reach (clamped the same way we push)
  if (ReachTiles != SimState.ArmLen) {
    ReachTiles = SimState.ArmLen;
  }

  // Sync wait state from simulation
  WaitLeft = SimState.WaitCounter;

  // Sync program counter from simulation
  PC = SimState.PC;

  // Holding state + item payload
  bHolding = SimState.bHolding;
  if (bHolding) {
    HeldOrb = SimState.Held; // assumes FArmState has Held (or adapt if
                             // different field name)
  } else {
    HeldOrb = FItem();
  }

  UpdateHeldOrbVisual();
}

void ABasicArm::SyncFromCurrentSim() {
  if (!Sim || SimArmIndex < 0 || SimArmIndex >= Sim->Arms.Num()) {
    return;
  }
  SyncFromSim(Sim->Arms[SimArmIndex]);
}

void ABasicArm::UpdateHeldClusterOnly() {
  if (!Sim || SimArmIndex < 0 || SimArmIndex >= Sim->Arms.Num()) {
    return;
  }

  // Only update the held orb data and visual, don't touch orientation or other
  // state
  const FArmState &SimState = Sim->Arms[SimArmIndex];

  // Update holding state and item payload only
  bool bWasHolding = bHolding;
  bHolding = SimState.bHolding;

  if (bHolding) {
    HeldOrb = SimState.Held;
  } else {
    HeldOrb = FItem();
  }

  // Only update visual if holding state changed or cluster composition changed
  const int32 NewOrbCount = bHolding ? HeldOrb.Orbs.Num() : 0;
  const bool bClusterChanged = (NewOrbCount != LastHeldOrbCount);

  if (bWasHolding != bHolding || (bHolding && bClusterChanged)) {
    UpdateHeldOrbVisual();
  }
}

void ABasicArm::DetachHeldOrb() {
  if (!HeldOrbComp)
    return;

  // Destroy any child components (cluster meshes)
  TArray<USceneComponent *> ChildComps;
  HeldOrbComp->GetChildrenComponents(false, ChildComps);
  for (USceneComponent *C : ChildComps) {
    if (C && C != HeldOrbComp) {
      C->DestroyComponent();
    }
  }

  HeldOrbComp->SetStaticMesh(nullptr);
  HeldOrbComp->SetVisibility(false, true);
  HeldOrbComp->DetachFromComponent(
      FDetachmentTransformRules::KeepWorldTransform);
}

void ABasicArm::UpdateHeldClusterTransforms(float InAlpha) {
  // Interpolate cluster facing (degrees) just like reach
  const float StartDeg = NormDeg(HeldClusterFacingStartDeg);
  const float EndDeg = NormDeg(HeldClusterFacingEndDeg);
  const float DeltaDeg = FMath::FindDeltaAngleDegrees(StartDeg, EndDeg);
  const float ClusterYawDeg = NormDeg(StartDeg + DeltaDeg * InAlpha);

  // Apply as a local rotation on the held container so the whole cluster spins
  if (HeldOrbComp) {
    HeldOrbComp->SetRelativeRotation(FRotator(0.f, ClusterYawDeg, 0.f));
  }
}

FVector ABasicArm::ComputeGripLocalOffset() const {
  const float Tile = (Driver ? Driver->GetTileSize() : 100.f);
  const float Dist = (GripDistanceOverride > 0.f)
                         ? GripDistanceOverride
                         : (float)FMath::Max(1, ReachTiles) * Tile;

  // Forward = +X (assumed arm mesh forward axis)
  return FVector(Dist, GripSideOffset, GripZOffset);
}

void ABasicArm::GenerateBondedOrbMeshes() {
  UStaticMesh *BondedOrbMesh = GetBondedOrbMesh();
  if (!bHolding || !HeldOrb.bIsCluster || !BondedOrbMesh || !Sim) {
    return;
  }

  // Get tile size from driver
  const float Tile = (Driver ? Driver->GetTileSize() : 100.f);

  // Clear existing bonded orb meshes
  TArray<USceneComponent *> ChildComps;
  HeldOrbComp->GetChildrenComponents(false, ChildComps);
  for (USceneComponent *C : ChildComps) {
    if (C && C->GetName().StartsWith(TEXT("BondedOrb_"))) {
      C->DestroyComponent();
    }
  }

  // Get the bonds for this held cluster
  const TArray<USimWorldHex::FBond> &Bonds = Sim->GetBonds();

  // Find bonds that involve sub-orbs in this held cluster
  for (const auto &Bond : Bonds) {
    if (!Bond.bActive)
      continue;

    // Check if both items are in this held cluster
    bool bItemAInCluster = false;
    bool bItemBInCluster = false;
    FVector PosA = FVector::ZeroVector;
    FVector PosB = FVector::ZeroVector;

    for (const FOrbData &Sub : HeldOrb.Orbs) {
      if (Sub.ItemId == Bond.ItemA) {
        bItemAInCluster = true;
        // Calculate position using the same logic as sub-orb positioning
        const EHexOrientation GridOrientation =
            (Driver ? Driver->GetGridOrientation()
                    : EHexOrientation::PointyTop);
        const FVector2D HexOffset = HexCoord::AxialToXY(
            FHexAxial(Sub.dQ, Sub.dR), Tile, GridOrientation);
        FVector2D Off2D = HexOffset * ClusterOffsetScale;

        // Apply rotation based on GrabFacing
        // if (HeldOrb.GrabFacing >= 0)
        //{
        //	const float GrabYawDeg = 60.f * (HeldOrb.GrabFacing % 6);
        //	const float YawRad = FMath::DegreesToRadians(GrabYawDeg);
        //	const float CosA = FMath::Cos(YawRad);
        //	const float SinA = FMath::Sin(YawRad);
        //	const float RotX = Off2D.X * CosA - Off2D.Y * SinA;
        //	const float RotY = Off2D.X * SinA + Off2D.Y * CosA;
        //	Off2D = FVector2D(RotX, RotY);
        //}

        if (bInvertClusterVisualFacing) {
          Off2D *= -1.f;
        }

        PosA = FVector(Off2D.X, Off2D.Y, 0.f);
      }

      if (Sub.ItemId == Bond.ItemB) {
        bItemBInCluster = true;
        // Calculate position using the same logic as sub-orb positioning
        const EHexOrientation GridOrientation =
            (Driver ? Driver->GetGridOrientation()
                    : EHexOrientation::PointyTop);
        const FVector2D HexOffset = HexCoord::AxialToXY(
            FHexAxial(Sub.dQ, Sub.dR), Tile, GridOrientation);
        FVector2D Off2D = HexOffset * ClusterOffsetScale;

        // Apply rotation based on GrabFacing
        // if (HeldOrb.GrabFacing >= 0)
        //{
        //	const float GrabYawDeg = 60.f * (HeldOrb.GrabFacing % 6);
        //	const float YawRad = FMath::DegreesToRadians(GrabYawDeg);
        //	const float CosA = FMath::Cos(YawRad);
        //	const float SinA = FMath::Sin(YawRad);
        //	const float RotX = Off2D.X * CosA - Off2D.Y * SinA;
        //	const float RotY = Off2D.X * SinA + Off2D.Y * CosA;
        //	Off2D = FVector2D(RotX, RotY);
        //}

        if (bInvertClusterVisualFacing) {
          Off2D *= -1.f;
        }

        PosB = FVector(Off2D.X, Off2D.Y, 0.f);
      }
    }

    // If both items are in this cluster, create a bonded orb mesh
    if (bItemAInCluster && bItemBInCluster) {
      UStaticMeshComponent *BondMesh = NewObject<UStaticMeshComponent>(
          this,
          *FString::Printf(TEXT("BondedOrb_%d_%d"), Bond.ItemA, Bond.ItemB));
      BondMesh->RegisterComponent();
      BondMesh->AttachToComponent(
          HeldOrbComp, FAttachmentTransformRules::KeepRelativeTransform);
      BondMesh->SetStaticMesh(BondedOrbMesh);

      UMaterialInterface *BondedOrbMaterial = GetBondedOrbMaterial();
      if (BondedOrbMaterial) {
        BondMesh->SetMaterial(0, BondedOrbMaterial);
      }

      // Calculate midpoint and direction
      FVector Midpoint = (PosA + PosB) * 0.5f;
      FVector Direction = (PosB - PosA).GetSafeNormal();
      float Distance = FVector::Dist(PosA, PosB);

      // For a cylinder that's standing up by default, rotate it to align with
      // the bond direction
      FVector Up = Direction;
      FVector Right =
          FVector::CrossProduct(Up, FVector::UpVector).GetSafeNormal();
      if (Right.IsNearlyZero()) {
        Right = FVector::RightVector;
      }
      FVector Forward = FVector::CrossProduct(Right, Up).GetSafeNormal();

      // Create rotation matrix from the three axes
      FMatrix RotationMatrix = FMatrix(Forward, Right, Up, FVector::ZeroVector);
      FRotator Rotation = FQuat(RotationMatrix).Rotator();

      // Scale the cylinder to fit the bond length
      float BondedOrbScale = GetBondedOrbScale();
      FVector Scale = FVector(BondedOrbScale, BondedOrbScale, 1.f);

      // Set the transform
      BondMesh->SetRelativeLocation(Midpoint);
      BondMesh->SetRelativeRotation(Rotation);
      BondMesh->SetRelativeScale3D(Scale);
    }
  }
}

UStaticMesh *ABasicArm::GetBondedOrbMesh() const {
  if (UWorld *World = GetWorld()) {
    for (TActorIterator<AActor> It(World); It; ++It) {
      if (UOrbRenderer *Renderer =
              (*It)->FindComponentByClass<UOrbRenderer>()) {
        return Renderer->BondedOrbMesh;
      }
    }
  }
  return nullptr;
}

UMaterialInterface *ABasicArm::GetBondedOrbMaterial() const {
  if (UWorld *World = GetWorld()) {
    for (TActorIterator<AActor> It(World); It; ++It) {
      if (UOrbRenderer *Renderer =
              (*It)->FindComponentByClass<UOrbRenderer>()) {
        return Renderer->BondedOrbMaterial;
      }
    }
  }
  return nullptr;
}

float ABasicArm::GetBondedOrbScale() const {
  if (UWorld *World = GetWorld()) {
    for (TActorIterator<AActor> It(World); It; ++It) {
      if (UOrbRenderer *Renderer =
              (*It)->FindComponentByClass<UOrbRenderer>()) {
        return Renderer->BondedOrbScale;
      }
    }
  }
  return 0.15f; // Default fallback
}

// ------------ Timeline Widget Implementation ------------

void ABasicArm::InitializeTimelineWidget() {
  if (!TimelineWidget) {
    UE_LOG(LogAGArm, Warning, TEXT("BasicArm '%s': TimelineWidget is null"),
           *GetName());
    return;
  }

  // Set up the timeline widget
  // Get program from simulation state if available, otherwise use local Program
  TArray<FArmInstr> ProgramToUse = Program;
  if (Sim && SimArmIndex >= 0 && SimArmIndex < Sim->Arms.Num()) {
    const FArmState &SimState = Sim->Arms[SimArmIndex];
    // Convert simulation tape back to arm instructions for display
    ProgramToUse = ConvertTapeToArmInstructions(SimState.Tape);
    UE_LOG(LogAGArm, Log,
           TEXT("BasicArm '%s': Using simulation program (%d instructions) for "
                "timeline"),
           *GetName(), ProgramToUse.Num());
  } else {
    UE_LOG(LogAGArm, Log,
           TEXT("BasicArm '%s': Using local program (%d instructions) for "
                "timeline"),
           *GetName(), Program.Num());
  }

  TimelineWidget->SetArmProgram(ProgramToUse);

  // Set the associated arm in the UMG widget so it can sync changes back
  if (UUserWidget *WidgetInstance =
          TimelineWidget->GetTimelineWidgetInstance()) {
    if (UArmTimelineUMGWidget *UMGWidget =
            Cast<UArmTimelineUMGWidget>(WidgetInstance)) {
      UMGWidget->SetAssociatedArm(this);
    }
  }

  // Set the associated arm in the timeline widget for line drawing
  TimelineWidget->SetAssociatedArm(this);

  // Timeline visibility will be set by GameManager based on bShowArmTimelines
  TimelineWidget->SetTimelineScale(1.0f);

  // Position timeline widget above the arm for top-down view
  // Use actual arm position for positioning
  TimelineWidget->SetTimelineOffset(FVector(0.0f, 0.0f, 150.0f));

  // Set initial position using arm's world position (hex-based coordinates)
  FVector ArmWorldPosition = GetArmWorldPosition();
  TimelineWidget->UpdateWidgetPosition(ArmWorldPosition, GetActorRotation());

  UE_LOG(
      LogAGArm, Log,
      TEXT("BasicArm '%s': Timeline widget positioned at (%.2f, %.2f, %.2f)"),
      *GetName(), ArmWorldPosition.X, ArmWorldPosition.Y, ArmWorldPosition.Z);

  // Get command textures and widget class from GameManager
  if (UWorld *World = GetWorld()) {
    for (TActorIterator<AGameManager> It(World); It; ++It) {
      AGameManager *GameManager = *It;
      if (GameManager) {
        UE_LOG(LogAGArm, Log,
               TEXT("BasicArm '%s': Found GameManager, setting timeline "
                    "properties"),
               *GetName());
        UE_LOG(LogAGArm, Log, TEXT("  - ArmTimelineWidgetClass: %s"),
               GameManager->ArmTimelineWidgetClass
                   ? *GameManager->ArmTimelineWidgetClass->GetName()
                   : TEXT("NULL"));
        UE_LOG(LogAGArm, Log, TEXT("  - bShowArmTimelines: %s"),
               GameManager->bShowArmTimelines ? TEXT("true") : TEXT("false"));
        UE_LOG(LogAGArm, Log, TEXT("  - CommandTextures: %d"),
               GameManager->CommandTextures.Num());

        TimelineWidget->SetCommandTextures(GameManager->CommandTextures);
        TimelineWidget->SetDefaultCommandTexture(
            GameManager->DefaultCommandTexture);
        TimelineWidget->SetIconSize(GameManager->TimelineIconSize);
        TimelineWidget->SetIconBackgroundColor(
            GameManager->TimelineIconBackgroundColor);
        TimelineWidget->SetNormalCommandColor(
            GameManager->TimelineNormalCommandColor);
        TimelineWidget->SetCurrentCommandColor(
            GameManager->TimelineCurrentCommandColor);
        TimelineWidget->SetCompletedCommandColor(
            GameManager->TimelineCompletedCommandColor);
        TimelineWidget->SetTimelineWidgetClass(
            GameManager->ArmTimelineWidgetClass);
        TimelineWidget->SetTimelineScale(GameManager->TimelineWidgetScale);
        TimelineWidget->SetTimelineOffset(GameManager->TimelineWidgetOffset);
        TimelineWidget->SetTimelineWidgetVisible(
            GameManager->bShowArmTimelines);
        break;
      }
    }
  } else {
    UE_LOG(LogAGArm, Warning,
           TEXT("BasicArm '%s': No World found for GameManager lookup"),
           *GetName());
  }

  UE_LOG(LogAGArm, Log, TEXT("BasicArm '%s': TimelineWidget initialized"),
         *GetName());
}

TArray<FArmInstr>
ABasicArm::ConvertTapeToArmInstructions(const TArray<FOp> &Tape) const {
  TArray<FArmInstr> Instructions;

  for (const FOp &Op : Tape) {
    FArmInstr Instr;
    Instr.Arg = Op.Arg;

    switch (Op.Cmd) {
    case ECmd::Wait:
      Instr.Cmd = EArmCmd::Wait;
      break;
    case ECmd::Grab:
      Instr.Cmd = EArmCmd::Grab;
      break;
    case ECmd::Drop:
      Instr.Cmd = EArmCmd::Drop;
      break;
    case ECmd::RotateR:
      Instr.Cmd = EArmCmd::RotateCW;
      break;
    case ECmd::RotateL:
      Instr.Cmd = EArmCmd::RotateCCW;
      break;
    case ECmd::Extend:
      Instr.Cmd = EArmCmd::Extend;
      break;
    case ECmd::Retract:
      Instr.Cmd = EArmCmd::Retract;
      break;
    case ECmd::Reset:
      Instr.Cmd = EArmCmd::Reset;
      break;
    case ECmd::RotateHeldL:
      Instr.Cmd = EArmCmd::RotateHeldCCW;
      break;
    case ECmd::RotateHeldR:
      Instr.Cmd = EArmCmd::RotateHeldCW;
      break;
    case ECmd::WaitForBond:
      Instr.Cmd = EArmCmd::WaitForBond;
      break;
    default:
      Instr.Cmd = EArmCmd::NoOp;
      break;
    }

    Instructions.Add(Instr);
  }

  return Instructions;
}

void ABasicArm::UpdateTimelineWidget() {
  if (!TimelineWidget) {
    return;
  }

  // Use the original program, not the simulation tape (which includes
  // Reset-generated commands) This ensures Reset commands don't appear in the
  // timeline
  TArray<FArmInstr> ProgramToUse = Program;
  TimelineWidget->SetArmProgram(ProgramToUse);

  // Update current command and progress
  int32 CurrentCommandIndex = PC;
  float CommandProgress = 0.0f;

  // Calculate progress based on current state
  if (WaitLeft > 0) {
    // We're in a wait command, calculate progress
    const FArmInstr &CurrentInstr =
        (ProgramToUse.Num() > 0 && PC < ProgramToUse.Num()) ? ProgramToUse[PC]
                                                            : FArmInstr();
    if (CurrentInstr.Cmd == EArmCmd::Wait && CurrentInstr.Arg > 0) {
      CommandProgress = 1.0f - (float(WaitLeft) / float(CurrentInstr.Arg));
    }
  } else {
    // We're executing a command, use StepAlpha for smooth progress
    CommandProgress = StepAlpha;
  }

  TimelineWidget->SetCurrentCommand(CurrentCommandIndex, CommandProgress);

  // Update position to ensure widgets stay with arms (in case of any movement)
  FVector ArmWorldPosition = GetArmWorldPosition();
  TimelineWidget->UpdateWidgetPosition(ArmWorldPosition, GetActorRotation());
}

void ABasicArm::SetTimelineVisible(bool bVisible) {
  bShowTimeline = bVisible;

  if (TimelineWidget) {
    TimelineWidget->SetTimelineWidgetVisible(bVisible);
  }

  UE_LOG(LogAGArm, Log, TEXT("BasicArm '%s': Timeline visibility set to %s"),
         *GetName(), bVisible ? TEXT("true") : TEXT("false"));
}

FVector ABasicArm::GetArmWorldPosition() const {
  // Get the arm's world position based on its hex coordinates
  return HexToWorld(FHex(Q, R, Level)) + FVector(0, 0, BaseZ);
}
