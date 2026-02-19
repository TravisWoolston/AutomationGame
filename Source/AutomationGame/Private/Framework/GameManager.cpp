#include "Framework/GameManager.h"
#include "Actors/BasicArm.h"
#include "Actors/BasicWalker.h"
#include "Actors/HexTopDownPawn.h"
#include "Actors/OrbSimDriver.h"
#include "Actors/RequestBeacon.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/PoseableMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Data/OrbMaterialManager.h"
#include "DrawDebugHelpers.h"
#include "Engine/Engine.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Framework/GameTickManager.h"
#include "Framework/WaveTypes.h"
#include "Geometry/GoldbergGrid.h" // NEW
#include "Kismet/KismetRenderingLibrary.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialParameterCollectionInstance.h"
#include "Sim/HexCoord.h"
#include "Sim/HexTypes.h" // <-- ADDED for HexUtil
#include "Sim/OrbTypes.h"
#include "Sim/SimWorldHex.h" // <-- ADDED for USimWorldHex / FArmState
#include "UI/ArmTimelineWidget.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"

// Forward declaration for helper function
// (FHex, USimWorldHex, and AOrbSimDriver are already included via headers)

// Local helper to build orthonormal basis from Up + arbitrary forward (similar
// to HexFactoryActor)
namespace {
inline void BuildSafeBasisGM(const FVector &InUpRaw,
                             const FVector &InForwardRaw, FVector &OutF,
                             FVector &OutR, FVector &OutU) {
  OutU = InUpRaw.GetSafeNormal();
  if (OutU.IsNearlyZero())
    OutU = FVector::UpVector;
  FVector F = InForwardRaw - OutU * FVector::DotProduct(InForwardRaw, OutU);
  if (F.SizeSquared() < KINDA_SMALL_NUMBER) {
    F = FVector::CrossProduct(OutU, FVector::RightVector);
    if (F.SizeSquared() < KINDA_SMALL_NUMBER)
      F = FVector::CrossProduct(OutU, FVector::ForwardVector);
  }
  OutF = F.GetSafeNormal();
  OutR = FVector::CrossProduct(OutF, OutU).GetSafeNormal();
  if (OutR.IsNearlyZero()) {
    FVector Aux = FVector::CrossProduct(OutU, FVector::ForwardVector);
    if (Aux.IsNearlyZero())
      Aux = FVector::CrossProduct(OutU, FVector::RightVector);
    OutF = Aux.GetSafeNormal();
    OutR = FVector::CrossProduct(OutF, OutU).GetSafeNormal();
  }
  OutF = FVector::CrossProduct(OutU, OutR).GetSafeNormal();
}

// Walker calibration logic moved to GameManagerWalkerCalibration.cpp

static float SignedAngleAroundAxis(const FVector &From, const FVector &To,
                                   const FVector &Axis) {
  const FVector NAxis = Axis.GetSafeNormal();
  if (NAxis.IsNearlyZero()) {
    return 0.f;
  }

  FVector FromProj = From - NAxis * FVector::DotProduct(From, NAxis);
  FVector ToProj = To - NAxis * FVector::DotProduct(To, NAxis);

  if (!FromProj.Normalize() || !ToProj.Normalize()) {
    return 0.f;
  }

  const float Dot =
      FMath::Clamp(FVector::DotProduct(FromProj, ToProj), -1.f, 1.f);
  float Angle = FMath::Acos(Dot);
  const FVector Cross = FVector::CrossProduct(FromProj, ToProj);
  const float Sign = FVector::DotProduct(Cross, NAxis) >= 0.f ? 1.f : -1.f;
  Angle *= Sign;
  return Angle;
}
} // namespace

AGameManager::AGameManager() {
  PrimaryActorTick.bCanEverTick = true; // needed for auto-recenter

  // Initialize timeline system defaults
  bShowArmTimelines = false;
  TimelineWidgetScale = 1.0f;
  TimelineGlobalHeight = 250.0f;
  TimelineSeparation = 250.0f;
  TimelineRepulsionStrength = 2.0f;
  // Keep legacy offset zeroed effectively as we use GlobalHeight
  TimelineWidgetOffset = FVector(0.0f, 0.0f, 0.0f);
}

AGameManager::~AGameManager() {
  // Destructor
}

void AGameManager::BeginDestroy() {
  // Clean up walker calibration data before destruction (when UE closes or
  // actor is destroyed)
  if (bWalkerCalibrationActive) {
    CancelWalkerCalibration();
    UE_LOG(LogTemp, Log,
           TEXT("GameManager: Cancelled walker calibration on BeginDestroy"));
  }

  // Clear cluster storage maps safely
  ClusterCounts.Empty();
  StoredClusterTemplates.Empty();
  ClusterIcons.Empty();
  TemplateIdToCanonicalKey.Empty();
  StoredOrbsOnlyKeysCache.Empty();

  Super::BeginDestroy();
}

void AGameManager::BeginPlay() {
  Super::BeginPlay();

  // Initialize ripple and wave render targets
  InitializeRippleRenderTarget();
  InitializeWaveRenderTarget();

  // Cancel walker calibration if active (calibration is editor-only, not for
  // PIE) This runs when PIE starts
  if (bWalkerCalibrationActive) {
    CancelWalkerCalibration();
    UE_LOG(LogTemp, Log,
           TEXT("GameManager: Cancelled walker calibration on BeginPlay (PIE "
                "started)"));
  }

  // Load walker calibration settings from config to ensure runtime properties
  // are up to date
  LoadWalkerCalibration();

  // Auto-initialize leg calibration data if not already present
  AutoInitializeWalkerCalibration();

  AutoFindRefs();
  TryLoadInitial();
  EnsureISM();
  Rebuild();

  // Apply loaded calibration values to any walkers in the world/simulation
  ApplyCalibrationValuesToAllWalkers();

  const FVector AnchorLoc =
      GridAnchor ? GridAnchor->GetActorLocation() : GetActorLocation();
  LastGridCenter = AnchorLoc;

  UE_LOG(LogTemp, Log,
         TEXT("GameManager::BeginPlay - TickManager=%s, OrbSimDriver=%s, "
              "Anchor=(%.1f,%.1f,%.1f), TileSize=%.1f, Radius=%d"),
         TickManager ? *TickManager->GetName() : TEXT("null"),
         OrbSimDriver ? *OrbSimDriver->GetName() : TEXT("null"), AnchorLoc.X,
         AnchorLoc.Y, AnchorLoc.Z, TileSizeUU, Radius);

  // Apply timeline visibility settings to all existing arms
  UpdateAllArmTimelineWidgets();
}

void AGameManager::EndPlay(const EEndPlayReason::Type EndPlayReason) {
  // Cancel walker calibration if active (calibration is editor-only)
  if (bWalkerCalibrationActive) {
    CancelWalkerCalibration();
    UE_LOG(
        LogTemp, Log,
        TEXT(
            "GameManager: Cancelled walker calibration on EndPlay (Reason=%d)"),
        (int32)EndPlayReason);
  }

  Super::EndPlay(EndPlayReason);
}

void AGameManager::Tick(float DeltaSeconds) {
  Super::Tick(DeltaSeconds);

  // Clear ripple and wave render targets at start of each frame
  if (RippleActive) {
    ClearRippleRenderTarget();
  }
  if (WaveActive) {
    ClearWaveRenderTarget();
  }

  // Auto-update RT offset to follow player camera (top-down view)
  if (bAutoFollowPlayerCamera) {
    if (UWorld *World = GetWorld()) {
      // Find the player pawn (HexTopDownPawn)
      for (TActorIterator<AHexTopDownPawn> It(World); It; ++It) {
        AHexTopDownPawn *PlayerPawn = *It;
        if (PlayerPawn && PlayerPawn->IsValidLowLevel()) {
          // For top-down view, use the pawn's root location (camera looks
          // straight down)
          FVector PlayerLoc = PlayerPawn->GetActorLocation();
          RippleRTOffset = FVector2D(PlayerLoc.X, PlayerLoc.Y);
          break;
        }
      }
    }
  }

  // Draw discovery mode helix
  if (bDiscoveryMode) {
    DrawSourceHelix();
  }

  // Auto-update wave RT offset to follow player camera (top-down view)
  if (bAutoFollowPlayerCameraForWaves) {
    if (UWorld *World = GetWorld()) {
      // Find the player pawn (HexTopDownPawn)
      for (TActorIterator<AHexTopDownPawn> It(World); It; ++It) {
        AHexTopDownPawn *PlayerPawn = *It;
        if (PlayerPawn && PlayerPawn->IsValidLowLevel()) {
          // For top-down view, use the pawn's root location (camera looks
          // straight down)
          FVector PlayerLoc = PlayerPawn->GetActorLocation();
          WaveRTOffset = FVector2D(PlayerLoc.X, PlayerLoc.Y);
          break;
        }
      }
    }
  }

  // Update grid material parameters for ripple RT conversion
  if (GridTileMID) {
    // Get RT center position (can be offset to follow camera/viewport)
    FVector2D RTCenter;
    if (RippleRTOffset.IsNearlyZero()) {
      // Default: center RT at grid anchor
      FVector AnchorLoc =
          GridAnchor ? GridAnchor->GetActorLocation() : GetActorLocation();
      RTCenter = FVector2D(AnchorLoc.X, AnchorLoc.Y);
    } else {
      // Use custom offset (e.g., camera position)
      RTCenter = RippleRTOffset;
    }

    GridTileMID->SetVectorParameterValue(
        FName("RippleRTAnchor"),
        FLinearColor(RTCenter.X, RTCenter.Y, 0.0f, 1.0f));
    GridTileMID->SetScalarParameterValue(FName("RippleRTWorldSize"),
                                         RippleRenderTargetWorldSize);
    GridTileMID->SetVectorParameterValue(FName("RipplePositiveColor"),
                                         RipplePositiveColor);
    GridTileMID->SetVectorParameterValue(FName("RippleNegativeColor"),
                                         RippleNegativeColor);
    GridTileMID->SetScalarParameterValue(FName("RippleRTSize"),
                                         (float)RippleRenderTargetSize);

    UTextureRenderTarget2D *RT = GetRippleRenderTarget();
    if (RT) {
      GridTileMID->SetTextureParameterValue(FName("RippleRenderTarget"), RT);
    }

    // Update wave render target parameters
    FVector2D WaveRTCenter;
    if (WaveRTOffset.IsNearlyZero()) {
      // Default: center RT at grid anchor
      FVector AnchorLoc =
          GridAnchor ? GridAnchor->GetActorLocation() : GetActorLocation();
      WaveRTCenter = FVector2D(AnchorLoc.X, AnchorLoc.Y);
    } else {
      // Use custom offset (e.g., camera position)
      WaveRTCenter = WaveRTOffset;
    }

    GridTileMID->SetVectorParameterValue(
        FName("WaveRTAnchor"),
        FLinearColor(WaveRTCenter.X, WaveRTCenter.Y, 0.0f, 1.0f));
    GridTileMID->SetScalarParameterValue(FName("WaveRTWorldSize"),
                                         WaveRenderTargetWorldSize);
    GridTileMID->SetScalarParameterValue(FName("MaxWaveHeight"), MaxWaveHeight);
    GridTileMID->SetScalarParameterValue(
        FName("WaveTime"), GetWorld()->GetTimeSeconds() * WaveAnimationSpeed);
    GridTileMID->SetScalarParameterValue(FName("WaveRTSize"),
                                         (float)WaveRenderTargetSize);

    UTextureRenderTarget2D *WaveRT = GetWaveRenderTarget();
    if (WaveRT) {
      GridTileMID->SetTextureParameterValue(FName("WaveRenderTarget"), WaveRT);
    }
  }

  // Update active waves lifecycle and redraw to RT
  if (ActiveWaves.Num() > 0) {
    float CurrentTime = GetWorld()->GetTimeSeconds();
    for (int32 i = ActiveWaves.Num() - 1; i >= 0; --i) {
      if (CurrentTime > ActiveWaves[i].StartTime + ActiveWaves[i].LifeTime) {
        ActiveWaves.RemoveAt(i);
      }
    }

    // Redraw all active waves to the RT
    // Note: ClearWaveRenderTarget() was already called at start of Tick
    // Since we use PF_FloatRGBA and correct Additive Blending logic, this sums
    // up all active waves for the frame.
    if (WaveActive) {
      UCanvas *Canvas;
      FVector2D Size;
      FDrawToRenderTargetContext Context;
      UKismetRenderingLibrary::BeginDrawCanvasToRenderTarget(
          GetWorld(), WaveRenderTarget, Canvas, Size, Context);

      if (Canvas) {
        for (const FGridWaveInstance &Wave : ActiveWaves) {
          InternalDrawWaveStamp(Canvas, Size, Wave);
        }
      }

      UKismetRenderingLibrary::EndDrawCanvasToRenderTarget(GetWorld(), Context);
    }
  }

  if (!bAutoExpandAtRuntime)
    return;

  const FVector AnchorLoc =
      GridAnchor ? GridAnchor->GetActorLocation() : GetActorLocation();
  const float Dist = FVector::Dist2D(LastGridCenter, AnchorLoc);
  if (Dist > RecenterThresholdUU) {
    UE_LOG(LogTemp, Verbose,
           TEXT("GameManager::Tick - recentering grid (%.1f uu)"), Dist);
    Rebuild();
    LastGridCenter = AnchorLoc;
  }
}

#if WITH_EDITOR
void AGameManager::OnConstruction(const FTransform &) {
  EnsureISM();
  Rebuild();
}

void AGameManager::PostEditChangeProperty(
    FPropertyChangedEvent &PropertyChangedEvent) {
  Super::PostEditChangeProperty(PropertyChangedEvent);

  if (PropertyChangedEvent.Property) {
    const FName PropName = PropertyChangedEvent.Property->GetFName();
    if (PropName == GET_MEMBER_NAME_CHECKED(AGameManager, ClusterIconScale)) {
      ClusterIcons.Empty();
      UE_LOG(
          LogTemp, Log,
          TEXT("GameManager: ClusterIconScale changed - cleared icon cache"));
    } else if (PropName ==
               GET_MEMBER_NAME_CHECKED(AGameManager, DesiredStandHeight)) {
      DesiredStandHeight = FMath::Clamp(DesiredStandHeight, -200.f, 1000.f);
      if (bWalkerCalibrationActive && CalibratingWalker.IsValid()) {
        SetStandHeight();
      }
    } else if (PropName == GET_MEMBER_NAME_CHECKED(AGameManager,
                                                   StartingFootDistance) ||
               PropName == GET_MEMBER_NAME_CHECKED(AGameManager,
                                                   StartingFootOrientation)) {
      if (bWalkerCalibrationActive && CalibratingWalker.IsValid()) {
        USkeletalMeshComponent *BodyComp =
            CalibratingWalker->GetBodyComponent();
        if (BodyComp) {
          ApplyStartingFootDistance(BodyComp);
          // Force update to make changes visible immediately
          BodyComp->TickAnimation(0.f, false);
          // BodyComp->RefreshBoneTransforms();
          BodyComp->UpdateComponentToWorld();
          SetIKRigGoalPositions(BodyComp, StandHeight);
          UpdateWalkerDebugMeshes(BodyComp);

          UE_LOG(LogTemp, Log,
                 TEXT("GameManager: StartingFoot params updated - "
                      "Distance=%.1f, Orientation=%d"),
                 StartingFootDistance, StartingFootOrientation);

          // Apply to all walkers in simulation
          ApplyCalibrationValuesToAllWalkers();
        }
      }
    } else if (PropName == GET_MEMBER_NAME_CHECKED(AGameManager,
                                                   bShowLegTileQuadrants) ||
               PropName == GET_MEMBER_NAME_CHECKED(AGameManager,
                                                   QuadrantRotationOffset) ||
               PropName == GET_MEMBER_NAME_CHECKED(AGameManager,
                                                   bDynamicBodyRotation)) {
      if (bWalkerCalibrationActive) {
        if (CalibratingWalker.IsValid()) {
          CalibratingWalker->bDynamicBodyRotation = bDynamicBodyRotation;
        }
        UpdateLegTileQuadrantVisualization();
      }
      ApplyCalibrationValuesToAllWalkers(); // Propagate dynamic rotation
                                            // setting
    } else if (PropName ==
               GET_MEMBER_NAME_CHECKED(AGameManager, ReachDistance)) {
      if (bWalkerCalibrationActive) {
        UpdateLegTileQuadrantVisualization();
      }
      // Apply to all walkers in simulation
      ApplyCalibrationValuesToAllWalkers();
    } else if (PropName == GET_MEMBER_NAME_CHECKED(AGameManager, StepHeight) ||
               PropName ==
                   GET_MEMBER_NAME_CHECKED(AGameManager, StepRaiseTicks) ||
               PropName ==
                   GET_MEMBER_NAME_CHECKED(AGameManager, TilesPerStep) ||
               PropName ==
                   GET_MEMBER_NAME_CHECKED(AGameManager, StepLowerTicks) ||
               PropName == GET_MEMBER_NAME_CHECKED(AGameManager, StandHeight)) {
      if (bWalkerCalibrationActive && CalibratingWalker.IsValid()) {
        USkeletalMeshComponent *BodyComp =
            CalibratingWalker->GetBodyComponent();
        if (BodyComp) {
          if (PropName == GET_MEMBER_NAME_CHECKED(AGameManager, StepHeight) &&
              bActiveFootPreview) {
            // Apply step height to active foot
            SetStepHeight();
          } else if (PropName ==
                     GET_MEMBER_NAME_CHECKED(AGameManager, StandHeight)) {
            // StandHeight is handled by SetStandHeight() via DesiredStandHeight
          } else {
            // Reset all feet to base position when preview is disabled
            ApplyStartingFootDistance(BodyComp);
            SetIKRigGoalPositions(BodyComp, StandHeight);
            UpdateWalkerDebugMeshes(BodyComp);
          }
        }
      }
      // Apply to all walkers in simulation
      ApplyCalibrationValuesToAllWalkers();
    } else if (PropName == GET_MEMBER_NAME_CHECKED(AGameManager, ActiveFoot) ||
               PropName ==
                   GET_MEMBER_NAME_CHECKED(AGameManager, bActiveFootPreview)) {
      if (bWalkerCalibrationActive && CalibratingWalker.IsValid()) {
        USkeletalMeshComponent *BodyComp =
            CalibratingWalker->GetBodyComponent();
        if (BodyComp) {
          if (bActiveFootPreview) {
            // Apply step height to active foot
            SetStepHeight();
          } else {
            // Reset all feet to base position when preview is disabled
            ApplyStartingFootDistance(BodyComp);
            SetIKRigGoalPositions(BodyComp, StandHeight);
            UpdateWalkerDebugMeshes(BodyComp);
          }
        }
      }
    } else if (PropName ==
               GET_MEMBER_NAME_CHECKED(AGameManager, MinimumReachDistance)) {
      MinimumReachDistance = FMath::Max(0.f, MinimumReachDistance);
      if (ReachDistance > 0 && MinimumReachDistance > ReachDistance) {
        UE_LOG(
            LogTemp, Warning,
            TEXT("GameManager: MinimumReachDistance (%.2f) exceeds "
                 "ReachDistance (%d) - clamping to ReachDistance when applied"),
            MinimumReachDistance, ReachDistance);
      }
      // Apply to all walkers in simulation
      ApplyCalibrationValuesToAllWalkers();
      UE_LOG(LogTemp, Log,
             TEXT("GameManager: MinimumReachDistance updated to %.2f"),
             MinimumReachDistance);
    } else if (PropName.ToString().Contains(TEXT("Timeline")) ||
               PropName ==
                   GET_MEMBER_NAME_CHECKED(AGameManager, bShowArmTimelines) ||
               PropName ==
                   GET_MEMBER_NAME_CHECKED(AGameManager, CommandTextures) ||
               PropName == GET_MEMBER_NAME_CHECKED(AGameManager,
                                                   ArmTimelineWidgetClass)) {
      UpdateAllArmTimelineWidgets();
      UE_LOG(LogTemp, Log,
             TEXT("GameManager: Arm Timeline settings updated - refreshed all "
                  "arm widgets"));
    }
  }
}
#endif

void AGameManager::AutoFindRefs() {
  UWorld *W = GetWorld();
  if (!W)
    return;

  if (!TickManager)
    for (TActorIterator<AGameTickManager> It(W); It; ++It) {
      TickManager = *It;
      break;
    }

  if (!OrbSimDriver)
    for (TActorIterator<AOrbSimDriver> It(W); It; ++It) {
      OrbSimDriver = *It;
      break;
    } // <-- typed

  if (!OrbSimDriver && OrbSimDriverClass) {
    FActorSpawnParameters S;
    S.SpawnCollisionHandlingOverride =
        ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    OrbSimDriver = W->SpawnActor<AOrbSimDriver>(
        OrbSimDriverClass, GetActorTransform(), S); // <-- typed
  }

  // Sync OrbSimDriver parameters with GameManager (GameManager is the source of
  // truth)
  SyncOrbSimDriverParameters();
}

void AGameManager::SetGridRadius(int32 NewRadius) {
  Radius = FMath::Max(1, NewRadius);
  UE_LOG(LogTemp, Log, TEXT("GameManager::SetGridRadius - Radius=%d"), Radius);
  Rebuild();
}

void AGameManager::SetTileMeshAndMaterial(UStaticMesh *Mesh,
                                          UMaterialInterface *Mat) {
  TileMesh = Mesh;
  TileMaterial = Mat;
  UE_LOG(LogTemp, Log,
         TEXT("GameManager::SetTileMeshAndMaterial - Mesh=%s, Mat=%s"),
         TileMesh ? *TileMesh->GetName() : TEXT("null"),
         TileMaterial ? *TileMaterial->GetName() : TEXT("null"));
  Rebuild();
}

void AGameManager::TryLoadInitial() {
  if (InitialMapToLoad.IsEmpty() || !OrbSimDriver)
    return;
  const FString Cmd = FString::Printf(TEXT("LoadMap %s"), *InitialMapToLoad);
  if (!OrbSimDriver->CallFunctionByNameWithArguments(*Cmd, *GLog, nullptr,
                                                     true)) {
    UE_LOG(LogTemp, Warning,
           TEXT("GameManager: OrbSimDriver didn't accept Exec 'LoadMap'."));
  } else {
    UE_LOG(LogTemp, Log,
           TEXT("GameManager::TryLoadInitial - Requested map '%s'"),
           *InitialMapToLoad);
  }
}

void AGameManager::EnsureISM() {
  if (!ISM) {
    ISM = FindComponentByClass<UInstancedStaticMeshComponent>();
    if (!ISM) {
      ISM = NewObject<UInstancedStaticMeshComponent>(this, TEXT("GridISM"));
      ISM->SetupAttachment(GetRootComponent());
      ISM->RegisterComponent();
    }
    ISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
  }
}

void AGameManager::BuildGridInstances() {
  if (!ISM)
    return;
  ISM->ClearInstances();
  if (TileMesh)
    ISM->SetStaticMesh(TileMesh);

  // Create/update MID and apply it to ISM
  UpdateGridTileMID();
  if (GridTileMID) {
    ISM->SetMaterial(0, GridTileMID);
  } else if (TileMaterial) {
    ISM->SetMaterial(0, TileMaterial);
  }

  const float TileSpacing = TileSizeUU; // alias for clarity
  const float S = FMath::Max(1.f, TileSpacing);
  const FVector Origin =
      GridAnchor ? GridAnchor->GetActorLocation() : GetActorLocation();
  const float OriYawOffsetDegPlanar =
      (GridOrientation == EHexOrientation::PointyTop) ? 30.f : 0.f;
  USimWorldHex *Sim = OrbSimDriver ? OrbSimDriver->GetSim() : nullptr;

  if (IsGoldbergMode()) {
    // Build Goldberg grid
    if (!GoldbergGrid)
      GoldbergGrid = NewObject<UGoldbergGrid>(this, TEXT("GoldbergGrid"));
    GoldbergGrid->Build(GoldbergFrequency, GoldbergRadius, Origin);
    const TArray<FGoldbergCell> &Cells = GoldbergGrid->GetCells();
    const bool bPointy = (GridOrientation == EHexOrientation::PointyTop);

    for (int32 i = 0; i < Cells.Num(); ++i) {
      const FGoldbergCell &C = Cells[i];
      if (C.bPentagon)
        continue; // Skip pentagon cells

      FVector Up = (C.Position - Origin).GetSafeNormal();
      if (Up.IsNearlyZero())
        Up = FVector::UpVector;
      FVector East = FVector::CrossProduct(FVector::UpVector, Up);
      if (East.IsNearlyZero())
        East = FVector::RightVector;
      East.Normalize();
      FVector North = FVector::CrossProduct(Up, East).GetSafeNormal();

      // Unreal forward = X (use East), right = Y (use North), up = Z
      FMatrix M(East, North, Up, FVector::ZeroVector);
      FTransform XForm(M.Rotator(), C.Position, FVector(1));

      if (bPointy) {
        // Apply same 30 deg yaw offset used in planar grid so hex mesh aligns
        // consistently
        FQuat OffsetRot(Up, FMath::DegreesToRadians(30.f));
        XForm.ConcatenateRotation(OffsetRot);
      }
      ISM->AddInstance(XForm);
    }
    return;
  }

  // Planar grid path
  const auto ToWorld = [S, Origin, this](int32 Q, int32 R) {
    FVector2D XY = HexCoord::AxialToXY(FHexAxial(Q, R), S, GridOrientation);
    FVector2D RotXY = HexCoord::Rotate2D(XY, GridYawDeg);
    return Origin + FVector(RotXY.X, RotXY.Y, 0.f);
  };

  for (int r = -Radius; r <= Radius; ++r) {
    for (int q = -Radius; q <= Radius; ++q) {
      int s = -q - r;
      if (FMath::Max3(FMath::Abs(q), FMath::Abs(r), FMath::Abs(s)) > Radius)
        continue;

      // Skip background tile if an Output or Source tile exists at this hex
      if (Sim && Sim->TileMap.Contains(FHex(q, r, 0))) {
        const FTile &PT = Sim->TileMap[FHex(q, r, 0)];
        if (PT.Kind == ETileKind::Output || PT.Kind == ETileKind::Source)
          continue;
      }

      FVector P = ToWorld(q, r);
      FRotator RotR(0.f, GridYawDeg + OriYawOffsetDegPlanar, 0.f);
      ISM->AddInstance(FTransform(RotR, P, FVector(1)));
    }
  }
}

FVector AGameManager::GetGoldbergCellPosition(int32 CellIndex) const {
  if (!GoldbergGrid || CellIndex < 0 ||
      CellIndex >= GoldbergGrid->GetCells().Num()) {
    return FVector::ZeroVector;
  }
  return GoldbergGrid->GetCells()[CellIndex].Position;
}

FRotator AGameManager::GetGoldbergCellRotation(int32 CellIndex) const {
  if (!GoldbergGrid) {
    return FRotator::ZeroRotator;
  }
  return GoldbergGrid->GetCellRotation(CellIndex);
}

void AGameManager::Rebuild() {
  EnsureISM();
  BuildGridInstances();
  // Sync OrbSimDriver whenever we rebuild the grid
  SyncOrbSimDriverParameters();
}

void AGameManager::ToggleGridVisibility() {
  if (ISM) {
    SetGridVisibility(!ISM->IsVisible());
  }
}

void AGameManager::SetGridVisibility(bool bVisible) {
  if (ISM) {
    ISM->SetVisibility(bVisible, true);
    UE_LOG(LogTemp, Log, TEXT("GameManager: Set grid visibility: %s"),
           bVisible ? TEXT("Visible") : TEXT("Hidden"));

    // Update all RequestBeacon actors
    if (UWorld *World = GetWorld()) {
      for (TActorIterator<ARequestBeacon> It(World); It; ++It) {
        ARequestBeacon *Beacon = *It;
        if (Beacon && IsValid(Beacon)) {
          Beacon->SetIsGridVisible(bVisible);
        }
      }
    }
  }
}

// Local helper to mirror TestLevelDirector conversion so runtime-placed arms
// work too.
static TArray<FOp> ConvertProgramToOps(const TArray<FArmInstr> &Program) {
  TArray<FOp> Out;
  if (Program.Num() == 0) {
    Out.Add({ECmd::Wait, 1});
    return Out;
  }
  for (const FArmInstr &I : Program) {
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
    default:
      Out.Add({ECmd::Wait, 1});
      break;
    }
  }
  return Out;
}

ABasicArm *AGameManager::SpawnBasicArmAt(const FHex &Hex, uint8 Facing,
                                         const TArray<FArmInstr> &Program,
                                         bool bLoop) {
  if (!BasicArmClass) {
    UE_LOG(LogTemp, Warning,
           TEXT("GameManager: BasicArmClass not set; cannot spawn."));
    return nullptr;
  }
  if (!OrbSimDriver) {
    UE_LOG(LogTemp, Warning,
           TEXT("GameManager: OrbSimDriver not set; cannot compute grid "
                "transform for arm."));
    return nullptr;
  }

  FVector Loc = OrbSimDriver->MapHexToWorld(Hex);
  float Yaw = OrbSimDriver->GetGridYaw() + 60.f * float(Facing % 6);

  FActorSpawnParameters S;
  S.SpawnCollisionHandlingOverride =
      ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
  // Spawn at neutral rotation - bone rotation system will handle facing
  // direction
  ABasicArm *Arm = GetWorld()->SpawnActor<ABasicArm>(
      BasicArmClass, FTransform(FRotator::ZeroRotator, Loc), S);
  if (!Arm)
    return nullptr;

  // Initialize actor-side data
  Arm->Driver = OrbSimDriver;
  Arm->Q = Hex.Q;
  Arm->R = Hex.R;
  Arm->Level = Hex.Level;
  Arm->FacingDir = Facing % 6;
  Arm->StartingRotation = Facing % 6;
  Arm->StartingReach =
      Arm->ReachTiles; // Use current ReachTiles as starting reach
  // Don't call SetProgram yet - need SimArmIndex first

  // --- Add to simulation so it actually runs ---
  if (USimWorldHex *Sim = OrbSimDriver->GetSim()) {
    FArmState ArmState;
    ArmState.BaseHex = Hex;
    ArmState.Facing = static_cast<EHexDir>(Facing % 6);
    ArmState.HomeFacing = static_cast<EHexDir>(Arm->StartingRotation % 6);
    ArmState.HomeArmLen = FMath::Clamp(Arm->StartingReach, 1, 6);
    ArmState.HomeTrackIdx = 0;
    ArmState.ArmLen = FMath::Clamp(Arm->ReachTiles, 1, 6);
    ArmState.TrackIdx = 0;
    ArmState.PC = 0;
    ArmState.bHolding = false;
    ArmState.WaitCounter = 0; // (if field exists) NEW
    // Don't convert program here - let SetProgram handle it after SimArmIndex
    // is assigned
    ArmState.Tape.Add({ECmd::Wait, 1}); // Just add a default wait

    const int32 ArmIndex = Sim->AddArm(ArmState);
    Arm->SimArmIndex = ArmIndex;

    // Now that SimArmIndex is set, we can call SetProgram which will properly
    // expand Reset commands
    Arm->SetProgram(Program, bLoop);
    Arm->Start();

    UE_LOG(LogTemp, Warning,
           TEXT("GameManager: Spawned + registered arm at Hex(%d,%d) Facing=%d "
                "SimIndex=%d TapeLen=%d"),
           Hex.Q, Hex.R, (int32)ArmState.Facing, ArmIndex, ArmState.Tape.Num());
  } else {
    UE_LOG(LogTemp, Warning,
           TEXT("GameManager: Arm spawned visually but Sim is null; program "
                "will not run."));
  }
  return Arm;
}

ABasicWalker *AGameManager::SpawnBasicWalkerAt(
    const FHex &Hex, const TArray<FWalkerInstr> &Program, bool bLoop) {
  if (!BasicWalkerClass) {
    UE_LOG(LogTemp, Warning,
           TEXT("GameManager: BasicWalkerClass not set; cannot spawn."));
    return nullptr;
  }
  if (!OrbSimDriver) {
    UE_LOG(LogTemp, Warning,
           TEXT("GameManager: OrbSimDriver not set; cannot compute grid "
                "transform for walker."));
    return nullptr;
  }

  FVector Loc = OrbSimDriver->MapHexToWorld(Hex);
  // Apply BaseZ offset so walker spawns at correct height (default is 10.f)
  // Get BaseZ from the walker class default object
  float WalkerBaseZ = 10.f;
  if (BasicWalkerClass) {
    if (const ABasicWalker *DefaultWalker =
            GetDefault<ABasicWalker>(BasicWalkerClass)) {
      WalkerBaseZ = DefaultWalker->BaseZ;
    }
  }
  Loc.Z += WalkerBaseZ;

  FActorSpawnParameters SpawnParams;
  SpawnParams.SpawnCollisionHandlingOverride =
      ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
  ABasicWalker *Walker = GetWorld()->SpawnActor<ABasicWalker>(
      BasicWalkerClass, FTransform(FRotator::ZeroRotator, Loc), SpawnParams);
  if (!Walker)
    return nullptr;

  // Initialize actor-side data
  Walker->Driver = OrbSimDriver;
  Walker->Q = Hex.Q;
  Walker->R = Hex.R;
  Walker->HomeHex = Hex;

  // Set GameManager-assigned values on BasicWalker for runtime
  // verification/editing
  Walker->StepHeight = StepHeight;
  Walker->StepRaiseTicks = StepRaiseTicks;
  Walker->TilesPerStep = TilesPerStep;
  Walker->StepLowerTicks = StepLowerTicks;
  Walker->ReachDistance = ReachDistance;
  Walker->MinimumReachDistance = SavedWalkerCalibration.MinimumReachDistance;
  Walker->StandHeight = SavedWalkerCalibration.StandHeight;
  Walker->StartingFootDistance = SavedWalkerCalibration.StartingFootDistance;
  Walker->bDynamicBodyRotation = SavedWalkerCalibration.bDynamicBodyRotation;
  Walker->Q = Hex.Q;
  Walker->R = Hex.R;
  Walker->Level = Hex.Level;

  // Don't call SetProgram yet - need SimWalkerIndex first

  // --- Add to simulation so it actually runs ---
  if (USimWorldHex *Sim = OrbSimDriver->GetSim()) {
    FWalkerState WalkerState;
    WalkerState.BaseHex = Hex;
    WalkerState.HomeHex = Hex;
    WalkerState.TargetHex = Hex;
    WalkerState.PC = 0;
    WalkerState.WaitCounter = 0;
    WalkerState.bMoving = false;
    WalkerState.MovementProgress = 0.0f;

    // Apply calibration settings from GameManager
    WalkerState.StepHeight = StepHeight;
    WalkerState.StepRaiseTicks = StepRaiseTicks;
    WalkerState.TilesPerStep = TilesPerStep;
    WalkerState.StepLowerTicks = StepLowerTicks;
    WalkerState.ReachDistance = ReachDistance;
    WalkerState.MinimumReachDistance =
        SavedWalkerCalibration
            .MinimumReachDistance; // Apply saved minimum reach distance
    WalkerState.StandHeight =
        SavedWalkerCalibration.StandHeight; // Apply saved stand height
    WalkerState.QuadrantRotationOffset =
        SavedWalkerCalibration
            .QuadrantRotationOffset; // Apply saved rotation offset

    // Don't set program here - let SetProgram handle it after SimWalkerIndex is
    // assigned
    WalkerState.Program = TArray<FWalkerInstr>(); // Empty for now

    const int32 WalkerIndex = Sim->AddWalker(WalkerState);
    Walker->SimWalkerIndex = WalkerIndex;

    // Update BasicWalker's StartingFootDistance property (already set above,
    // but ensure it's current)
    Walker->StartingFootDistance = SavedWalkerCalibration.StartingFootDistance;

    // Apply StartingFootDistance to walker's foot positions in the sim
    // This applies the calibration setting to runtime walkers
    const int32 RingRadius =
        FMath::RoundToInt(SavedWalkerCalibration.StartingFootDistance);
    if (WalkerIndex >= 0 && WalkerIndex < Sim->Walkers.Num() &&
        RingRadius > 0) {
      FWalkerState &SimWalkerState = Sim->Walkers[WalkerIndex];

      // Get body world position
      const FVector BodyWorldPos = OrbSimDriver->MapHexToWorld(Hex);
      const FVector BodyWorld2D = FVector(BodyWorldPos.X, BodyWorldPos.Y, 0.f);

      // Generate Ring of hex offsets (relative to center 0,0)
      TArray<FHex> Ring;
      FHex H = FHex(-RingRadius, RingRadius); // Start at SW corner
      for (int SideIdx = 0; SideIdx < 6; ++SideIdx) {
        for (int j = 0; j < RingRadius; ++j) {
          Ring.Add(H);
          H = HexUtil::Neighbor(H, (EHexDir)SideIdx);
        }
      }

      // Determine body orientation
      FVector BodyForward = FVector::ForwardVector;
      FVector BodyRight = FVector::RightVector;

      if (Walker) {
        const FTransform BodyTransform =
            Walker->GetBodyComponent()->GetComponentTransform();
        BodyForward = BodyTransform.GetUnitAxis(EAxis::X);
        BodyRight = BodyTransform.GetUnitAxis(EAxis::Y);
        // Project to 2D
        BodyForward =
            FVector(BodyForward.X, BodyForward.Y, 0.f).GetSafeNormal();
        BodyRight = FVector(BodyRight.X, BodyRight.Y, 0.f).GetSafeNormal();
        if (BodyForward.IsNearlyZero())
          BodyForward = FVector::ForwardVector;
        if (BodyRight.IsNearlyZero())
          BodyRight = FVector::RightVector;
      }

      // Apply QuadrantRotationOffset
      if (SavedWalkerCalibration.QuadrantRotationOffset != 0.f) {
        FQuat Rot = FQuat(FVector::UpVector,
                          FMath::DegreesToRadians(
                              SavedWalkerCalibration.QuadrantRotationOffset));
        BodyForward = Rot.RotateVector(BodyForward);
        BodyRight = Rot.RotateVector(BodyRight);
      }

      // Quadrant Definitions matching CalibrateWalker
      struct FQuadDef {
        FVector Dir;
        EWalkerFoot Leg;
      };
      FQuadDef Quads[] = {{BodyForward, SavedWalkerCalibration.NQuadrantLeg},
                          {BodyRight, SavedWalkerCalibration.EQuadrantLeg},
                          {-BodyForward, SavedWalkerCalibration.SQuadrantLeg},
                          {-BodyRight, SavedWalkerCalibration.WQuadrantLeg}};

      // Helper to map foot enum to index
      auto GetLegIndex = [](EWalkerFoot Foot) -> int32 {
        switch (Foot) {
        case EWalkerFoot::DFoot:
          return 0;
        case EWalkerFoot::LFoot:
          return 1;
        case EWalkerFoot::RFoot:
          return 2;
        case EWalkerFoot::UFoot:
          return 3;
        default:
          return -1;
        }
      };

      FFootState *Feet[] = {&SimWalkerState.DFoot, &SimWalkerState.LFoot,
                            &SimWalkerState.RFoot, &SimWalkerState.UFoot};
      const float TileSize = OrbSimDriver->GetTileSize();

      for (const FQuadDef &Q : Quads) {
        int32 FootIdx = GetLegIndex(Q.Leg);
        if (FootIdx < 0)
          continue;

        // Target position in world space
        FVector IdealPos =
            BodyWorld2D +
            Q.Dir * (SavedWalkerCalibration.StartingFootDistance * TileSize);

        // Find best tile in Ring
        FHex BestOffsetHex = Ring[0];
        float BestDistSq = FLT_MAX;

        for (const FHex &OffsetH : Ring) {
          FHex AbsH(Hex.Q + OffsetH.Q, Hex.R + OffsetH.R, Hex.Level);
          FVector WorldPos = OrbSimDriver->MapHexToWorld(AbsH);
          float DistSq = FVector::DistSquared2D(WorldPos, IdealPos);
          if (DistSq < BestDistSq) {
            BestDistSq = DistSq;
            BestOffsetHex = OffsetH;
          }
        }

        // Apply StartingFootOrientation (shift index in ring)
        if (SavedWalkerCalibration.StartingFootOrientation != 0) {
          int32 Index = Ring.IndexOfByKey(BestOffsetHex);
          if (Index != INDEX_NONE) {
            int32 NewIndex =
                (Index + SavedWalkerCalibration.StartingFootOrientation) %
                Ring.Num();
            if (NewIndex < 0)
              NewIndex += Ring.Num();
            BestOffsetHex = Ring[NewIndex];
          }
        }

        // Set new foot position
        FHex NewFootHex(Hex.Q + BestOffsetHex.Q, Hex.R + BestOffsetHex.R,
                        Hex.Level);

        // Validate distance
        const int32 dq = NewFootHex.Q - Hex.Q;
        const int32 dr = NewFootHex.R - Hex.R;
        const int32 HexDist = FMath::Max(
            FMath::Max(FMath::Abs(dq), FMath::Abs(dr)), FMath::Abs(dq + dr));

        bool bValid = true;
        if (ReachDistance > 0 && HexDist > ReachDistance) {
          bValid = false;
          UE_LOG(LogTemp, Warning,
                 TEXT("GameManager: StartingFootDistance for foot %d resulted "
                      "in hex too far (dist=%d > %d), keeping original"),
                 FootIdx, HexDist, ReachDistance);
        } else if (SavedWalkerCalibration.MinimumReachDistance > 0.f &&
                   (float)HexDist <
                       SavedWalkerCalibration.MinimumReachDistance) {
          bValid = false;
          UE_LOG(LogTemp, Warning,
                 TEXT("GameManager: StartingFootDistance for foot %d resulted "
                      "in hex too close (dist=%d < %.1f), keeping original"),
                 FootIdx, HexDist, SavedWalkerCalibration.MinimumReachDistance);
        }

        if (bValid) {
          Feet[FootIdx]->CurrentTile = NewFootHex;
          Feet[FootIdx]->TargetTile = NewFootHex;
          Feet[FootIdx]->StepState = EFootStepState::Planted;
        }
      }

      UE_LOG(LogTemp, Log,
             TEXT("GameManager: Applied StartingFootDistance=%.1f (Radius=%d, "
                  "Orientation=%d) to walker %d foot positions"),
             SavedWalkerCalibration.StartingFootDistance, RingRadius,
             SavedWalkerCalibration.StartingFootOrientation, WalkerIndex);
    }

    // Now that SimWalkerIndex is set, we can call SetProgram
    Walker->SetProgram(Program, bLoop);
    Walker->Start();

    UE_LOG(LogTemp, Warning,
           TEXT("GameManager: Spawned + registered walker at Hex(%d,%d) "
                "SimIndex=%d ProgramLen=%d, StepHeight=%.1f, ReachDist=%d, "
                "TilesPerStep=(%d,%.2f,%d), StartingFootDist=%.1f"),
           Hex.Q, Hex.R, WalkerIndex, Program.Num(), StepHeight, ReachDistance,
           StepRaiseTicks, TilesPerStep, StepLowerTicks,
           SavedWalkerCalibration.StartingFootDistance);
  } else {
    UE_LOG(LogTemp, Warning,
           TEXT("GameManager: Walker spawned visually but Sim is null; program "
                "will not run."));
  }

  return Walker;
}

// Walker calibration functions are implemented in
// GameManagerWalkerCalibration.cpp

void AGameManager::SyncOrbSimDriverParameters() {
  if (!OrbSimDriver)
    return;

  // Store previous values to detect changes
  static EHexOrientation PrevOrientation = EHexOrientation::PointyTop;
  static float PrevYaw = 0.f;
  static float PrevTileSize = 0.f;
  static float PrevOrbZOffset = 0.f;
  static float PrevGoldbergR = 0.f;
  static EGridMode PrevGridMode = EGridMode::Planar;

  const bool bParamsChanged =
      (PrevOrientation != GridOrientation) ||
      !FMath::IsNearlyEqual(PrevYaw, GridYawDeg, 0.01f) ||
      !FMath::IsNearlyEqual(PrevTileSize, TileSizeUU, 0.01f) ||
      !FMath::IsNearlyEqual(PrevOrbZOffset, OrbZOffset, 0.01f) ||
      !FMath::IsNearlyEqual(PrevGoldbergR, GoldbergRadius, 0.01f) ||
      (PrevGridMode != GridMode);

  OrbSimDriver->SetGridParameters(GridOrientation, GridYawDeg, TileSizeUU,
                                  OrbZOffset, GridAnchor);
  OrbSimDriver->SetGoldbergMode(IsGoldbergMode(), GoldbergGrid);

  // Only log if parameters actually changed
  if (bParamsChanged) {
    UE_LOG(
        LogTemp, Log,
        TEXT("GameManager: Synced OrbSimDriver params Orientation=%d "
             "Yaw=%.1f TileSpacing=%.1f ZOff=%.1f GridMode=%d GoldbergR=%.1f"),
        (int32)GridOrientation, GridYawDeg, TileSizeUU, OrbZOffset,
        (int32)GridMode, GoldbergRadius);

    // Update stored values
    PrevOrientation = GridOrientation;
    PrevYaw = GridYawDeg;
    PrevTileSize = TileSizeUU;
    PrevOrbZOffset = OrbZOffset;
    PrevGoldbergR = GoldbergRadius;
    PrevGridMode = GridMode;
  }

  // Propagate orb cap settings to sim
  if (USimWorldHex *Sim = OrbSimDriver->GetSim()) {
    Sim->bGridOrbLimit = bGridOrbLimit;
    Sim->GridOrbCapPerType = GridOrbCapPerType;
    Sim->MaxEjectWeight = MaxEjectWeight;
  }
}

FVector AGameManager::GetWorldForHex(const FHex &H) const {
  if (OrbSimDriver) {
    return OrbSimDriver->MapHexToWorld(H);
  }
  // Fallback to previous planar conversion
  const float S = FMath::Max(1.f, TileSizeUU);
  const FVector Origin =
      GridAnchor ? GridAnchor->GetActorLocation() : GetActorLocation();
  const FVector2D XY =
      HexCoord::AxialToXY(FHexAxial(H.Q, H.R), S, GridOrientation);
  const FVector2D RotXY = HexCoord::Rotate2D(XY, GridYawDeg);
  return Origin + FVector(RotXY.X, RotXY.Y, 0.f);
}

// ------------ Arm Timeline System Implementation ------------

void AGameManager::SetCommandTextures(
    const TMap<EArmCmd, UTexture2D *> &Textures) {
  CommandTextures = Textures;

  UE_LOG(LogTemp, Log,
         TEXT("GameManager: SetCommandTextures - %d textures set"),
         Textures.Num());

  // Update all existing arm timeline widgets
  if (UWorld *World = GetWorld()) {
    for (TActorIterator<ABasicArm> It(World); It; ++It) {
      ABasicArm *Arm = *It;
      if (Arm && Arm->IsValidLowLevel()) {
        UArmTimelineWidget *TimelineWidget =
            Arm->FindComponentByClass<UArmTimelineWidget>();
        if (TimelineWidget) {
          TimelineWidget->SetCommandTextures(CommandTextures);
        }
      }
    }
  }
}

UTexture2D *AGameManager::GetTextureForCommand(EArmCmd Command) const {
  if (const UTexture2D *const *FoundTexture = CommandTextures.Find(Command)) {
    return const_cast<UTexture2D *>(*FoundTexture);
  }

  return DefaultCommandTexture;
}

void AGameManager::SetTimelineVisibility(bool bVisible) {
  bShowArmTimelines = bVisible;

  // Update all existing arm timeline widgets
  if (UWorld *World = GetWorld()) {
    for (TActorIterator<ABasicArm> It(World); It; ++It) {
      ABasicArm *Arm = *It;
      if (Arm && Arm->IsValidLowLevel()) {
        UArmTimelineWidget *TimelineWidget =
            Arm->FindComponentByClass<UArmTimelineWidget>();
        if (TimelineWidget) {
          TimelineWidget->SetTimelineWidgetVisible(bVisible);
          TimelineWidget->SetTimelineWidgetClass(ArmTimelineWidgetClass);
        }
      }
    }
  }
}

void AGameManager::UpdateArmTimeline(ABasicArm *Arm, int32 CommandIndex,
                                     float Progress) {
  if (!Arm || !bShowArmTimelines) {
    return;
  }

  UArmTimelineWidget *TimelineWidget =
      Arm->FindComponentByClass<UArmTimelineWidget>();
  if (TimelineWidget) {
    TimelineWidget->SetCurrentCommand(CommandIndex, Progress);
  }
}

void AGameManager::UpdateAllArmTimelineWidgets() {

  // Update all existing arm timeline widgets with current settings
  if (UWorld *World = GetWorld()) {
    for (TActorIterator<ABasicArm> It(World); It; ++It) {
      ABasicArm *Arm = *It;
      if (Arm && Arm->IsValidLowLevel()) {
        UArmTimelineWidget *TimelineWidget =
            Arm->FindComponentByClass<UArmTimelineWidget>();
        if (TimelineWidget) {
          TimelineWidget->SetCommandTextures(CommandTextures);
          TimelineWidget->SetDefaultCommandTexture(DefaultCommandTexture);
          TimelineWidget->SetIconSize(TimelineIconSize);
          TimelineWidget->SetIconBackgroundColor(TimelineIconBackgroundColor);
          TimelineWidget->SetNormalCommandColor(TimelineNormalCommandColor);
          TimelineWidget->SetCurrentCommandColor(TimelineCurrentCommandColor);
          TimelineWidget->SetCompletedCommandColor(
              TimelineCompletedCommandColor);
          TimelineWidget->SetTimelineWidgetClass(ArmTimelineWidgetClass);
          TimelineWidget->SetTimelineWidgetVisible(bShowArmTimelines);
          TimelineWidget->SetMaxCommandIcons(MaxTimelineIcons);
          TimelineWidget->LerpSpeed = TimelineLerpSpeed;
          // Push separation distance to individual widgets so they can use it
          // if needed locally
          TimelineWidget->MinSeparationDistance = TimelineSeparation;
        }
      }
    }
  }
}

void AGameManager::SetActiveTimeline(UArmTimelineWidget *Timeline) {
  ActiveTimelineWidget = Timeline;
}

UArmTimelineWidget *AGameManager::GetActiveTimeline() const {
  return ActiveTimelineWidget;
}

FVector
AGameManager::AdjustTimelinePositionForOverlap(UArmTimelineWidget *Timeline,
                                               const FVector &DesiredPosition,
                                               const FVector &ArmLocation) {
  if (!Timeline || !GetWorld()) {
    return DesiredPosition;
  }

  // Start with the desired 2D position but enforce global height
  FVector CurrentPos = DesiredPosition;
  CurrentPos.Z = ArmLocation.Z + TimelineGlobalHeight;

  // Collect other timelines once
  TArray<FVector> Others;
  for (TActorIterator<ABasicArm> It(GetWorld()); It; ++It) {
    ABasicArm *Arm = *It;
    if (Arm && Arm->TimelineWidget && Arm->TimelineWidget != Timeline &&
        Arm->TimelineWidget->bTimelineVisible) {
      // Use target location to avoid jittering based on current interpolated
      // position
      Others.Add(Arm->TimelineWidget->GetTargetWorldLocation());
    }
  }

  // Iterative relaxation (Boids-like separation)
  const int32 Iterations = 15;
  const float MinSepSq = FMath::Square(TimelineSeparation);

  for (int32 i = 0; i < Iterations; ++i) {
    FVector SeparationForce = FVector::ZeroVector;
    int32 NeighborCount = 0;

    for (const FVector &OtherPos : Others) {
      // Calculate 2D distance (ignore Z as we enforce plane)
      float DistSq = FVector::DistSquaredXY(CurrentPos, OtherPos);
      if (DistSq < MinSepSq && DistSq > KINDA_SMALL_NUMBER) {
        float Dist = FMath::Sqrt(DistSq);
        // Normalized vector pointing away from neighbor
        FVector PushDir = (CurrentPos - OtherPos).GetSafeNormal2D();

        // Force increases as distance decreases
        float PushFactor = (TimelineSeparation - Dist) / TimelineSeparation;
        SeparationForce +=
            PushDir * PushFactor * TimelineRepulsionStrength * 10.0f;
        NeighborCount++;
      }
    }

    // Apply separation
    if (NeighborCount > 0) {
      CurrentPos += SeparationForce; // Move away from neighbors
    }

    // Apply attraction to Arm (tether) to prevent drifting too far
    // Calculate desired "ideal" slot relative to arm (e.g. where it started)
    // For now, just attract back to ArmLocation (projected to Z plane) but
    // respect min distance
    FVector ArmPosPlane = ArmLocation;
    ArmPosPlane.Z = CurrentPos.Z;

    FVector ToArm = ArmPosPlane - CurrentPos;
    float DistToArm = ToArm.Size2D();

    // Soft tether: if too far, pull back. If too close, push out.
    // We want to be roughly 'TimelineSeparation' away from the arm itself too,
    // to avoid covering it? Actually, CalculateOptimalTimelinePosition likely
    // gave us a good starting spot (offset). Let's attract back to the original
    // DesiredPosition (projected)
    FVector OriginalTarget = DesiredPosition;
    OriginalTarget.Z = CurrentPos.Z;

    FVector ToTarget = OriginalTarget - CurrentPos;
    // Weak spring to original target
    CurrentPos += ToTarget * 0.1f;
  }

  // Final Z enforcement (just in case)
  CurrentPos.Z = ArmLocation.Z + TimelineGlobalHeight;

  return CurrentPos;
}

ABasicArm *AGameManager::GetActiveArm() const {
  if (!ActiveTimelineWidget) {
    return nullptr;
  }

  // Find the arm that owns this timeline widget
  if (UWorld *World = GetWorld()) {
    for (TActorIterator<ABasicArm> It(World); It; ++It) {
      ABasicArm *Arm = *It;
      if (Arm && Arm->IsValidLowLevel()) {
        UArmTimelineWidget *TimelineWidget =
            Arm->FindComponentByClass<UArmTimelineWidget>();
        if (TimelineWidget == ActiveTimelineWidget) {
          return Arm;
        }
      }
    }
  }

  return nullptr;
}

int32 AGameManager::GenerateRandomCluster(USimWorldHex *OverrideSim) {
  // Get the simulation
  USimWorldHex *Sim = OverrideSim;
  if (!Sim) {
    if (!OrbSimDriver) {
      UE_LOG(LogTemp, Warning,
             TEXT("GenerateRandomCluster: No OrbSimDriver found"));
      return -1;
    }
    Sim = OrbSimDriver->GetSim();
  }
  if (!Sim) {
    UE_LOG(LogTemp, Warning, TEXT("GenerateRandomCluster: No Sim found"));
    return -1;
  }

  // Get available orb types based on difficulty checkpoints
  TArray<uint8> UnlockedTypes = GetUnlockedOrbTypes(DifficultyValue);

  // Target difficulty score
  int32 TargetScore = DifficultyValue;

  // Difficulty scoring helper:
  // - Orb count: +1 per orb
  // - Different-type adjacency: +2 per different-type bond
  // This makes type diversity the primary difficulty driver, not structure
  auto CalcScore = [](const TArray<FOrbData> &Orbs) -> int32 {
    int32 Score = Orbs.Num(); // Each orb = +1

    // Count different-type adjacencies
    for (int32 i = 0; i < Orbs.Num(); i++) {
      for (int32 j = i + 1; j < Orbs.Num(); j++) {
        int32 dQ = Orbs[i].dQ - Orbs[j].dQ;
        int32 dR = Orbs[i].dR - Orbs[j].dR;
        int32 Dist =
            (FMath::Abs(dQ) + FMath::Abs(dQ + dR) + FMath::Abs(dR)) / 2;
        if (Dist == 1) {
          // Different type adjacent = +2
          if (Orbs[i].Type != Orbs[j].Type) {
            Score += 2;
          }
        }
      }
    }
    return Score;
  };

  // Build cluster by growing until we reach target score
  TArray<FOrbData> Orbs;
  TSet<FHex> Used;
  TArray<FHex> Neighbors = {FHex(1, 0),  FHex(1, -1), FHex(0, -1),
                            FHex(-1, 0), FHex(-1, 1), FHex(0, 1)};

  // Start with one orb at origin
  FOrbData First;
  First.dQ = 0;
  First.dR = 0;
  First.Type = UnlockedTypes[0];
  if (First.Type == 0)
    First.Type = (uint8)EOrbType::BaseA;
  Orbs.Add(First);
  Used.Add(FHex(0, 0));

  int32 MaxIterations = 100;
  while (CalcScore(Orbs) < TargetScore && MaxIterations-- > 0) {
    // Find all candidate positions adjacent to existing orbs
    TArray<FHex> Candidates;
    for (const FOrbData &O : Orbs) {
      FHex H(O.dQ, O.dR);
      for (const FHex &N : Neighbors) {
        FHex C(H.Q + N.Q, H.R + N.R);
        if (!Used.Contains(C)) {
          Candidates.AddUnique(C);
        }
      }
    }

    if (Candidates.Num() == 0)
      break;

    // Pick a random candidate and type
    FHex Pick = Candidates[FMath::RandRange(0, Candidates.Num() - 1)];
    FOrbData NewOrb;
    NewOrb.dQ = Pick.Q;
    NewOrb.dR = Pick.R;
    uint8 NewType = UnlockedTypes[FMath::RandRange(0, UnlockedTypes.Num() - 1)];
    if (NewType == 0)
      NewType = (uint8)EOrbType::BaseA;
    NewOrb.Type = NewType;
    Orbs.Add(NewOrb);
    Used.Add(Pick);
  }

  // Optionally adjust types to hit target score more precisely
  int32 CurrentScore = CalcScore(Orbs);
  int32 TypeAdjustIter = 50;
  while (CurrentScore != TargetScore && TypeAdjustIter-- > 0 &&
         Orbs.Num() > 1) {
    int32 Idx = FMath::RandRange(0, Orbs.Num() - 1);
    uint8 OldType = Orbs[Idx].Type;
    uint8 NewType = UnlockedTypes[FMath::RandRange(0, UnlockedTypes.Num() - 1)];
    if (NewType == 0)
      NewType = (uint8)EOrbType::BaseA;
    Orbs[Idx].Type = NewType;
    int32 NewScore = CalcScore(Orbs);
    if (FMath::Abs(NewScore - TargetScore) >
        FMath::Abs(CurrentScore - TargetScore)) {
      Orbs[Idx].Type = OldType; // Revert
    } else {
      CurrentScore = NewScore;
    }
  }

  // Build bonds
  TArray<TPair<int32, int32>> Bonds;
  for (int32 i = 0; i < Orbs.Num(); i++) {
    for (int32 j = i + 1; j < Orbs.Num(); j++) {
      int32 dQ = Orbs[i].dQ - Orbs[j].dQ;
      int32 dR = Orbs[i].dR - Orbs[j].dR;
      int32 Dist = (FMath::Abs(dQ) + FMath::Abs(dQ + dR) + FMath::Abs(dR)) / 2;
      if (Dist == 1) {
        Bonds.Add({i, j});
      }
    }
  }

  // Register
  uint8 Id = Sim->GenerateNewTemplateId();
  FString Name = FString::Printf(TEXT("Random_D%d_S%d"), Id, CalcScore(Orbs));
  Sim->RegisterClusterTemplate(Id, Orbs, Bonds, Name);

  UE_LOG(LogTemp, Log,
         TEXT("GenerateRandomCluster: Created template %d with %d orbs, score "
              "%d (target %d)"),
         Id, Orbs.Num(), CalcScore(Orbs), TargetScore);

  return (int32)Id;
}

int32 AGameManager::GenerateAncestralCluster(USimWorldHex *OverrideSim) {
  USimWorldHex *Sim = OverrideSim;
  if (!Sim) {
    if (!OrbSimDriver)
      return -1;
    Sim = OrbSimDriver->GetSim();
  }
  if (!Sim)
    return -1;

  // Get available orb types based on difficulty checkpoints
  TArray<uint8> UnlockedTypes = GetUnlockedOrbTypes(DifficultyValue);
  int32 TargetScore = DifficultyValue;

  // Difficulty scoring helper (same as GenerateRandomCluster)
  // - Orb count: +1 per orb
  // - Different-type adjacency: +2 per different-type bond
  // Difficulty scoring helper (uses member function)
  auto CalcScore = [this](const TArray<FOrbData> &Orbs) -> int32 {
    return CalculateClusterScore(Orbs);
  };

  // Connectivity check helper
  auto IsConnected = [](const TArray<FOrbData> &Orbs) -> bool {
    if (Orbs.Num() <= 1)
      return true;
    TSet<int32> Visited;
    TArray<int32> Stack;
    Stack.Push(0);
    Visited.Add(0);
    while (Stack.Num() > 0) {
      int32 Curr = Stack.Pop();
      for (int32 i = 0; i < Orbs.Num(); i++) {
        if (Visited.Contains(i))
          continue;
        int32 dQ = Orbs[Curr].dQ - Orbs[i].dQ;
        int32 dR = Orbs[Curr].dR - Orbs[i].dR;
        int32 Dist =
            (FMath::Abs(dQ) + FMath::Abs(dQ + dR) + FMath::Abs(dR)) / 2;
        if (Dist == 1) {
          Visited.Add(i);
          Stack.Push(i);
        }
      }
    }
    return Visited.Num() == Orbs.Num();
  };

  // If no seed, generate a random cluster first
  if (LastAncestralTemplateId == -1) {
    int32 InitialId = GenerateRandomCluster(Sim);
    LastAncestralTemplateId = InitialId;
    return InitialId;
  }

  // Get seed template
  const USimWorldHex::FClusterTemplate *Seed =
      Sim->GetClusterTemplates().Find((uint8)LastAncestralTemplateId);
  if (!Seed) {
    LastAncestralTemplateId = -1;
    return GenerateAncestralCluster(Sim);
  }

  // Copy seed orbs and work on them
  TArray<FOrbData> WorkingOrbs = Seed->Orbs;

  // First, replace any orb types not in UnlockedTypes
  for (FOrbData &O : WorkingOrbs) {
    if (!UnlockedTypes.Contains(O.Type)) {
      O.Type = UnlockedTypes[FMath::RandRange(0, UnlockedTypes.Num() - 1)];
    }
  }

  TArray<FHex> Neighbors = {FHex(1, 0),  FHex(1, -1), FHex(0, -1),
                            FHex(-1, 0), FHex(-1, 1), FHex(0, 1)};

  // Evolution loop: mutate until we reach target score
  int32 MaxMutations = 50;
  for (int32 Iter = 0; Iter < MaxMutations; Iter++) {
    int32 CurrentScore = CalcScore(WorkingOrbs);
    if (CurrentScore == TargetScore)
      break;

    // Favor type changes (80%) over structural changes (20%) to keep clusters
    // related
    int32 MutationType =
        FMath::RandRange(0, 9); // 0-1 = structural, 2-9 = type change

    if (CurrentScore < TargetScore) {
      // Need to increase score: add orb or change type to create diff-bonds
      if (MutationType <= 1) {
        // Add an orb adjacent to existing cluster
        TSet<FHex> Used;
        for (const FOrbData &O : WorkingOrbs)
          Used.Add(FHex(O.dQ, O.dR));

        TArray<FHex> Candidates;
        for (const FOrbData &O : WorkingOrbs) {
          FHex H(O.dQ, O.dR);
          for (const FHex &N : Neighbors) {
            FHex C(H.Q + N.Q, H.R + N.R);
            if (!Used.Contains(C))
              Candidates.AddUnique(C);
          }
        }

        if (Candidates.Num() > 0) {
          FHex Pick = Candidates[FMath::RandRange(0, Candidates.Num() - 1)];
          FOrbData NewOrb;
          NewOrb.dQ = Pick.Q;
          NewOrb.dR = Pick.R;
          // Pick type that creates most diff-bonds with neighbors
          uint8 BestType = UnlockedTypes[0];
          if (UnlockedTypes.Num() > 1) {
            BestType =
                UnlockedTypes[FMath::RandRange(0, UnlockedTypes.Num() - 1)];
          }
          // Ensure we never use type 0 (None)
          if (BestType == 0 && UnlockedTypes.Num() > 0) {
            BestType = (uint8)EOrbType::BaseA;
          }
          NewOrb.Type = BestType;
          WorkingOrbs.Add(NewOrb);
        }
      } else {
        // Change an orb's type to create more diff-bonds
        if (WorkingOrbs.Num() > 0 && UnlockedTypes.Num() > 1) {
          int32 Idx = FMath::RandRange(0, WorkingOrbs.Num() - 1);
          uint8 NewType =
              UnlockedTypes[FMath::RandRange(0, UnlockedTypes.Num() - 1)];
          // Ensure we never use type 0 (None)
          if (NewType == 0)
            NewType = (uint8)EOrbType::BaseA;
          WorkingOrbs[Idx].Type = NewType;
        }
      }
    } else {
      // Need to decrease score: remove orb or unify types
      // Favor unifying types (80%) over removing orbs (20%)
      if (MutationType <= 1 && WorkingOrbs.Num() > 1) {
        // Try removing an orb (if cluster stays connected)
        int32 Idx = FMath::RandRange(0, WorkingOrbs.Num() - 1);
        TArray<FOrbData> Test = WorkingOrbs;
        Test.RemoveAt(Idx);
        if (IsConnected(Test)) {
          WorkingOrbs = Test;
        }
      } else {
        // Change type to reduce diff-bonds
        if (WorkingOrbs.Num() > 0) {
          int32 Idx = FMath::RandRange(0, WorkingOrbs.Num() - 1);
          // Set to same type as a neighbor to reduce diff-bonds (but never type
          // 0)
          uint8 UnifyType = UnlockedTypes[0];
          if (UnifyType == 0)
            UnifyType = (uint8)EOrbType::BaseA;
          WorkingOrbs[Idx].Type = UnifyType;
        }
      }
    }
  }

  // Build bonds
  TArray<TPair<int32, int32>> Bonds;
  for (int32 i = 0; i < WorkingOrbs.Num(); i++) {
    for (int32 j = i + 1; j < WorkingOrbs.Num(); j++) {
      int32 dQ = WorkingOrbs[i].dQ - WorkingOrbs[j].dQ;
      int32 dR = WorkingOrbs[i].dR - WorkingOrbs[j].dR;
      int32 Dist = (FMath::Abs(dQ) + FMath::Abs(dQ + dR) + FMath::Abs(dR)) / 2;
      if (Dist == 1) {
        Bonds.Add({i, j});
      }
    }
  }

  // Register new template
  uint8 NewId = Sim->GenerateNewTemplateId();
  FString Name =
      FString::Printf(TEXT("Ancestral_D%d_S%d"), NewId, CalcScore(WorkingOrbs));
  Sim->RegisterClusterTemplate(NewId, WorkingOrbs, Bonds, Name);
  LastAncestralTemplateId = (int32)NewId;

  UE_LOG(LogTemp, Log,
         TEXT("GenerateAncestralCluster: Created template %d with %d orbs, "
              "score %d (target %d)"),
         NewId, WorkingOrbs.Num(), CalcScore(WorkingOrbs), TargetScore);

  return (int32)NewId;
}

int32 AGameManager::GenerateRelatedCluster(uint8 TemplateId,
                                           USimWorldHex *OverrideSim) {
  USimWorldHex *Sim = OverrideSim;
  if (!Sim) {
    if (!OrbSimDriver)
      return -1;
    Sim = OrbSimDriver->GetSim();
  }
  if (!Sim)
    return -1;

  const USimWorldHex::FClusterTemplate *Base =
      Sim->GetClusterTemplates().Find(TemplateId);
  if (!Base)
    return GenerateRandomCluster(Sim);

  // Get available types and target score
  TArray<uint8> UnlockedTypes = GetUnlockedOrbTypes(DifficultyValue);
  int32 TargetScore = DifficultyValue;

  // Difficulty scoring helper (same as other generation methods)
  // - Orb count: +1 per orb
  // - Different-type adjacency: +2 per different-type bond
  auto CalcScore = [](const TArray<FOrbData> &Orbs) -> int32 {
    int32 Score = Orbs.Num(); // Each orb = +1

    for (int32 i = 0; i < Orbs.Num(); i++) {
      for (int32 j = i + 1; j < Orbs.Num(); j++) {
        int32 dQ = Orbs[i].dQ - Orbs[j].dQ;
        int32 dR = Orbs[i].dR - Orbs[j].dR;
        int32 Dist =
            (FMath::Abs(dQ) + FMath::Abs(dQ + dR) + FMath::Abs(dR)) / 2;
        if (Dist == 1) {
          if (Orbs[i].Type != Orbs[j].Type) {
            Score += 2; // Different type adjacent = +2
          }
        }
      }
    }
    return Score;
  };

  // Connectivity check
  auto IsConnected = [](const TArray<FOrbData> &Orbs) -> bool {
    if (Orbs.Num() <= 1)
      return true;
    TSet<int32> Visited;
    TArray<int32> Stack;
    Stack.Push(0);
    Visited.Add(0);
    while (Stack.Num() > 0) {
      int32 Curr = Stack.Pop();
      for (int32 i = 0; i < Orbs.Num(); i++) {
        if (Visited.Contains(i))
          continue;
        int32 dQ = Orbs[Curr].dQ - Orbs[i].dQ;
        int32 dR = Orbs[Curr].dR - Orbs[i].dR;
        int32 Dist =
            (FMath::Abs(dQ) + FMath::Abs(dQ + dR) + FMath::Abs(dR)) / 2;
        if (Dist == 1) {
          Visited.Add(i);
          Stack.Push(i);
        }
      }
    }
    return Visited.Num() == Orbs.Num();
  };

  // Start with base orbs - this is the "related" part
  TArray<FOrbData> WorkingOrbs = Base->Orbs;

  // Replace any types not in UnlockedTypes
  for (FOrbData &O : WorkingOrbs) {
    if (!UnlockedTypes.Contains(O.Type) || O.Type == 0) {
      uint8 NewType =
          UnlockedTypes[FMath::RandRange(0, UnlockedTypes.Num() - 1)];
      if (NewType == 0)
        NewType = (uint8)EOrbType::BaseA;
      O.Type = NewType;
    }
  }

  TArray<FHex> Neighbors = {FHex(1, 0),  FHex(1, -1), FHex(0, -1),
                            FHex(-1, 0), FHex(-1, 1), FHex(0, 1)};

  // Mutate to reach target score (limited mutations to stay "related")
  int32 MaxMutations = 20; // Fewer mutations to preserve similarity
  for (int32 Iter = 0; Iter < MaxMutations; Iter++) {
    int32 CurrentScore = CalcScore(WorkingOrbs);
    if (CurrentScore == TargetScore)
      break;

    if (CurrentScore < TargetScore) {
      // Need more score - heavily prefer changing types over adding orbs to
      // stay related Only 10% chance of adding orbs, 90% chance of type changes
      if (FMath::RandRange(0, 9) == 0) {
        // Add an orb
        TSet<FHex> Used;
        for (const FOrbData &O : WorkingOrbs)
          Used.Add(FHex(O.dQ, O.dR));
        TArray<FHex> Candidates;
        for (const FOrbData &O : WorkingOrbs) {
          FHex H(O.dQ, O.dR);
          for (const FHex &N : Neighbors) {
            FHex C(H.Q + N.Q, H.R + N.R);
            if (!Used.Contains(C))
              Candidates.AddUnique(C);
          }
        }
        if (Candidates.Num() > 0) {
          FHex Pick = Candidates[FMath::RandRange(0, Candidates.Num() - 1)];
          FOrbData NewOrb;
          NewOrb.dQ = Pick.Q;
          NewOrb.dR = Pick.R;
          uint8 NewType =
              UnlockedTypes[FMath::RandRange(0, UnlockedTypes.Num() - 1)];
          if (NewType == 0)
            NewType = (uint8)EOrbType::BaseA;
          NewOrb.Type = NewType;
          WorkingOrbs.Add(NewOrb);
        }
      } else if (UnlockedTypes.Num() > 1 && WorkingOrbs.Num() > 0) {
        // Change type to create diff-bonds
        int32 Idx = FMath::RandRange(0, WorkingOrbs.Num() - 1);
        uint8 NewType =
            UnlockedTypes[FMath::RandRange(0, UnlockedTypes.Num() - 1)];
        if (NewType == 0)
          NewType = (uint8)EOrbType::BaseA;
        WorkingOrbs[Idx].Type = NewType;
      }
    } else {
      // Need less score - heavily prefer unifying types over removing orbs
      // Only 10% chance of removing orbs, 90% chance of type changes
      if (FMath::RandRange(0, 9) == 0 && WorkingOrbs.Num() > 1) {
        // Try removing an orb
        int32 Idx = FMath::RandRange(0, WorkingOrbs.Num() - 1);
        TArray<FOrbData> Test = WorkingOrbs;
        Test.RemoveAt(Idx);
        if (IsConnected(Test)) {
          WorkingOrbs = Test;
        }
      } else if (WorkingOrbs.Num() > 0) {
        // Unify type to reduce diff-bonds
        int32 Idx = FMath::RandRange(0, WorkingOrbs.Num() - 1);
        uint8 UnifyType = UnlockedTypes[0];
        if (UnifyType == 0)
          UnifyType = (uint8)EOrbType::BaseA;
        WorkingOrbs[Idx].Type = UnifyType;
      }
    }
  }

  // Build bonds
  TArray<TPair<int32, int32>> Bonds;
  for (int32 i = 0; i < WorkingOrbs.Num(); i++) {
    for (int32 j = i + 1; j < WorkingOrbs.Num(); j++) {
      int32 dQ = WorkingOrbs[i].dQ - WorkingOrbs[j].dQ;
      int32 dR = WorkingOrbs[i].dR - WorkingOrbs[j].dR;
      int32 Dist = (FMath::Abs(dQ) + FMath::Abs(dQ + dR) + FMath::Abs(dR)) / 2;
      if (Dist == 1) {
        Bonds.Add({i, j});
      }
    }
  }

  uint8 NewId = Sim->GenerateNewTemplateId();
  FString Name =
      FString::Printf(TEXT("Related_D%d_S%d"), NewId, CalcScore(WorkingOrbs));
  Sim->RegisterClusterTemplate(NewId, WorkingOrbs, Bonds, Name);

  // Update LastAncestralTemplateId so the chain continues
  LastAncestralTemplateId = (int32)NewId;

  UE_LOG(LogTemp, Log,
         TEXT("GenerateRelatedCluster: Created template %d from base %d, score "
              "%d (target %d)"),
         NewId, TemplateId, CalcScore(WorkingOrbs), TargetScore);

  return (int32)NewId;
}

TArray<uint8> AGameManager::GetUnlockedOrbTypes(int32 Difficulty) const {
  TArray<uint8> Unlocked;
  for (int32 i = 0; i < AncestralDifficultyCheckpoints.Num(); i++) {
    if (Difficulty >= AncestralDifficultyCheckpoints[i]) {
      // Add i+1 as orb type (1 = BaseA, 2 = BaseB, etc.)
      // Ensure we never add type 0 (None)
      uint8 OrbType = (uint8)(i + 1);
      if (OrbType > 0) {
        Unlocked.AddUnique(OrbType);
      }
    }
  }
  // Ensure we always have at least BaseA, never return empty or with type 0
  Unlocked.Remove(0); // Remove None if somehow present
  if (Unlocked.Num() == 0)
    Unlocked.Add((uint8)EOrbType::BaseA);
  return Unlocked;
}

void AGameManager::CycleAncestralDifficultyNext() {
  if (AncestralDifficultyCheckpoints.Num() == 0)
    return;
  AncestralDifficultyIndex =
      (AncestralDifficultyIndex + 1) % AncestralDifficultyCheckpoints.Num();
  DifficultyValue = AncestralDifficultyCheckpoints[AncestralDifficultyIndex];
}

void AGameManager::CycleAncestralDifficultyPrevious() {
  if (AncestralDifficultyCheckpoints.Num() == 0)
    return;
  AncestralDifficultyIndex =
      (AncestralDifficultyIndex - 1 + AncestralDifficultyCheckpoints.Num()) %
      AncestralDifficultyCheckpoints.Num();
  DifficultyValue = AncestralDifficultyCheckpoints[AncestralDifficultyIndex];
}

// ------------ Cluster Storage Implementation ------------

// Comparison operator for FClusterCanonicalKey
bool FClusterCanonicalKey::operator==(const FClusterCanonicalKey &Other) const {
  if (CanonicalOrbs.Num() != Other.CanonicalOrbs.Num())
    return false;
  if (CanonicalBonds.Num() != Other.CanonicalBonds.Num())
    return false;

  // Compare orbs
  for (int32 i = 0; i < CanonicalOrbs.Num(); i++) {
    if (CanonicalOrbs[i].dQ != Other.CanonicalOrbs[i].dQ)
      return false;
    if (CanonicalOrbs[i].dR != Other.CanonicalOrbs[i].dR)
      return false;
    if (CanonicalOrbs[i].Type != Other.CanonicalOrbs[i].Type)
      return false;
  }

  // Compare bonds
  for (int32 i = 0; i < CanonicalBonds.Num(); i++) {
    if (CanonicalBonds[i].Key != Other.CanonicalBonds[i].Key)
      return false;
    if (CanonicalBonds[i].Value != Other.CanonicalBonds[i].Value)
      return false;
  }

  return true;
}

// Hash function for FClusterCanonicalKey
uint32 GetTypeHash(const FClusterCanonicalKey &Key) {
  uint32 Hash = 0;

  // Hash all orb data
  for (const FOrbData &Orb : Key.CanonicalOrbs) {
    Hash = HashCombine(Hash, ::GetTypeHash(Orb.dQ));
    Hash = HashCombine(Hash, ::GetTypeHash(Orb.dR));
    Hash = HashCombine(Hash, ::GetTypeHash(Orb.Type));
  }

  // Hash all bonds
  for (const TPair<int32, int32> &Bond : Key.CanonicalBonds) {
    Hash = HashCombine(Hash, ::GetTypeHash(Bond.Key));
    Hash = HashCombine(Hash, ::GetTypeHash(Bond.Value));
  }

  return Hash;
}

FClusterCanonicalKey AGameManager::GenerateCanonicalKey(
    const USimWorldHex::FClusterTemplate &ClusterTemplate) const {
  auto RotateHex = [](int32 Q, int32 R) -> TPair<int32, int32> {
    return TPair<int32, int32>(-R, Q + R);
  };

  auto CanonicalizeConfig =
      [](const TArray<FOrbData> &Orbs,
         const TArray<TPair<int32, int32>> &Bonds) -> FClusterCanonicalKey {
    FClusterCanonicalKey Key;

    if (Orbs.Num() == 0) {
      return Key;
    }

    // Find the minimum dQ and dR values
    int32 MinQ = Orbs[0].dQ;
    int32 MinR = Orbs[0].dR;
    for (const FOrbData &Orb : Orbs) {
      MinQ = FMath::Min(MinQ, Orb.dQ);
      MinR = FMath::Min(MinR, Orb.dR);
    }

    // Normalize all orbs by subtracting the minimum
    for (const FOrbData &Orb : Orbs) {
      FOrbData NormalizedOrb = Orb;
      NormalizedOrb.dQ -= MinQ;
      NormalizedOrb.dR -= MinR;
      NormalizedOrb.ItemId = 0; // Exclude ItemId from canonical representation
      Key.CanonicalOrbs.Add(NormalizedOrb);
    }

    // Sort orbs by dQ, then dR, then Type for consistent ordering
    Key.CanonicalOrbs.Sort([](const FOrbData &A, const FOrbData &B) {
      if (A.dQ != B.dQ)
        return A.dQ < B.dQ;
      if (A.dR != B.dR)
        return A.dR < B.dR;
      return A.Type < B.Type;
    });

    // Create mapping from old indices to new sorted indices
    TMap<int32, int32> OldToNewIndex;
    for (int32 OldIdx = 0; OldIdx < Orbs.Num(); OldIdx++) {
      const FOrbData &OriginalOrb = Orbs[OldIdx];
      for (int32 NewIdx = 0; NewIdx < Key.CanonicalOrbs.Num(); NewIdx++) {
        const FOrbData &CanonicalOrb = Key.CanonicalOrbs[NewIdx];
        // Compare normalized coordinates and type
        if (OriginalOrb.dQ - MinQ == CanonicalOrb.dQ &&
            OriginalOrb.dR - MinR == CanonicalOrb.dR &&
            OriginalOrb.Type == CanonicalOrb.Type) {
          OldToNewIndex.Add(OldIdx, NewIdx);
          break;
        }
      }
    }

    // Remap bonds to use new sorted indices
    for (const TPair<int32, int32> &OriginalBond : Bonds) {
      if (int32 *NewKey = OldToNewIndex.Find(OriginalBond.Key)) {
        if (int32 *NewValue = OldToNewIndex.Find(OriginalBond.Value)) {
          int32 A = FMath::Min(*NewKey, *NewValue);
          int32 B = FMath::Max(*NewKey, *NewValue);
          Key.CanonicalBonds.Add(TPair<int32, int32>(A, B));
        }
      }
    }

    // Sort bonds for consistent ordering
    Key.CanonicalBonds.Sort(
        [](const TPair<int32, int32> &A, const TPair<int32, int32> &B) {
          if (A.Key != B.Key)
            return A.Key < B.Key;
          return A.Value < B.Value;
        });

    return Key;
  };

  // Generate all six rotations and pick the lexicographically smallest
  FClusterCanonicalKey BestKey;
  bool bFirst = true;
  int32 BestRotation = 0;

  for (int32 Rotation = 0; Rotation < 6; Rotation++) {
    // Create rotated version of the cluster
    TArray<FOrbData> RotatedOrbs = ClusterTemplate.Orbs;

    // Apply rotation to all orbs
    for (FOrbData &Orb : RotatedOrbs) {
      int32 CurrentQ = Orb.dQ;
      int32 CurrentR = Orb.dR;

      // Apply rotation N times
      for (int32 r = 0; r < Rotation; r++) {
        TPair<int32, int32> Rotated = RotateHex(CurrentQ, CurrentR);
        CurrentQ = Rotated.Key;
        CurrentR = Rotated.Value;
      }

      Orb.dQ = CurrentQ;
      Orb.dR = CurrentR;
    }

    // Bonds don't need rotation - they reference orb indices which don't change
    FClusterCanonicalKey CandidateKey =
        CanonicalizeConfig(RotatedOrbs, ClusterTemplate.Bonds);

    // Compare keys lexicographically
    if (bFirst) {
      BestKey = CandidateKey;
      BestRotation = Rotation;
      bFirst = false;
    } else {
      // Compare orb arrays lexicographically
      bool bCandidateIsLess = false;

      if (CandidateKey.CanonicalOrbs.Num() != BestKey.CanonicalOrbs.Num()) {
        bCandidateIsLess =
            CandidateKey.CanonicalOrbs.Num() < BestKey.CanonicalOrbs.Num();
      } else {
        // Compare orb by orb
        for (int32 i = 0; i < CandidateKey.CanonicalOrbs.Num(); i++) {
          const FOrbData &CandOrb = CandidateKey.CanonicalOrbs[i];
          const FOrbData &BestOrb = BestKey.CanonicalOrbs[i];

          if (CandOrb.dQ != BestOrb.dQ) {
            bCandidateIsLess = CandOrb.dQ < BestOrb.dQ;
            break;
          }
          if (CandOrb.dR != BestOrb.dR) {
            bCandidateIsLess = CandOrb.dR < BestOrb.dR;
            break;
          }
          if (CandOrb.Type != BestOrb.Type) {
            bCandidateIsLess = CandOrb.Type < BestOrb.Type;
            break;
          }
        }
      }

      // If orbs are identical, compare bonds
      if (!bCandidateIsLess &&
          CandidateKey.CanonicalOrbs.Num() == BestKey.CanonicalOrbs.Num()) {
        // First check if all orbs are identical
        bool bOrbsIdentical = true;
        for (int32 i = 0; i < CandidateKey.CanonicalOrbs.Num(); i++) {
          const FOrbData &CandOrb = CandidateKey.CanonicalOrbs[i];
          const FOrbData &BestOrb = BestKey.CanonicalOrbs[i];
          if (CandOrb.dQ != BestOrb.dQ || CandOrb.dR != BestOrb.dR ||
              CandOrb.Type != BestOrb.Type) {
            bOrbsIdentical = false;
            break;
          }
        }

        if (bOrbsIdentical) {
          // Compare bonds
          for (int32 i = 0; i < FMath::Min(CandidateKey.CanonicalBonds.Num(),
                                           BestKey.CanonicalBonds.Num());
               i++) {
            if (CandidateKey.CanonicalBonds[i].Key !=
                BestKey.CanonicalBonds[i].Key) {
              bCandidateIsLess = CandidateKey.CanonicalBonds[i].Key <
                                 BestKey.CanonicalBonds[i].Key;
              break;
            }
            if (CandidateKey.CanonicalBonds[i].Value !=
                BestKey.CanonicalBonds[i].Value) {
              bCandidateIsLess = CandidateKey.CanonicalBonds[i].Value <
                                 BestKey.CanonicalBonds[i].Value;
              break;
            }
          }

          if (!bCandidateIsLess && CandidateKey.CanonicalBonds.Num() <
                                       BestKey.CanonicalBonds.Num()) {
            bCandidateIsLess = true;
          }
        }
      }

      if (bCandidateIsLess) {
        BestKey = CandidateKey;
        BestRotation = Rotation;
      }
    }
  }

  UE_LOG(LogTemp, Verbose,
         TEXT("GenerateCanonicalKey: Selected rotation %d as canonical (%d "
              "orbs, %d bonds)"),
         BestRotation, BestKey.CanonicalOrbs.Num(),
         BestKey.CanonicalBonds.Num());

  return BestKey;
}

void AGameManager::AddClusterToStorage(
    const USimWorldHex::FClusterTemplate &ClusterTemplate) {
  FClusterCanonicalKey Key = GenerateCanonicalKey(ClusterTemplate);

  // Log first orb of canonical key for debugging
  if (Key.CanonicalOrbs.Num() > 0) {
    const FOrbData &FirstOrb = Key.CanonicalOrbs[0];
  }

  if (int32 *ExistingCount = ClusterCounts.Find(Key)) {
    // Increment existing count
    (*ExistingCount)++;

  } else {
    // Add new cluster
    ClusterCounts.Add(Key, 1);
    StoredClusterTemplates.Add(Key, ClusterTemplate);
  }
}

int32 AGameManager::GetClusterCount(
    const USimWorldHex::FClusterTemplate &ClusterTemplate) const {
  FClusterCanonicalKey Key = GenerateCanonicalKey(ClusterTemplate);
  if (const int32 *Count = ClusterCounts.Find(Key)) {
    return *Count;
  }
  return 0;
}

int32 AGameManager::GetTotalClusterCount() const {
  int32 Total = 0;
  for (const auto &KV : ClusterCounts) {
    Total += KV.Value;
  }
  return Total;
}

bool AGameManager::HasStorageRoomForCluster(
    const USimWorldHex::FClusterTemplate &ClusterTemplate) const {
  if (!UseStorageLimit) {
    return true;
  }
  return GetClusterCount(ClusterTemplate) < StorageLimit;
}

int32 AGameManager::GetClusterCountByTemplateId(uint8 TemplateId) const {
  if (const FClusterCanonicalKey *CachedKey =
          TemplateIdToCanonicalKey.Find(TemplateId)) {
    if (const int32 *Count = ClusterCounts.Find(*CachedKey)) {
      return *Count;
    }
  }
  return 0;
}

bool AGameManager::DecrementClusterFromStorageByTemplateId(uint8 TemplateId) {
  if (const FClusterCanonicalKey *CachedKey =
          TemplateIdToCanonicalKey.Find(TemplateId)) {
    if (int32 *Count = ClusterCounts.Find(*CachedKey)) {
      if (*Count > 0) {
        (*Count)--;
        // If count reaches zero, we keep the entry (don't remove) so it can be
        // tracked
        return true;
      }
    }
  }
  return false;
}

bool AGameManager::DecrementClusterFromStorageByCanonicalKey(
    const FClusterCanonicalKey &Key) {
  if (int32 *Count = ClusterCounts.Find(Key)) {
    if (*Count > 0) {
      (*Count)--;
      // If count reaches zero, we keep the entry (don't remove) so it can be
      // tracked
      return true;
    }
  }
  return false;
}

const USimWorldHex::FClusterTemplate *
AGameManager::GetStoredClusterTemplate(const FClusterCanonicalKey &Key) const {
  return StoredClusterTemplates.Find(Key);
}

bool AGameManager::DoesClusterExistInStorage(
    const USimWorldHex::FClusterTemplate &ClusterTemplate) const {
  FClusterCanonicalKey Key = GenerateCanonicalKey(ClusterTemplate);
  return ClusterCounts.Contains(Key);
}

bool AGameManager::DoesClusterExistInStorageByTemplateId(
    uint8 TemplateId) const {
  if (const FClusterCanonicalKey *CachedKey =
          TemplateIdToCanonicalKey.Find(TemplateId)) {
    return ClusterCounts.Contains(*CachedKey);
  }
  return false;
}

void AGameManager::CacheCanonicalKeyForTemplate(
    uint8 TemplateId, const USimWorldHex::FClusterTemplate &ClusterTemplate) {
  // Only cache if not already cached (templates shouldn't change structure)
  if (!TemplateIdToCanonicalKey.Contains(TemplateId)) {
    FClusterCanonicalKey Key = GenerateCanonicalKey(ClusterTemplate);
    TemplateIdToCanonicalKey.Add(TemplateId, Key);
    // UE_LOG(LogTemp, Warning, TEXT("Cached canonical key for template %d (%d
    // orbs, %d bonds)"), 	TemplateId, Key.CanonicalOrbs.Num(),
    // Key.CanonicalBonds.Num());
  }
}

void AGameManager::GetStoredClusters(
    TArray<USimWorldHex::FClusterTemplate> &OutTemplates,
    TArray<int32> &OutCounts) const {
  OutTemplates.Empty();
  OutCounts.Empty();

  for (const auto &KV : ClusterCounts) {
    if (const USimWorldHex::FClusterTemplate *Template =
            StoredClusterTemplates.Find(KV.Key)) {
      OutTemplates.Add(*Template);
      OutCounts.Add(KV.Value);
    }
  }
}

bool AGameManager::FindStoredClusterByOrbsOnly(
    const USimWorldHex::FClusterTemplate &OrbsOnlyTemplate,
    const USimWorldHex::FClusterTemplate *&OutTemplate, int32 &OutCount,
    FClusterCanonicalKey &OutMatchingKey) const {
  // Generate canonical key for orbs-only template
  FClusterCanonicalKey OrbsOnlyKey = GenerateCanonicalKey(OrbsOnlyTemplate);

  // Search through stored templates by rebuilding their orbs-only canonical
  // keys on demand
  for (const auto &KV : ClusterCounts) {
    const FClusterCanonicalKey &StoredFullKey = KV.Key;

    if (const USimWorldHex::FClusterTemplate *FoundTemplate =
            StoredClusterTemplates.Find(StoredFullKey)) {
      USimWorldHex::FClusterTemplate StoredOrbsOnlyTemplate;
      StoredOrbsOnlyTemplate.Orbs =
          FoundTemplate->Orbs; // ignore bonds for orbs-only match

      FClusterCanonicalKey StoredOrbsOnlyKey =
          GenerateCanonicalKey(StoredOrbsOnlyTemplate);

      if (OrbsOnlyKey == StoredOrbsOnlyKey) {
        OutTemplate = FoundTemplate;
        OutCount = KV.Value;
        OutMatchingKey = StoredFullKey;
        return true;
      }
    }
  }

  return false;
}

uint8 AGameManager::FindOrRegisterTemplateId(
    const USimWorldHex::FClusterTemplate &ClusterTemplate) {
  if (!OrbSimDriver || !OrbSimDriver->GetSim()) {
    UE_LOG(LogTemp, Warning,
           TEXT("FindOrRegisterTemplateId: No sim available"));
    return 0;
  }

  USimWorldHex *Sim = OrbSimDriver->GetSim();

  // Generate canonical key for the template
  FClusterCanonicalKey Key = GenerateCanonicalKey(ClusterTemplate);

  // Check if we have a cached TemplateId for this canonical key
  for (const auto &KV : TemplateIdToCanonicalKey) {
    if (KV.Value == Key) {
      // Found existing TemplateId
      return KV.Key;
    }
  }

  // Check if sim already has a matching template
  const TMap<uint8, USimWorldHex::FClusterTemplate> &SimTemplates =
      Sim->GetClusterTemplates();
  for (const auto &KV : SimTemplates) {
    // Compare templates by checking orb count and structure
    if (KV.Value.Orbs.Num() == ClusterTemplate.Orbs.Num() &&
        KV.Value.Bonds.Num() == ClusterTemplate.Bonds.Num()) {
      // Detailed comparison could be added here, but for now use canonical key
      FClusterCanonicalKey SimKey = GenerateCanonicalKey(KV.Value);
      if (SimKey == Key) {
        // Cache this mapping
        TemplateIdToCanonicalKey.Add(KV.Key, Key);
        return KV.Key;
      }
    }
  }

  // No existing template found - register a new one
  uint8 NewTemplateId = Sim->GenerateNewTemplateId();

  // Convert bonds to TPair format
  TArray<TPair<int32, int32>> BondPairs;
  for (const TPair<int32, int32> &Bond : ClusterTemplate.Bonds) {
    BondPairs.Add(Bond);
  }

  // Register the template in the sim
  Sim->RegisterClusterTemplate(NewTemplateId, ClusterTemplate.Orbs, BondPairs,
                               ClusterTemplate.Name);

  // Cache the mapping
  TemplateIdToCanonicalKey.Add(NewTemplateId, Key);

  UE_LOG(LogTemp, Log,
         TEXT("FindOrRegisterTemplateId: Registered new template ID %d for "
              "cluster with %d orbs"),
         NewTemplateId, ClusterTemplate.Orbs.Num());

  return NewTemplateId;
}

void AGameManager::ClearClusterStorage() {
  ClusterCounts.Empty();
  StoredClusterTemplates.Empty();
  ClusterIcons.Empty();
  UE_LOG(LogTemp, Log, TEXT("Cleared all cluster storage"));
}

void AGameManager::ClearClusterIcons() {
  ClusterIcons.Empty();
  UE_LOG(LogTemp, Log, TEXT("Cleared all cluster icons"));
}

UTexture2D *AGameManager::GenerateClusterIcon(
    const USimWorldHex::FClusterTemplate &ClusterTemplate) {
  // Create a larger 256x256 texture for better detail
  const int32 IconSize = 256;
  TArray<FColor> Pixels;
  Pixels.SetNum(IconSize * IconSize);

  // Fill with transparent background
  for (FColor &Pixel : Pixels) {
    Pixel = FColor(0, 0, 0, 0);
  }

  // Get OrbMaterialManager for colors
  UOrbMaterialManager *MaterialManager = UOrbMaterialManager::FindInWorld(this);

  // Calculate bounds to fit all orbs
  int32 MinQ = INT_MAX, MaxQ = INT_MIN;
  int32 MinR = INT_MAX, MaxR = INT_MIN;
  for (const FOrbData &Orb : ClusterTemplate.Orbs) {
    MinQ = FMath::Min(MinQ, Orb.dQ);
    MaxQ = FMath::Max(MaxQ, Orb.dQ);
    MinR = FMath::Min(MinR, Orb.dR);
    MaxR = FMath::Max(MaxR, Orb.dR);
  }

  // Calculate the center of the cluster for proper centering
  float CenterQ = (MinQ + MaxQ) * 0.5f;
  float CenterR = (MinR + MaxR) * 0.5f;

  // Calculate scale to fit everything
  int32 HexWidth = MaxQ - MinQ + 1;
  int32 HexHeight = MaxR - MinR + 1;

  // Use ClusterIconScale to adjust overall icon size
  const float BaseOrbRadius = 18.0f;
  const float OrbRadius = BaseOrbRadius * ClusterIconScale;
  const float Padding =
      OrbRadius; // Minimal padding - just enough for orb radius

  // Calculate scale with proper padding to fit everything
  const float ScaleX =
      (IconSize - Padding * 2) / (HexWidth * 1.732f); // sqrt(3) for hex width
  const float ScaleY = (IconSize - Padding * 2) / (HexHeight * 1.5f);
  const float Scale = FMath::Min(ScaleX, ScaleY); // Removed extra 0.85f scaling
  for (const FOrbData &Orb : ClusterTemplate.Orbs) {
    // Convert hex coordinates to pixel coordinates (pointy-top hex), offset by
    // cluster center
    float OrbCenterX =
        ((Orb.dQ - CenterQ) * 1.732f + (Orb.dR - CenterR) * 0.866f) * Scale;
    float OrbCenterY = (Orb.dR - CenterR) * 1.5f * Scale;

    // Center in the icon
    OrbCenterX += IconSize * 0.5f;
    OrbCenterY += IconSize * 0.5f;

    // Get color for this orb type
    EOrbType OrbType = (EOrbType)Orb.Type;
    FLinearColor OrbColor = MaterialManager
                                ? MaterialManager->GetOrbTypeUIColor(OrbType)
                                : FLinearColor::White;
    FColor PixelColor = OrbColor.ToFColor(true);

    // Draw a filled circle
    for (int32 Y = 0; Y < IconSize; Y++) {
      for (int32 X = 0; X < IconSize; X++) {
        float Dx = X - OrbCenterX;
        float Dy = Y - OrbCenterY;
        float Distance = FMath::Sqrt(Dx * Dx + Dy * Dy);

        if (Distance <= OrbRadius) {
          int32 Index = Y * IconSize + X;
          if (Index >= 0 && Index < Pixels.Num()) {
            Pixels[Index] = PixelColor;
          }
        }
      }
    }
  }

  // Draw bonds as lines - scale thickness with icon scale
  const float BaseLineThickness = 3.0f;
  const float LineThickness = BaseLineThickness * ClusterIconScale;
  for (const TPair<int32, int32> &Bond : ClusterTemplate.Bonds) {
    if (Bond.Key >= 0 && Bond.Key < ClusterTemplate.Orbs.Num() &&
        Bond.Value >= 0 && Bond.Value < ClusterTemplate.Orbs.Num()) {
      const FOrbData &OrbA = ClusterTemplate.Orbs[Bond.Key];
      const FOrbData &OrbB = ClusterTemplate.Orbs[Bond.Value];

      // Convert hex coordinates to pixel coordinates, offset by cluster center
      float X1 = ((OrbA.dQ - CenterQ) * 1.732f + (OrbA.dR - CenterR) * 0.866f) *
                     Scale +
                 IconSize * 0.5f;
      float Y1 = (OrbA.dR - CenterR) * 1.5f * Scale + IconSize * 0.5f;
      float X2 = ((OrbB.dQ - CenterQ) * 1.732f + (OrbB.dR - CenterR) * 0.866f) *
                     Scale +
                 IconSize * 0.5f;
      float Y2 = (OrbB.dR - CenterR) * 1.5f * Scale + IconSize * 0.5f;

      // Draw a line using Bresenham's algorithm
      int32 X1i = FMath::RoundToInt(X1);
      int32 Y1i = FMath::RoundToInt(Y1);
      int32 X2i = FMath::RoundToInt(X2);
      int32 Y2i = FMath::RoundToInt(Y2);

      int32 Dx = FMath::Abs(X2i - X1i);
      int32 Dy = FMath::Abs(Y2i - Y1i);
      int32 Sx = (X1i < X2i) ? 1 : -1;
      int32 Sy = (Y1i < Y2i) ? 1 : -1;
      int32 Err = Dx - Dy;

      int32 X = X1i, Y = Y1i;
      int32 HalfThickness = FMath::Max(1, (int32)(LineThickness / 2.0f));
      while (true) {
        // Draw a thicker line by drawing multiple pixels around each point
        for (int32 OffsetY = -HalfThickness; OffsetY <= HalfThickness;
             OffsetY++) {
          for (int32 OffsetX = -HalfThickness; OffsetX <= HalfThickness;
               OffsetX++) {
            int32 DrawX = X + OffsetX;
            int32 DrawY = Y + OffsetY;
            if (DrawX >= 0 && DrawX < IconSize && DrawY >= 0 &&
                DrawY < IconSize) {
              int32 Index = DrawY * IconSize + DrawX;
              if (Index >= 0 && Index < Pixels.Num()) {
                Pixels[Index] = FColor::White;
              }
            }
          }
        }

        if (X == X2i && Y == Y2i)
          break;

        int32 Err2 = Err * 2;
        if (Err2 > -Dy) {
          Err -= Dy;
          X += Sx;
        }
        if (Err2 < Dx) {
          Err += Dx;
          Y += Sy;
        }
      }
    }
  }

  // Create texture from pixel data
  UTexture2D *Texture =
      UTexture2D::CreateTransient(IconSize, IconSize, PF_B8G8R8A8);
  if (Texture) {
    FTexture2DMipMap &Mip = Texture->GetPlatformData()->Mips[0];
    void *TextureData = Mip.BulkData.Lock(LOCK_READ_WRITE);
    FMemory::Memcpy(TextureData, Pixels.GetData(),
                    Pixels.Num() * sizeof(FColor));
    Mip.BulkData.Unlock();

    Texture->UpdateResource();
  }

  return Texture;
}

UTexture2D *AGameManager::GetClusterIcon(
    const USimWorldHex::FClusterTemplate &ClusterTemplate) {
  FClusterCanonicalKey Key = GenerateCanonicalKey(ClusterTemplate);

  // Check if icon already exists
  if (UTexture2D **ExistingIcon = ClusterIcons.Find(Key)) {
    return *ExistingIcon;
  }

  // Generate new icon
  UTexture2D *NewIcon = GenerateClusterIcon(ClusterTemplate);
  if (NewIcon) {
    ClusterIcons.Add(Key, NewIcon);
  }

  return NewIcon;
}

void AGameManager::ApplyCalibrationValuesToAllWalkers() {
  if (!OrbSimDriver) {
    UE_LOG(LogTemp, Warning,
           TEXT("ApplyCalibrationValuesToAllWalkers: No OrbSimDriver"));
    return;
  }

  USimWorldHex *Sim = OrbSimDriver->GetSim();
  if (!Sim) {
    UE_LOG(LogTemp, Warning,
           TEXT("ApplyCalibrationValuesToAllWalkers: No Sim"));
    return;
  }

  // Map SimIndex to Actor to get rotation
  TMap<int32, ABasicWalker *> IndexToActor;
  if (UWorld *World = GetWorld()) {
    for (TActorIterator<ABasicWalker> It(World); It; ++It) {
      ABasicWalker *Walker = *It;
      if (Walker && Walker->SimWalkerIndex >= 0) {
        IndexToActor.Add(Walker->SimWalkerIndex, Walker);

        // Also update actor properties here to ensure they are in sync
        if (Walker->SimWalkerIndex < Sim->Walkers.Num()) {
          Walker->StepHeight = StepHeight;
          Walker->StepRaiseTicks = StepRaiseTicks;
          Walker->TilesPerStep = TilesPerStep;
          Walker->StepLowerTicks = StepLowerTicks;
          Walker->ReachDistance = ReachDistance;
          Walker->MinimumReachDistance = MinimumReachDistance;
          Walker->StandHeight = StandHeight;
          Walker->StartingFootDistance = StartingFootDistance;
          Walker->QuadrantRotationOffset = QuadrantRotationOffset;
        }
      }
    }
  }

  const float TileSize = OrbSimDriver->GetTileSize();
  const int32 EffectiveReachDist = FMath::Max(1, ReachDistance);

  // Update all walkers in the simulation
  for (int32 i = 0; i < Sim->Walkers.Num(); ++i) {
    FWalkerState &WalkerState = Sim->Walkers[i];

    // Apply walking parameters
    WalkerState.StepHeight = StepHeight;
    WalkerState.StepRaiseTicks = StepRaiseTicks;
    WalkerState.TilesPerStep = TilesPerStep;
    WalkerState.StepLowerTicks = StepLowerTicks;
    WalkerState.ReachDistance = ReachDistance;
    WalkerState.MinimumReachDistance = MinimumReachDistance;
    WalkerState.StandHeight = StandHeight;
    WalkerState.QuadrantRotationOffset = QuadrantRotationOffset;

    // Apply StartingFootDistance by recalculating foot positions
    // StartingFootDistance is the number of tiles away from the body tile
    const int32 RingRadius = FMath::RoundToInt(StartingFootDistance);
    if (RingRadius > 0) {
      const FHex &BodyHex = WalkerState.BaseHex;
      const FVector BodyWorld = OrbSimDriver->MapHexToWorld(BodyHex);
      const FVector BodyWorld2D = FVector(BodyWorld.X, BodyWorld.Y, 0.f);

      // Determine body orientation
      FVector BodyForward = FVector::ForwardVector;
      FVector BodyRight = FVector::RightVector;

      if (ABasicWalker **WalkerPtr = IndexToActor.Find(i)) {
        const FTransform BodyTransform =
            (*WalkerPtr)->GetBodyComponent()->GetComponentTransform();
        BodyForward = BodyTransform.GetUnitAxis(EAxis::X);
        BodyRight = BodyTransform.GetUnitAxis(EAxis::Y);
        // Project to 2D
        BodyForward =
            FVector(BodyForward.X, BodyForward.Y, 0.f).GetSafeNormal();
        BodyRight = FVector(BodyRight.X, BodyRight.Y, 0.f).GetSafeNormal();
        if (BodyForward.IsNearlyZero())
          BodyForward = FVector::ForwardVector;
        if (BodyRight.IsNearlyZero())
          BodyRight = FVector::RightVector;
      }

      // Apply QuadrantRotationOffset
      if (QuadrantRotationOffset != 0.f) {
        FQuat Rot = FQuat(FVector::UpVector,
                          FMath::DegreesToRadians(QuadrantRotationOffset));
        BodyForward = Rot.RotateVector(BodyForward);
        BodyRight = Rot.RotateVector(BodyRight);
      }

      // Generate Ring of hex offsets (relative to center 0,0)
      TArray<FHex> Ring;
      FHex H = FHex(-RingRadius, RingRadius); // Start at SW corner
      for (int SideIdx = 0; SideIdx < 6; ++SideIdx) {
        for (int j = 0; j < RingRadius; ++j) {
          Ring.Add(H);
          H = HexUtil::Neighbor(H, (EHexDir)SideIdx);
        }
      }

      // Quadrant Definitions matching CalibrateWalker
      struct FQuadDef {
        FVector Dir;
        EWalkerFoot Leg;
      };
      FQuadDef Quads[] = {{BodyForward, NQuadrantLeg},
                          {BodyRight, EQuadrantLeg},
                          {-BodyForward, SQuadrantLeg},
                          {-BodyRight, WQuadrantLeg}};

      // Helper to map foot enum to index
      auto GetLegIndex = [](EWalkerFoot Foot) -> int32 {
        switch (Foot) {
        case EWalkerFoot::DFoot:
          return 0;
        case EWalkerFoot::LFoot:
          return 1;
        case EWalkerFoot::RFoot:
          return 2;
        case EWalkerFoot::UFoot:
          return 3;
        default:
          return -1;
        }
      };

      FFootState *Feet[] = {&WalkerState.DFoot, &WalkerState.LFoot,
                            &WalkerState.RFoot, &WalkerState.UFoot};

      for (const FQuadDef &Q : Quads) {
        int32 FootIdx = GetLegIndex(Q.Leg);
        if (FootIdx < 0)
          continue;

        // Target position in world space
        FVector IdealPos =
            BodyWorld2D + Q.Dir * (StartingFootDistance * TileSize);

        // Find best tile in Ring
        FHex BestOffsetHex = Ring[0];
        float BestDistSq = FLT_MAX;

        for (const FHex &OffsetH : Ring) {
          FHex AbsH(BodyHex.Q + OffsetH.Q, BodyHex.R + OffsetH.R);
          FVector WorldPos = OrbSimDriver->MapHexToWorld(AbsH);
          float DistSq = FVector::DistSquared2D(WorldPos, IdealPos);
          if (DistSq < BestDistSq) {
            BestDistSq = DistSq;
            BestOffsetHex = OffsetH;
          }
        }

        // Apply StartingFootOrientation (shift index in ring)
        if (StartingFootOrientation != 0) {
          int32 Index = Ring.IndexOfByKey(BestOffsetHex);
          if (Index != INDEX_NONE) {
            int32 NewIndex = (Index + StartingFootOrientation) % Ring.Num();
            if (NewIndex < 0)
              NewIndex += Ring.Num();
            BestOffsetHex = Ring[NewIndex];
          }
        }

        // Set new foot position
        FHex NewFootHex(BodyHex.Q + BestOffsetHex.Q,
                        BodyHex.R + BestOffsetHex.R);

        // Validate distance
        const int32 dq = NewFootHex.Q - BodyHex.Q;
        const int32 dr = NewFootHex.R - BodyHex.R;
        const int32 HexDist = FMath::Max(
            FMath::Max(FMath::Abs(dq), FMath::Abs(dr)), FMath::Abs(dq + dr));

        bool bValid = true;
        if (ReachDistance > 0 && HexDist > ReachDistance) {
          bValid = false;
        } else if (MinimumReachDistance > 0.f &&
                   (float)HexDist < MinimumReachDistance) {
          bValid = false;
        }

        if (bValid) {
          Feet[FootIdx]->CurrentTile = NewFootHex;
          Feet[FootIdx]->TargetTile = NewFootHex;
          Feet[FootIdx]->StepState = EFootStepState::Planted;
        }
      }
    }
  }

  // Also update BasicWalker actor instances with these values
  if (UWorld *World = GetWorld()) {
    for (TActorIterator<ABasicWalker> It(World); It; ++It) {
      ABasicWalker *Walker = *It;
      if (Walker && Walker->SimWalkerIndex >= 0 &&
          Walker->SimWalkerIndex < Sim->Walkers.Num()) {
        Walker->StepHeight = StepHeight;
        Walker->StepRaiseTicks = StepRaiseTicks;
        Walker->TilesPerStep = TilesPerStep;
        Walker->StepLowerTicks = StepLowerTicks;
        Walker->ReachDistance = ReachDistance;
        Walker->MinimumReachDistance = MinimumReachDistance;
        Walker->StandHeight = StandHeight;
        Walker->StartingFootDistance = StartingFootDistance;
        Walker->QuadrantRotationOffset = QuadrantRotationOffset;
        Walker->bDynamicBodyRotation = bDynamicBodyRotation;
      }
    }
  }

  UE_LOG(LogTemp, Log,
         TEXT("ApplyCalibrationValuesToAllWalkers: Updated %d walkers with "
              "StepHeight=%.1f, ReachDistance=%d, StartingFootDistance=%.1f, "
              "StandHeight=%.1f"),
         Sim->Walkers.Num(), StepHeight, ReachDistance, StartingFootDistance,
         StandHeight);
}

void AGameManager::UpdateWalkerQuadrantVisualization(ABasicWalker *Walker) {
  if (!Walker || !OrbSimDriver || !bDebugWalkers) {
    return;
  }

  // Clear existing markers for this walker
  ClearWalkerQuadrantVisualization(Walker);

  // Get walker state from simulation
  USimWorldHex *Sim = OrbSimDriver->GetSim();
  if (!Sim || Walker->SimWalkerIndex < 0 ||
      Walker->SimWalkerIndex >= Sim->Walkers.Num()) {
    return;
  }

  const FWalkerState &WalkerState = Sim->Walkers[Walker->SimWalkerIndex];
  const FHex BodyHex = WalkerState.BaseHex;
  const int32 ReachDist = WalkerState.ReachDistance;
  const float MinReachDist = WalkerState.MinimumReachDistance;

  // Helper to get quadrant from hex (updated to match
  // UpdateLegTileQuadrantVisualization with rotation)
  auto GetFixedQuadrantFromHex = [&](const FHex &BodyHex,
                                     const FHex &TestHex) -> FString {
    FVector BodyPos = OrbSimDriver->MapHexToWorld(BodyHex);
    FVector HexPos = OrbSimDriver->MapHexToWorld(TestHex);
    FVector Dir = (HexPos - BodyPos).GetSafeNormal2D();

    // Calculate angle in degrees (-180 to 180)
    // X+ (Forward) = 0, Y+ (Right) = 90
    float AngleDeg = FMath::RadiansToDegrees(FMath::Atan2(Dir.Y, Dir.X));

    // Apply offset (subtract to rotate boundaries)
    // Use Walker's offset if available to support per-walker dynamic rotation
    float EffectiveOffset = QuadrantRotationOffset;
    if (Walker) {
      EffectiveOffset = Walker->QuadrantRotationOffset;
    }
    AngleDeg -= EffectiveOffset;

    // Normalize to -180..180
    while (AngleDeg > 180.f)
      AngleDeg -= 360.f;
    while (AngleDeg <= -180.f)
      AngleDeg += 360.f;

    // Assign based on angle (centered on axes)
    // N (Forward/X+): -45 to 45
    // E (Right/Y+): 45 to 135
    // S (Back/X-): 135 to 180 or -180 to -135
    // W (Left/Y-): -135 to -45

    if (AngleDeg >= -45.f && AngleDeg < 45.f)
      return TEXT("N");
    if (AngleDeg >= 45.f && AngleDeg < 135.f)
      return TEXT("E");
    if (AngleDeg >= -135.f && AngleDeg < -45.f)
      return TEXT("W");
    return TEXT("S");
  };

  // Helper to apply quadrant offset
  auto ApplyQuadrantOffset = [](const FString &OriginalQuadrant,
                                int32 Offset) -> FString {
    static const TArray<FString> QuadrantOrder = {TEXT("N"), TEXT("E"),
                                                  TEXT("S"), TEXT("W")};
    int32 OriginalIndex = -1;
    for (int32 i = 0; i < QuadrantOrder.Num(); ++i) {
      if (QuadrantOrder[i] == OriginalQuadrant) {
        OriginalIndex = i;
        break;
      }
    }
    if (OriginalIndex < 0)
      return OriginalQuadrant;
    int32 OffsetValue = FMath::Clamp(Offset, 0, 3);
    int32 NewIndex = (OriginalIndex + OffsetValue) % 4;
    return QuadrantOrder[NewIndex];
  };

  // Map fixed quadrants to distinct colors
  static const TMap<FString, FLinearColor> QuadrantToColorMap = {
      {TEXT("N"), FLinearColor(0.0f, 1.0f, 0.0f)}, // Green
      {TEXT("E"), FLinearColor(1.0f, 0.0f, 0.0f)}, // Red
      {TEXT("W"), FLinearColor(0.0f, 0.0f, 1.0f)}, // Blue
      {TEXT("S"), FLinearColor(1.0f, 1.0f, 0.0f)}  // Yellow
  };

  // Helper to check if a quadrant corresponds to a holding foot
  auto IsQuadrantHolding = [&](const FString &Q) -> bool {
    // Map Quadrant to Foot using calibration settings
    EWalkerFoot TargetLeg = EWalkerFoot::DFoot; // default

    if (Q == TEXT("N"))
      TargetLeg = NQuadrantLeg;
    else if (Q == TEXT("E"))
      TargetLeg = EQuadrantLeg;
    else if (Q == TEXT("S"))
      TargetLeg = SQuadrantLeg;
    else if (Q == TEXT("W"))
      TargetLeg = WQuadrantLeg;
    else
      return false;

    switch (TargetLeg) {
    case EWalkerFoot::DFoot:
      return WalkerState.DFoot.bHoldingItem;
    case EWalkerFoot::LFoot:
      return WalkerState.LFoot.bHoldingItem;
    case EWalkerFoot::RFoot:
      return WalkerState.RFoot.bHoldingItem;
    case EWalkerFoot::UFoot:
      return WalkerState.UFoot.bHoldingItem;
    default:
      return false;
    }
  };

  // Find all reachable tiles and assign them to quadrants
  TMap<FHex, FString> HexToQuadrantMap;
  for (int32 q = -ReachDist; q <= ReachDist; ++q) {
    for (int32 r = -ReachDist; r <= ReachDist; ++r) {
      FHex TestHex(BodyHex.Q + q, BodyHex.R + r);
      if (TestHex == BodyHex)
        continue;

      const int32 dq = TestHex.Q - BodyHex.Q;
      const int32 dr = TestHex.R - BodyHex.R;
      const int32 HexDist = FMath::Max(
          FMath::Max(FMath::Abs(dq), FMath::Abs(dr)), FMath::Abs(dq + dr));

      if (HexDist > ReachDist ||
          (MinReachDist > 0.f && (float)HexDist < MinReachDist)) {
        continue;
      }

      FString FixedQuadrant = GetFixedQuadrantFromHex(BodyHex, TestHex);
      if (TileQuadrantOffset != 0) {
        FixedQuadrant = ApplyQuadrantOffset(FixedQuadrant, TileQuadrantOffset);
      }
      HexToQuadrantMap.Add(TestHex, FixedQuadrant);
    }
  }

  // Create material instances for each quadrant
  TMap<FString, UMaterialInstanceDynamic *> QuadrantMaterials;
  if (UMaterialInterface *BaseMaterial = GEngine->DebugMeshMaterial) {
    for (const auto &QuadrantColorPair : QuadrantToColorMap) {
      UMaterialInstanceDynamic *QuadrantMaterial =
          UMaterialInstanceDynamic::Create(BaseMaterial, this);
      if (QuadrantMaterial) {
        FLinearColor QuadColor = QuadrantColorPair.Value;

        // Override with Pink if holding
        if (IsQuadrantHolding(QuadrantColorPair.Key)) {
          QuadColor = FLinearColor(1.0f, 0.05f, 0.8f); // Hot Pink
        }

        QuadrantMaterial->SetVectorParameterValue(TEXT("Color"), QuadColor);
        QuadrantMaterial->SetVectorParameterValue(TEXT("BaseColor"), QuadColor);
        QuadrantMaterial->SetVectorParameterValue(TEXT("EmissiveColor"),
                                                  QuadColor);
        QuadrantMaterial->SetVectorParameterValue(TEXT("DiffuseColor"),
                                                  QuadColor);
        QuadrantMaterials.Add(QuadrantColorPair.Key, QuadrantMaterial);
      }
    }
  }

  // Create tile markers
  UStaticMesh *SphereMesh = LoadObject<UStaticMesh>(
      nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere"));
  UStaticMesh *TileOutlineMesh = TileMesh ? TileMesh : SphereMesh;
  const float MarkerScale = 0.95f;
  const float BaseZOffset = 20.f;

  TArray<UStaticMeshComponent *> WalkerMarkers;
  for (const auto &HexQuadPair : HexToQuadrantMap) {
    const FHex &ReachableHex = HexQuadPair.Key;
    const FString &Quadrant = HexQuadPair.Value;

    UMaterialInstanceDynamic **QuadrantMaterialPtr =
        QuadrantMaterials.Find(Quadrant);
    if (!QuadrantMaterialPtr || !*QuadrantMaterialPtr) {
      continue;
    }

    const FVector TileWorld = OrbSimDriver->MapHexToWorld(ReachableHex);

    UStaticMeshComponent *MarkerComp = NewObject<UStaticMeshComponent>(Walker);
    if (MarkerComp) {
      MarkerComp->SetStaticMesh(TileOutlineMesh);
      MarkerComp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
      MarkerComp->SetMobility(EComponentMobility::Movable);
      MarkerComp->SetCastShadow(false);
#if WITH_EDITOR
      MarkerComp->SetIsVisualizationComponent(true);
#endif
      MarkerComp->SetMaterial(0, *QuadrantMaterialPtr);
      MarkerComp->RegisterComponent();
      MarkerComp->AttachToComponent(
          Walker->GetRootComponent(),
          FAttachmentTransformRules::KeepWorldTransform);
      MarkerComp->SetWorldScale3D(FVector(MarkerScale));
      MarkerComp->SetWorldLocation(TileWorld + FVector(0.f, 0.f, BaseZOffset));
      MarkerComp->SetVisibility(true, true);
      WalkerMarkers.Add(MarkerComp);
    }
  }

  // Highlight assigned tiles with white markers
  if (UMaterialInterface *BaseMaterial = GEngine->DebugMeshMaterial) {
    UMaterialInstanceDynamic *FootHighlightMaterial =
        UMaterialInstanceDynamic::Create(BaseMaterial, this);
    if (FootHighlightMaterial) {
      const FLinearColor WhiteColor = FLinearColor::White;
      FootHighlightMaterial->SetVectorParameterValue(TEXT("Color"), WhiteColor);
      FootHighlightMaterial->SetVectorParameterValue(TEXT("BaseColor"),
                                                     WhiteColor);
      FootHighlightMaterial->SetVectorParameterValue(TEXT("EmissiveColor"),
                                                     WhiteColor);
      FootHighlightMaterial->SetVectorParameterValue(TEXT("DiffuseColor"),
                                                     WhiteColor);

      FHex AssignedTiles[4] = {
          WalkerState.DFoot.CurrentTile, WalkerState.LFoot.CurrentTile,
          WalkerState.RFoot.CurrentTile, WalkerState.UFoot.CurrentTile};

      for (int32 i = 0; i < 4; ++i) {
        const FHex &FootHex = AssignedTiles[i];
        const FVector FootTileWorld = OrbSimDriver->MapHexToWorld(FootHex);

        UStaticMeshComponent *FootMarkerComp =
            NewObject<UStaticMeshComponent>(Walker);
        if (FootMarkerComp) {
          FootMarkerComp->SetStaticMesh(TileOutlineMesh);
          FootMarkerComp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
          FootMarkerComp->SetMobility(EComponentMobility::Movable);
          FootMarkerComp->SetCastShadow(false);
#if WITH_EDITOR
          FootMarkerComp->SetIsVisualizationComponent(true);
#endif
          FootMarkerComp->SetMaterial(0, FootHighlightMaterial);
          FootMarkerComp->RegisterComponent();
          FootMarkerComp->AttachToComponent(
              Walker->GetRootComponent(),
              FAttachmentTransformRules::KeepWorldTransform);
          const float HighlightScaleMult = 1.35f;
          const float HighlightZOffset = 24.f;
          FootMarkerComp->SetWorldScale3D(
              FVector(MarkerScale * HighlightScaleMult));
          FootMarkerComp->SetWorldLocation(FootTileWorld +
                                           FVector(0.f, 0.f, HighlightZOffset));
          FootMarkerComp->SetVisibility(true, true);
          WalkerMarkers.Add(FootMarkerComp);
        }
      }
    }
  }

  // Draw line to current target if processing request
  if (WalkerState.bProcessingRequest) {
    const FVector StartPos =
        OrbSimDriver->MapHexToWorld(BodyHex) + FVector(0.f, 0.f, 100.f);
    const FVector EndPos =
        OrbSimDriver->MapHexToWorld(WalkerState.CurrentTargetHex) +
        FVector(0.f, 0.f, 50.f);

    DrawDebugLine(GetWorld(), StartPos, EndPos, FColor::Cyan, false, -1.0f, 0,
                  5.0f);
    DrawDebugSphere(GetWorld(), EndPos, 20.0f, 12, FColor::Cyan, false, -1.0f,
                    0, 2.0f);
  }

  // Store markers for this walker
  RuntimeWalkerQuadrantMarkers.Add(Walker, WalkerMarkers);
}

void AGameManager::ClearWalkerQuadrantVisualization(ABasicWalker *Walker) {
  if (!Walker) {
    return;
  }

  TArray<UStaticMeshComponent *> *MarkersPtr =
      RuntimeWalkerQuadrantMarkers.Find(Walker);
  if (MarkersPtr) {
    for (UStaticMeshComponent *MarkerComp : *MarkersPtr) {
      if (MarkerComp) {
        MarkerComp->DestroyComponent();
      }
    }
    RuntimeWalkerQuadrantMarkers.Remove(Walker);
  }
}

// ------------ Ripple/Wave Render Target Implementation ------------

UTextureRenderTarget2D *AGameManager::GetRippleRenderTarget() {
  if (!RippleRenderTarget) {
    InitializeRippleRenderTarget();
  }
  return RippleRenderTarget;
}

void AGameManager::InitializeRippleRenderTarget() {
  if (!RippleRenderTarget) {
    RippleRenderTarget = NewObject<UTextureRenderTarget2D>(this);
  }

  if (RippleRenderTarget->SizeX != RippleRenderTargetSize ||
      RippleRenderTarget->SizeY != RippleRenderTargetSize ||
      RippleRenderTarget->AddressX != TA_Clamp) {
    // Use PF_B8G8R8A8 for standard compatibility
    RippleRenderTarget->InitCustomFormat(
        RippleRenderTargetSize, RippleRenderTargetSize, PF_B8G8R8A8, false);
    RippleRenderTarget->AddressX = TA_Clamp;
    RippleRenderTarget->AddressY = TA_Clamp;
    RippleRenderTarget->UpdateResourceImmediate(true);

    UE_LOG(LogTemp, Log,
           TEXT("GameManager: Initialized ripple render target %dx%d (Address: "
                "Clamp)"),
           RippleRenderTargetSize, RippleRenderTargetSize);
  }
}

void AGameManager::ClearRippleRenderTarget() {
  if (!RippleRenderTarget) {
    GetRippleRenderTarget();
  }

  if (RippleRenderTarget) {
    // Clear to black (no ripples)
    UKismetRenderingLibrary::ClearRenderTarget2D(this, RippleRenderTarget,
                                                 FLinearColor::Black);
  }
}

FVector2D
AGameManager::WorldPositionToRippleRTUV(const FVector &WorldPos) const {
  if (!RippleRenderTarget || RippleRenderTargetWorldSize <= 0.0f) {
    return FVector2D(0.5f, 0.5f); // Center if not initialized
  }

  // Get RT center position (can be offset from grid anchor to follow camera)
  FVector2D RTCenter;
  if (RippleRTOffset.IsNearlyZero()) {
    // Default: center RT at grid anchor
    FVector AnchorPos =
        GridAnchor ? GridAnchor->GetActorLocation() : GetActorLocation();
    RTCenter = FVector2D(AnchorPos.X, AnchorPos.Y);
  } else {
    // Use custom offset (e.g., camera position)
    RTCenter = RippleRTOffset;
  }

  // Convert world position to local space relative to RT center
  FVector2D LocalPos = FVector2D(WorldPos.X, WorldPos.Y) - RTCenter;

  // Convert to UV coordinates (0-1 range)
  // RT covers a square area centered at RTCenter
  float HalfSize = RippleRenderTargetWorldSize * 0.5f;
  FVector2D UV;
  UV.X = (LocalPos.X + HalfSize) / RippleRenderTargetWorldSize;
  UV.Y = (LocalPos.Y + HalfSize) / RippleRenderTargetWorldSize;

  // Don't clamp - let values outside 0-1 be handled by the material
  // Clamping to edges causes beams when positions are outside RT bounds
  // The material should handle out-of-bounds by not drawing

  return UV;
}

void AGameManager::UpdateGridTileMID() {
  if (!TileMaterial) {
    GridTileMID = nullptr;
    CachedGridTileBaseMaterial = nullptr;
    return;
  }

  // Get the base material (parent if TileMaterial is already a MID)
  UMaterialInterface *BaseMaterial = TileMaterial;
  if (UMaterialInstanceDynamic *ExistingMID =
          Cast<UMaterialInstanceDynamic>(BaseMaterial)) {
    BaseMaterial = ExistingMID->Parent;
  }

  // Recreate MID if material changed or doesn't exist
  if (!GridTileMID || CachedGridTileBaseMaterial != BaseMaterial) {
    if (BaseMaterial) {
      GridTileMID = UMaterialInstanceDynamic::Create(BaseMaterial, this);
      CachedGridTileBaseMaterial = BaseMaterial;
      UE_LOG(LogTemp, Log,
             TEXT("GameManager: Created grid tile MID from material %s"),
             *BaseMaterial->GetName());
    } else {
      GridTileMID = nullptr;
      CachedGridTileBaseMaterial = nullptr;
    }
  }
}

// ------------ Wave Render Target Implementation ------------

UTextureRenderTarget2D *AGameManager::GetWaveRenderTarget() {
  if (!WaveRenderTarget) {
    InitializeWaveRenderTarget();
  }
  return WaveRenderTarget;
}

void AGameManager::InitializeWaveRenderTarget() {
  if (!WaveRenderTarget) {
    WaveRenderTarget = NewObject<UTextureRenderTarget2D>(this);
  }

  if (WaveRenderTarget->SizeX != WaveRenderTargetSize ||
      WaveRenderTarget->SizeY != WaveRenderTargetSize ||
      WaveRenderTarget->AddressX != TA_Clamp) {
    // Use FloatRGBA for high precision wave accumulation
    WaveRenderTarget->InitCustomFormat(
        WaveRenderTargetSize, WaveRenderTargetSize, PF_FloatRGBA, false);
    WaveRenderTarget->AddressX = TA_Clamp;
    WaveRenderTarget->AddressY = TA_Clamp;
    WaveRenderTarget->UpdateResourceImmediate(true);

    UE_LOG(LogTemp, Log,
           TEXT("GameManager: Initialized wave render target %dx%d (FloatRGBA, "
                "Address: Clamp)"),
           WaveRenderTargetSize, WaveRenderTargetSize);
  }
}

void AGameManager::ClearWaveRenderTarget() {
  if (!WaveRenderTarget) {
    GetWaveRenderTarget();
  }

  if (WaveRenderTarget) {
    // Clear to neutral (0.5 = no displacement, stored in R channel for height)
    // This ensures the grid doesn't sink when no waves are present
    UKismetRenderingLibrary::ClearRenderTarget2D(
        this, WaveRenderTarget, FLinearColor(0.5f, 0.0f, 0.0f, 1.0f));
  }
}

FVector2D AGameManager::WorldPositionToWaveRTUV(const FVector &WorldPos) const {
  if (!WaveRenderTarget || WaveRenderTargetWorldSize <= 0.0f) {
    return FVector2D(0.5f, 0.5f); // Center if not initialized
  }

  // Get RT center position (can be offset from grid anchor to follow camera)
  FVector2D RTCenter;
  if (WaveRTOffset.IsNearlyZero()) {
    // Default: center RT at grid anchor
    FVector AnchorPos =
        GridAnchor ? GridAnchor->GetActorLocation() : GetActorLocation();
    RTCenter = FVector2D(AnchorPos.X, AnchorPos.Y);
  } else {
    // Use custom offset (e.g., camera position)
    RTCenter = WaveRTOffset;
  }

  // Convert world position to local space relative to RT center
  FVector2D LocalPos = FVector2D(WorldPos.X, WorldPos.Y) - RTCenter;

  // Convert to UV coordinates (0-1 range)
  // RT covers a square area centered at RTCenter
  float HalfSize = WaveRenderTargetWorldSize * 0.5f;
  FVector2D UV;
  UV.X = (LocalPos.X + HalfSize) / WaveRenderTargetWorldSize;
  UV.Y = (LocalPos.Y + HalfSize) / WaveRenderTargetWorldSize;

  // Don't clamp - let values outside 0-1 be handled by the material
  // Clamping to edges causes beams when positions are outside RT bounds
  // The material should handle out-of-bounds by not drawing

  return UV;
}

void AGameManager::DrawWaveToRenderTarget(const FVector &WorldPosition,
                                          float WaveHeight, float WaveRadius,
                                          float WaveFalloff, float LifeTime) {
  UWorld *World = GetWorld();
  if (!World || (World->WorldType != EWorldType::Game &&
                 World->WorldType != EWorldType::PIE)) {
    // In Editor, we don't simulate waves (or maybe we do, but safest to skip)
    // Actually, if called from Blutility, we might want to.
    // For now, allow PIE and Game.
  }

  FGridWaveInstance NewWave;
  NewWave.WorldPosition = WorldPosition;
  NewWave.StartTime = World->GetTimeSeconds();
  // Scale lifetime inversely with animation speed so waves reach the same final
  // size Faster speed = shorter life, slower speed = longer life
  float SpeedScale =
      FMath::Max(0.1f, WaveAnimationSpeed); // Prevent division by zero
  NewWave.LifeTime = LifeTime / SpeedScale;
  NewWave.MaxHeight = WaveHeight;
  NewWave.Radius = WaveRadius;
  NewWave.Falloff = WaveFalloff;

  ActiveWaves.Add(NewWave);

  // If needed, we could trigger an immediate update, but Tick handles it better
  // for batching.
}

// ----------------------------------------------------------------------------------
// CRITICAL MATERIAL SETUP INSTRUCTION:
// To prevent "Square Artifacts", "Resetting Waves", and "Global Level Shifts",
// the assigned WaveDrawMaterial MUST be configured as follows:
//
// 1. Blend Mode: ADDITIVE
//    - Opaque or Translucent modes will overwrite/reset the water level,
//    causing artifacts.
//    - Additive mode sums the wave heights correctly.
//
// 2. Emissive/Base Color Output:
//    - Must be 0.0 (Black) where there is no wave.
//    - Must NOT output a "0.5" offset base. The C++ code now outputs pure delta
//    values.
//    - Only output the Ripple Height (Positive for crests).
//
// 3. Shading Model: Unlit (Preferred for pure data writing).
// ----------------------------------------------------------------------------------
void AGameManager::InternalDrawWaveStamp(class UCanvas *Canvas,
                                         const FVector2D &Size,
                                         const FGridWaveInstance &Wave) {
  if (!WaveDrawMaterial || !WaveRenderTarget || !Canvas) {
    return;
  }

  UWorld *World = GetWorld();
  if (!World)
    return;

  // Create or get material instance for drawing
  UMaterialInterface *BaseMat = WaveDrawMaterial;
  if (UMaterialInstanceDynamic *ExistingMID =
          Cast<UMaterialInstanceDynamic>(BaseMat)) {
    BaseMat = ExistingMID->Parent;
  }

  // Recreate MID if material changed or doesn't exist
  if (!WaveDrawMID || CachedWaveDrawBaseMaterial != BaseMat) {
    if (BaseMat && BaseMat->IsValidLowLevel()) {
      WaveDrawMID = UMaterialInstanceDynamic::Create(BaseMat, this);
      CachedWaveDrawBaseMaterial = BaseMat;
      if (!WaveDrawMID) {
        UE_LOG(
            LogTemp, Error,
            TEXT(
                "InternalDrawWaveStamp: Failed to create MID from material %s"),
            *BaseMat->GetName());
        return;
      }
    } else {
      WaveDrawMID = nullptr;
      CachedWaveDrawBaseMaterial = nullptr;
      // Only log once to avoid spam
      static bool bLoggedInvalidMat = false;
      if (!bLoggedInvalidMat) {
        UE_LOG(LogTemp, Warning,
               TEXT("InternalDrawWaveStamp: WaveDrawMaterial is invalid"));
        bLoggedInvalidMat = true;
      }
      return;
    }
  }

  if (WaveDrawMID && WaveDrawMID->IsValidLowLevel()) {
    // Convert world position to RT UV coordinates
    FVector2D RTUV = WorldPositionToWaveRTUV(Wave.WorldPosition);

    // Normalize parameters
    float RTWorldSize = WaveRenderTargetWorldSize;
    if (RTWorldSize <= 100.0f) {
      RTWorldSize = 2000.0f;
    }

    float ElapsedTime = World->GetTimeSeconds() - Wave.StartTime;
    float LifeFraction = FMath::Clamp(ElapsedTime / Wave.LifeTime, 0.0f, 1.0f);

    // Animate Radius: Start small and expand
    // This calculates the PHYSICAL RADIUS of the wave at this moment.
    // Scale expansion speed by WaveAnimationSpeed to give user control.
    float ExpansionSpeed = 150.0f * WaveAnimationSpeed;
    float AnimatedRadius = Wave.Radius + (ExpansionSpeed * ElapsedTime);

    // Fade Out: Use configurable exponent for fade curve
    // Lower WaveFadeExponent = more gradual "simmer" effect
    // Higher WaveFadeExponent = faster fade
    float FadeAlpha = FMath::Pow(1.0f - LifeFraction, WaveFadeExponent);

    // Fade In: Avoid popping (quick fade-in over first 0.1 seconds)
    float FadeIn = FMath::Clamp(ElapsedTime / 0.1f, 0.0f, 1.0f);
    FadeAlpha *= FadeIn;

    // Calculate Normalized Height
    // 0.5 Bias is REMOVED for Additive Blending compatibility.
    // Output 0.0 = No Change. >0.0 = Add height.
    float BaseHeight =
        FMath::Clamp(Wave.MaxHeight / MaxWaveHeight, -1.0f, 1.0f);
    float CurrentHeightNorm = (BaseHeight * FadeAlpha) * 0.5f;

    // Normalized for Material (0-1 in UV space)
    float NormalizedRadius = AnimatedRadius / RTWorldSize;

    // One texel in RT UV space
    float TexelSizeUV = 1.0f / (Size.X > 0 ? Size.X : 2048.0f);

    // Animate falloff: Ring gets thinner as it expands (like real water
    // ripples) The ring thickness shrinks proportionally with the fade
    float AnimatedFalloff = Wave.Falloff * FadeAlpha;
    // Ensure minimum physical thickness (0.1x original)
    AnimatedFalloff = FMath::Max(AnimatedFalloff, Wave.Falloff * 0.1f);
    float NormalizedFalloff = AnimatedFalloff / RTWorldSize;

    // --- FIX: Enforce minimum falloff (thickness) to maintain detail at small
    // scales --- Ensure falloff is at least 1.5 texels wide so it doesn't
    // disappear/alias away This solves the 'loss of detail' for smaller radius
    // values
    float MinFalloffUV = TexelSizeUV * 1.5f;
    NormalizedFalloff = FMath::Max(NormalizedFalloff, MinFalloffUV);

    // Set material parameters
    WaveDrawMID->SetVectorParameterValue(
        FName("WaveCenter"), FLinearColor(RTUV.X, RTUV.Y, 0.0f, 1.0f));
    WaveDrawMID->SetScalarParameterValue(FName("WaveHeight"),
                                         CurrentHeightNorm);
    // IMPORTANT: We pass the ANIMATED radius here.
    // This means the Material "WaveRadius" parameter grows over time.
    WaveDrawMID->SetScalarParameterValue(FName("WaveRadius"), NormalizedRadius);
    // Falloff also animates - ring gets thinner as it expands
    WaveDrawMID->SetScalarParameterValue(FName("WaveFalloff"),
                                         NormalizedFalloff);

    WaveDrawMID->SetScalarParameterValue(FName("WaveTime"),
                                         ElapsedTime * WaveAnimationSpeed);

    // Calculate localized quad bounds (2.0x radius margin)
    float TotalRadiusUV = NormalizedRadius + NormalizedFalloff;

    // --- FIX: Enforce minimum quad size to prevent rasterization artifacts for
    // small waves --- Ensure the draw quad is at least 16 pixels wide on the
    // render target
    float MinTotalRadiusUV = 8.0f / (Size.X > 0 ? Size.X : 2048.0f);
    TotalRadiusUV = FMath::Max(TotalRadiusUV, MinTotalRadiusUV);

    TotalRadiusUV *= 2.0f;

    FVector2D UVTopLeft = RTUV - FVector2D(TotalRadiusUV, TotalRadiusUV);
    FVector2D UVSize = FVector2D(TotalRadiusUV * 2.0f, TotalRadiusUV * 2.0f);

    FVector2D ScreenPos = UVTopLeft * Size;
    FVector2D ScreenSize = UVSize * Size;

    // Draw localized quad
    Canvas->K2_DrawMaterial(WaveDrawMID, ScreenPos, ScreenSize, UVTopLeft,
                            UVSize);
  }
}

// ============== Discovery Mode ==============

void AGameManager::SetDiscoveryMode(bool bEnabled) {
  bDiscoveryMode = bEnabled;
  UE_LOG(LogTemp, Log, TEXT("GameManager: Discovery Mode set to %s"),
         bEnabled ? TEXT("TRUE") : TEXT("FALSE"));
}

void AGameManager::DrawSourceHelix() {
  UWorld *World = GetWorld();
  if (!World || !OrbSimDriver || !OrbSimDriver->GetSim())
    return;
  USimWorldHex *Sim = OrbSimDriver->GetSim();

  // Draw Discovery Mode Helix (Rungs Falling Down)
  if (bDiscoveryMode && SourceHelixRungs.Num() > 0) {
    // (Existing Discovery Mode Drawing Logic)
    const float ZStart = 1200.0f;       // Start height above grid
    const float ZEnd = 200.0f;          // End height (above Source tile)
    const float TotalDropTime = 100.0f; // Ticks to drop
    const float RadiusBase = 200.0f;    // Helix radius

    // Draw rungs
    for (int32 i = SourceHelixRungs.Num() - 1; i >= 0; --i) {
      const FHelixRung &Rung = SourceHelixRungs[i];

      float LogicAge = (float)(Sim->GetStepCounter() - Rung.SpawnTick);
      if (LogicAge > TotalDropTime + 50)
        continue;

      float Progress = FMath::Clamp(LogicAge / TotalDropTime, 0.0f, 1.0f);
      float Z =
          FMath::Lerp(ZStart, ZEnd, Progress * Progress); // Quadratic ease-in

      float Angle = LogicAge * 10.0f + (i * 30.0f);
      float Rad = FMath::DegreesToRadians(Angle);
      float HelixRadius = FMath::Lerp(RadiusBase, 0.0f, Progress);

      FVector Center(0, 0, 0);
      if (SourceTilePositions.Num() > 0) {
        if (OrbSimDriver) {
          FHex TargetHex = SourceTilePositions[0];
          Center = OrbSimDriver->MapHexToWorld(TargetHex);
        }
      }

      FVector Offset(FMath::Cos(Rad) * HelixRadius,
                     FMath::Sin(Rad) * HelixRadius, Z);
      FVector Pos = Center + Offset;

      for (const FOrbData &Orb : Rung.Orbs) {
        FColor Col = OrbSimDriver->GetDebugColorForOrbType(Orb.Type);
        DrawDebugSphere(World, Pos, 15.0f, 8, Col, false, -1.0f, 0, 2.0f);
      }
    }
  }

  // Draw Runtime Source Groups (Strands Rising Up)
  if (Sim->SourceGroups.Num() > 0) {
    float Alpha = 0.f;
    if (auto *TM = AGameTickManager::Get(this)) {
      Alpha = TM->GetStepAlpha();
    }

    for (const auto &KV : Sim->SourceGroups) {
      const FSourceGroup &Group = KV.Value;
      for (const auto &Row : Group.PendingRows) {
        int32 TravelTime = 150;
        float Age = (float)(Sim->GetStepCounter() - Row.SpawnTick) + Alpha;
        float Progress = FMath::Clamp(Age / (float)TravelTime, 0.f, 1.f);

        // Rise from -800 to 0
        float ZStart = -800.f;
        float Z = FMath::Lerp(ZStart, 0.f, Progress);

        // Skip if already arrived (logic handles spawning, visuals stop at 0)
        if (Progress >= 1.f)
          Z = 0.f;

        for (int32 i = 0; i < Row.OrbTypes.Num(); ++i) {
          if (Row.OrbTypes[i] != 0 && Group.SortedTiles.IsValidIndex(i)) {
            FVector Pos = OrbSimDriver->MapHexToWorld(Group.SortedTiles[i]);
            Pos.Z += Z;
            FColor Col = OrbSimDriver->GetDebugColorForOrbType(Row.OrbTypes[i]);
            DrawDebugSphere(World, Pos, 12.f, 8, Col, false, -1.f, 0, 1.5f);
          }
        }
      }
    }
  }
}

void AGameManager::GenerateDiscoveryScenario() {
  if (!bDiscoveryMode)
    return;
  if (!OrbSimDriver || !OrbSimDriver->GetSim())
    return;

  USimWorldHex *Sim = OrbSimDriver->GetSim();

  // 1. Generate the scenario cluster at DiscoveryDifficulty
  int32 SavedDiff = DifficultyValue;
  DifficultyValue = 4; // Start with a small seed
  int32 SeedId = GenerateRandomCluster(Sim);
  DifficultyValue = SavedDiff;

  if (SeedId < 0) {
    UE_LOG(LogTemp, Error,
           TEXT("GameManager: Failed to generate seed cluster for discovery."));
    return;
  }

  // Recursively grow to target difficulty
  DiscoveryScenarioClusterId =
      GenerateRecursiveAncestralCluster(SeedId, DiscoveryDifficulty);

  const USimWorldHex::FClusterTemplate *ScenarioCluster =
      Sim->GetClusterTemplates().Find((uint8)DiscoveryScenarioClusterId);
  if (!ScenarioCluster || ScenarioCluster->Orbs.Num() == 0) {
    UE_LOG(LogTemp, Error,
           TEXT("GameManager: Failed to generate scenario cluster at diff %d."),
           DiscoveryDifficulty);
    return;
  }

  UE_LOG(LogTemp, Log,
         TEXT("GameManager: Discovery scenario cluster ID=%d with %d orbs."),
         DiscoveryScenarioClusterId, ScenarioCluster->Orbs.Num());

  // 2. Place SourceTiles along center Q-axis
  SourceTilePositions.Empty();
  int32 HalfCount = SourceTileCount / 2;
  for (int32 i = 0; i < SourceTileCount; ++i) {
    int32 Q = i - HalfCount;
    FHex SourceHex(Q, 0, 0);
    SourceTilePositions.Add(SourceHex);

    FTile NewTile;
    NewTile.Kind = ETileKind::Source;
    NewTile.Arg = 0;
    Sim->PlaceTile(SourceHex, NewTile);
  }

  UE_LOG(LogTemp, Log,
         TEXT("GameManager: Placed %d Source tiles along Q-axis."),
         SourceTileCount);

  // 3. Initialize source strand
  GenerateSourceStrand();

  // 4. Initialize target cluster sequence
  GenerateTargetClusterSequence();

  // 5. Initialize world orb counts
  RefreshWorldOrbCounts();

  // 6. Reset state
  SourceStrandCursor = 0;
  bSourcePaused = false;
  SourceHelixRungs.Empty();
  SourceHelixRotation = 0.f;

  // Pre-roll helix animation so it looks populated immediately
  for (int32 i = 0; i < 200; ++i) {
    int32 PreCount = SourceHelixRungs.Num();
    StepSourceHelix();
    if (SourceHelixRungs.Num() > PreCount) {
      // Fake history relative to current step
      SourceHelixRungs.Last().SpawnTick = Sim->StepCounter - i;

      // Clear the item we just placed to allow next rung to generate
      // (This simulates items being moved away from the source)
      for (const FHex &Pos : SourceTilePositions) {
        Sim->ItemMap.Remove(Pos);
        Sim->OrbAt.Remove(Pos);
      }
    }
  }

  CurrentTargetClusterIndex = 0;
  CompletedUniqueTargetClusters.Empty();

  // Force visual update
  Rebuild();

  UE_LOG(LogTemp, Log,
         TEXT("GameManager: Discovery scenario fully initialized. Strand "
              "length=%d, Targets=%d."),
         SourceStrand.Num(), TargetClusterSequence.Num());
}

void AGameManager::GenerateSourceStrand() {
  if (!OrbSimDriver || !OrbSimDriver->GetSim())
    return;
  USimWorldHex *Sim = OrbSimDriver->GetSim();

  const USimWorldHex::FClusterTemplate *ScenarioCluster =
      Sim->GetClusterTemplates().Find((uint8)DiscoveryScenarioClusterId);
  if (!ScenarioCluster || ScenarioCluster->Orbs.Num() == 0)
    return;

  // Copy all orbs from the scenario cluster
  SourceStrand = ScenarioCluster->Orbs;

  // Shuffle the strand randomly (Fisher-Yates)
  for (int32 i = SourceStrand.Num() - 1; i > 0; --i) {
    int32 j = FMath::RandRange(0, i);
    SourceStrand.Swap(i, j);
  }

  SourceStrandCursor = 0;

  UE_LOG(LogTemp, Log,
         TEXT("GameManager: Generated source strand with %d orbs."),
         SourceStrand.Num());
}

void AGameManager::StepSourceHelix() {
  if (!bDiscoveryMode || bSourcePaused)
    return;
  if (!OrbSimDriver || !OrbSimDriver->GetSim())
    return;
  if (SourceStrand.Num() == 0 || SourceTilePositions.Num() == 0)
    return;

  USimWorldHex *Sim = OrbSimDriver->GetSim();

  // Check if the next orb would exceed world limit
  const FOrbData &NextOrb = SourceStrand[SourceStrandCursor];
  uint8 OrbType = NextOrb.Type;

  int32 CurrentCount = 0;
  if (int32 *Found = WorldOrbCounts.Find(OrbType)) {
    CurrentCount = *Found;
  }

  if (CurrentCount >= GridOrbCapPerType) {
    bSourcePaused = true;
    return;
  }

  // Determine which source tile to spawn at (round-robin through positions)
  int32 TileIndex = SourceStrandCursor % SourceTilePositions.Num();
  FHex SpawnHex = SourceTilePositions[TileIndex];

  // Check if there's already an item at this hex
  if (Sim->ItemMap.Contains(SpawnHex)) {
    // Wait until the tile is clear
    return;
  }

  // Spawn the orb
  FOrb NewOrb;
  NewOrb.Type = (EOrbType)OrbType;

  FItem NewItem = FItem::MakeSingle(NewOrb);
  NewItem.Id = Sim->NextItemId++;
  Sim->ItemMap.Add(SpawnHex, NewItem);
  Sim->OrbAt.Add(SpawnHex, Sim->KindFromItem(NewItem));

  // Update world orb counts
  WorldOrbCounts.FindOrAdd(OrbType)++;

  // Create a helix rung for animation
  FHelixRung Rung;
  FOrbData RungOrb;
  RungOrb.Type = OrbType;
  RungOrb.dQ = 0;
  RungOrb.dR = 0;
  Rung.Orbs.Add(RungOrb);
  Rung.SpawnTick = Sim->StepCounter;
  SourceHelixRungs.Add(Rung);

  // Advance cursor (wrap around for continuous fountain)
  SourceStrandCursor = (SourceStrandCursor + 1) % SourceStrand.Num();

  // If we've gone through the entire strand, reshuffle for variety
  if (SourceStrandCursor == 0) {
    // Reshuffle the strand
    for (int32 i = SourceStrand.Num() - 1; i > 0; --i) {
      int32 j = FMath::RandRange(0, i);
      SourceStrand.Swap(i, j);
    }
  }

  // Advance helix rotation
  SourceHelixRotation += SourceHelixDegreesPerRung;
  if (SourceHelixRotation >= 360.f) {
    SourceHelixRotation -= 360.f;
  }

  // Cleanup old rungs (older than drop time + buffer)
  while (SourceHelixRungs.Num() > 0) {
    // Check age (using signed logic for safety against wrap in distant future,
    // though uint is fine generally)
    if ((Sim->StepCounter - SourceHelixRungs[0].SpawnTick) > 200) {
      SourceHelixRungs.RemoveAt(0);
    } else {
      break;
    }
  }
}

int32 AGameManager::CountWorldOrbsOfType(uint8 OrbType) const {
  if (!OrbSimDriver || !OrbSimDriver->GetSim())
    return 0;

  const USimWorldHex *Sim = OrbSimDriver->GetSim();
  int32 Count = 0;

  for (const auto &KV : Sim->ItemMap) {
    const FItem &Item = KV.Value;
    if (!Item.bIsCluster) {
      if ((uint8)Item.Single.Type == OrbType) {
        ++Count;
      }
    }
  }

  return Count;
}

void AGameManager::RefreshWorldOrbCounts() {
  WorldOrbCounts.Empty();

  if (!OrbSimDriver || !OrbSimDriver->GetSim())
    return;

  const USimWorldHex *Sim = OrbSimDriver->GetSim();

  for (const auto &KV : Sim->ItemMap) {
    const FItem &Item = KV.Value;
    if (!Item.bIsCluster) {
      uint8 OrbType = (uint8)Item.Single.Type;
      WorldOrbCounts.FindOrAdd(OrbType)++;
    }
  }

  // Check if source should be unpaused
  if (bSourcePaused && SourceStrand.Num() > 0) {
    const FOrbData &NextOrb = SourceStrand[SourceStrandCursor];
    int32 CurrentCount = 0;
    if (int32 *Found = WorldOrbCounts.Find(NextOrb.Type)) {
      CurrentCount = *Found;
    }
    if (CurrentCount < GridOrbCapPerType) {
      bSourcePaused = false;
    }
  }
}

void AGameManager::GenerateTargetClusterSequence() {
  TargetClusterSequence.Empty();

  if (!OrbSimDriver || !OrbSimDriver->GetSim())
    return;

  USimWorldHex *Sim = OrbSimDriver->GetSim();
  const USimWorldHex::FClusterTemplate *ScenarioCluster =
      Sim->GetClusterTemplates().Find((uint8)DiscoveryScenarioClusterId);
  if (!ScenarioCluster || ScenarioCluster->Orbs.Num() == 0)
    return;

  // Step 1: Collect orb types and their counts
  TMap<uint8, int32> TypeCounts;
  for (const FOrbData &Orb : ScenarioCluster->Orbs) {
    TypeCounts.FindOrAdd(Orb.Type)++;
  }

  // Sort types by count (smallest first)
  TArray<TPair<uint8, int32>> SortedTypes;
  for (const auto &KV : TypeCounts) {
    SortedTypes.Add(TPair<uint8, int32>(KV.Key, KV.Value));
  }
  SortedTypes.Sort(
      [](const TPair<uint8, int32> &A, const TPair<uint8, int32> &B) {
        return A.Value < B.Value;
      });

  // Step 1: Single-type targets (one orb of each type, smallest count first)
  for (const auto &TypePair : SortedTypes) {
    USimWorldHex::FClusterTemplate SingleTarget;
    FOrbData SingleOrb;
    SingleOrb.Type = TypePair.Key;
    SingleOrb.dQ = 0;
    SingleOrb.dR = 0;
    SingleTarget.Orbs.Add(SingleOrb);
    TargetClusterSequence.Add(SingleTarget);
  }

  // Step 2: Different-type pairs
  for (int32 i = 0; i < SortedTypes.Num(); ++i) {
    for (int32 j = i + 1; j < SortedTypes.Num(); ++j) {
      USimWorldHex::FClusterTemplate PairTarget;

      FOrbData OrbA;
      OrbA.Type = SortedTypes[i].Key;
      OrbA.dQ = 0;
      OrbA.dR = 0;
      PairTarget.Orbs.Add(OrbA);

      FOrbData OrbB;
      OrbB.Type = SortedTypes[j].Key;
      OrbB.dQ = 1;
      OrbB.dR = 0;
      PairTarget.Orbs.Add(OrbB);

      PairTarget.Bonds.Add(TPair<int32, int32>(0, 1));
      TargetClusterSequence.Add(PairTarget);
    }
  }

  // Step 3+: Combine previously submitted targets pairwise until we reach
  // the final cluster size. This is a simplified version - pairs of previous
  // targets bonded together.
  int32 PrevStart = 0;
  int32 PrevEnd = TargetClusterSequence.Num();

  while (TargetClusterSequence.Num() < ScenarioCluster->Orbs.Num() &&
         PrevEnd > PrevStart + 1) {
    for (int32 i = PrevStart; i + 1 < PrevEnd; i += 2) {
      if (TargetClusterSequence.Num() >= ScenarioCluster->Orbs.Num())
        break;

      const USimWorldHex::FClusterTemplate &A = TargetClusterSequence[i];
      const USimWorldHex::FClusterTemplate &B = TargetClusterSequence[i + 1];

      USimWorldHex::FClusterTemplate Combined;

      // Add orbs from A
      for (const FOrbData &Orb : A.Orbs) {
        Combined.Orbs.Add(Orb);
      }

      // Add orbs from B, offset to avoid overlap
      int32 OffsetQ = A.Orbs.Num(); // Simple offset along Q-axis
      int32 BaseIdx = Combined.Orbs.Num();
      for (const FOrbData &Orb : B.Orbs) {
        FOrbData Shifted = Orb;
        Shifted.dQ += OffsetQ;
        Combined.Orbs.Add(Shifted);
      }

      // Copy bonds from A
      for (const auto &Bond : A.Bonds) {
        Combined.Bonds.Add(Bond);
      }

      // Copy bonds from B with offset
      for (const auto &Bond : B.Bonds) {
        Combined.Bonds.Add(
            TPair<int32, int32>(Bond.Key + BaseIdx, Bond.Value + BaseIdx));
      }

      // Add connecting bond between last orb of A and first orb of B
      Combined.Bonds.Add(TPair<int32, int32>(A.Orbs.Num() - 1, BaseIdx));

      TargetClusterSequence.Add(Combined);
    }

    PrevStart = PrevEnd;
    PrevEnd = TargetClusterSequence.Num();
  }

  // Final target: the complete scenario cluster itself
  TargetClusterSequence.Add(*ScenarioCluster);

  UE_LOG(LogTemp, Log,
         TEXT("GameManager: Generated %d target clusters in sequence."),
         TargetClusterSequence.Num());
}

void AGameManager::OnTargetClusterCompleted() {
  if (CurrentTargetClusterIndex < TargetClusterSequence.Num()) {
    // Track unique target for radius unlocking
    if (CurrentTargetClusterIndex < TargetClusterSequence.Num()) {
      const USimWorldHex::FClusterTemplate &Completed =
          TargetClusterSequence[CurrentTargetClusterIndex];
      FClusterCanonicalKey Key = GenerateCanonicalKey(Completed);
      bool bAlreadyInSet = false;
      CompletedUniqueTargetClusters.Add(Key, &bAlreadyInSet);

      if (!bAlreadyInSet) {
        // New unique cluster! Expand radius and rebuild
        UnlockedGridRadius++;
        if (OrbSimDriver) {
          SetGridRadius(UnlockedGridRadius);
          Rebuild();
        }
        UE_LOG(LogTemp, Log,
               TEXT("GameManager: Unique target cluster stored! Radius "
                    "expanded to %d."),
               UnlockedGridRadius);
      }
    }

    CurrentTargetClusterIndex++;
    UE_LOG(LogTemp, Log, TEXT("GameManager: Target cluster %d/%d completed."),
           CurrentTargetClusterIndex, TargetClusterSequence.Num());
  }
}

void AGameManager::PreviewCurrentTargetCluster() {
  if (!OrbSimDriver || !OrbSimDriver->GetSim())
    return;

  USimWorldHex *Sim = OrbSimDriver->GetSim();

  if (CurrentTargetClusterIndex >= TargetClusterSequence.Num()) {
    UE_LOG(LogTemp, Warning,
           TEXT("GameManager: No current target cluster to preview."));
    return;
  }

  const USimWorldHex::FClusterTemplate &Target =
      TargetClusterSequence[CurrentTargetClusterIndex];

  // Place DiscoveryClusterOutput tiles at a visible position
  FHex PreviewCenter(0, -(UnlockedGridRadius + 3), 0);

  for (const FOrbData &Orb : Target.Orbs) {
    FHex PlaceHex(PreviewCenter.Q + Orb.dQ, PreviewCenter.R + Orb.dR,
                  PreviewCenter.Level);
    FTile NewTile;
    NewTile.Kind = ETileKind::DiscoveryClusterOutput;
    NewTile.Arg = Orb.Type;
    Sim->PlaceTile(PlaceHex, NewTile);
  }

  UE_LOG(LogTemp, Log,
         TEXT("GameManager: Previewing target cluster %d/%d with %d orbs."),
         CurrentTargetClusterIndex + 1, TargetClusterSequence.Num(),
         Target.Orbs.Num());
}

void AGameManager::PreviewFinalCluster() {
  if (!OrbSimDriver || !OrbSimDriver->GetSim())
    return;

  USimWorldHex *Sim = OrbSimDriver->GetSim();

  const USimWorldHex::FClusterTemplate *ScenarioCluster =
      Sim->GetClusterTemplates().Find((uint8)DiscoveryScenarioClusterId);
  if (!ScenarioCluster || ScenarioCluster->Orbs.Num() == 0) {
    UE_LOG(LogTemp, Warning,
           TEXT("GameManager: No scenario cluster to preview."));
    return;
  }

  // Place DiscoveryClusterOutput tiles at a visible position
  FHex PreviewCenter(UnlockedGridRadius + 5, 0, 0);

  for (const FOrbData &Orb : ScenarioCluster->Orbs) {
    FHex PlaceHex(PreviewCenter.Q + Orb.dQ, PreviewCenter.R + Orb.dR,
                  PreviewCenter.Level);
    FTile NewTile;
    NewTile.Kind = ETileKind::DiscoveryClusterOutput;
    NewTile.Arg = Orb.Type;
    Sim->PlaceTile(PlaceHex, NewTile);
  }

  UE_LOG(LogTemp, Log,
         TEXT("GameManager: Previewing final cluster with %d orbs."),
         ScenarioCluster->Orbs.Num());
}

void AGameManager::OnDiscoveryClusterProcessed(
    const USimWorldHex::FClusterTemplate &ProcessedTemplate,
    const FHex &Location) {
  // Legacy function - body emptied for new discovery mode
  // Cluster processing is now handled through target cluster sequence
}

// --- Helper ---
int32 AGameManager::CalculateClusterScore(const TArray<FOrbData> &Orbs) const {
  int32 Score = Orbs.Num(); // Each orb = +1

  for (int32 i = 0; i < Orbs.Num(); i++) {
    for (int32 j = i + 1; j < Orbs.Num(); j++) {
      int32 dQ = Orbs[i].dQ - Orbs[j].dQ;
      int32 dR = Orbs[i].dR - Orbs[j].dR;
      int32 Dist = (FMath::Abs(dQ) + FMath::Abs(dQ + dR) + FMath::Abs(dR)) / 2;
      if (Dist == 1) {
        if (Orbs[i].Type != Orbs[j].Type) {
          Score += 2; // Different type adjacent = +2
        }
      }
    }
  }
  return Score;
}

int32 AGameManager::GenerateRecursiveAncestralCluster(int32 AncestorId,
                                                      int32 TargetDifficulty) {
  if (!OrbSimDriver || !OrbSimDriver->GetSim())
    return -1;
  USimWorldHex *Sim = OrbSimDriver->GetSim();

  const USimWorldHex::FClusterTemplate *Ancestor =
      Sim->GetClusterTemplates().Find((uint8)AncestorId);
  if (!Ancestor)
    return -1;

  int32 CurrentScore = CalculateClusterScore(Ancestor->Orbs);
  if (CurrentScore >= TargetDifficulty) {
    return AncestorId;
  }

  int32 SavedDiff = DifficultyValue;
  int32 SavedSeed = LastAncestralTemplateId;

  DifficultyValue = CurrentScore + 1;
  LastAncestralTemplateId = AncestorId;

  // Attempt generation with retry logic (try up to 5 times if score doesn't
  // increase)
  int32 NextId = -1;
  for (int32 Retry = 0; Retry < 5; ++Retry) {
    if (Retry > 0)
      DifficultyValue++; // Force higher difficulty target
    NextId = GenerateAncestralCluster(Sim);

    const USimWorldHex::FClusterTemplate *NextTemplate =
        Sim->GetClusterTemplates().Find((uint8)NextId);

    if (NextTemplate) {
      int32 NextScore = CalculateClusterScore(NextTemplate->Orbs);
      if (NextScore > CurrentScore) {
        break; // Success!
      }
    }
  }

  DifficultyValue = SavedDiff;
  LastAncestralTemplateId = SavedSeed;

  if (NextId < 0)
    return AncestorId;

  // Verify progress again to prevent infinite loop
  const USimWorldHex::FClusterTemplate *NextTemplate =
      Sim->GetClusterTemplates().Find((uint8)NextId);
  if (NextTemplate) {
    int32 NextScore = CalculateClusterScore(NextTemplate->Orbs);
    if (NextScore <= CurrentScore) {
      UE_LOG(LogTemp, Warning,
             TEXT("GenerateRecursiveAncestralCluster: Failed to grow cluster "
                  "score (Current=%d, Next=%d). Breaking recursion."),
             CurrentScore, NextScore);
      return NextId;
    }
  }

  return GenerateRecursiveAncestralCluster(NextId, TargetDifficulty);
}
