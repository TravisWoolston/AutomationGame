#include "Actors/HexTopDownPawn.h"
#include "Actors/BasicArm.h"
#include "Actors/BasicWalker.h"
#include "Actors/HexFactoryActor.h"
#include "Actors/OrbSimDriver.h"
#include "Actors/RequestBeacon.h"
#include "Actors/TriggerTile.h"
#include "Blueprints/PlayerBlueprintLibrarySave.h"
#include "Camera/CameraComponent.h"
#include "Components/HexPlacementComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Data/OrbMaterialManager.h"
#include "DrawDebugHelpers.h"
#include "Engine/DirectionalLight.h"
#include "EngineUtils.h"
#include "Framework/GameManager.h"
#include "Framework/GameTickManager.h"
#include "Framework/TestLevelDirector.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/SpringArmComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Rendering/OrbRenderer.h"
#include "Sim/ArmTypes.h"
#include "Sim/HexCoord.h"
#include "Sim/HexTypes.h"
#include "Sim/SimWorldHex.h"
#include "UI/ArmTimelineEditorWidget.h"
#include "UI/ArmTimelineUMGWidget.h"
#include "UI/ArmTimelineWidget.h"
#include "UI/AutomationGameHUD.h"

AHexTopDownPawn::AHexTopDownPawn() {
  PrimaryActorTick.bCanEverTick = true;

  RootComponent = CreateDefaultSubobject<USceneComponent>("Root");

  SpringArm = CreateDefaultSubobject<USpringArmComponent>("SpringArm");
  SpringArm->SetupAttachment(RootComponent);
  SpringArm->TargetArmLength = 2000.f;
  SpringArm->bDoCollisionTest = false;
  SpringArm->SetRelativeRotation(FRotator(-60.f, 0.f, 0.f));

  Camera = CreateDefaultSubobject<UCameraComponent>("Camera");
  Camera->SetupAttachment(SpringArm);
  Camera->bUsePawnControlRotation = false;

  Placer = CreateDefaultSubobject<UHexPlacementComponent>("HexPlacer");
  Placer->bUseInputPolling = false; // we drive it via input events

  TileHighlightMesh =
      CreateDefaultSubobject<UStaticMeshComponent>("TileHighlightMesh");
  TileHighlightMesh->SetupAttachment(RootComponent);
  TileHighlightMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
  TileHighlightMesh->SetRenderCustomDepth(true);
  TileHighlightMesh->SetRenderInMainPass(false);
  TileHighlightMesh->SetVisibility(false);
}

void AHexTopDownPawn::ToggleEditorUI() {
  const bool bShouldShow = (TimelineEditorWidget == nullptr);
  ShowTimelineEditor(bShouldShow);
}

void AHexTopDownPawn::BeginPlay() {
  Super::BeginPlay();

  // Cache common actors
  if (UWorld *World = GetWorld()) {
    CachedOrbSimDriver = AOrbSimDriver::Find(World);
    for (TActorIterator<AGameManager> It(World); It; ++It) {
      CachedGameManager = *It;
      break;
    }
  }

  // Ensure placement preview is enabled by default
  if (Placer) {
    Placer->SetBlueprintMode(false);
  }

  // Bind to GameTickManager for NextTick functionality
  AGameTickManager *TickManager = AGameTickManager::Get(this);
  if (TickManager) {
    TickManager->OnFixedTick.AddDynamic(this,
                                        &AHexTopDownPawn::HandleNextTickStop);
  }

  // Editor UI now toggled explicitly via action

  // Load saved blueprints from disk
  UPlayerBlueprintLibrarySave *SaveGameInstance =
      Cast<UPlayerBlueprintLibrarySave>(
          UGameplayStatics::LoadGameFromSlot(BlueprintLibrarySaveSlotName, 0));

  if (SaveGameInstance) {
    // Convert from wrapper struct to TArray<TArray<>>
    SavedBlueprints.Empty();
    for (const FBlueprintData &Data : SaveGameInstance->SavedBlueprints) {
      SavedBlueprints.Add(Data.Tiles);
    }
    UE_LOG(LogTemp, Log,
           TEXT("HexTopDownPawn: Loaded %d blueprints from save file"),
           SavedBlueprints.Num());
  } else {
    UE_LOG(LogTemp, Log,
           TEXT("HexTopDownPawn: No saved blueprints found, starting with "
                "empty library"));
  }
}

void AHexTopDownPawn::Tick(float DeltaSeconds) {
  Super::Tick(DeltaSeconds);

  // Editor visibility is now managed by explicit calls to
  // UpdateTimelineEditorVisibility()

  // Forward Timeline Editor hotkeys without needing input actions
  if (TimelineEditorWidget) {
    APlayerController *PC =
        GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
    if (PC) {
      // Remove key first so it can override
      if (TimelineEditorWidget->RemoveButtonInputKey.IsValid() &&
          PC->WasInputKeyJustPressed(
              TimelineEditorWidget->RemoveButtonInputKey)) {
        TimelineEditorWidget->HandleKeyInput(
            TimelineEditorWidget->RemoveButtonInputKey);
      }

      // Command slot keys
      const TArray<FCommandSlotConfig> SlotConfigs =
          TimelineEditorWidget->GetCommandSlotConfigs();
      for (const FCommandSlotConfig &Cfg : SlotConfigs) {
        if (Cfg.InputKey.IsValid() &&
            PC->WasInputKeyJustPressed(Cfg.InputKey)) {
          TimelineEditorWidget->HandleKeyInput(Cfg.InputKey);
          // Do not break; allow multiple keys same frame if user pressed them
        }
      }
    }
  }

  AGameManager *GM = CachedGameManager;
  if (!GM && GetWorld()) {
    for (TActorIterator<AGameManager> It(GetWorld()); It; ++It) {
      CachedGameManager = *It;
      GM = CachedGameManager;
      break;
    }
  }

  const bool bGoldbergMode = (GM && GM->IsGoldbergMode());
  if (bGoldbergMode) {
    // Goldberg orbital movement - camera orbits OUTSIDE the sphere, always
    // looking at center
    const FVector GridCenter = GM->GridAnchor
                                   ? GM->GridAnchor->GetActorLocation()
                                   : GM->GetActorLocation();
    const float OrbitRadius = GM->GoldbergRadius * OrbitalDistanceMultiplier;

    // Get current orbital position
    FVector ToCamera = GetActorLocation() - GridCenter;
    float CurrentRadius = ToCamera.Size();
    if (CurrentRadius < 1.f) {
      // Initialize position if too close to center
      ToCamera = FVector(0.f, 0.f, OrbitRadius);
      CurrentRadius = OrbitRadius;
    }
    ToCamera /= CurrentRadius;

    // Build tangent frame for movement
    FVector Up = ToCamera;
    FVector East = FVector::CrossProduct(FVector::UpVector, Up);
    if (East.IsNearlyZero())
      East = FVector::RightVector;
    East.Normalize();
    FVector North = FVector::CrossProduct(Up, East).GetSafeNormal();

    // Apply orbital movement from input axes
    float AngularSpeed = MoveSpeed / OrbitRadius;
    float dLon = AxisX * AngularSpeed * DeltaSeconds;
    float dLat = AxisY * AngularSpeed * DeltaSeconds;

    // Apply latitude change (move toward poles)
    if (!FMath::IsNearlyZero(dLat)) {
      ToCamera = (ToCamera * FMath::Cos(dLat) + North * FMath::Sin(dLat))
                     .GetSafeNormal();
    }

    // Recompute frame after latitude change
    East = FVector::CrossProduct(FVector::UpVector, ToCamera);
    if (East.IsNearlyZero())
      East = FVector::RightVector;
    East.Normalize();

    // Apply longitude change
    if (!FMath::IsNearlyZero(dLon)) {
      ToCamera = (ToCamera * FMath::Cos(dLon) + East * FMath::Sin(dLon))
                     .GetSafeNormal();
    }

    // Set position outside sphere at fixed orbit radius
    SetActorLocation(GridCenter + ToCamera * OrbitRadius);

    // Orient camera to look at grid center
    FRotator LookAtRotation = (GridCenter - GetActorLocation()).Rotation();
    SpringArm->SetWorldRotation(LookAtRotation);
  } else {
    // Planar panning
    const FVector Right = GetActorRightVector();
    const FVector Fwd = GetActorForwardVector();
    FVector Delta = (Right * AxisX + Fwd * AxisY) * MoveSpeed * DeltaSeconds;
    AddActorWorldOffset(Delta, true);
  }

  // Zoom (disabled when TypeSelectionActive is held)
  if (!bTypeSelectionActive) {
    SpringArm->TargetArmLength = FMath::Clamp(
        SpringArm->TargetArmLength - AxisZoom * ZoomSpeed * DeltaSeconds,
        MinArm, MaxArm);
  }

  // Update selection if active
  UpdateSelection();
  UpdateDeletionSelection();

  // Update HUD selection box if active (both blueprint and deletion use the
  // same box)
  if ((bBlueprintSelectionActive || bDeletionSelectionActive) && GetWorld()) {
    APlayerController *PC = GetWorld()->GetFirstPlayerController();
    if (PC) {
      AAutomationGameHUD *GameHUD = Cast<AAutomationGameHUD>(PC->GetHUD());
      if (GameHUD) {
        FVector2D MousePosition;
        if (PC->GetMousePosition(MousePosition.X, MousePosition.Y)) {
          GameHUD->UpdateSelectionBox(MousePosition);
        }
      }
    }
  }

  // Update blueprint preview meshes if in placement mode
  if (bBlueprintPlacementMode && BlueprintTiles.Num() > 0 && GetWorld()) {
    UpdateBlueprintPreviewMeshes(DeltaSeconds);
  } else {
    ClearBlueprintPreviewComponents();
  }

  UpdateHoverHighlight(DeltaSeconds);

  // Discovery Mode Dynamic Difficulty
  // Skip dynamic recalculation when in Kind 12 blueprint placement mode
  // to allow manual difficulty cycling via scroll wheel
  if (GM && GM->IsDiscoveryMode() && Placer && Placer->IsHoverValid() &&
      !(bBlueprintPlacementMode && ActiveGenerationMode == 12)) {
    const FHex &H = Placer->GetHoverHex();
    int32 Dist =
        (FMath::Abs(H.Q) + FMath::Abs(H.Q + H.R) + FMath::Abs(H.R)) / 2;
    GM->DifficultyValue = GM->DiscoveryStartingDifficulty +
                          (Dist * GM->DiscoveryDifficultyScale) / 20;
  }

  // Handle continuous placement (painting) for belts and cluster sends
  if (bPlaceHeld && Placer &&
      (Placer->PlaceKind == EPlaceKind::Belt ||
       Placer->PlaceKind == EPlaceKind::ClusterSend) &&
      !bBlueprintMode && !bDeletionMode) {
    if (Placer->IsHoverValid()) {
      Placer->HandlePlace(true);
    }
  }
}

void AHexTopDownPawn::PlacePressed() {
  bPlaceHeld = true;
  if (bBlueprintPlacementMode && BlueprintTiles.Num() > 0) {
    PlaceBlueprint();
  } else if (bBlueprintMode && !bBlueprintSelectionActive) {
    StartSelection();
  } else if (bDeletionMode && !bDeletionSelectionActive) {
    StartDeletionSelection();
  } else if (Placer) {
    // Check if there's an arm at the target location first
    if (Placer->IsHoverValid()) {
      FHex TargetHex = Placer->GetHoverHex();
      ABasicArm *ArmAtLocation = FindArmAtHex(TargetHex);
      if (ArmAtLocation) {
        // Don't toggle timeline if we are in the middle of a multi-step
        // placement (like TriggerTile owner selection)
        bool bInMultiStepPlacement = false;
        if (Placer->PlaceKind == EPlaceKind::TriggerTile &&
            Placer->TriggerTilePlacementStep > 0)
          bInMultiStepPlacement = true;
        if (Placer->PlaceKind == EPlaceKind::RequestBeacon &&
            Placer->BeaconPlacementStep > 0)
          bInMultiStepPlacement = true;

        if (!bInMultiStepPlacement) {
          // Toggle the individual arm's timeline
          ToggleArmTimeline(ArmAtLocation);
          return;
        }
      }
    }
    Placer->HandlePlace(true);
  }
}

void AHexTopDownPawn::PlaceReleased() {
  bPlaceHeld = false;
  if (bBlueprintMode && bBlueprintSelectionActive) {
    EndSelection();
  } else if (bDeletionMode && bDeletionSelectionActive) {
    EndDeletionSelection();
  }
}

UStaticMesh *AHexTopDownPawn::GetHighlightMeshForHex(const FHex &Hex,
                                                     float &OutYaw,
                                                     FVector &OutScale,
                                                     FVector &OutWorldPos,
                                                     bool &bOutWorldPosSet) {
  OutYaw = 0.f;
  OutScale = FVector(1.f);
  bOutWorldPosSet = false;
  OutWorldPos = FVector::ZeroVector;

  AOrbSimDriver *Driver = CachedOrbSimDriver;
  if (!Driver && GetWorld()) {
    Driver = AOrbSimDriver::Find(GetWorld());
    CachedOrbSimDriver = Driver;
  }
  if (!Driver)
    return nullptr;

  USimWorldHex *Sim = Driver->GetSim();
  if (!Sim)
    return nullptr;

  if (!CachedHexFactory) {
    for (TActorIterator<AHexFactoryActor> It(GetWorld()); It; ++It) {
      CachedHexFactory = *It;
      break;
    }
  }

  // Helper to compute yaw to a specific neighbor direction
  auto GetYawToNeighbor = [&](const FHex &Start, uint8 Dir) -> float {
    FHex Neighbor = HexUtil::Neighbor(Start, (EHexDir)(Dir % 6));
    FVector PosA = Driver->MapHexToWorld(Start);
    FVector PosB = Driver->MapHexToWorld(Neighbor);
    FVector DirVec = (PosB - PosA).GetSafeNormal();
    return DirVec.IsNearlyZero()
               ? 0.f
               : FMath::RadiansToDegrees(FMath::Atan2(DirVec.Y, DirVec.X));
  };

  // 1. Check for Arms (Actors)
  if (ABasicArm *Arm = FindArmAtHex(Hex)) {
    // For selection box, we use a hex highlight at the base
    OutYaw = GetYawToNeighbor(Hex, 0);
    if (Driver->GetGridOrientation() == EHexOrientation::FlatTop)
      OutYaw += 30.f;
    return Placer ? Placer->GenericTilePreviewMesh
                  : (TileHighlightMesh
                         ? (UStaticMesh *)TileHighlightMesh->GetStaticMesh()
                         : nullptr);
  }

  // 2. Check for Walkers (Actors)
  for (TActorIterator<ABasicWalker> It(GetWorld()); It; ++It) {
    ABasicWalker *Walker = *It;
    if (Walker && Walker->Q == Hex.Q && Walker->R == Hex.R &&
        Walker->Level == Hex.Level) {
      OutYaw = GetYawToNeighbor(Hex, 0);
      if (Driver->GetGridOrientation() == EHexOrientation::FlatTop)
        OutYaw += 30.f;
      return Placer ? Placer->GenericTilePreviewMesh
                    : (TileHighlightMesh
                           ? (UStaticMesh *)TileHighlightMesh->GetStaticMesh()
                           : nullptr);
    }
  }

  // 3. Check for Tiles (via Sim)
  FTile *Tile = Sim->TileMap.Find(Hex);
  if (!Tile)
    return nullptr;

  UStaticMesh *MeshToUse = nullptr;

  switch (Tile->Kind) {
  case ETileKind::Belt:
    MeshToUse = Placer ? Placer->GetActualBeltMesh() : nullptr;
    {
      uint8 Dir = (uint8)Tile->Arg;
      OutYaw = GetYawToNeighbor(Hex, Dir);
      if (CachedHexFactory) {
        OutScale = CachedHexFactory->BeltScale;
      }
    }
    break;
  case ETileKind::Input:
  case ETileKind::ClusterInput:
    MeshToUse = Placer ? Placer->GetActualOrbMesh() : nullptr;
    OutYaw = GetYawToNeighbor(Hex, 0);
    if (Driver->GetGridOrientation() == EHexOrientation::FlatTop)
      OutYaw += 30.f;
    break;
  case ETileKind::Output:
  case ETileKind::ClusterOutput:
    MeshToUse = Placer ? Placer->GetActualOutputOrbMesh() : nullptr;
    OutYaw = GetYawToNeighbor(Hex, 0);
    if (Driver->GetGridOrientation() == EHexOrientation::FlatTop)
      OutYaw += 30.f;

    if (Tile->Kind == ETileKind::ClusterOutput) {
      if (CachedGameManager) {
        OutScale = FVector(CachedGameManager->ClusterOutputScale);
      } else if (UWorld *World = GetWorld()) {
        for (TActorIterator<AGameManager> It(World); It; ++It) {
          OutScale = FVector((*It)->ClusterOutputScale);
          CachedGameManager = *It;
          break;
        }
      }
    }
    break;
  case ETileKind::Bond:
    MeshToUse = Placer ? Placer->BondPreviewMesh : nullptr;
    {
      uint8 Dir = (uint8)Tile->Arg;
      FVector CenterA = Driver->MapHexToWorld(Hex);
      FVector CenterB =
          Driver->MapHexToWorld(HexUtil::Neighbor(Hex, (EHexDir)(Dir % 6)));
      OutWorldPos = (CenterA + CenterB) * 0.5f;
      bOutWorldPosSet = true;
      FVector DirVec = (CenterB - CenterA).GetSafeNormal();
      OutYaw = FMath::RadiansToDegrees(FMath::Atan2(DirVec.Y, DirVec.X));
      if (CachedHexFactory) {
        OutScale = CachedHexFactory->BondScale;
      }
    }
    break;
  case ETileKind::Unbond:
    MeshToUse = (Placer && Placer->UnbondPreviewMesh)
                    ? Placer->UnbondPreviewMesh
                    : (Placer ? Placer->BondPreviewMesh : nullptr);
    {
      uint8 Dir = (uint8)Tile->Arg;
      FVector CenterA = Driver->MapHexToWorld(Hex);
      FVector CenterB =
          Driver->MapHexToWorld(HexUtil::Neighbor(Hex, (EHexDir)(Dir % 6)));
      OutWorldPos = (CenterA + CenterB) * 0.5f;
      bOutWorldPosSet = true;
      FVector DirVec = (CenterB - CenterA).GetSafeNormal();
      OutYaw = FMath::RadiansToDegrees(FMath::Atan2(DirVec.Y, DirVec.X));
      if (CachedHexFactory) {
        OutScale = CachedHexFactory->UnbondScale;
      }
    }
    break;
  default:
    break;
  }

  return MeshToUse;
}

void AHexTopDownPawn::UpdateHoverHighlight(float DeltaSeconds) {
  if (!Placer || !GetWorld())
    return;

  if (CurrentViewMode == EViewMode::OrbView) {
    if (TileHighlightMesh)
      TileHighlightMesh->SetVisibility(false);

    const FHex CursorHex = Placer ? Placer->GetHoverHex() : FHex(-1, -1, -1);
    TArray<FHex> BondedAnchors;
    TArray<int32> BondIndices;
    TArray<int32> ArmIndices;
    TArray<FHex> ClusterHexes = FindItemConnectedComponent(
        CursorHex, BondedAnchors, BondIndices, ArmIndices);

    if (ClusterHexes.Num() > 0) {
      // Use the actual orb mesh for highlighting if available
      UStaticMesh *OrbMesh = Placer ? Placer->GetActualOrbMesh() : nullptr;
      UStaticMesh *BondMesh = Placer ? Placer->BondPreviewMesh : nullptr;
      float OrbScale = 0.35f;

      int32 TotalNeeded = ClusterHexes.Num() + BondIndices.Num();

      // Ensure we have enough components
      while (OrbHighlightComponents.Num() < TotalNeeded) {
        UStaticMeshComponent *Comp = NewObject<UStaticMeshComponent>(this);
        Comp->RegisterComponent();
        Comp->AttachToComponent(
            RootComponent, FAttachmentTransformRules::KeepRelativeTransform);
        Comp->SetRenderCustomDepth(true);
        Comp->SetCustomDepthStencilValue(1); // Outline color
        Comp->SetRenderInMainPass(false);    // Only see outline
        Comp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        Comp->SetCollisionProfileName(TEXT("NoCollision"));
        Comp->SetCastShadow(false);
        OrbHighlightComponents.Add(Comp);
      }

      // Hide excess
      for (int32 i = TotalNeeded; i < OrbHighlightComponents.Num(); ++i) {
        OrbHighlightComponents[i]->SetVisibility(false);
      }

      // Position and show orbs at orb height
      float ZOffset = (CachedOrbSimDriver && CachedOrbSimDriver->GetSim())
                          ? CachedOrbSimDriver->GetSim()->OrbZOffset
                          : 50.0f;

      AOrbSimDriver *Driver = CachedOrbSimDriver;
      if (!Driver)
        Driver = AOrbSimDriver::Find(GetWorld());

      // 1. Position Orbs
      for (int32 i = 0; i < ClusterHexes.Num(); ++i) {
        UStaticMeshComponent *Comp = OrbHighlightComponents[i];

        if (OrbMesh && Comp->GetStaticMesh() != OrbMesh) {
          Comp->SetStaticMesh(OrbMesh);
          Comp->SetWorldScale3D(FVector(OrbScale));
        } else if (!OrbMesh && TileHighlightMesh &&
                   Comp->GetStaticMesh() !=
                       TileHighlightMesh->GetStaticMesh()) {
          Comp->SetStaticMesh(TileHighlightMesh->GetStaticMesh());
          Comp->SetWorldScale3D(TileHighlightMesh->GetRelativeScale3D() * 0.4f);
        }

        FVector WorldPos =
            CachedGameManager
                ? CachedGameManager->GetWorldForHex(ClusterHexes[i])
                : FVector::ZeroVector;
        WorldPos.Z += ZOffset;
        Comp->SetWorldLocation(WorldPos);
        Comp->SetWorldRotation(FRotator::ZeroRotator);
        Comp->SetVisibility(true);
      }

      // 2. Position Bonds
      if (Driver && Driver->GetSim()) {
        USimWorldHex *Sim = Driver->GetSim();
        for (int32 i = 0; i < BondIndices.Num(); ++i) {
          int32 BondIdx = BondIndices[i];
          UStaticMeshComponent *Comp =
              OrbHighlightComponents[ClusterHexes.Num() + i];

          if (BondMesh && Comp->GetStaticMesh() != BondMesh) {
            Comp->SetStaticMesh(BondMesh);
          }

          if (Sim->Bonds.IsValidIndex(BondIdx)) {
            const USimWorldHex::FBond &B = Sim->Bonds[BondIdx];
            FHex AnchorA = B.Root;
            FHex AnchorB(B.Root.Q + B.dQ, B.Root.R + B.dR, B.Root.Level);

            FVector PosA = Driver->MapHexToWorld(AnchorA);
            FVector PosB = Driver->MapHexToWorld(AnchorB);
            FVector Mid = (PosA + PosB) * 0.5f;
            Mid.Z += ZOffset; // Same height as orbs

            FVector DirVec = (PosB - PosA).GetSafeNormal();
            float Yaw =
                FMath::RadiansToDegrees(FMath::Atan2(DirVec.Y, DirVec.X));

            Comp->SetWorldLocation(Mid);
            Comp->SetWorldRotation(FRotator(0, Yaw, 0));

            FVector BScale(1.0f);
            if (CachedHexFactory)
              BScale = CachedHexFactory->BondScale;
            Comp->SetWorldScale3D(BScale);
            Comp->SetVisibility(true);
          }
        }
      }
    } else {
      for (auto *Comp : OrbHighlightComponents)
        Comp->SetVisibility(false);
    }
    return;
  } else {
    // Hidden in other modes if it was used
    if (OrbHighlightComponents.Num() > 0)
      ClearOrbHighlightComponents();
  }

  // Reset previous actor highlight if any
  if (IsValid(LastHighlightedActor)) {
    TArray<UPrimitiveComponent *> Comps;
    LastHighlightedActor->GetComponents(Comps);
    for (UPrimitiveComponent *C : Comps) {
      C->SetRenderCustomDepth(false);
    }
  }
  LastHighlightedActor = nullptr;

  // Reset tile highlight
  if (TileHighlightMesh) {
    TileHighlightMesh->SetVisibility(false);
  }

  // 4. Handle global category highlight based on current selection
  if (Placer) {
    EOrbType CurrentOrbType = EOrbType::None;
    if (Placer->PlaceKind == EPlaceKind::Input ||
        Placer->PlaceKind == EPlaceKind::ClusterInput) {
      CurrentOrbType = Placer->GetCurrentInputOrbType();
    } else if (Placer->PlaceKind == EPlaceKind::Output ||
               Placer->PlaceKind == EPlaceKind::ClusterOutput ||
               Placer->PlaceKind == EPlaceKind::ClusterSend) {
      CurrentOrbType = Placer->GetCurrentOutputOrbType();
    } else if (Placer->PlaceKind == EPlaceKind::TriggerTile) {
      CurrentOrbType = Placer->GetCurrentTriggerOrbType();
    }

    if (Placer->PlaceKind != LastGlobalHighlightKind ||
        CurrentOrbType != LastGlobalHighlightOrbType) {
      // Turn off old highlight
      if (LastGlobalHighlightKind != (EPlaceKind)255) {
        SetGlobalHighlightForKind(LastGlobalHighlightKind, false);
      }
      // Turn on new highlight
      SetGlobalHighlightForKind(Placer->PlaceKind, true);
      LastGlobalHighlightKind = Placer->PlaceKind;
      LastGlobalHighlightOrbType = CurrentOrbType;
    }
  } else if (LastGlobalHighlightKind != (EPlaceKind)255) {
    SetGlobalHighlightForKind(LastGlobalHighlightKind, false);
    LastGlobalHighlightKind = (EPlaceKind)255;
    LastGlobalHighlightOrbType = EOrbType::None;
  }

  // If dragging selection/blueprint, usually we don't highlight single tiles,
  // but for now let's allow it if the cursor is valid.
  if (!Placer->IsHoverValid()) {
    bHighlightSmoothingInitialized = false;
    return;
  }

  FHex HoverHex = Placer->GetHoverHex();

  // Log selected tile if enabled and hex has changed
  if (LogSelectedTile && !(HoverHex == LastLoggedHex)) {
    UE_LOG(LogTemp, Log, TEXT("Hovered Tile: Q=%d, R=%d, Level=%d"), HoverHex.Q,
           HoverHex.R, HoverHex.Level);
    LastLoggedHex = HoverHex;
  }

  // 1. Check for Arms/Walkers first for special per-actor highlight
  if (ABasicArm *Arm = FindArmAtHex(HoverHex)) {
    TArray<UPrimitiveComponent *> Comps;
    Arm->GetComponents(Comps);
    for (UPrimitiveComponent *C : Comps) {
      if (C->IsA<UMeshComponent>()) {
        C->SetRenderCustomDepth(true);
      }
    }
    LastHighlightedActor = Arm;
    bHighlightSmoothingInitialized = false;
    return;
  }
  for (TActorIterator<ABasicWalker> It(GetWorld()); It; ++It) {
    ABasicWalker *Walker = *It;
    if (Walker && Walker->Q == HoverHex.Q && Walker->R == HoverHex.R &&
        Walker->Level == HoverHex.Level) {
      TArray<UPrimitiveComponent *> Comps;
      Walker->GetComponents(Comps);
      for (UPrimitiveComponent *C : Comps) {
        if (C->IsA<UMeshComponent>()) {
          C->SetRenderCustomDepth(true);
        }
      }
      LastHighlightedActor = Walker;
      bHighlightSmoothingInitialized = false;
      return;
    }
  }

  // 2. Otherwise use the ghost mesh via GetHighlightMeshForHex
  float RotationYaw = 0.f;
  FVector Scale = FVector(1.f);
  FVector TargetLocation = FVector::ZeroVector;
  bool bTargetLocationSet = false;

  UStaticMesh *MeshToUse = GetHighlightMeshForHex(
      HoverHex, RotationYaw, Scale, TargetLocation, bTargetLocationSet);

  if (MeshToUse && TileHighlightMesh) {
    TileHighlightMesh->SetStaticMesh(MeshToUse);

    if (!bTargetLocationSet) {
      AOrbSimDriver *Driver = CachedOrbSimDriver;
      if (!Driver)
        Driver = AOrbSimDriver::Find(GetWorld());
      TargetLocation =
          Driver ? Driver->MapHexToWorld(HoverHex) : FVector::ZeroVector;
    }

    FRotator TargetRotation = FRotator(0.f, RotationYaw, 0.f);

    if (!bHighlightSmoothingInitialized) {
      HighlightSmoothedLocation = TargetLocation;
      HighlightSmoothedRotation = TargetRotation;
      bHighlightSmoothingInitialized = true;
    } else {
      HighlightSmoothedLocation =
          FMath::VInterpTo(HighlightSmoothedLocation, TargetLocation,
                           DeltaSeconds, HighlightSmoothingSpeed);
      HighlightSmoothedRotation =
          FMath::RInterpTo(HighlightSmoothedRotation, TargetRotation,
                           DeltaSeconds, HighlightSmoothingSpeed);
    }

    TileHighlightMesh->SetWorldLocation(HighlightSmoothedLocation);
    TileHighlightMesh->SetWorldRotation(HighlightSmoothedRotation);
    TileHighlightMesh->SetWorldScale3D(Scale);
    TileHighlightMesh->SetVisibility(true);
  } else {
    bHighlightSmoothingInitialized = false;
  }
}

void AHexTopDownPawn::SetupPlayerInputComponent(UInputComponent *IC) {
  Super::SetupPlayerInputComponent(IC);
  // Axis (set up in Project Settings → Input)
  IC->BindAxis("MoveRight", this, &AHexTopDownPawn::MoveX);
  IC->BindAxis("MoveForward", this, &AHexTopDownPawn::MoveY);
  IC->BindAxis("Zoom", this, &AHexTopDownPawn::Zoom);

  // Actions
  IC->BindAction("Place", IE_Pressed, this, &AHexTopDownPawn::PlacePressed);
  IC->BindAction("Place", IE_Released, this, &AHexTopDownPawn::PlaceReleased);
  IC->BindAction("Remove", IE_Pressed, this, &AHexTopDownPawn::RemovePressed);

  // Editor UI toggle (user assigns key in Project Settings)
  IC->BindAction("EditorUIToggle", IE_Pressed, this,
                 &AHexTopDownPawn::ToggleEditorUI);

  // Blueprint mode (hold to select, toggle for placement)
  IC->BindAction("BlueprintMode", IE_Pressed, this,
                 &AHexTopDownPawn::BlueprintModePressed);
  IC->BindAction("BlueprintMode", IE_Released, this,
                 &AHexTopDownPawn::BlueprintModeReleased);

  // Deletion mode (hold to select, then delete on release)
  IC->BindAction("DeletionMode", IE_Pressed, this,
                 &AHexTopDownPawn::DeletionModePressed);
  IC->BindAction("DeletionMode", IE_Released, this,
                 &AHexTopDownPawn::DeletionModeReleased);

  // Blueprint placement actions (use same keys as regular rotation)
  // IC->BindAction("BlueprintRotateCW", IE_Pressed, this,
  // &AHexTopDownPawn::RotateBlueprintCW); IC->BindAction("BlueprintRotateCCW",
  // IE_Pressed, this, &AHexTopDownPawn::RotateBlueprintCCW);

  IC->BindAction("RotateCW", IE_Pressed, this, &AHexTopDownPawn::RotCW);
  IC->BindAction("RotateCCW", IE_Pressed, this, &AHexTopDownPawn::RotCCW);

  // Walker control
  IC->BindAction("RequestWalkerMove", IE_Pressed, this,
                 &AHexTopDownPawn::RequestMoveToTile);

  IC->BindAction("Kind1", IE_Pressed, this, &AHexTopDownPawn::SetInputMode1);
  IC->BindAction("Kind2", IE_Pressed, this, &AHexTopDownPawn::SetInputMode2);
  IC->BindAction("Kind3", IE_Pressed, this, &AHexTopDownPawn::SetInputMode3);
  IC->BindAction("Kind4", IE_Pressed, this, &AHexTopDownPawn::SetInputMode4);
  IC->BindAction("Kind5", IE_Pressed, this, &AHexTopDownPawn::SetInputMode5);
  IC->BindAction("Kind6", IE_Pressed, this, &AHexTopDownPawn::SetInputMode6);
  IC->BindAction("Kind7", IE_Pressed, this, &AHexTopDownPawn::SetInputMode7);
  IC->BindAction("Kind8", IE_Pressed, this, &AHexTopDownPawn::SetInputMode8);
  IC->BindAction("Kind9", IE_Pressed, this, &AHexTopDownPawn::SetInputMode9);
  IC->BindAction("Kind10", IE_Pressed, this, &AHexTopDownPawn::SetInputMode10);
  IC->BindAction("Kind11", IE_Pressed, this, &AHexTopDownPawn::SetInputMode11);
  IC->BindAction("Kind12", IE_Pressed, this,
                 &AHexTopDownPawn::SetInputMode12Pressed);
  IC->BindAction("Kind13", IE_Pressed, this, &AHexTopDownPawn::SetInputMode13);
  // Type cycling (works for both Input and Output based on current placement
  // mode)
  IC->BindAction("CycleNextType", IE_Pressed, this,
                 &AHexTopDownPawn::CycleInputTypeNext);
  IC->BindAction("CyclePreviousType", IE_Pressed, this,
                 &AHexTopDownPawn::CycleInputTypePrevious);

  // Glyph cycling (Belt, Bond, Unbond)
  IC->BindAction("CycleGlyphType", IE_Pressed, this,
                 &AHexTopDownPawn::CycleGlyphType);

  // Selection modifier key (Left Alt)
  IC->BindAction("TypeSelectionActive", IE_Pressed, this,
                 &AHexTopDownPawn::SelectionActivePressed);
  IC->BindAction("TypeSelectionActive", IE_Released, this,
                 &AHexTopDownPawn::SelectionActiveReleased);

  // Simulation control
  IC->BindAction("NextTick", IE_Pressed, this, &AHexTopDownPawn::NextTick);
  IC->BindAction("IncreaseTickSpeed", IE_Pressed, this,
                 &AHexTopDownPawn::IncreaseTickSpeed);
  IC->BindAction("DecreaseTickSpeed", IE_Pressed, this,
                 &AHexTopDownPawn::DecreaseTickSpeed);

  // Timeline control
  IC->BindAction("ToggleArmTimelines", IE_Pressed, this,
                 &AHexTopDownPawn::ToggleArmTimelines);

  // Timeline editing - cursor control
  IC->BindAction("NextCommand", IE_Pressed, this,
                 &AHexTopDownPawn::NextCommand);
  IC->BindAction("PrevCommand", IE_Pressed, this,
                 &AHexTopDownPawn::PrevCommand);
  IC->BindAction("ContinuePlacement", IE_Pressed, this,
                 &AHexTopDownPawn::ContinuePlacementActivePressed);
  IC->BindAction("ContinuePlacement", IE_Released, this,
                 &AHexTopDownPawn::ContinuePlacementActiveReleased);
  // Blueprint library - save selection as blueprint
  IC->BindAction("SaveSelectionAsBlueprint", IE_Pressed, this,
                 &AHexTopDownPawn::SaveSelectionAsBlueprint);

  // Grid visibility toggle
  IC->BindAction("ToggleGridVisibility", IE_Pressed, this,
                 &AHexTopDownPawn::ToggleGridVisibility);

  // View mode cycling
  IC->BindAction("NextViewMode", IE_Pressed, this,
                 &AHexTopDownPawn::NextViewMode);

  // Switch active grid level
  IC->BindAction("SwitchGrid", IE_Pressed, this, &AHexTopDownPawn::SwitchGrid);

  // Walker debug visualization toggle
  IC->BindAction("ToggleDebugWalkers", IE_Pressed, this,
                 &AHexTopDownPawn::ToggleDebugWalkers);
}

void AHexTopDownPawn::SetGlobalHighlightForKind(EPlaceKind Kind, bool bEnable) {
  if (!GetWorld())
    return;

  // 1. Determine OrbType if applicable
  EOrbType CurrentOrbType = EOrbType::None;
  if (Placer) {
    if (Kind == EPlaceKind::Input || Kind == EPlaceKind::ClusterInput) {
      CurrentOrbType = Placer->GetCurrentInputOrbType();
    } else if (Kind == EPlaceKind::Output ||
               Kind == EPlaceKind::ClusterOutput ||
               Kind == EPlaceKind::ClusterSend) {
      CurrentOrbType = Placer->GetCurrentOutputOrbType();
    } else if (Kind == EPlaceKind::TriggerTile) {
      CurrentOrbType = Placer->GetCurrentTriggerOrbType();
    }
  }

  // 1. Handle HexFactory (ISM/Procedural Mesh based)
  if (!CachedHexFactory) {
    for (TActorIterator<AHexFactoryActor> It(GetWorld()); It; ++It) {
      CachedHexFactory = *It;
      break;
    }
  }

  if (CachedHexFactory) {
    // Forward to the new global highlight system in HexFactoryActor
    CachedHexFactory->UpdateGlobalHighlight(Kind, bEnable, CurrentLevel,
                                            CurrentOrbType);
  }

  // 2. Handle Actors
  if (bEnable) {
    GlobalHighlightedActors.Empty();
    uint32 Stencil = 1; // Match factory stencil

    auto HighlightActors = [&](UClass *Class) {
      for (TActorIterator<AActor> It(GetWorld(), Class); It; ++It) {
        AActor *A = *It;
        if (IsValid(A)) {
          // Filter by Level
          int32 ActorLevel = 0;
          if (ABasicArm *Arm = Cast<ABasicArm>(A))
            ActorLevel = Arm->Level;
          else if (ABasicWalker *Walker = Cast<ABasicWalker>(A))
            ActorLevel = Walker->Level;
          else if (ARequestBeacon *Beacon = Cast<ARequestBeacon>(A))
            ActorLevel = Beacon->Level;
          else if (ATriggerTile *Trigger = Cast<ATriggerTile>(A)) {
            ActorLevel = Trigger->Level;
            // Also filter by TriggerOrbType
            if (Trigger->TriggerOrbType != CurrentOrbType)
              continue;
          }

          if (ActorLevel != CurrentLevel)
            continue;

          TArray<UPrimitiveComponent *> Comps;
          A->GetComponents(Comps);
          for (UPrimitiveComponent *C : Comps) {
            C->SetRenderCustomDepth(true);
            C->SetCustomDepthStencilValue(Stencil);
          }
          GlobalHighlightedActors.Add(A);
        }
      }
    };

    switch (Kind) {
    case EPlaceKind::Arm:
      HighlightActors(ABasicArm::StaticClass());
      break;
    case EPlaceKind::Walker:
      HighlightActors(ABasicWalker::StaticClass());
      break;
    case EPlaceKind::RequestBeacon:
      HighlightActors(ARequestBeacon::StaticClass());
      break;
    case EPlaceKind::TriggerTile:
      HighlightActors(ATriggerTile::StaticClass());
      break;
    default:
      break;
    }
  } else {
    for (auto &WeakA : GlobalHighlightedActors) {
      if (AActor *A = WeakA.Get()) {
        TArray<UPrimitiveComponent *> Comps;
        A->GetComponents(Comps);
        for (UPrimitiveComponent *C : Comps) {
          C->SetRenderCustomDepth(false);
        }
      }
    }
    GlobalHighlightedActors.Empty();
  }
}

void AHexTopDownPawn::SetInputMode1() {
  if (Placer)
    Placer->SetPlaceKind(EPlaceKind::Input);
}
void AHexTopDownPawn::SetInputMode2() {
  if (Placer)
    Placer->SetPlaceKind(EPlaceKind::Output);
}
void AHexTopDownPawn::SetInputMode3() {
  if (Placer)
    Placer->SetPlaceKind(EPlaceKind::Belt);
}
void AHexTopDownPawn::SetInputMode4() {
  if (Placer) {
    // Cycle between Arm and Walker
    if (Placer->PlaceKind == EPlaceKind::Arm) {
      Placer->SetPlaceKind(EPlaceKind::Walker);
    } else {
      Placer->SetPlaceKind(EPlaceKind::Arm);
    }
  }
}
void AHexTopDownPawn::SetInputMode5() {
  if (Placer)
    Placer->SetPlaceKind(EPlaceKind::Bond);
}
void AHexTopDownPawn::SetInputMode6() {
  if (Placer)
    Placer->SetPlaceKind(EPlaceKind::Unbond);
}
void AHexTopDownPawn::SetInputMode7() {
  if (Placer)
    Placer->SetPlaceKind(EPlaceKind::ClusterOutput);
}
void AHexTopDownPawn::SetInputMode9() {
  if (Placer)
    Placer->SetPlaceKind(EPlaceKind::RequestBeacon);
}
void AHexTopDownPawn::SetInputMode10() {
  if (Placer) {
    if (Placer->PlaceKind == EPlaceKind::TriggerTile) {
      Placer->CycleTriggerVariant();
    } else {
      Placer->SetPlaceKind(EPlaceKind::TriggerTile);
    }
  }
}
void AHexTopDownPawn::SetInputMode11() {
  if (Placer)
    Placer->SetPlaceKind(EPlaceKind::ClusterSend);
}

void AHexTopDownPawn::SetInputMode13() {
  if (Placer)
    Placer->SetPlaceKind(EPlaceKind::HelixOutput);
}

void AHexTopDownPawn::SwitchGrid() {
  CurrentLevel = 1 - CurrentLevel;

  if (Placer) {
    Placer->ActiveLevel = CurrentLevel;
  }

  // Toggle display flip on OrbSimDriver - this swaps the Z positions of Level 0
  // and Level 1 objects without changing the camera position. The simulation
  // levels remain unchanged.
  AOrbSimDriver *Driver = CachedOrbSimDriver;
  if (!Driver && GetWorld()) {
    Driver = AOrbSimDriver::Find(GetWorld());
    CachedOrbSimDriver = Driver;
  }

  if (Driver) {
    Driver->SetDisplayFlipped(CurrentLevel == 1);
  }

  // Tell the factory actor to rebuild everything to reflect the new display
  // positions
  if (UWorld *World = GetWorld()) {
    for (TActorIterator<AHexFactoryActor> It(World); It; ++It) {
      if (AHexFactoryActor *Factory = *It) {
        Factory->Rebuild();
      }
    }

    // Sync locations for actors that don't depend solely on OrbRenderer
    // BasicArm and BasicWalker update themselves in Tick using MapHexToWorld,
    // so they will move automatically on the next frame.
    // TriggerTiles and Beacons might need an explicit update if they don't move
    // in Tick.
    for (TActorIterator<ATriggerTile> It(World); It; ++It) {
      It->SyncWorldLocation();
    }
    for (TActorIterator<ARequestBeacon> It(World); It; ++It) {
      It->SyncWorldLocation();
    }

    // Rebuild all orb renderers to pick up new world positions
    for (TActorIterator<AActor> It(World); It; ++It) {
      if (UOrbRenderer *OrbRenderer =
              It->FindComponentByClass<UOrbRenderer>()) {
        OrbRenderer->RebuildCurrPos();
      }
    }
  }

  UE_LOG(LogTemp, Log,
         TEXT("AHexTopDownPawn::SwitchGrid - Switched to Level %d "
              "(DisplayFlipped: %s)"),
         CurrentLevel,
         Driver && Driver->IsDisplayFlipped() ? TEXT("true") : TEXT("false"));

  // Refresh global highlight for the new grid
  if (LastGlobalHighlightKind != (EPlaceKind)255) {
    SetGlobalHighlightForKind(LastGlobalHighlightKind, true);
  }
}

void AHexTopDownPawn::RequestMoveToTile() {
  if (!Placer || !Placer->IsHoverValid()) {
    return;
  }

  FHex TargetHex = Placer->GetHoverHex();

  // Get simulation
  UWorld *W = GetWorld();
  if (!W) {
    return;
  }

  AOrbSimDriver *Driver = CachedOrbSimDriver;
  if (!Driver) {
    for (TActorIterator<AOrbSimDriver> It(W); It; ++It) {
      Driver = *It;
      CachedOrbSimDriver = Driver;
      break;
    }
  }

  if (!Driver || !Driver->GetSim()) {
    UE_LOG(LogTemp, Warning,
           TEXT("HexTopDownPawn: No simulation found for walker request."));
    return;
  }

  USimWorldHex *Sim = Driver->GetSim();

  // Find nearest available walker
  int32 WalkerIndex = Sim->FindNearestAvailableWalker(TargetHex);
  if (WalkerIndex == INDEX_NONE) {
    UE_LOG(LogTemp, Warning,
           TEXT("HexTopDownPawn: No walkers found for move request."));
    return;
  }

  // Issue move request
  if (Sim->RequestWalkerMoveTo(WalkerIndex, TargetHex, 0)) {
    UE_LOG(LogTemp, Log,
           TEXT("HexTopDownPawn: Requested walker %d to move to (%d, %d)"),
           WalkerIndex, TargetHex.Q, TargetHex.R);
  } else {
    UE_LOG(LogTemp, Warning,
           TEXT("HexTopDownPawn: Failed to request walker move."));
  }
}

void AHexTopDownPawn::SetInputMode8() {
  // Find GameManager to get DifficultyValue
  AGameManager *GameManager = CachedGameManager;
  if (!GameManager && GetWorld()) {
    for (TActorIterator<AGameManager> It(GetWorld()); It; ++It) {
      GameManager = *It;
      CachedGameManager = GameManager;
      break;
    }
  }

  if (!GameManager) {
    UE_LOG(LogTemp, Warning, TEXT("SetInputMode8: No GameManager found"));
    if (GEngine) {
      GEngine->AddOnScreenDebugMessage(-1, 3.0f, FColor::Red,
                                       TEXT("No GameManager found"));
    }
    return;
  }

  // Get the simulation to access templates
  AOrbSimDriver *Driver = CachedOrbSimDriver;
  if (!Driver) {
    Driver = AOrbSimDriver::Find(GetWorld());
    CachedOrbSimDriver = Driver;
  }
  if (!Driver) {
    UE_LOG(LogTemp, Warning, TEXT("SetInputMode8: No OrbSimDriver found"));
    return;
  }

  USimWorldHex *Sim = Driver->GetSim();
  if (!Sim) {
    UE_LOG(LogTemp, Warning, TEXT("SetInputMode8: No Sim found"));
    return;
  }

  // Generate a random cluster based on difficulty
  int32 TemplateId = GameManager->GenerateRandomCluster();

  if (TemplateId < 0) {
    UE_LOG(LogTemp, Warning, TEXT("SetInputMode8: Failed to generate cluster"));
    if (GEngine) {
      GEngine->AddOnScreenDebugMessage(-1, 3.0f, FColor::Red,
                                       TEXT("Failed to generate cluster"));
    }
    return;
  }

  UE_LOG(LogTemp, Log,
         TEXT("SetInputMode8: Generated cluster template with ID %d"),
         TemplateId);

  // Get the template to convert it into a blueprint
  const USimWorldHex::FClusterTemplate *Template =
      Sim->GetClusterTemplates().Find((uint8)TemplateId);
  if (!Template) {
    UE_LOG(LogTemp, Warning, TEXT("SetInputMode8: Template %d not found"),
           TemplateId);
    if (GEngine) {
      GEngine->AddOnScreenDebugMessage(-1, 3.0f, FColor::Red,
                                       TEXT("Template not found"));
    }
    return;
  }

  // Clear any existing blueprint
  ClearSelection();

  // Convert template into blueprint tiles (ClusterOutput tiles)
  for (const FOrbData &Orb : Template->Orbs) {
    // Create a FBlueprintTile for each orb in the cluster
    FBlueprintTile BlueprintTile;
    BlueprintTile.LocalHex = FHex(Orb.dQ, Orb.dR);
    BlueprintTile.TileType = ETileKind::ClusterOutput;
    BlueprintTile.TileDirection =
        (uint8)Orb.Type; // Store OrbType (needed for rendering)
    BlueprintTiles.Add(BlueprintTile);
  }

  ActiveGenerationMode = 8;

  // Enter blueprint placement mode
  bBlueprintPlacementMode = true;
  bBlueprintMode = false;
  BlueprintRotation = 0;
  BlueprintPlacementOffset = FHex(0, 0);

  // Disable HexPlacementComponent preview when in blueprint mode
  if (Placer) {
    Placer->CancelPlacement();
  }

  // Ensure preview components are created
  EnsureBlueprintPreviewComponents();

  UE_LOG(LogTemp, Log,
         TEXT("SetInputMode8: Blueprint mode active with %d tiles"),
         BlueprintTiles.Num());
}

void AHexTopDownPawn::SetInputMode12(bool bForceRegenerate) {
  // Find GameManager to get Difficulty/Ancestral logic
  AGameManager *GameManager = CachedGameManager;
  if (!GameManager && GetWorld()) {
    for (TActorIterator<AGameManager> It(GetWorld()); It; ++It) {
      GameManager = *It;
      CachedGameManager = GameManager;
      break;
    }
  }

  if (!GameManager) {
    UE_LOG(LogTemp, Warning, TEXT("SetInputMode12: No GameManager found"));
    return;
  }

  // Get the simulation to access templates
  AOrbSimDriver *Driver = CachedOrbSimDriver;
  if (!Driver) {
    Driver = AOrbSimDriver::Find(GetWorld());
    CachedOrbSimDriver = Driver;
  }
  if (!Driver)
    return;

  USimWorldHex *Sim = Driver->GetSim();
  if (!Sim)
    return;

  int32 TemplateId = -1;

  // If forcing regeneration (e.g., difficulty change), generate a related
  // cluster
  if (bForceRegenerate && GameManager->LastAncestralTemplateId != -1) {
    // Generate a related cluster (variation of the previous one)
    TemplateId = GameManager->GenerateRelatedCluster(
        (uint8)GameManager->LastAncestralTemplateId, Sim);
    if (TemplateId >= 0) {
      UE_LOG(
          LogTemp, Log,
          TEXT("SetInputMode12: Generated related cluster template with ID %d"),
          TemplateId);
    } else {
      // Generation failed - keep the existing template
      UE_LOG(LogTemp, Warning,
             TEXT("SetInputMode12: GenerateRelatedCluster failed, keeping "
                  "existing template %d"),
             GameManager->LastAncestralTemplateId);
      TemplateId = GameManager->LastAncestralTemplateId;
    }
  }

  // Check for toggle: If already Kind12 active and in placement mode (and not
  // forcing regeneration)
  if (ActiveGenerationMode == 12 && bBlueprintPlacementMode &&
      !bForceRegenerate) {
    // Toggle between ClusterOutput and DiscoveryClusterInput
    bAncestralInputMode = !bAncestralInputMode;
    UE_LOG(LogTemp, Log,
           TEXT("SetInputMode12: Toggled Kind12 mode. AncestralInputMode: %s"),
           bAncestralInputMode ? TEXT("ON (Inputs)") : TEXT("OFF (Outputs)"));

    // Reuse the last generated template if available from GameManager
    if (GameManager->LastAncestralTemplateId != -1) {
      TemplateId = GameManager->LastAncestralTemplateId;
    }
  }

  if (TemplateId == -1) {
    // Not toggling or no valid previous template - generate new
    // Reset input mode to default (Output) only if not already in mode 12
    if (ActiveGenerationMode != 12) {
      bAncestralInputMode = false;
    }

    // Generate ancestral cluster (GameManager updates its
    // LastAncestralTemplateId internally)
    TemplateId = GameManager->GenerateAncestralCluster(Sim);

    if (TemplateId < 0) {
      UE_LOG(LogTemp, Warning,
             TEXT("SetInputMode12: GenerateAncestralCluster failed, trying "
                  "GenerateRandomCluster"));
      // Last resort - generate a simple random cluster
      TemplateId = GameManager->GenerateRandomCluster(Sim);
      if (TemplateId < 0) {
        UE_LOG(LogTemp, Warning,
               TEXT("SetInputMode12: All generation methods failed"));
        if (GEngine) {
          GEngine->AddOnScreenDebugMessage(-1, 3.0f, FColor::Red,
                                           TEXT("Failed to generate cluster"));
        }
        return;
      }
      // Update LastAncestralTemplateId to use this new one
      GameManager->LastAncestralTemplateId = TemplateId;
    }

    UE_LOG(LogTemp, Log,
           TEXT("SetInputMode12: Generated ancestral cluster template with ID "
                "%d"),
           TemplateId);
  }

  const USimWorldHex::FClusterTemplate *Template =
      Sim->GetClusterTemplates().Find((uint8)TemplateId);
  if (!Template)
    return;

  // Clear any existing blueprint
  ClearSelection();

  // Convert template into blueprint tiles
  for (const FOrbData &Orb : Template->Orbs) {
    FBlueprintTile BlueprintTile;
    BlueprintTile.LocalHex = FHex(Orb.dQ, Orb.dR);

    if (bAncestralInputMode) {
      BlueprintTile.TileType =
          ETileKind::DiscoveryClusterInput; // Use DiscoveryClusterInput
    } else {
      BlueprintTile.TileType =
          ETileKind::ClusterOutput; // Use regular ClusterOutput
    }

    BlueprintTile.TileDirection =
        (uint8)Orb.Type; // Store OrbType for rendering
    BlueprintTiles.Add(BlueprintTile);
  }

  ActiveGenerationMode = 12;

  // Enter blueprint placement mode
  bBlueprintPlacementMode = true;
  bBlueprintMode = false;
  BlueprintRotation = 0;
  BlueprintPlacementOffset = FHex(0, 0);

  if (Placer)
    Placer->CancelPlacement();

  // Reset smoothing to ensure the structure updates visually immediately
  bBlueprintSmoothingInitialized = false;

  EnsureBlueprintPreviewComponents();
}

void AHexTopDownPawn::SelectionActivePressed() { bTypeSelectionActive = true; }
void AHexTopDownPawn::SelectionActiveReleased() {
  bTypeSelectionActive = false;
}
void AHexTopDownPawn::ContinuePlacementActivePressed() {
  bContinuePlacementActive = true;
}

void AHexTopDownPawn::ContinuePlacementActiveReleased() {
  bContinuePlacementActive = false;
}
void AHexTopDownPawn::CycleInputTypeNext() {
  // Check if we're in blueprint placement mode with TypeSelectionActive
  if (bBlueprintPlacementMode && bTypeSelectionActive &&
      BlueprintTiles.Num() > 0) {
    // Find GameManager
    AGameManager *GameManager = nullptr;
    if (UWorld *World = GetWorld()) {
      for (TActorIterator<AGameManager> It(World); It; ++It) {
        GameManager = *It;
        break;
      }
    }

    if (GameManager) {
      if (ActiveGenerationMode == 12) {
        // Ancestral Mode: Increase DifficultyValue (Incremental evolution like
        // Kind 8)
        GameManager->DifficultyValue =
            FMath::Min(GameManager->DifficultyValue + 1, 1000);
        UE_LOG(LogTemp, Log, TEXT("Ancestral difficulty increased to %d"),
               GameManager->DifficultyValue);
        SetInputMode12(
            true); // Force regeneration without toggling input/output
      } else if (ActiveGenerationMode ==
                 8) // Default to 8 logic if not 12, or specifically 8
      {
        // Random Mode: Increase difficulty (Raw value)
        GameManager->DifficultyValue =
            FMath::Min(GameManager->DifficultyValue + 1, 100);
        UE_LOG(LogTemp, Log, TEXT("Kind8 difficulty increased to %d"),
               GameManager->DifficultyValue);
        SetInputMode8();
      }
    }
  } else if (Placer) {
    Placer->CycleNextType();
  }
}

void AHexTopDownPawn::CycleInputTypePrevious() {
  // Check if we're in blueprint placement mode with TypeSelectionActive
  if (bBlueprintPlacementMode && bTypeSelectionActive &&
      BlueprintTiles.Num() > 0) {
    // Find GameManager
    AGameManager *GameManager = nullptr;
    if (UWorld *World = GetWorld()) {
      for (TActorIterator<AGameManager> It(World); It; ++It) {
        GameManager = *It;
        break;
      }
    }

    if (GameManager) {
      if (ActiveGenerationMode == 12) {
        // Ancestral Mode: Decrease DifficultyValue (Incremental evolution like
        // Kind 8)
        GameManager->DifficultyValue =
            FMath::Max(GameManager->DifficultyValue - 1, 4);
        UE_LOG(LogTemp, Log, TEXT("Ancestral difficulty decreased to %d"),
               GameManager->DifficultyValue);
        SetInputMode12(
            true); // Force regeneration without toggling input/output
      } else if (ActiveGenerationMode == 8) {
        // Random Mode: Decrease difficulty (Raw value)
        GameManager->DifficultyValue =
            FMath::Max(GameManager->DifficultyValue - 1, 4);
        UE_LOG(LogTemp, Log, TEXT("Kind8 difficulty decreased to %d"),
               GameManager->DifficultyValue);
        SetInputMode8();
      }
    }
  } else if (Placer) {
    Placer->CyclePreviousType();
  }
}

void AHexTopDownPawn::NextTick() {
  // Find the GameTickManager
  AGameTickManager *TickManager = AGameTickManager::Get(this);
  if (!TickManager) {
    UE_LOG(LogTemp, Warning, TEXT("NextTick: No GameTickManager found"));
    return;
  }

  // Set flag to pause on the next GTM tick
  bPauseOnNextTick = true;

  // If paused, start playing so we can reach the next tick
  if (!TickManager->IsPlaying()) {
    TickManager->Play();
    UE_LOG(LogTemp, Log, TEXT("NextTick: Playing until next tick"));
  } else {
    // Already playing, will pause on next tick
    UE_LOG(LogTemp, Log, TEXT("NextTick: Will pause on next tick"));
  }
}

void AHexTopDownPawn::IncreaseTickSpeed() {
  AGameTickManager *TickManager = AGameTickManager::Get(this);
  if (!TickManager) {
    UE_LOG(LogTemp, Warning,
           TEXT("IncreaseTickSpeed: No GameTickManager found"));
    return;
  }

  TickManager->TicksPerSecond =
      FMath::Clamp(TickManager->TicksPerSecond + 1.f, 0.01f, 240.0f);
  UE_LOG(LogTemp, Log, TEXT("Tick speed increased to %.2f ticks/second"),
         TickManager->TicksPerSecond);
}

void AHexTopDownPawn::DecreaseTickSpeed() {
  AGameTickManager *TickManager = AGameTickManager::Get(this);
  if (!TickManager) {
    UE_LOG(LogTemp, Warning,
           TEXT("DecreaseTickSpeed: No GameTickManager found"));
    return;
  }

  TickManager->TicksPerSecond =
      FMath::Clamp(TickManager->TicksPerSecond - 1.f, 0.01f, 240.0f);
  UE_LOG(LogTemp, Log, TEXT("Tick speed decreased to %.2f ticks/second"),
         TickManager->TicksPerSecond);
}

void AHexTopDownPawn::HandleNextTickStop(float StepSeconds) {
  if (bPauseOnNextTick) {
    AGameTickManager *TickManager = AGameTickManager::Get(this);
    if (TickManager) {
      TickManager->Pause();
      UE_LOG(LogTemp, Log, TEXT("NextTick: Paused at tick boundary"));
    }
    bPauseOnNextTick = false;
  }
}

void AHexTopDownPawn::ToggleArmTimelines() {
  // Find the GameManager
  AGameManager *GameManager = CachedGameManager;
  if (!GameManager && GetWorld()) {
    for (TActorIterator<AGameManager> It(GetWorld()); It; ++It) {
      GameManager = *It;
      CachedGameManager = GameManager;
      break;
    }
  }

  if (GameManager) {
    // Toggle the timeline visibility
    bool bNewVisibility = !GameManager->bShowArmTimelines;
    GameManager->SetTimelineVisibility(bNewVisibility);
    // Editor UI no longer auto-toggles here
  } else {
    UE_LOG(LogTemp, Warning, TEXT("ToggleArmTimelines: No GameManager found"));
  }
}

void AHexTopDownPawn::NextCommand() {
  // Find the GameManager and get the active timeline
  AGameManager *GameManager = CachedGameManager;
  if (!GameManager && GetWorld()) {
    for (TActorIterator<AGameManager> It(GetWorld()); It; ++It) {
      GameManager = *It;
      CachedGameManager = GameManager;
      break;
    }
  }

  if (GameManager) {
    UArmTimelineWidget *ActiveTimeline = GameManager->GetActiveTimeline();
    if (ActiveTimeline) {
      UUserWidget *WidgetInstance = ActiveTimeline->GetTimelineWidgetInstance();
      if (WidgetInstance) {
        UArmTimelineUMGWidget *UMGWidget =
            Cast<UArmTimelineUMGWidget>(WidgetInstance);
        if (UMGWidget) {
          UE_LOG(LogTemp, Log, TEXT("NextCommand: Moving cursor right"));
          UMGWidget->MoveCursorRight();
        }
      }
    }
  }
}

void AHexTopDownPawn::PrevCommand() {
  // Find the GameManager and get the active timeline
  AGameManager *GameManager = CachedGameManager;
  if (!GameManager && GetWorld()) {
    for (TActorIterator<AGameManager> It(GetWorld()); It; ++It) {
      GameManager = *It;
      CachedGameManager = GameManager;
      break;
    }
  }

  if (GameManager) {
    UArmTimelineWidget *ActiveTimeline = GameManager->GetActiveTimeline();
    if (ActiveTimeline) {
      UUserWidget *WidgetInstance = ActiveTimeline->GetTimelineWidgetInstance();
      if (WidgetInstance) {
        UArmTimelineUMGWidget *UMGWidget =
            Cast<UArmTimelineUMGWidget>(WidgetInstance);
        if (UMGWidget) {
          UMGWidget->MoveCursorLeft();
        }
      }
    }
  }
}

ABasicArm *AHexTopDownPawn::FindArmAtHex(const FHex &Hex) {
  if (UWorld *World = GetWorld()) {
    for (TActorIterator<ABasicArm> It(World); It; ++It) {
      ABasicArm *Arm = *It;
      if (Arm && Arm->Q == Hex.Q && Arm->R == Hex.R &&
          Arm->Level == Hex.Level) {
        return Arm;
      }
    }
  }
  return nullptr;
}

void AHexTopDownPawn::ShowTimelineEditor(bool bShow) {
  if (!GetWorld()) {
    return;
  }

  APlayerController *PC = GetWorld()->GetFirstPlayerController();
  if (!PC) {
    return;
  }

  if (bShow) {
    // Remove existing editor if it exists
    if (TimelineEditorWidget) {
      TimelineEditorWidget->RemoveFromParent();
      TimelineEditorWidget = nullptr;
    }

    // Create new editor widget
    if (TimelineEditorWidgetClass) {
      TimelineEditorWidget = CreateWidget<UArmTimelineEditorWidget>(
          GetWorld(), TimelineEditorWidgetClass);
      if (TimelineEditorWidget) {
        TimelineEditorWidget->AddToViewport();

        // Use GameAndUI mode to allow both game and widget interaction
        // Explicitly set parameters to prevent the UI from 'stealing' the mouse
        // click
        FInputModeGameAndUI InputMode;
        InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
        InputMode.SetHideCursorDuringCapture(false);
        // Setting WidgetToFocus to nullptr ensures the game still receives
        // input
        InputMode.SetWidgetToFocus(nullptr);
        PC->SetInputMode(InputMode);
        PC->bShowMouseCursor = true;
      }
    }
  } else {
    // Remove the editor widget from viewport
    if (TimelineEditorWidget) {
      TimelineEditorWidget->RemoveFromParent();
      TimelineEditorWidget = nullptr;

      // When closing, ensure we return to a clean state.
      // Keeping GameAndUI is okay if other widgets are active, but let's be
      // explicit.
      FInputModeGameAndUI InputMode;
      InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
      InputMode.SetHideCursorDuringCapture(false);
      PC->SetInputMode(InputMode);
      PC->bShowMouseCursor = true;
    }
  }
}

void AHexTopDownPawn::SetActiveTimelineForEditing(ABasicArm *Arm) {
  if (!Arm || !Arm->TimelineWidget) {
    return;
  }

  // Find the GameManager and set the active timeline
  AGameManager *GameManager = CachedGameManager;
  if (!GameManager && GetWorld()) {
    for (TActorIterator<AGameManager> It(GetWorld()); It; ++It) {
      GameManager = *It;
      CachedGameManager = GameManager;
      break;
    }
  }

  if (GameManager) {
    // Set this arm's timeline as the active one
    GameManager->SetActiveTimeline(Arm->TimelineWidget);
  }
}

void AHexTopDownPawn::UpdateTimelineEditorVisibility() {
  // No-op: editor UI is toggled explicitly via EditorUIToggle action
}

void AHexTopDownPawn::ToggleArmTimeline(ABasicArm *Arm) {
  if (Arm && Arm->TimelineWidget) {
    // Toggle the timeline visibility using the public method
    bool bCurrentVisibility = Arm->TimelineWidget->bTimelineVisible;
    Arm->TimelineWidget->SetTimelineWidgetVisible(!bCurrentVisibility);

    // If making timeline visible, set it as the active timeline (editor UI is
    // independent)
    if (!bCurrentVisibility) {
      SetActiveTimelineForEditing(Arm);

      // Enable editing mode on the UMG widget when timeline becomes visible
      if (UUserWidget *WidgetInstance =
              Arm->TimelineWidget->GetTimelineWidgetInstance()) {
        if (UArmTimelineUMGWidget *UMGWidget =
                Cast<UArmTimelineUMGWidget>(WidgetInstance)) {
          UMGWidget->EnableEditingMode();
        }
      }
    } else {
      // Making timeline invisible - clear the active timeline and update editor
      // visibility
      AGameManager *GameManager = nullptr;
      if (UWorld *World = GetWorld()) {
        for (TActorIterator<AGameManager> It(World); It; ++It) {
          GameManager = *It;
          break;
        }
      }

      if (GameManager) {
        // Clear the active timeline if it's the one being hidden
        UArmTimelineWidget *ActiveTimeline = GameManager->GetActiveTimeline();
        if (ActiveTimeline == Arm->TimelineWidget) {
          GameManager->SetActiveTimeline(nullptr);
        }
      }

      // Disable editing mode on the UMG widget when timeline becomes hidden
      if (UUserWidget *WidgetInstance =
              Arm->TimelineWidget->GetTimelineWidgetInstance()) {
        if (UArmTimelineUMGWidget *UMGWidget =
                Cast<UArmTimelineUMGWidget>(WidgetInstance)) {
          UMGWidget->DisableEditingMode();
        }
      }

      // Editor UI visibility is independent; do nothing here
    }
  }
}

void AHexTopDownPawn::StartSelection() {
  UE_LOG(LogTemp, Warning, TEXT("StartSelection called, blueprint mode: %s"),
         bBlueprintMode ? TEXT("ON") : TEXT("OFF"));

  if (!bBlueprintMode)
    return;

  APlayerController *PC =
      GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
  if (!PC)
    return;

  FVector2D MousePosition;
  if (PC->GetMousePosition(MousePosition.X, MousePosition.Y)) {
    ClearSelection();
    SelectionStart = MousePosition;
    SelectionEnd = MousePosition;
    bBlueprintSelectionActive = true;

    // Start HUD selection box
    AAutomationGameHUD *GameHUD = Cast<AAutomationGameHUD>(PC->GetHUD());
    if (GameHUD) {
      GameHUD->StartSelectionBox(MousePosition);
    }

    UE_LOG(LogTemp, Warning, TEXT("Selection started at (%f, %f)"),
           MousePosition.X, MousePosition.Y);
  } else {
    // UE_LOG(LogTemp, Warning, TEXT("Failed to get mouse position for
    // selection"));
  }
}

void AHexTopDownPawn::EndSelection() {
  UE_LOG(LogTemp, Warning,
         TEXT("EndSelection called, blueprint mode: %s, selection active: %s"),
         bBlueprintMode ? TEXT("ON") : TEXT("OFF"),
         bBlueprintSelectionActive ? TEXT("YES") : TEXT("NO"));

  if (!bBlueprintMode || !bBlueprintSelectionActive)
    return;

  bBlueprintSelectionActive = false;

  // End HUD selection box
  APlayerController *PC =
      GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
  if (PC) {
    AAutomationGameHUD *GameHUD = Cast<AAutomationGameHUD>(PC->GetHUD());
    if (GameHUD) {
      GameHUD->EndSelectionBox();
    }
  }

  CreateBlueprint();
}

void AHexTopDownPawn::UpdateSelection() {
  if (!bBlueprintSelectionActive || !GetWorld())
    return;

  FVector2D MousePosition;
  if (GetWorld()->GetFirstPlayerController() &&
      GetWorld()->GetFirstPlayerController()->GetMousePosition(
          MousePosition.X, MousePosition.Y)) {
    SelectionEnd = MousePosition;

    // Update selected tiles based on current selection box
    SelectedTiles.Empty();

    // Get tile size and grid info from driver
    AOrbSimDriver *Driver = CachedOrbSimDriver;
    if (!Driver) {
      Driver = AOrbSimDriver::Find(GetWorld());
      CachedOrbSimDriver = Driver;
    }
    float TileSize = Driver ? Driver->GetTileSize() : 100.f;
    EHexOrientation Orientation =
        Driver ? Driver->GetGridOrientation() : EHexOrientation::PointyTop;

    // Updated: GridPlaneZ now accurately reflects the active level's visual Z
    float GridPlaneZ = Driver ? Driver->GetGridPlaneZ(this) : 0.f;

    // Convert screen coordinates to world coordinates by projecting rays to the
    // grid plane
    FVector WorldStart, WorldEnd;
    FVector WorldStartDir, WorldEndDir;

    // Deproject both screen points to get rays
    GetWorld()->GetFirstPlayerController()->DeprojectScreenPositionToWorld(
        SelectionStart.X, SelectionStart.Y, WorldStart, WorldStartDir);
    GetWorld()->GetFirstPlayerController()->DeprojectScreenPositionToWorld(
        SelectionEnd.X, SelectionEnd.Y, WorldEnd, WorldEndDir);

    // Project the rays down to the grid plane (Z = GridPlaneZ)
    if (FMath::Abs(WorldStartDir.Z) > KINDA_SMALL_NUMBER) {
      float T = (GridPlaneZ - WorldStart.Z) / WorldStartDir.Z;
      WorldStart = WorldStart + WorldStartDir * T;
    }
    if (FMath::Abs(WorldEndDir.Z) > KINDA_SMALL_NUMBER) {
      float T = (GridPlaneZ - WorldEnd.Z) / WorldEndDir.Z;
      WorldEnd = WorldEnd + WorldEndDir * T;
    }

    // Convert world coordinates to hex coordinates
    FHexAxial HexStartAxial = HexCoord::XYToAxialRound(
        FVector2D(WorldStart.X, WorldStart.Y), TileSize, Orientation);
    FHexAxial HexEndAxial = HexCoord::XYToAxialRound(
        FVector2D(WorldEnd.X, WorldEnd.Y), TileSize, Orientation);
    FHex HexStart(HexStartAxial.Q, HexStartAxial.R, CurrentLevel);
    FHex HexEnd(HexEndAxial.Q, HexEndAxial.R, CurrentLevel);

    // Debug: Log the selection area with grid plane info
    UE_LOG(LogTemp, Warning,
           TEXT("Selection area: Screen(%f,%f) to (%f,%f) -> World(%f,%f,%.1f) "
                "to (%f,%f,%.1f) -> Hex(%d,%d,%d) to (%d,%d,%d) [GridZ=%.1f]"),
           SelectionStart.X, SelectionStart.Y, SelectionEnd.X, SelectionEnd.Y,
           WorldStart.X, WorldStart.Y, WorldStart.Z, WorldEnd.X, WorldEnd.Y,
           WorldEnd.Z, HexStart.Q, HexStart.R, HexStart.Level, HexEnd.Q,
           HexEnd.R, HexEnd.Level, GridPlaneZ);

    // Use improved hexagonal selection algorithm with actual world positions
    GetHexesInSelectionArea(HexStart, HexEnd, WorldStart, WorldEnd,
                            CurrentLevel, SelectedTiles);

    UE_LOG(LogTemp, Warning, TEXT("Blueprint selection: %d tiles selected"),
           SelectedTiles.Num());

    // Update visual highlights
    UpdateSelectionHighlights();
  }
}

void AHexTopDownPawn::BlueprintModePressed() {
  UE_LOG(LogTemp, Warning, TEXT("BlueprintMode pressed"));
  if (bBlueprintPlacementMode) {
    // If we're in placement mode, toggle to exit
    bBlueprintPlacementMode = false;
    bBlueprintMode = false;
    ClearSelection();
  } else {
    // Enter blueprint selection mode temporarily
    bBlueprintMode = true;
  }
}

void AHexTopDownPawn::BlueprintModeReleased() {
  UE_LOG(LogTemp, Warning, TEXT("BlueprintMode released"));
  if (bBlueprintSelectionActive) {
    // End selection and create blueprint
    EndSelection();
  } else if (!bBlueprintPlacementMode) {
    // Exit blueprint mode if not in placement mode
    bBlueprintMode = false;
  }
}

void AHexTopDownPawn::ToggleBlueprintMode() {
  bBlueprintMode = !bBlueprintMode;
  UE_LOG(LogTemp, Warning, TEXT("Blueprint mode toggled: %s"),
         bBlueprintMode ? TEXT("ON") : TEXT("OFF"));

  if (!bBlueprintMode) {
    ClearSelection();
    bBlueprintPlacementMode = false;
  } else {
    // Enter blueprint mode - clear any existing blueprint
    ClearSelection();
    bBlueprintPlacementMode = false;
  }
}

void AHexTopDownPawn::CreateBlueprint() {
  UE_LOG(LogTemp, Warning,
         TEXT("CreateBlueprint called with %d selected tiles"),
         SelectedTiles.Num());

  if (SelectedTiles.Num() == 0) {
    UE_LOG(LogTemp, Warning,
           TEXT("No tiles selected, exiting CreateBlueprint"));
    return;
  }

  BlueprintTiles.Empty();

  // Find the simulation world
  AOrbSimDriver *Driver = CachedOrbSimDriver;
  if (!Driver) {
    Driver = AOrbSimDriver::Find(GetWorld());
    CachedOrbSimDriver = Driver;
  }
  if (!Driver)
    return;

  USimWorldHex *Sim = Driver->GetSim();
  if (!Sim)
    return;

  // Find the minimum hex coordinates to use as the blueprint origin
  FHex MinHex = SelectedTiles[0];
  for (const FHex &Hex : SelectedTiles) {
    MinHex.Q = FMath::Min(MinHex.Q, Hex.Q);
    MinHex.R = FMath::Min(MinHex.R, Hex.R);
  }

  // Debug: Log what we're checking
  UE_LOG(LogTemp, Warning, TEXT("Checking %d arms and %d tiles in simulation"),
         Sim->Arms.Num(), Sim->TileMap.Num());

  // Convert selected tiles to blueprint tiles
  for (const FHex &Hex : SelectedTiles) {
    UE_LOG(LogTemp, Warning, TEXT("Checking hex (%d,%d) for content"), Hex.Q,
           Hex.R);

    // Calculate local hex coordinates relative to blueprint origin
    FHex LocalHex(Hex.Q - MinHex.Q, Hex.R - MinHex.R);

    // Check if there's an arm at this position first
    bool bFoundArm = false;
    for (int32 ArmIndex = 0; ArmIndex < Sim->Arms.Num(); ArmIndex++) {
      const FArmState &ArmState = Sim->Arms[ArmIndex];
      if (ArmState.BaseHex == Hex) {
        // Found the arm - capture its program and HOME facing/reach (reset
        // position) Get the starting values from the arm actor
        uint8 StartingRot = (uint8)ArmState.HomeFacing; // Default to HomeFacing
        int32 StartingRch = ArmState.HomeArmLen;        // Default to HomeArmLen

        // Find the arm actor to get its original program and starting values
        ABasicArm *ArmActor = nullptr;
        if (UWorld *World = GetWorld()) {
          for (TActorIterator<ABasicArm> It(World); It; ++It) {
            ABasicArm *TestArm = *It;
            if (TestArm && TestArm->SimArmIndex == ArmIndex) {
              ArmActor = TestArm;
              break;
            }
          }
        }

        // Use the original program from the arm actor, not the current
        // simulation tape The simulation tape may contain temporary commands
        // generated by Reset commands
        TArray<FArmInstr> CapturedProgram;
        if (ArmActor) {
          CapturedProgram =
              ArmActor->Program; // Use original program, not ArmState.Tape
          StartingRot = ArmActor->StartingRotation;
          StartingRch = ArmActor->StartingReach;
        } else {
          // Fallback: convert from simulation tape if arm actor not found
          CapturedProgram = ConvertOpsToProgram(ArmState.Tape);
          UE_LOG(LogTemp, Warning,
                 TEXT("Blueprint: Arm actor not found, using simulation tape "
                      "(may include Reset-generated commands)"));
        }

        FBlueprintTile BlueprintTile(LocalHex, (uint8)ArmState.HomeFacing,
                                     ArmState.HomeArmLen, StartingRot,
                                     StartingRch, CapturedProgram,
                                     true); // Assume looping for now
        BlueprintTiles.Add(BlueprintTile);

        bFoundArm = true;
        break;
      }
    }

    // If no arm found, check for regular tiles
    if (!bFoundArm) {
      FTile *Tile = Sim->TileMap.Find(Hex);
      if (Tile) {
        // Get tile direction from the tile's Arg field
        uint8 TileDirection = Tile->Arg;

        // Regular tile (not an arm)
        BlueprintTiles.Add(FBlueprintTile(LocalHex, Tile->Kind, TileDirection));

        // Debug log for ClusterOutput and ClusterInput tiles
        if (Tile->Kind == ETileKind::ClusterOutput) {
          UE_LOG(LogTemp, Warning,
                 TEXT("CreateBlueprint: Captured ClusterOutput at (%d,%d) with "
                      "TileDirection=%d (from Tile->Arg)"),
                 Hex.Q, Hex.R, TileDirection);
        } else if (Tile->Kind == ETileKind::ClusterInput) {
          UE_LOG(LogTemp, Warning,
                 TEXT("CreateBlueprint: Captured ClusterInput at (%d,%d) with "
                      "TileDirection=%d (orb type from Tile->Arg)"),
                 Hex.Q, Hex.R, TileDirection);
        }
      } else {
        UE_LOG(LogTemp, Warning, TEXT("No tile or arm found at (%d,%d)"), Hex.Q,
               Hex.R);
      }
    }
  }

  UE_LOG(LogTemp, Log, TEXT("Created blueprint with %d tiles"),
         BlueprintTiles.Num());

  // Enable blueprint placement mode after creating a blueprint
  if (BlueprintTiles.Num() > 0) {
    bBlueprintPlacementMode = true;
    BlueprintRotation = 0;
    BlueprintPlacementOffset = FHex(0, 0);

    // Disable HexPlacementComponent preview when in blueprint mode
    if (Placer) {
      Placer->SetBlueprintMode(true);
    }

    // Reset smoothing
    bBlueprintSmoothingInitialized = false;
  }
}

void AHexTopDownPawn::ClearSelection() {
  SelectedTiles.Empty();
  BlueprintTiles.Empty();
  bBlueprintSelectionActive = false;
  ClearBlueprintPreviewComponents();

  // Clear selection highlights
  for (UStaticMeshComponent *Comp : SelectionHighlightComponents) {
    if (Comp) {
      Comp->SetVisibility(false);
    }
  }
}

void AHexTopDownPawn::CancelBlueprintPlacement() {
  UE_LOG(LogTemp, Warning, TEXT("Canceling blueprint placement"));

  // Clear preview components before exiting
  ClearBlueprintPreviewComponents();

  // Exit blueprint placement mode and return to normal kind placement
  bBlueprintPlacementMode = false;
  bBlueprintMode = false;
  ClearSelection();

  // Re-enable HexPlacementComponent preview when exiting blueprint mode
  if (Placer) {
    Placer->SetBlueprintMode(false);
  }

  // Clear any HUD selection box that might still be active
  APlayerController *PC =
      GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
  if (PC) {
    AAutomationGameHUD *GameHUD = Cast<AAutomationGameHUD>(PC->GetHUD());
    if (GameHUD) {
      GameHUD->EndSelectionBox();
    }
  }
}

void AHexTopDownPawn::PlaceBlueprint() {
  if (BlueprintTiles.Num() == 0)
    return;

  // Find the simulation world
  AOrbSimDriver *Driver = CachedOrbSimDriver;
  if (!Driver) {
    Driver = AOrbSimDriver::Find(GetWorld());
    CachedOrbSimDriver = Driver;
  }
  if (!Driver)
    return;

  USimWorldHex *Sim = Driver->GetSim();
  if (!Sim)
    return;

  // Get the current mouse position in world coordinates
  FVector2D MousePosition;
  if (!GetWorld()->GetFirstPlayerController() ||
      !GetWorld()->GetFirstPlayerController()->GetMousePosition(
          MousePosition.X, MousePosition.Y)) {
    return;
  }

  // Get tile size and grid info from driver
  float TileSize = Driver ? Driver->GetTileSize() : 100.f;
  EHexOrientation Orientation =
      Driver ? Driver->GetGridOrientation() : EHexOrientation::PointyTop;
  float GridPlaneZ = Driver ? Driver->GetGridPlaneZ(this) : 0.f;

  // Convert mouse position to world coordinates by projecting ray to the grid
  // plane
  FVector WorldPosition, WorldDirection;
  GetWorld()->GetFirstPlayerController()->DeprojectScreenPositionToWorld(
      MousePosition.X, MousePosition.Y, WorldPosition, WorldDirection);

  // Project the ray down to the grid plane (Z = GridPlaneZ)
  if (FMath::Abs(WorldDirection.Z) > KINDA_SMALL_NUMBER) {
    float T = (GridPlaneZ - WorldPosition.Z) / WorldDirection.Z;
    WorldPosition = WorldPosition + WorldDirection * T;
  }

  // Convert world coordinates to hex coordinates
  FHexAxial TargetHexAxial = HexCoord::XYToAxialRound(
      FVector2D(WorldPosition.X, WorldPosition.Y), TileSize, Orientation);
  FHex TargetHex(TargetHexAxial.Q, TargetHexAxial.R, CurrentLevel);

  // Calculate the center of the blueprint to center it at the mouse position
  FHex BlueprintCenter(0, 0, 0);
  if (BlueprintTiles.Num() > 0) {
    // Find the bounds of the blueprint
    FHex MinHex(BlueprintTiles[0].LocalHex.Q, BlueprintTiles[0].LocalHex.R, 0);
    FHex MaxHex(BlueprintTiles[0].LocalHex.Q, BlueprintTiles[0].LocalHex.R, 0);

    for (const FBlueprintTile &Tile : BlueprintTiles) {
      MinHex.Q = FMath::Min(MinHex.Q, Tile.LocalHex.Q);
      MinHex.R = FMath::Min(MinHex.R, Tile.LocalHex.R);
      MaxHex.Q = FMath::Max(MaxHex.Q, Tile.LocalHex.Q);
      MaxHex.R = FMath::Max(MaxHex.R, Tile.LocalHex.R);
    }

    // Calculate center as the average of min and max
    BlueprintCenter.Q = (MinHex.Q + MaxHex.Q) / 2;
    BlueprintCenter.R = (MinHex.R + MaxHex.R) / 2;
  }

  // Apply blueprint rotation to the placement offset
  FHex RotatedOffset = BlueprintPlacementOffset;
  if (BlueprintRotation > 0) {
    // Rotate the offset by the blueprint rotation
    for (uint8 i = 0; i < BlueprintRotation; i++) {
      RotatedOffset =
          HexUtil::Neighbor(RotatedOffset, EHexDir::D1); // 60 degree rotation
    }
  }

  // Place each tile in the blueprint
  for (const FBlueprintTile &BlueprintTile : BlueprintTiles) {
    // Calculate the offset of this tile from the blueprint center
    FHex OffsetFromCenter =
        FHex(BlueprintTile.LocalHex.Q - BlueprintCenter.Q,
             BlueprintTile.LocalHex.R - BlueprintCenter.R, 0);

    // Rotate the offset by BlueprintRotation * 60 degrees
    // Use proper hexagonal rotation formula
    FHex RotatedTileOffset = OffsetFromCenter;
    if (BlueprintRotation > 0) {
      // Hexagonal rotation formula for 60-degree increments
      // Each rotation step rotates the hex coordinates by 60 degrees clockwise
      for (uint8 j = 0; j < BlueprintRotation; j++) {
        int32 NewQ = RotatedTileOffset.Q + RotatedTileOffset.R;
        int32 NewR = -RotatedTileOffset.Q;
        RotatedTileOffset = FHex(NewQ, NewR, 0);
      }
    }

    // Calculate the final hex position
    // Apply the rotated offset to the mouse position (which becomes the new
    // center)
    FHex FinalHex = TargetHex;
    FinalHex.Q += RotatedTileOffset.Q;
    FinalHex.R += RotatedTileOffset.R;

    if (BlueprintTile.bIsArm) {
      // Use GameManager to spawn arm (same as HexPlacementComponent does)
      // Find GameManager and let it handle both spawning and simulation
      AGameManager *GameManager = nullptr;
      if (UWorld *World = GetWorld()) {
        for (TActorIterator<AGameManager> It(World); It; ++It) {
          GameManager = *It;
          break;
        }
      }

      if (GameManager) {
        // Apply blueprint rotation to both the arm facing and starting rotation
        uint8 RotatedArmFacing =
            (BlueprintTile.ArmFacing + BlueprintRotation) % 6;
        uint8 RotatedStartingRotation =
            (BlueprintTile.StartingRotation + BlueprintRotation) % 6;

        UE_LOG(LogTemp, Warning,
               TEXT("Blueprint: Placing arm at (%d,%d,%d) with facing=%d "
                    "startRot=%d (rotated from facing=%d startRot=%d)"),
               FinalHex.Q, FinalHex.R, FinalHex.Level, RotatedArmFacing,
               RotatedStartingRotation, BlueprintTile.ArmFacing,
               BlueprintTile.StartingRotation);

        // Use GameManager to spawn the arm with the rotated arm facing (home
        // position)
        ABasicArm *SpawnedArm = GameManager->SpawnBasicArmAt(
            FinalHex, RotatedArmFacing, BlueprintTile.ArmProgram,
            BlueprintTile.bArmLoopProgram);

        if (SpawnedArm) {
          // Set the captured reach distance (reset position)
          SpawnedArm->ReachTiles = BlueprintTile.StartingReach;
          SpawnedArm->StartingReach = BlueprintTile.StartingReach;

          // Set the rotation values
          SpawnedArm->FacingDir = RotatedStartingRotation;
          SpawnedArm->StartingRotation = RotatedStartingRotation;

          // Also update the simulation to match the captured reset reach and
          // rotation
          if (SpawnedArm->SimArmIndex >= 0 &&
              SpawnedArm->SimArmIndex < Sim->Arms.Num()) {
            FArmState &ArmState = Sim->Arms[SpawnedArm->SimArmIndex];
            ArmState.Facing =
                (EHexDir)RotatedStartingRotation; // Current facing is the
                                                  // starting rotation
            ArmState.HomeFacing =
                (EHexDir)RotatedArmFacing; // Home facing is where it resets to
            ArmState.ArmLen = BlueprintTile.StartingReach;
            ArmState.HomeArmLen = BlueprintTile.StartingReach;

            UE_LOG(LogTemp, Warning,
                   TEXT("Blueprint: Sim state for spawned arm - HomeFacing=%d, "
                        "StartingRotation=%d, Blueprint rotation=%d"),
                   RotatedArmFacing, RotatedStartingRotation,
                   BlueprintRotation);
          }

          UE_LOG(LogTemp, Warning,
                 TEXT("Blueprint: Spawned arm at (%d,%d) - FacingDir=%d "
                      "StartFacingDir=%d SimIndex=%d"),
                 FinalHex.Q, FinalHex.R, SpawnedArm->FacingDir,
                 SpawnedArm->GetStartFacingDir(), SpawnedArm->SimArmIndex);
        } else {
          UE_LOG(LogTemp, Warning,
                 TEXT("GameManager failed to spawn arm at (%d,%d)"), FinalHex.Q,
                 FinalHex.R);
        }
      } else {
        UE_LOG(LogTemp, Warning,
               TEXT("No GameManager found - cannot spawn arm at (%d,%d)"),
               FinalHex.Q, FinalHex.R);
      }
    } else {
      // Create a regular tile
      FTile NewTile;
      NewTile.Kind = BlueprintTile.TileType;

      // Apply blueprint rotation to tile direction for directional tiles
      uint8 RotatedTileDirection = BlueprintTile.TileDirection;
      switch (BlueprintTile.TileType) {
      case ETileKind::Belt:
        RotatedTileDirection =
            (BlueprintTile.TileDirection + BlueprintRotation) % 6;
        break;
      case ETileKind::Bond:
      case ETileKind::Unbond:
        RotatedTileDirection =
            (BlueprintTile.TileDirection + BlueprintRotation) % 6;
        break;
      default:
        // Non-directional tiles don't need direction rotation
        break;
      }

      // For ClusterOutput tiles in blueprints, check if TileDirection is a
      // template ID Only use template if this is a Kind8-generated template
      // (template exists and LocalHex matches)
      if (BlueprintTile.TileType == ETileKind::ClusterOutput) {
        const USimWorldHex::FClusterTemplate *Template =
            Sim->GetClusterTemplates().Find(BlueprintTile.TileDirection);
        FHex LocalPos = BlueprintTile.LocalHex;

        // Check if this is a Kind8-generated template:
        // Template must exist AND LocalHex must match an orb position in the
        // template
        bool bIsKind8Template = false;
        if (Template) {
          // Verify LocalHex matches an orb position in the template
          for (const FOrbData &Orb : Template->Orbs) {
            if (Orb.dQ == LocalPos.Q && Orb.dR == LocalPos.R) {
              bIsKind8Template = true;
              break;
            }
          }
        }

        if (bIsKind8Template && Template) {
          // This is a Kind8 placement - find the orb type from the template
          uint8 OrbTypeForThisTile = 0;
          for (const FOrbData &Orb : Template->Orbs) {
            if (Orb.dQ == LocalPos.Q && Orb.dR == LocalPos.R) {
              OrbTypeForThisTile = Orb.Type;
              break;
            }
          }
          NewTile.Arg = OrbTypeForThisTile;
        } else {
          // Regular ClusterOutput tile - TileDirection is the orb type directly
          NewTile.Arg = RotatedTileDirection;
        }
      } else if (BlueprintTile.TileType == ETileKind::ClusterInput ||
                 BlueprintTile.TileType == ETileKind::DiscoveryClusterInput) {
        // ClusterInput and DiscoveryClusterInput tiles use TileDirection as the
        // orb type directly (no rotation needed) TileDirection was captured
        // from Tile->Arg which contains the orb type
        NewTile.Arg = BlueprintTile.TileDirection;
      } else {
        // For other tile types, TileDirection is the direction/type
        NewTile.Arg = RotatedTileDirection;
      }

      // Place the tile in the simulation
      Sim->PlaceTile(FinalHex, NewTile);

      UE_LOG(
          LogTemp, Log,
          TEXT("Placed tile at (%d,%d) Kind=%d (BlueprintTileType=%d) Arg=%d"),
          FinalHex.Q, FinalHex.R, (int32)NewTile.Kind,
          (int32)BlueprintTile.TileType, NewTile.Arg);
    }
  }

  // Force GameManager to rebuild grid to reflect tile changes
  if (UWorld *GameWorld = GetWorld()) {
    for (TActorIterator<AGameManager> It(GameWorld); It; ++It) {
      AGameManager *GameManager = *It;
      if (GameManager) {
        GameManager->Rebuild();
        break;
      }
    }
  }

  UE_LOG(LogTemp, Log, TEXT("Placed blueprint with %d tiles"),
         BlueprintTiles.Num());

  // Exit blueprint placement mode after successful placement, unless
  // ContinuePlacement is held
  if (!bContinuePlacementActive) {
    bBlueprintPlacementMode = false;
    bBlueprintMode = false;
    ClearSelection();

    // Re-enable HexPlacementComponent preview
    if (Placer) {
      Placer->SetBlueprintMode(false);
    }
  }
}

void AHexTopDownPawn::RotateBlueprintCW() {
  if (BlueprintTiles.Num() == 0)
    return;

  // Invert rotation direction on flipped grid (Level 1)
  if (CurrentLevel == 1) {
    BlueprintRotation = (BlueprintRotation + 1) % 6;
  } else {
    BlueprintRotation = (BlueprintRotation + 5) % 6;
  }

  UE_LOG(LogTemp, Log, TEXT("Blueprint rotated CW, new rotation: %d"),
         BlueprintRotation);
}

void AHexTopDownPawn::RotateBlueprintCCW() {
  if (BlueprintTiles.Num() == 0)
    return;

  // Invert rotation direction on flipped grid (Level 1)
  if (CurrentLevel == 1) {
    BlueprintRotation = (BlueprintRotation + 5) % 6;
  } else {
    BlueprintRotation = (BlueprintRotation + 1) % 6;
  }

  UE_LOG(LogTemp, Log, TEXT("Blueprint rotated CCW, new rotation: %d"),
         BlueprintRotation);
}

void AHexTopDownPawn::UpdateBlueprintPreview() {
  // TODO: Implement blueprint preview visualization
  // This would show a ghost/preview of where the blueprint would be placed
}

TArray<FArmInstr> AHexTopDownPawn::ConvertOpsToProgram(const TArray<FOp> &Ops) {
  TArray<FArmInstr> Out;
  for (const FOp &Op : Ops) {
    FArmInstr Instr;
    switch (Op.Cmd) {
    case ECmd::Wait:
      Instr.Cmd = EArmCmd::Wait;
      Instr.Arg = FMath::Max(1, Op.Arg);
      break;
    case ECmd::Grab:
      Instr.Cmd = EArmCmd::Grab;
      Instr.Arg = 0;
      break;
    case ECmd::Drop:
      Instr.Cmd = EArmCmd::Drop;
      Instr.Arg = 0;
      break;
    case ECmd::RotateR:
      Instr.Cmd = EArmCmd::RotateCW;
      Instr.Arg = FMath::Max(1, Op.Arg);
      break;
    case ECmd::RotateL:
      Instr.Cmd = EArmCmd::RotateCCW;
      Instr.Arg = FMath::Max(1, Op.Arg);
      break;
    case ECmd::Extend:
      Instr.Cmd = EArmCmd::Extend;
      Instr.Arg = FMath::Max(1, Op.Arg);
      break;
    case ECmd::Retract:
      Instr.Cmd = EArmCmd::Retract;
      Instr.Arg = FMath::Max(1, Op.Arg);
      break;
    case ECmd::Reset:
      Instr.Cmd = EArmCmd::Reset;
      Instr.Arg = 0;
      break;
    case ECmd::RotateHeldL:
      Instr.Cmd = EArmCmd::RotateHeldCCW;
      Instr.Arg = FMath::Max(1, Op.Arg);
      break;
    case ECmd::RotateHeldR:
      Instr.Cmd = EArmCmd::RotateHeldCW;
      Instr.Arg = FMath::Max(1, Op.Arg);
      break;
    case ECmd::WaitForBond:
      Instr.Cmd = EArmCmd::WaitForBond;
      Instr.Arg = 0;
      break;
    default:
      Instr.Cmd = EArmCmd::Wait;
      Instr.Arg = 1;
      break;
    }
    Out.Add(Instr);
  }
  return Out;
}

void AHexTopDownPawn::DeletionModePressed() {
  UE_LOG(LogTemp, Warning,
         TEXT("DeletionMode pressed - entering deletion selection mode"));
  bDeletionMode = true;
}

void AHexTopDownPawn::DeletionModeReleased() {
  UE_LOG(LogTemp, Warning, TEXT("DeletionMode released"));
  if (bDeletionSelectionActive) {
    // End deletion selection and delete selected tiles
    EndDeletionSelection();
  } else {
    // Exit deletion mode
    bDeletionMode = false;
  }
}

void AHexTopDownPawn::DeleteTileAndOrbs(const FHex &Hex) {
  // Find the simulation world
  AOrbSimDriver *Driver = CachedOrbSimDriver;
  if (!Driver) {
    Driver = AOrbSimDriver::Find(GetWorld());
    CachedOrbSimDriver = Driver;
  }
  if (!Driver)
    return;

  USimWorldHex *Sim = Driver->GetSim();
  if (!Sim)
    return;

  // View mode removal rules
  bool bIsAdvanced = false;
  if (UWorld *World = GetWorld()) {
    for (TActorIterator<ATriggerTile> It(World); It; ++It) {
      if (It->Q == Hex.Q && It->R == Hex.R && It->Level == Hex.Level) {
        bIsAdvanced = true;
        break;
      }
    }
    if (!bIsAdvanced) {
      for (TActorIterator<ARequestBeacon> It(World); It; ++It) {
        if (It->Q == Hex.Q && It->R == Hex.R && It->Level == Hex.Level) {
          bIsAdvanced = true;
          break;
        }
      }
    }
  }

  if (CurrentViewMode == EViewMode::Standard) {
    if (bIsAdvanced) {
      UE_LOG(LogTemp, Warning,
             TEXT("Cannot delete advanced tiles in Standard Mode"));
      return;
    }
  } else if (CurrentViewMode == EViewMode::AdvancedFilter) {
    if (!bIsAdvanced) {
      UE_LOG(LogTemp, Warning,
             TEXT("Only advanced tiles are removable in Advanced Filter Mode"));
      return;
    }

    // Perform advanced deletion (with scenario capture)
    if (UWorld *World = GetWorld()) {
      // Delete RequestBeacons at this location
      for (TActorIterator<ARequestBeacon> It(World); It; ++It) {
        ARequestBeacon *Beacon = *It;
        if (Beacon && Beacon->Q == Hex.Q && Beacon->R == Hex.R &&
            Beacon->Level == Hex.Level) {
          UE_LOG(LogTemp, Log, TEXT("Destroying RequestBeacon at (%d,%d,%d)"),
                 Hex.Q, Hex.R, Hex.Level);

          // Scenario capture for beacon removal
          for (TActorIterator<ATestLevelDirector> TLDIt(World); TLDIt;
               ++TLDIt) {
            ATestLevelDirector *TLD = *TLDIt;
            if (TLD) {
              TLD->RemoveBeaconSpec(Hex.Q, Hex.R, Hex.Level);
              break;
            }
          }
          Beacon->Destroy();
        }
      }

      // Delete TriggerTiles at this location
      for (TActorIterator<ATriggerTile> It(World); It; ++It) {
        ATriggerTile *TriggerTile = *It;
        if (TriggerTile && TriggerTile->Q == Hex.Q && TriggerTile->R == Hex.R &&
            TriggerTile->Level == Hex.Level) {
          UE_LOG(LogTemp, Log, TEXT("Destroying TriggerTile at (%d,%d,%d)"),
                 Hex.Q, Hex.R, Hex.Level);

          // Capture removal in scenario via TestLevelDirector
          for (TActorIterator<ATestLevelDirector> TLDIt(World); TLDIt;
               ++TLDIt) {
            ATestLevelDirector *TLD = *TLDIt;
            if (TLD) {
              TLD->RemoveTriggerTileSpec(Hex.Q, Hex.R, Hex.Level);
              break;
            }
          }
          TriggerTile->Destroy();
        }
      }
    }
    return;
  } else if (CurrentViewMode == EViewMode::OrbView) {
    // Orb View ONLY deletes orbs/clusters (including bonded groups)
    TArray<FHex> BondedAnchors;
    TArray<int32> BondIndices;
    TArray<int32> ArmIndices;
    TArray<FHex> ClusterHexes =
        FindItemConnectedComponent(Hex, BondedAnchors, BondIndices, ArmIndices);

    bool bChanged = false;
    if (BondedAnchors.Num() > 0) {
      for (const FHex &Anchor : BondedAnchors) {
        FItem RemovedItem;
        if (Sim->TryTakeItem(Anchor, RemovedItem)) {
          bChanged = true;
        }
      }
    }

    if (ArmIndices.Num() > 0) {
      for (int32 ArmIndex : ArmIndices) {
        if (Sim->Arms.IsValidIndex(ArmIndex)) {
          Sim->Arms[ArmIndex].bHolding = false;
          Sim->Arms[ArmIndex].Held = FItem();
          bChanged = true;
        }
      }
    }

    if (bChanged) {
      UE_LOG(LogTemp, Log,
             TEXT("Orb View: Removed bonded group with %d board items and %d "
                  "held items"),
             BondedAnchors.Num(), ArmIndices.Num());
      Sim->TileRevision++;
    }
    return; // Don't delete tiles in Orb View
  }

  // 1. Remove any orbs/items at this hex (Disabled in Standard Mode per user
  // request)
  if (CurrentViewMode != EViewMode::Standard) {
    FItem RemovedItem;
    if (Sim->TryTakeItem(Hex, RemovedItem)) {
      UE_LOG(LogTemp, Log, TEXT("Removed orb/item ID %d at (%d,%d)"),
             RemovedItem.Id, Hex.Q, Hex.R);
    }
  }

  // 2. Remove any arms at this hex (check both simulation and world actors)
  // First remove from simulation
  int32 RemovedArmIndex = -1;
  for (int32 ArmIndex = Sim->Arms.Num() - 1; ArmIndex >= 0; ArmIndex--) {
    const FArmState &ArmState = Sim->Arms[ArmIndex];
    if (ArmState.BaseHex == Hex) {
      UE_LOG(
          LogTemp, Warning,
          TEXT("HexTopDownPawn::DeleteTileAndOrbs - Removing arm from "
               "simulation at (%d,%d,%d) SimIndex=%d (total arms before: %d)"),
          Hex.Q, Hex.R, Hex.Level, ArmIndex, Sim->Arms.Num());
      RemovedArmIndex = ArmIndex;
      Sim->Arms.RemoveAt(ArmIndex);

      // Update SimArmIndex for all arms with higher indices
      if (UWorld *World = GetWorld()) {
        for (TActorIterator<ABasicArm> It(World); It; ++It) {
          if (It->SimArmIndex > ArmIndex) {
            It->SimArmIndex--;
          }
        }
      }

      // Mark arm base cache as dirty since we removed an arm
      Sim->MarkArmBaseCacheDirty();

      UE_LOG(LogTemp, Warning,
             TEXT("HexTopDownPawn::DeleteTileAndOrbs - After removal, total "
                  "arms: %d"),
             Sim->Arms.Num());
      break;
    }
  }

  // Remove runtime spawned arm actors at this hex
  if (UWorld *World = GetWorld()) {
    for (TActorIterator<ABasicArm> It(World); It; ++It) {
      ABasicArm *Arm = *It;
      if (Arm && Arm->Q == Hex.Q && Arm->R == Hex.R &&
          Arm->Level == Hex.Level) {
        UE_LOG(LogTemp, Log, TEXT("Destroying arm actor at (%d,%d,%d)"), Hex.Q,
               Hex.R, Hex.Level);
        Arm->Destroy();
        break;
      }
    }
  }

  // Remove walkers at this hex
  int32 RemovedWalkerIndex = -1;
  for (int32 WalkerIndex = Sim->Walkers.Num() - 1; WalkerIndex >= 0;
       WalkerIndex--) {
    const FWalkerState &WalkerState = Sim->Walkers[WalkerIndex];
    if (WalkerState.BaseHex == Hex) {
      UE_LOG(LogTemp, Warning,
             TEXT("HexTopDownPawn::DeleteTileAndOrbs - Removing walker from "
                  "simulation at (%d,%d,%d) SimIndex=%d (total walkers before: "
                  "%d)"),
             Hex.Q, Hex.R, Hex.Level, WalkerIndex, Sim->Walkers.Num());
      RemovedWalkerIndex = WalkerIndex;
      Sim->Walkers.RemoveAt(WalkerIndex);
      UE_LOG(LogTemp, Warning,
             TEXT("HexTopDownPawn::DeleteTileAndOrbs - After removal, total "
                  "walkers: %d"),
             Sim->Walkers.Num());
      break;
    }
  }

  if (RemovedWalkerIndex >= 0) {
    if (UWorld *World = GetWorld()) {
      for (TActorIterator<ABasicWalker> It(World); It; ++It) {
        ABasicWalker *WalkerActor = *It;
        if (WalkerActor && WalkerActor->SimWalkerIndex > RemovedWalkerIndex) {
          WalkerActor->SimWalkerIndex--;
          UE_LOG(LogTemp, Warning,
                 TEXT("HexTopDownPawn::DeleteTileAndOrbs - Updated walker at "
                      "(%d,%d) SimIndex: %d -> %d"),
                 WalkerActor->Q, WalkerActor->R,
                 WalkerActor->SimWalkerIndex + 1, WalkerActor->SimWalkerIndex);
        }
      }
    }
  }

  if (UWorld *World = GetWorld()) {
    for (TActorIterator<ABasicWalker> It(World); It; ++It) {
      ABasicWalker *Walker = *It;
      if (Walker && Walker->Q == Hex.Q && Walker->R == Hex.R &&
          Walker->Level == Hex.Level) {
        UE_LOG(LogTemp, Log, TEXT("Destroying walker actor at (%d,%d,%d)"),
               Hex.Q, Hex.R, Hex.Level);
        Walker->Destroy();
        break;
      }
    }
  }

  // Update scenario specs for walkers when deleting tiles
  if (UWorld *World = GetWorld()) {
    for (TActorIterator<ATestLevelDirector> It(World); It; ++It) {
      ATestLevelDirector *TLD = *It;
      if (TLD) {
        TLD->RemoveWalkerSpec(Hex.Q, Hex.R, Hex.Level);
      }
      break;
    }
  }

  // 3. Remove the tile itself
  // First, check if this is a ClusterOutput tile and clean up its
  // partnerships
  if (FTile *Tile = Sim->TileMap.Find(Hex)) {
    if (Tile->Kind == ETileKind::ClusterOutput) {
      // Find and remove all partnerships involving this tile
      TArray<FHex> PartnersToRemove;
      for (const auto &PartnerKV : Sim->GetIOBondPartners()) {
        if (PartnerKV.Key == Hex || PartnerKV.Value == Hex) {
          PartnersToRemove.Add(PartnerKV.Key);
        }
      }

      TMap<FHex, FHex> &IOBondPartners = Sim->GetIOBondPartnersMutable();
      for (const FHex &PartnerHex : PartnersToRemove) {
        IOBondPartners.Remove(PartnerHex);
        UE_LOG(LogTemp, Log, TEXT("Removed IOBondPartners entry for (%d,%d)"),
               PartnerHex.Q, PartnerHex.R);
      }
    }

    if (Tile->Kind == ETileKind::HelixOutput) {
      Sim->UnregisterHelixOutputTile(Hex);
    }
  }

  if (Sim->TileMap.Remove(Hex) > 0) {
    UE_LOG(LogTemp, Log, TEXT("Removed tile at (%d,%d,%d)"), Hex.Q, Hex.R,
           Hex.Level);
    Sim->TileRevision++; // Increment revision to trigger visual updates
  }
}

void AHexTopDownPawn::DeleteSelectedTilesAndOrbs() {
  UE_LOG(LogTemp, Log, TEXT("Deleting %d selected tiles and their orbs"),
         SelectedTiles.Num());

  for (const FHex &Hex : SelectedTiles) {
    DeleteTileAndOrbs(Hex);
  }
}

void AHexTopDownPawn::RemoveTileAndOrbsAtCursor() {
  if (!Placer)
    return;

  // Use the placement component's hover hex detection
  if (Placer->IsHoverValid()) {
    FHex CursorHex = Placer->GetHoverHex();
    UE_LOG(LogTemp, Log,
           TEXT("Removing tile and orbs at cursor position (%d,%d,%d)"),
           CursorHex.Q, CursorHex.R, CursorHex.Level);
    DeleteTileAndOrbs(CursorHex);
  } else {
    UE_LOG(LogTemp, Warning, TEXT("No valid hex under cursor for removal"));
  }
}

void AHexTopDownPawn::StartDeletionSelection() {
  UE_LOG(LogTemp, Warning, TEXT("StartDeletionSelection called"));

  if (!bDeletionMode)
    return;

  APlayerController *PC =
      GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
  if (!PC)
    return;

  FVector2D MousePosition;
  if (PC->GetMousePosition(MousePosition.X, MousePosition.Y)) {
    SelectionStart = MousePosition;
    SelectionEnd = MousePosition;
    bDeletionSelectionActive = true;
    SelectedTiles.Empty();
    UpdateSelectionHighlights();

    // Start HUD selection box (reuse the same box but with different purpose)
    AAutomationGameHUD *GameHUD = Cast<AAutomationGameHUD>(PC->GetHUD());
    if (GameHUD) {
      GameHUD->StartSelectionBox(MousePosition);
    }

    UE_LOG(LogTemp, Warning, TEXT("Deletion selection started at (%f, %f)"),
           MousePosition.X, MousePosition.Y);
  } else {
    // UE_LOG(LogTemp, Warning, TEXT("Failed to get mouse position for
    // deletion selection"));
  }
}

void AHexTopDownPawn::EndDeletionSelection() {
  UE_LOG(LogTemp, Warning, TEXT("EndDeletionSelection called"));

  if (!bDeletionMode || !bDeletionSelectionActive)
    return;

  bDeletionSelectionActive = false;
  bDeletionMode = false; // Exit deletion mode after selection

  // End HUD selection box
  APlayerController *PC =
      GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
  if (PC) {
    AAutomationGameHUD *GameHUD = Cast<AAutomationGameHUD>(PC->GetHUD());
    if (GameHUD) {
      GameHUD->EndSelectionBox();
    }
  }

  // Delete selected tiles immediately
  if (SelectedTiles.Num() > 0) {
    UE_LOG(LogTemp, Warning, TEXT("Deleting %d selected tiles"),
           SelectedTiles.Num());
    DeleteSelectedTilesAndOrbs();
    ClearSelection();
  } else {
    UE_LOG(LogTemp, Warning, TEXT("No tiles selected for deletion"));
  }
}

void AHexTopDownPawn::UpdateDeletionSelection() {
  if (!bDeletionSelectionActive || !GetWorld())
    return;

  // Use the same logic as UpdateSelection but for deletion
  FVector2D MousePosition;
  if (GetWorld()->GetFirstPlayerController() &&
      GetWorld()->GetFirstPlayerController()->GetMousePosition(
          MousePosition.X, MousePosition.Y)) {
    SelectionEnd = MousePosition;

    // Update selected tiles based on current selection box
    SelectedTiles.Empty();

    // Get tile size and grid info from driver
    AOrbSimDriver *Driver = CachedOrbSimDriver;
    if (!Driver) {
      Driver = AOrbSimDriver::Find(GetWorld());
      CachedOrbSimDriver = Driver;
    }
    float TileSize = Driver ? Driver->GetTileSize() : 100.f;
    EHexOrientation Orientation =
        Driver ? Driver->GetGridOrientation() : EHexOrientation::PointyTop;

    // Updated: GridPlaneZ now accurately reflects the active level's visual Z
    float GridPlaneZ = Driver ? Driver->GetGridPlaneZ(this) : 0.f;

    // Convert screen coordinates to world coordinates by projecting rays to
    // the grid plane
    FVector WorldStart, WorldEnd;
    FVector WorldStartDir, WorldEndDir;

    // Deproject both screen points to get rays
    GetWorld()->GetFirstPlayerController()->DeprojectScreenPositionToWorld(
        SelectionStart.X, SelectionStart.Y, WorldStart, WorldStartDir);
    GetWorld()->GetFirstPlayerController()->DeprojectScreenPositionToWorld(
        SelectionEnd.X, SelectionEnd.Y, WorldEnd, WorldEndDir);

    // Project the rays down to the grid plane (Z = GridPlaneZ)
    if (FMath::Abs(WorldStartDir.Z) > KINDA_SMALL_NUMBER) {
      float T = (GridPlaneZ - WorldStart.Z) / WorldStartDir.Z;
      WorldStart = WorldStart + WorldStartDir * T;
    }
    if (FMath::Abs(WorldEndDir.Z) > KINDA_SMALL_NUMBER) {
      float T = (GridPlaneZ - WorldEnd.Z) / WorldEndDir.Z;
      WorldEnd = WorldEnd + WorldEndDir * T;
    }

    // Convert world coordinates to hex coordinates
    FHexAxial HexStartAxial = HexCoord::XYToAxialRound(
        FVector2D(WorldStart.X, WorldStart.Y), TileSize, Orientation);
    FHexAxial HexEndAxial = HexCoord::XYToAxialRound(
        FVector2D(WorldEnd.X, WorldEnd.Y), TileSize, Orientation);
    FHex HexStart(HexStartAxial.Q, HexStartAxial.R, CurrentLevel);
    FHex HexEnd(HexEndAxial.Q, HexEndAxial.R, CurrentLevel);

    // Use improved hexagonal selection algorithm with actual world positions
    GetHexesInSelectionArea(HexStart, HexEnd, WorldStart, WorldEnd,
                            CurrentLevel, SelectedTiles);

    UE_LOG(LogTemp, Warning, TEXT("Deletion selection: %d tiles selected"),
           SelectedTiles.Num());

    // Update visual highlights
    UpdateSelectionHighlights();
  }
}

void AHexTopDownPawn::GetHexesInSelectionArea(
    const FHex &HexStart, const FHex &HexEnd, const FVector &WorldStart,
    const FVector &WorldEnd, int32 Level, TArray<FHex> &OutHexes) {
  OutHexes.Empty();

  // Get the driver to access tile size and orientation
  AOrbSimDriver *Driver = CachedOrbSimDriver;
  if (!Driver) {
    Driver = AOrbSimDriver::Find(GetWorld());
    CachedOrbSimDriver = Driver;
  }
  if (!Driver)
    return;

  float TileSize = Driver->GetTileSize();
  EHexOrientation Orientation = Driver->GetGridOrientation();

  // Use the actual world positions from the camera projection
  FVector2D StartWorld2D(WorldStart.X, WorldStart.Y);
  FVector2D EndWorld2D(WorldEnd.X, WorldEnd.Y);

  // Create a proper selection rectangle in world space using the actual
  // projected positions
  FVector2D MinWorld(FMath::Min(StartWorld2D.X, EndWorld2D.X),
                     FMath::Min(StartWorld2D.Y, EndWorld2D.Y));
  FVector2D MaxWorld(FMath::Max(StartWorld2D.X, EndWorld2D.X),
                     FMath::Max(StartWorld2D.Y, EndWorld2D.Y));

  // Calculate a much larger search area to ensure we don't miss any tiles
  // Use the world bounds to determine a reasonable hex search area
  int32 SearchRadius = FMath::CeilToInt(FMath::Max(MaxWorld.X - MinWorld.X,
                                                   MaxWorld.Y - MinWorld.Y) /
                                        TileSize) +
                       5;

  // Calculate center of selection area in hex coordinates
  int32 CenterQ = (HexStart.Q + HexEnd.Q) / 2;
  int32 CenterR = (HexStart.R + HexEnd.R) / 2;

  // Search in a hexagonal pattern around the center
  int32 MinQ = CenterQ - SearchRadius;
  int32 MaxQ = CenterQ + SearchRadius;
  int32 MinR = CenterR - SearchRadius;
  int32 MaxR = CenterR + SearchRadius;

  UE_LOG(
      LogTemp, Warning,
      TEXT("GetHexesInSelectionArea: World bounds (%.1f,%.1f) to (%.1f,%.1f), "
           "Search radius %d, Hex bounds Q(%d-%d) R(%d-%d) Level %d"),
      MinWorld.X, MinWorld.Y, MaxWorld.X, MaxWorld.Y, SearchRadius, MinQ, MaxQ,
      MinR, MaxR, Level);

  int32 TilesChecked = 0;
  int32 TilesAdded = 0;

  // Check all hexes in the expanded range
  for (int32 q = MinQ; q <= MaxQ; q++) {
    for (int32 r = MinR; r <= MaxR; r++) {
      FHex TestHex(q, r, Level);
      TilesChecked++;

      // Convert this hex to world position using the same method as the
      // selection
      FVector2D HexWorld =
          HexCoord::AxialToXY(FHexAxial(q, r), TileSize, Orientation);

      // Check if this hex center is within the actual selection rectangle
      bool bInSelection =
          (HexWorld.X >= MinWorld.X && HexWorld.X <= MaxWorld.X &&
           HexWorld.Y >= MinWorld.Y && HexWorld.Y <= MaxWorld.Y);

      if (bInSelection) {
        OutHexes.Add(TestHex);
        TilesAdded++;
        UE_LOG(LogTemp, Log,
               TEXT("  Added hex (%d,%d,%d) at world (%.1f,%.1f)"), q, r, Level,
               HexWorld.X, HexWorld.Y);
      }
    }
  }

  UE_LOG(LogTemp, Warning,
         TEXT("GetHexesInSelectionArea: Checked %d hexes, found %d in "
              "selection area"),
         TilesChecked, TilesAdded);
}

void AHexTopDownPawn::EnsureBlueprintPreviewComponents() {
  // Create preview components for each blueprint tile if they don't exist
  while (BlueprintPreviewComponents.Num() < BlueprintTiles.Num()) {
    UStaticMeshComponent *PreviewComp = NewObject<UStaticMeshComponent>(this);
    PreviewComp->SetMobility(EComponentMobility::Movable);
    PreviewComp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    PreviewComp->SetVisibility(false);
    PreviewComp->AttachToComponent(
        RootComponent, FAttachmentTransformRules::KeepRelativeTransform);
    PreviewComp->RegisterComponent();
    BlueprintPreviewComponents.Add(PreviewComp);
  }

  // Remove excess components
  while (BlueprintPreviewComponents.Num() > BlueprintTiles.Num()) {
    UStaticMeshComponent *PreviewComp = BlueprintPreviewComponents.Pop();
    if (PreviewComp) {
      PreviewComp->DestroyComponent();
    }
  }
}

void AHexTopDownPawn::UpdateBlueprintPreviewMeshes(float DeltaSeconds) {
  if (!bBlueprintPlacementMode || BlueprintTiles.Num() == 0 || !GetWorld()) {
    ClearBlueprintPreviewComponents();
    return;
  }

  EnsureBlueprintPreviewComponents();

  // Get tile size and grid info from driver
  AOrbSimDriver *Driver = CachedOrbSimDriver;
  if (!Driver) {
    Driver = AOrbSimDriver::Find(GetWorld());
    CachedOrbSimDriver = Driver;
  }
  if (!Driver)
    return;

  float TileSize = Driver->GetTileSize();
  EHexOrientation Orientation = Driver->GetGridOrientation();
  float GridPlaneZ = Driver->GetGridPlaneZ(this);

  // Get mouse position and convert to hex coordinates
  FVector2D MousePosition;
  if (!GetWorld()->GetFirstPlayerController() ||
      !GetWorld()->GetFirstPlayerController()->GetMousePosition(
          MousePosition.X, MousePosition.Y)) {
    return;
  }

  // Convert mouse position to world coordinates
  FVector WorldPosition, WorldDirection;
  GetWorld()->GetFirstPlayerController()->DeprojectScreenPositionToWorld(
      MousePosition.X, MousePosition.Y, WorldPosition, WorldDirection);

  // Project the ray down to the grid plane
  if (FMath::Abs(WorldDirection.Z) > KINDA_SMALL_NUMBER) {
    float T = (GridPlaneZ - WorldPosition.Z) / WorldDirection.Z;
    WorldPosition = WorldPosition + WorldDirection * T;
  }

  // Convert world coordinates to hex coordinates
  FHexAxial TargetHexAxial = HexCoord::XYToAxialRound(
      FVector2D(WorldPosition.X, WorldPosition.Y), TileSize, Orientation);
  FHex TargetHex(TargetHexAxial.Q, TargetHexAxial.R, CurrentLevel);

  // Calculate the center of the blueprint
  FHex BlueprintCenter(0, 0);
  if (BlueprintTiles.Num() > 0) {
    FHex MinHex(BlueprintTiles[0].LocalHex.Q, BlueprintTiles[0].LocalHex.R);
    FHex MaxHex(BlueprintTiles[0].LocalHex.Q, BlueprintTiles[0].LocalHex.R);

    for (const FBlueprintTile &Tile : BlueprintTiles) {
      MinHex.Q = FMath::Min(MinHex.Q, Tile.LocalHex.Q);
      MinHex.R = FMath::Min(MinHex.R, Tile.LocalHex.R);
      MaxHex.Q = FMath::Max(MaxHex.Q, Tile.LocalHex.Q);
      MaxHex.R = FMath::Max(MaxHex.R, Tile.LocalHex.R);
    }

    BlueprintCenter.Q = (MinHex.Q + MaxHex.Q) / 2;
    BlueprintCenter.R = (MinHex.R + MaxHex.R) / 2;
  }

  // Apply blueprint rotation to the placement offset
  FHex RotatedOffset = BlueprintPlacementOffset;
  if (BlueprintRotation > 0) {
    for (uint8 i = 0; i < BlueprintRotation; i++) {
      RotatedOffset =
          HexUtil::Neighbor(RotatedOffset, EHexDir::D1); // 60 degree rotation
    }
  }

  // Update each preview mesh
  for (int32 i = 0;
       i < BlueprintTiles.Num() && i < BlueprintPreviewComponents.Num(); i++) {
    const FBlueprintTile &BlueprintTile = BlueprintTiles[i];
    UStaticMeshComponent *PreviewComp = BlueprintPreviewComponents[i];

    if (!PreviewComp)
      continue;

    // Calculate the offset of this tile from the blueprint center
    FHex OffsetFromCenter = FHex(BlueprintTile.LocalHex.Q - BlueprintCenter.Q,
                                 BlueprintTile.LocalHex.R - BlueprintCenter.R);

    // Rotate the offset by BlueprintRotation * 60 degrees
    // Use proper hexagonal rotation formula
    FHex RotatedTileOffset = OffsetFromCenter;
    if (BlueprintRotation > 0) {
      // Hexagonal rotation formula for 60-degree increments
      // Each rotation step rotates the hex coordinates by 60 degrees
      // clockwise
      for (uint8 j = 0; j < BlueprintRotation; j++) {
        int32 NewQ = RotatedTileOffset.Q + RotatedTileOffset.R;
        int32 NewR = -RotatedTileOffset.Q;
        RotatedTileOffset = FHex(NewQ, NewR);
      }
    }

    // Calculate the preview hex position
    // Apply the rotated offset to the mouse position (which becomes the new
    // center)
    FHex PreviewHex = TargetHex;
    PreviewHex.Q += RotatedTileOffset.Q;
    PreviewHex.R += RotatedTileOffset.R;

    // Convert hex to world position
    FVector PreviewWorldPos;
    if (Driver->IsSphereEnabled()) {
      PreviewWorldPos = Driver->MapHexToWorld(PreviewHex);
    } else {
      FVector2D PreviewWorldPos2D = HexCoord::AxialToXY(
          FHexAxial(PreviewHex.Q, PreviewHex.R), TileSize, Orientation);
      PreviewWorldPos =
          FVector(PreviewWorldPos2D.X, PreviewWorldPos2D.Y, GridPlaneZ);
    }

    // Get the appropriate mesh for this tile type
    UStaticMesh *PreviewMesh = nullptr;
    float PreviewYaw = 0.f;

    if (BlueprintTile.bIsArm) {
      // For arms, use ArmBasePreviewMesh from HexPlacementComponent
      if (Placer) {
        PreviewMesh = Placer->ArmBasePreviewMesh
                          ? Placer->ArmBasePreviewMesh
                          : Placer->GenericTilePreviewMesh;
      }
      // Apply arm rotation using StartingRotation (facing direction)
      PreviewYaw = (BlueprintTile.StartingRotation + BlueprintRotation) * 60.0f;
      if (Placer) {
        PreviewYaw =
            FMath::UnwindDegrees(PreviewYaw + Placer->ArmPreviewFacingFixDeg);
      }
    } else {
      // Get mesh based on tile type - use actual meshes like single tile
      // placement does
      switch (BlueprintTile.TileType) {
      case ETileKind::Input:
        if (Placer) {
          // Use CustomInputTileMesh if available, otherwise GetActualOrbMesh
          AGameManager *GameManager = nullptr;
          if (UWorld *World = GetWorld()) {
            for (TActorIterator<AGameManager> It(World); It; ++It) {
              GameManager = *It;
              break;
            }
          }
          if (GameManager && GameManager->CustomInputTileMesh) {
            PreviewMesh = GameManager->CustomInputTileMesh;
          } else if (GameManager && GameManager->TileMesh) {
            PreviewMesh = GameManager->TileMesh;
          } else {
            PreviewMesh = Placer->GetActualOrbMesh();
          }
          // Compute base yaw the same way as individual placement (direction
          // 0)
          {
            const FHex NeighborHex = HexUtil::Neighbor(PreviewHex, (EHexDir)0);
            const FVector NeighborWorldPos =
                Driver->IsSphereEnabled()
                    ? Driver->MapHexToWorld(NeighborHex)
                    : FVector(HexCoord::AxialToXY(
                                  FHexAxial(NeighborHex.Q, NeighborHex.R),
                                  TileSize, Orientation),
                              GridPlaneZ);
            const FVector2D DirVec(NeighborWorldPos.X - PreviewWorldPos.X,
                                   NeighborWorldPos.Y - PreviewWorldPos.Y);
            if (!DirVec.IsNearlyZero()) {
              PreviewYaw =
                  FMath::RadiansToDegrees(FMath::Atan2(DirVec.Y, DirVec.X));
            }
            // For input tiles, handle grid orientation (same as output)
            if (Orientation == EHexOrientation::FlatTop) {
              PreviewYaw = FMath::UnwindDegrees(PreviewYaw + 30.0f);
            }
          }
        }
        break;
      case ETileKind::ClusterInput:
      case ETileKind::DiscoveryClusterInput:
        if (Placer) {
          // Use CustomInputTileMesh if available, otherwise TileMesh
          AGameManager *GameManager = nullptr;
          if (UWorld *World = GetWorld()) {
            for (TActorIterator<AGameManager> It(World); It; ++It) {
              GameManager = *It;
              break;
            }
          }
          if (GameManager && GameManager->CustomInputTileMesh) {
            PreviewMesh = GameManager->CustomInputTileMesh;
          } else if (GameManager && GameManager->TileMesh) {
            PreviewMesh = GameManager->TileMesh;
          } else {
            PreviewMesh = Placer->GenericTilePreviewMesh;
          }
          // Compute base yaw the same way as individual placement (direction
          // 0)
          {
            const FHex NeighborHex = HexUtil::Neighbor(PreviewHex, (EHexDir)0);
            const FVector NeighborWorldPos =
                Driver->IsSphereEnabled()
                    ? Driver->MapHexToWorld(NeighborHex)
                    : FVector(HexCoord::AxialToXY(
                                  FHexAxial(NeighborHex.Q, NeighborHex.R),
                                  TileSize, Orientation),
                              GridPlaneZ);
            const FVector2D DirVec(NeighborWorldPos.X - PreviewWorldPos.X,
                                   NeighborWorldPos.Y - PreviewWorldPos.Y);
            if (!DirVec.IsNearlyZero()) {
              PreviewYaw =
                  FMath::RadiansToDegrees(FMath::Atan2(DirVec.Y, DirVec.X));
            }
            // For cluster input tiles, handle grid orientation (same as
            // output)
            if (Orientation == EHexOrientation::FlatTop) {
              PreviewYaw = FMath::UnwindDegrees(PreviewYaw + 30.0f);
            }
          }
        }
        break;
      case ETileKind::Output:
        if (Placer) {
          PreviewMesh = Placer->GetActualOutputOrbMesh();
          // Compute base yaw the same way as individual placement (direction
          // 0)
          {
            const FHex NeighborHex = HexUtil::Neighbor(PreviewHex, (EHexDir)0);
            const FVector NeighborWorldPos =
                Driver->IsSphereEnabled()
                    ? Driver->MapHexToWorld(NeighborHex)
                    : FVector(HexCoord::AxialToXY(
                                  FHexAxial(NeighborHex.Q, NeighborHex.R),
                                  TileSize, Orientation),
                              GridPlaneZ);
            const FVector2D DirVec(NeighborWorldPos.X - PreviewWorldPos.X,
                                   NeighborWorldPos.Y - PreviewWorldPos.Y);
            if (!DirVec.IsNearlyZero()) {
              PreviewYaw =
                  FMath::RadiansToDegrees(FMath::Atan2(DirVec.Y, DirVec.X));
            }
            // For output tiles, handle grid orientation
            if (Orientation == EHexOrientation::FlatTop) {
              PreviewYaw = FMath::UnwindDegrees(PreviewYaw + 30.0f);
            }
          }
        }
        break;
      case ETileKind::ClusterOutput:
        if (Placer) {
          PreviewMesh = Placer->GetActualOutputOrbMesh();
          // Compute base yaw the same way as individual placement (direction
          // 0)
          {
            const FHex NeighborHex = HexUtil::Neighbor(PreviewHex, (EHexDir)0);
            const FVector NeighborWorldPos =
                Driver->IsSphereEnabled()
                    ? Driver->MapHexToWorld(NeighborHex)
                    : FVector(HexCoord::AxialToXY(
                                  FHexAxial(NeighborHex.Q, NeighborHex.R),
                                  TileSize, Orientation),
                              GridPlaneZ);
            const FVector2D DirVec(NeighborWorldPos.X - PreviewWorldPos.X,
                                   NeighborWorldPos.Y - PreviewWorldPos.Y);
            if (!DirVec.IsNearlyZero()) {
              PreviewYaw =
                  FMath::RadiansToDegrees(FMath::Atan2(DirVec.Y, DirVec.X));
            }
            // For cluster output tiles, handle grid orientation
            if (Orientation == EHexOrientation::FlatTop) {
              PreviewYaw = FMath::UnwindDegrees(PreviewYaw + 30.0f);
            }
          }
        }
        break;
      case ETileKind::Belt:
        if (Placer) {
          PreviewMesh = Placer->GetActualBeltMesh();
        }
        // Apply belt direction rotation using proper world position
        // calculation
        {
          const uint8 RotatedDir =
              (BlueprintTile.TileDirection + BlueprintRotation) % 6;
          const FHex NeighborHex =
              HexUtil::Neighbor(PreviewHex, (EHexDir)RotatedDir);
          const FVector NeighborWorldPos =
              Driver->IsSphereEnabled()
                  ? Driver->MapHexToWorld(NeighborHex)
                  : FVector(HexCoord::AxialToXY(
                                FHexAxial(NeighborHex.Q, NeighborHex.R),
                                TileSize, Orientation),
                            GridPlaneZ);
          const FVector2D DirVec(NeighborWorldPos.X - PreviewWorldPos.X,
                                 NeighborWorldPos.Y - PreviewWorldPos.Y);
          if (!DirVec.IsNearlyZero()) {
            PreviewYaw =
                FMath::RadiansToDegrees(FMath::Atan2(DirVec.Y, DirVec.X));
          }
        }
        break;
      case ETileKind::Bond:
        if (Placer) {
          PreviewMesh = Placer->BondPreviewMesh
                            ? Placer->BondPreviewMesh
                            : Placer->GenericTilePreviewMesh;
        }
        // Apply bond direction rotation using proper world position
        // calculation
        {
          const uint8 RotatedDir =
              (BlueprintTile.TileDirection + BlueprintRotation) % 6;
          const FHex NeighborHex =
              HexUtil::Neighbor(PreviewHex, (EHexDir)RotatedDir);
          const FVector NeighborWorldPos =
              Driver->IsSphereEnabled()
                  ? Driver->MapHexToWorld(NeighborHex)
                  : FVector(HexCoord::AxialToXY(
                                FHexAxial(NeighborHex.Q, NeighborHex.R),
                                TileSize, Orientation),
                            GridPlaneZ);
          const FVector2D DirVec(NeighborWorldPos.X - PreviewWorldPos.X,
                                 NeighborWorldPos.Y - PreviewWorldPos.Y);
          if (!DirVec.IsNearlyZero()) {
            PreviewYaw =
                FMath::RadiansToDegrees(FMath::Atan2(DirVec.Y, DirVec.X));
          }
        }
        break;
      case ETileKind::Unbond:
        if (Placer) {
          PreviewMesh = Placer->UnbondPreviewMesh
                            ? Placer->UnbondPreviewMesh
                            : Placer->GenericTilePreviewMesh;
        }
        // Apply unbond direction rotation using proper world position
        // calculation
        {
          const uint8 RotatedDir =
              (BlueprintTile.TileDirection + BlueprintRotation) % 6;
          const FHex NeighborHex =
              HexUtil::Neighbor(PreviewHex, (EHexDir)RotatedDir);
          const FVector NeighborWorldPos =
              Driver->IsSphereEnabled()
                  ? Driver->MapHexToWorld(NeighborHex)
                  : FVector(HexCoord::AxialToXY(
                                FHexAxial(NeighborHex.Q, NeighborHex.R),
                                TileSize, Orientation),
                            GridPlaneZ);
          const FVector2D DirVec(NeighborWorldPos.X - PreviewWorldPos.X,
                                 NeighborWorldPos.Y - PreviewWorldPos.Y);
          if (!DirVec.IsNearlyZero()) {
            PreviewYaw =
                FMath::RadiansToDegrees(FMath::Atan2(DirVec.Y, DirVec.X));
          }
        }
        break;
      default:
        if (Placer) {
          PreviewMesh = Placer->GenericTilePreviewMesh;
        }
        break;
      }
    }

    // Set the mesh and position
    if (PreviewMesh) {
      PreviewComp->SetStaticMesh(PreviewMesh);

      // Calculate target transform
      FVector TargetLoc = PreviewWorldPos;
      FRotator TargetRot = FRotator(0.f, PreviewYaw, 0.f);

      // If not Bond/Unbond (which set their own location above), set location
      // to hex center
      if (BlueprintTile.TileType == ETileKind::Bond ||
          BlueprintTile.TileType == ETileKind::Unbond) {
        const uint8 EffectiveDir =
            (BlueprintTile.TileDirection + BlueprintRotation) % 6;
        const FHex NeighborHex =
            HexUtil::Neighbor(PreviewHex, (EHexDir)EffectiveDir);

        FVector CenterA = PreviewWorldPos;
        FVector CenterB;
        if (Driver->IsSphereEnabled()) {
          CenterB = Driver->MapHexToWorld(NeighborHex);
        } else {
          FVector2D CenterB2D = HexCoord::AxialToXY(
              FHexAxial(NeighborHex.Q, NeighborHex.R), TileSize, Orientation);
          CenterB = FVector(CenterB2D.X, CenterB2D.Y, GridPlaneZ);
        }

        TargetLoc = (CenterA + CenterB) * 0.5f;
      }

      // Get current transform for smoothing
      FVector CurrentLoc = PreviewComp->GetComponentLocation();
      FRotator CurrentRot = PreviewComp->GetComponentRotation();
      bool bWasVisible = PreviewComp->IsVisible();

      // Apply smoothing
      if (!bBlueprintSmoothingInitialized || !bWasVisible) {
        // Snap to target if not initialized or just became visible
        CurrentLoc = TargetLoc;
        CurrentRot = TargetRot;
      } else {
        // Interpolate towards target
        CurrentLoc = FMath::VInterpTo(CurrentLoc, TargetLoc, DeltaSeconds,
                                      BlueprintSmoothingSpeed);
        CurrentRot = FMath::RInterpTo(CurrentRot, TargetRot, DeltaSeconds,
                                      BlueprintSmoothingSpeed);
      }

      PreviewComp->SetWorldLocationAndRotation(CurrentLoc, CurrentRot);
      PreviewComp->SetVisibility(true);

      // Apply preview material based on tile type
      if (BlueprintTile.TileType == ETileKind::ClusterOutput) {
        // For ClusterOutput tiles, get the material from the template
        UMaterialInterface *MaterialToApply = nullptr;

        // Check if this is a Kind8 generated cluster (template exists and
        // LocalHex matches)
        if (Placer && Driver) {
          USimWorldHex *Sim = Driver->GetSim();
          if (Sim) {
            const USimWorldHex::FClusterTemplate *Template =
                Sim->GetClusterTemplates().Find(BlueprintTile.TileDirection);
            FHex LocalPos = BlueprintTile.LocalHex;

            // Check if this is a Kind8-generated template:
            // Template must exist AND LocalHex must match an orb position in
            // the template
            bool bIsKind8Template = false;
            if (Template) {
              // Verify LocalHex matches an orb position in the template
              for (const FOrbData &Orb : Template->Orbs) {
                if (Orb.dQ == LocalPos.Q && Orb.dR == LocalPos.R) {
                  bIsKind8Template = true;
                  // Found the matching orb - get its material
                  if (UOrbMaterialManager *MaterialManager =
                          UOrbMaterialManager::FindInWorld(this)) {
                    MaterialToApply = MaterialManager->GetMaterialForOrbType(
                        (EOrbType)Orb.Type);
                  }
                  break;
                }
              }
            }

            // If material not set (either not Kind8 or template lookup
            // failed), use TileDirection as orb type
            if (!MaterialToApply) {
              // This is a regular ClusterOutput or copy/paste - TileDirection
              // stores the orb type directly
              if (UOrbMaterialManager *MaterialManager =
                      UOrbMaterialManager::FindInWorld(this)) {
                MaterialToApply = MaterialManager->GetMaterialForOrbType(
                    (EOrbType)BlueprintTile.TileDirection);
              }
            }
          }
        }

        // Apply the material with half opacity if found
        if (MaterialToApply) {
          UMaterialInstanceDynamic *OutputMID =
              UMaterialInstanceDynamic::Create(MaterialToApply, this);
          if (OutputMID) {
            OutputMID->SetScalarParameterValue(FName("Opacity"), 0.5f);
            PreviewComp->SetMaterial(0, OutputMID);
          } else {
            PreviewComp->SetMaterial(0, MaterialToApply);
          }
        } else if (Placer && Placer->PreviewBaseMaterial) {
          // Fallback to generic preview material
          PreviewComp->SetMaterial(0, Placer->PreviewBaseMaterial);
        }
      } else if (BlueprintTile.TileType == ETileKind::Output) {
        // For Output tiles, get material based on TileDirection (which stores
        // the orb type)
        if (UOrbMaterialManager *MaterialManager =
                UOrbMaterialManager::FindInWorld(this)) {
          UMaterialInterface *MaterialToApply =
              MaterialManager->GetMaterialForOrbType(
                  (EOrbType)BlueprintTile.TileDirection);
          if (MaterialToApply) {
            UMaterialInstanceDynamic *OutputMID =
                UMaterialInstanceDynamic::Create(MaterialToApply, this);
            if (OutputMID) {
              OutputMID->SetScalarParameterValue(FName("Opacity"), 0.5f);
              PreviewComp->SetMaterial(0, OutputMID);
            } else {
              PreviewComp->SetMaterial(0, MaterialToApply);
            }
          } else if (Placer && Placer->PreviewBaseMaterial) {
            PreviewComp->SetMaterial(0, Placer->PreviewBaseMaterial);
          }
        } else if (Placer && Placer->PreviewBaseMaterial) {
          PreviewComp->SetMaterial(0, Placer->PreviewBaseMaterial);
        }
      } else if (BlueprintTile.TileType == ETileKind::Input ||
                 BlueprintTile.TileType == ETileKind::ClusterInput ||
                 BlueprintTile.TileType == ETileKind::DiscoveryClusterInput) {
        // For Input and ClusterInput tiles, get material based on
        // TileDirection (which stores the orb type)
        if (UOrbMaterialManager *MaterialManager =
                UOrbMaterialManager::FindInWorld(this)) {
          UMaterialInterface *MaterialToApply =
              MaterialManager->GetMaterialForOrbType(
                  (EOrbType)BlueprintTile.TileDirection);
          if (MaterialToApply) {
            // Input/ClusterInput tiles use the material directly (no opacity
            // change)
            PreviewComp->SetMaterial(0, MaterialToApply);
          } else if (Placer && Placer->PreviewBaseMaterial) {
            PreviewComp->SetMaterial(0, Placer->PreviewBaseMaterial);
          }
        } else if (Placer && Placer->PreviewBaseMaterial) {
          PreviewComp->SetMaterial(0, Placer->PreviewBaseMaterial);
        }
      } else if (Placer && Placer->PreviewBaseMaterial) {
        // Apply generic preview material for other tiles
        PreviewComp->SetMaterial(0, Placer->PreviewBaseMaterial);
      }
    } else {
      PreviewComp->SetVisibility(false);
    }
  }

  // Flag initialization complete after first update pass
  if (!bBlueprintSmoothingInitialized) {
    bBlueprintSmoothingInitialized = true;
  }
}

void AHexTopDownPawn::ClearBlueprintPreviewComponents() {
  for (UStaticMeshComponent *PreviewComp : BlueprintPreviewComponents) {
    if (PreviewComp) {
      PreviewComp->SetVisibility(false);
    }
  }
}

const FString AHexTopDownPawn::BlueprintLibrarySaveSlotName =
    TEXT("PlayerBlueprintLibrary");

void AHexTopDownPawn::SaveSelectionAsBlueprint() {
  if (!bBlueprintSelectionActive || SelectedTiles.Num() == 0) {
    // If no selection is active, check if we have a current blueprint
    if (BlueprintTiles.Num() > 0) {
      SaveBlueprintToLibrary(BlueprintTiles);
    }
    return;
  }

  // Create blueprint from current selection first
  CreateBlueprint();

  // Then save it
  if (BlueprintTiles.Num() > 0) {
    SaveBlueprintToLibrary(BlueprintTiles);
  }
}

void AHexTopDownPawn::SaveBlueprintToLibrary(
    const TArray<FBlueprintTile> &Blueprint) {
  if (Blueprint.Num() == 0) {
    UE_LOG(LogTemp, Warning,
           TEXT("SaveBlueprintToLibrary: Cannot save empty blueprint"));
    return;
  }

  // Load existing save game
  UPlayerBlueprintLibrarySave *SaveGameInstance =
      Cast<UPlayerBlueprintLibrarySave>(
          UGameplayStatics::LoadGameFromSlot(BlueprintLibrarySaveSlotName, 0));

  if (!SaveGameInstance) {
    // Create new save game if none exists
    SaveGameInstance = Cast<UPlayerBlueprintLibrarySave>(
        UGameplayStatics::CreateSaveGameObject(
            UPlayerBlueprintLibrarySave::StaticClass()));
  }

  if (!SaveGameInstance) {
    UE_LOG(LogTemp, Error,
           TEXT("SaveBlueprintToLibrary: Failed to create save game instance"));
    return;
  }

  // Add the blueprint to the saved list (convert to wrapper struct)
  FBlueprintData BlueprintData;
  BlueprintData.Tiles = Blueprint;
  SaveGameInstance->SavedBlueprints.Add(BlueprintData);

  // Update in-memory cache (convert back from wrapper)
  SavedBlueprints.Empty();
  for (const FBlueprintData &Data : SaveGameInstance->SavedBlueprints) {
    SavedBlueprints.Add(Data.Tiles);
  }

  // Save to disk
  if (UGameplayStatics::SaveGameToSlot(SaveGameInstance,
                                       BlueprintLibrarySaveSlotName, 0)) {
    UE_LOG(LogTemp, Log,
           TEXT("SaveBlueprintToLibrary: Saved blueprint with %d tiles (total: "
                "%d blueprints)"),
           Blueprint.Num(), SavedBlueprints.Num());
  } else {
    UE_LOG(LogTemp, Error,
           TEXT("SaveBlueprintToLibrary: Failed to save to slot"));
  }
}

void AHexTopDownPawn::LoadBlueprintFromLibrary(int32 Index) {
  if (Index < 0 || Index >= SavedBlueprints.Num()) {
    UE_LOG(LogTemp, Warning,
           TEXT("LoadBlueprintFromLibrary: Invalid index %d (total: %d)"),
           Index, SavedBlueprints.Num());
    return;
  }

  SetBlueprintForPlacement(SavedBlueprints[Index]);
}

TArray<FBlueprintData> AHexTopDownPawn::GetSavedBlueprints() const {
  // Convert from internal TArray<TArray<>> to FBlueprintData wrapper
  TArray<FBlueprintData> Result;
  for (const TArray<FBlueprintTile> &Blueprint : SavedBlueprints) {
    FBlueprintData Data;
    Data.Tiles = Blueprint;
    Result.Add(Data);
  }
  return Result;
}

void AHexTopDownPawn::RemoveBlueprintFromLibrary(int32 Index) {
  if (Index < 0 || Index >= SavedBlueprints.Num()) {
    UE_LOG(LogTemp, Warning,
           TEXT("RemoveBlueprintFromLibrary: Invalid index %d (total: %d)"),
           Index, SavedBlueprints.Num());
    return;
  }

  // Remove from in-memory cache
  SavedBlueprints.RemoveAt(Index);

  // Load existing save game
  UPlayerBlueprintLibrarySave *SaveGameInstance =
      Cast<UPlayerBlueprintLibrarySave>(
          UGameplayStatics::LoadGameFromSlot(BlueprintLibrarySaveSlotName, 0));

  if (!SaveGameInstance) {
    // Create new save game if none exists
    SaveGameInstance = Cast<UPlayerBlueprintLibrarySave>(
        UGameplayStatics::CreateSaveGameObject(
            UPlayerBlueprintLibrarySave::StaticClass()));
  }

  if (!SaveGameInstance) {
    UE_LOG(LogTemp, Error,
           TEXT("RemoveBlueprintFromLibrary: Failed to create save game "
                "instance"));
    return;
  }

  // Update save game with removed blueprint (convert to wrapper struct)
  SaveGameInstance->SavedBlueprints.Empty();
  for (const TArray<FBlueprintTile> &Blueprint : SavedBlueprints) {
    FBlueprintData BlueprintData;
    BlueprintData.Tiles = Blueprint;
    SaveGameInstance->SavedBlueprints.Add(BlueprintData);
  }

  // Save to disk
  if (UGameplayStatics::SaveGameToSlot(SaveGameInstance,
                                       BlueprintLibrarySaveSlotName, 0)) {
    UE_LOG(LogTemp, Log,
           TEXT("RemoveBlueprintFromLibrary: Removed blueprint at index %d "
                "(remaining: %d)"),
           Index, SavedBlueprints.Num());
  } else {
    UE_LOG(LogTemp, Error,
           TEXT("RemoveBlueprintFromLibrary: Failed to save to slot"));
  }
}

void AHexTopDownPawn::SetBlueprintForPlacement(
    const TArray<FBlueprintTile> &Blueprint) {
  if (Blueprint.Num() == 0) {
    UE_LOG(LogTemp, Warning,
           TEXT("SetBlueprintForPlacement: Cannot set empty blueprint"));
    return;
  }

  // Copy the blueprint to placement tiles
  BlueprintTiles = Blueprint;

  // Enter blueprint placement mode
  bBlueprintPlacementMode = true;
  bBlueprintMode = false;
  BlueprintRotation = 0;
  BlueprintPlacementOffset = FHex(0, 0);

  // Disable HexPlacementComponent preview when in blueprint mode
  if (Placer) {
    Placer->SetBlueprintMode(true);
  }

  // Reset smoothing
  bBlueprintSmoothingInitialized = false;

  UE_LOG(LogTemp, Log,
         TEXT("SetBlueprintForPlacement: Set blueprint with %d tiles for "
              "placement"),
         Blueprint.Num());
}

void AHexTopDownPawn::ToggleGridVisibility() {
  // Find the GameManager
  if (UWorld *World = GetWorld()) {
    for (TActorIterator<AGameManager> It(World); It; ++It) {
      AGameManager *GameManager = *It;
      if (GameManager) {
        GameManager->ToggleGridVisibility();
        return;
      }
    }
  }

  UE_LOG(LogTemp, Warning,
         TEXT("ToggleGridVisibility: Could not find GameManager"));
}

void AHexTopDownPawn::ToggleDebugWalkers() {
  // Find the GameManager and toggle bDebugWalkers
  if (UWorld *World = GetWorld()) {
    for (TActorIterator<AGameManager> It(World); It; ++It) {
      AGameManager *GameManager = *It;
      if (GameManager) {
        GameManager->bDebugWalkers = !GameManager->bDebugWalkers;
        UE_LOG(LogTemp, Log, TEXT("ToggleDebugWalkers: bDebugWalkers = %s"),
               GameManager->bDebugWalkers ? TEXT("true") : TEXT("false"));
        return;
      }
    }
  }

  UE_LOG(LogTemp, Warning,
         TEXT("ToggleDebugWalkers: Could not find GameManager"));
}
void AHexTopDownPawn::NextViewMode() {
  // Cycle view mode
  uint8 NextMode = (uint8)CurrentViewMode + 1;
  if (NextMode > (uint8)EViewMode::OrbView) {
    NextMode = 0;
  }
  CurrentViewMode = (EViewMode)NextMode;

  FString ModeName = TEXT("Unknown");
  switch (CurrentViewMode) {
  case EViewMode::Standard:
    ModeName = TEXT("Standard");
    break;
  case EViewMode::AdvancedFilter:
    ModeName = TEXT("Advanced Filter");
    break;
  case EViewMode::OrbView:
    ModeName = TEXT("Orb View");
    break;
  }

  if (GEngine) {
    GEngine->AddOnScreenDebugMessage(
        100, 2.0f, FColor::Cyan,
        FString::Printf(TEXT("View Mode: %s"), *ModeName));
  }

  UE_LOG(LogTemp, Log, TEXT("View Mode changed to: %s"), *ModeName);

  // 1. Handle Grid Visibility
  bool bGridShouldBeVisible = (CurrentViewMode != EViewMode::AdvancedFilter);
  if (CachedGameManager) {
    CachedGameManager->SetGridVisibility(bGridShouldBeVisible);
  }

  // 2. Handle Factory Component Visibility
  if (!CachedHexFactory) {
    for (TActorIterator<AHexFactoryActor> It(GetWorld()); It; ++It) {
      CachedHexFactory = *It;
      break;
    }
  }
  if (CachedHexFactory) {
    CachedHexFactory->SetViewMode(CurrentViewMode);
  }

  // 3. Handle Actor Visibility (Arms, Walkers, Beacons, TriggerTiles)
  if (UWorld *World = GetWorld()) {
    for (TActorIterator<ABasicArm> It(World); It; ++It) {
      It->SetActorHiddenInGame(CurrentViewMode != EViewMode::Standard);
    }
    for (TActorIterator<ABasicWalker> It(World); It; ++It) {
      It->SetActorHiddenInGame(CurrentViewMode != EViewMode::Standard);
    }
    for (TActorIterator<ARequestBeacon> It(World); It; ++It) {
      It->SetActorHiddenInGame(CurrentViewMode == EViewMode::OrbView);
    }
    for (TActorIterator<ATriggerTile> It(World); It; ++It) {
      It->SetActorHiddenInGame(CurrentViewMode == EViewMode::OrbView);
    }
  }
}

FHex AHexTopDownPawn::FindItemAnchorAt(const FHex &Hex, FItem &OutItem) const {
  if (!CachedOrbSimDriver || !CachedOrbSimDriver->GetSim()) {
    return FHex(-1, -1, -1);
  }
  USimWorldHex *Sim = CachedOrbSimDriver->GetSim();

  // Try both grid levels - starting with the one the user is currently on
  TArray<int32> LevelsToTry;
  LevelsToTry.Add(Hex.Level);
  LevelsToTry.Add(1 - Hex.Level);

  for (int32 L : LevelsToTry) {
    FHex TestHex = Hex;
    TestHex.Level = L;

    // 1. Direct hit in item map (primary anchor)
    if (FItem *Item = Sim->ItemMap.Find(TestHex)) {
      OutItem = *Item;
      return TestHex;
    }

    // 2. Check all items to see if any cluster covers this hex
    for (auto &KV : Sim->ItemMap) {
      const FHex &Anchor = KV.Key;
      const FItem &Item = KV.Value;

      // Only check clusters on this level
      if (Anchor.Level == L && Item.bIsCluster) {
        for (const FOrbData &Orb : Item.Orbs) {
          FHex AbsoluteOrbHex(Anchor.Q + Orb.dQ, Anchor.R + Orb.dR,
                              Anchor.Level);
          if (AbsoluteOrbHex == TestHex) {
            OutItem = Item;
            return Anchor;
          }
        }
      }
    }

    // 3. Check all arms to see if any held item covers this hex
    for (int32 i = 0; i < Sim->Arms.Num(); ++i) {
      const FArmState &A = Sim->Arms[i];
      if (!A.bHolding || A.BaseHex.Level != L)
        continue;

      FHex Grip = Sim->GetArmGripHex_Sim(i);
      if (!A.Held.bIsCluster) {
        if (Grip == TestHex) {
          OutItem = A.Held;
          return Grip; // Return grip hex as proxy anchor
        }
      } else {
        uint8 ccw = Sim->GetHeldCCW_Steps_Sim(i);
        for (const FOrbData &Sub : A.Held.Orbs) {
          FHex Local(Sub.dQ, Sub.dR);
          FHex Rot = Sim->Rot60CCW(Local, ccw);
          FHex AbsOrb(Grip.Q + Rot.Q, Grip.R + Rot.R, Grip.Level);
          if (AbsOrb == TestHex) {
            OutItem = A.Held;
            return Grip; // Return grip hex as proxy anchor
          }
        }
      }
    }
  }

  return FHex(-1, -1, -1);
}

TArray<FHex> AHexTopDownPawn::FindItemConnectedComponent(
    const FHex &StartHex, TArray<FHex> &OutAnchors,
    TArray<int32> &OutBondIndices, TArray<int32> &OutArmIndices) const {
  TArray<FHex> AllOrbHexes;
  OutAnchors.Empty();
  OutBondIndices.Empty();
  OutArmIndices.Empty();

  if (!CachedOrbSimDriver || !CachedOrbSimDriver->GetSim())
    return AllOrbHexes;
  USimWorldHex *Sim = CachedOrbSimDriver->GetSim();

  FItem StartItem;
  FHex StartAnchor = FindItemAnchorAt(StartHex, StartItem);
  if (StartAnchor.Q == -1)
    return AllOrbHexes;

  TArray<int32> PendingItemIds;
  TSet<int32> VisitedItemIds;
  TSet<int32> CollectedBondIds;

  // Add initial item
  PendingItemIds.Add(StartItem.Id);

  while (PendingItemIds.Num() > 0) {
    int32 CurrentId = PendingItemIds.Pop();
    if (VisitedItemIds.Contains(CurrentId))
      continue;
    VisitedItemIds.Add(CurrentId);

    // 1. Search ItemMap for this ID
    bool bFoundOnBoard = false;
    for (auto &KV : Sim->ItemMap) {
      if (KV.Value.Id == CurrentId) {
        FHex Anchor = KV.Key;
        FItem Item = KV.Value;
        bFoundOnBoard = true;

        OutAnchors.Add(Anchor);
        if (Item.bIsCluster) {
          for (const FOrbData &Orb : Item.Orbs) {
            AllOrbHexes.Add(
                FHex(Anchor.Q + Orb.dQ, Anchor.R + Orb.dR, Anchor.Level));
          }
        } else {
          AllOrbHexes.Add(Anchor);
        }
        break;
      }
    }

    // 2. Search Arms for this ID (if not on board)
    for (int32 i = 0; i < Sim->Arms.Num(); ++i) {
      const FArmState &A = Sim->Arms[i];
      if (A.bHolding && A.Held.Id == CurrentId) {
        OutArmIndices.AddUnique(i);

        // Add orb hexes for highlighting
        FHex Grip = Sim->GetArmGripHex_Sim(i);
        if (!A.Held.bIsCluster) {
          AllOrbHexes.Add(Grip);
        } else {
          uint8 ccw = Sim->GetHeldCCW_Steps_Sim(i);
          for (const FOrbData &Sub : A.Held.Orbs) {
            FHex Local(Sub.dQ, Sub.dR);
            FHex Rot = Sim->Rot60CCW(Local, ccw);
            AllOrbHexes.Add(FHex(Grip.Q + Rot.Q, Grip.R + Rot.R, Grip.Level));
          }
        }
        break;
      }
    }

    // Regardless of where it was found (board or arm), traverse bonds from it
    if (const TArray<int32> *BondList = Sim->ItemToBonds.Find(CurrentId)) {
      for (int32 BondIdx : *BondList) {
        if (Sim->Bonds.IsValidIndex(BondIdx) && Sim->Bonds[BondIdx].bActive) {
          if (!CollectedBondIds.Contains(BondIdx)) {
            CollectedBondIds.Add(BondIdx);
            OutBondIndices.Add(BondIdx);
          }

          int32 OtherId = (Sim->Bonds[BondIdx].ItemA == CurrentId)
                              ? Sim->Bonds[BondIdx].ItemB
                              : Sim->Bonds[BondIdx].ItemA;
          if (OtherId > 0 && !VisitedItemIds.Contains(OtherId)) {
            PendingItemIds.Add(OtherId);
          }
        }
      }
    }
  }

  return AllOrbHexes;
}

void AHexTopDownPawn::ClearOrbHighlightComponents() {
  for (UStaticMeshComponent *Comp : OrbHighlightComponents) {
    if (Comp) {
      Comp->DestroyComponent();
    }
  }
  OrbHighlightComponents.Empty();
}

void AHexTopDownPawn::UpdateSelectionHighlights() {
  // Hide all current selection highlights
  for (UStaticMeshComponent *Comp : SelectionHighlightComponents) {
    if (Comp) {
      Comp->SetVisibility(false);
    }
  }

  // Get the driver to access tile size and orientation
  AOrbSimDriver *Driver = CachedOrbSimDriver;
  if (!Driver) {
    Driver = AOrbSimDriver::Find(GetWorld());
    CachedOrbSimDriver = Driver;
  }
  if (!Driver)
    return;

  if (SelectedTiles.Num() == 0)
    return;

  // 1. Identify occupied tiles within selection
  struct FValidHighlight {
    FHex Hex;
    UStaticMesh *Mesh;
    float Yaw;
    FVector Scale;
    FVector WorldPos;
    bool bWorldPosSet;
  };
  TArray<FValidHighlight> ValidTiles;
  for (const FHex &Hex : SelectedTiles) {
    float RotationYaw = 0.f;
    FVector Scale = FVector(1.f);
    FVector WorldPos = FVector::ZeroVector;
    bool bWorldPosSet = false;

    UStaticMesh *MeshToUse =
        GetHighlightMeshForHex(Hex, RotationYaw, Scale, WorldPos, bWorldPosSet);

    if (MeshToUse) {
      ValidTiles.Add(
          {Hex, MeshToUse, RotationYaw, Scale, WorldPos, bWorldPosSet});
    }
  }

  // 2. Find bonds within the selection (to create unified outlines)
  TArray<int32> BondsInSelection;
  if (Driver->GetSim()) {
    USimWorldHex *Sim = Driver->GetSim();
    TSet<FHex> SelectedSet(SelectedTiles);
    for (int32 i = 0; i < Sim->Bonds.Num(); i++) {
      const USimWorldHex::FBond &B = Sim->Bonds[i];
      FHex AnchorB(B.Root.Q + B.dQ, B.Root.R + B.dR, B.Root.Level);
      if (B.bActive && SelectedSet.Contains(B.Root) &&
          SelectedSet.Contains(AnchorB)) {
        BondsInSelection.Add(i);
      }
    }
  }

  int32 TotalNeeded = ValidTiles.Num() + BondsInSelection.Num();

  if (!CachedHexFactory) {
    for (TActorIterator<AHexFactoryActor> It(GetWorld()); It; ++It) {
      CachedHexFactory = *It;
      break;
    }
  }

  // Ensure we have enough components
  while (SelectionHighlightComponents.Num() < TotalNeeded) {
    UStaticMeshComponent *NewComp = NewObject<UStaticMeshComponent>(this);
    NewComp->RegisterComponent();
    NewComp->AttachToComponent(
        RootComponent, FAttachmentTransformRules::KeepRelativeTransform);
    NewComp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    NewComp->SetCastShadow(false);

    // Apply the same outline settings as TileHighlightMesh
    NewComp->SetRenderCustomDepth(true);
    NewComp->SetCustomDepthStencilValue(1); // Outline color
    NewComp->SetRenderInMainPass(false);    // Only see outline

    // Copy material from hover highlight if it exists
    if (TileHighlightMesh && TileHighlightMesh->GetMaterial(0)) {
      NewComp->SetMaterial(0, TileHighlightMesh->GetMaterial(0));
    }

    SelectionHighlightComponents.Add(NewComp);
  }

  // Hide excess
  for (int32 i = TotalNeeded; i < SelectionHighlightComponents.Num(); ++i) {
    SelectionHighlightComponents[i]->SetVisibility(false);
  }

  float ZOffset = 5.0f;
  if (CurrentViewMode == EViewMode::OrbView && Driver->GetSim()) {
    ZOffset = Driver->GetSim()->OrbZOffset;
  }

  int32 CompIdx = 0;

  // 1. Position Tile/Actor highlights
  for (const auto &Val : ValidTiles) {
    UStaticMeshComponent *Comp = SelectionHighlightComponents[CompIdx++];
    if (Comp) {
      if (Comp->GetStaticMesh() != Val.Mesh) {
        Comp->SetStaticMesh(Val.Mesh);
        if (TileHighlightMesh && TileHighlightMesh->GetMaterial(0)) {
          Comp->SetMaterial(0, TileHighlightMesh->GetMaterial(0));
        }
      }

      FVector Pos =
          Val.bWorldPosSet ? Val.WorldPos : Driver->MapHexToWorld(Val.Hex);
      Comp->SetWorldLocation(Pos + FVector(0, 0, ZOffset));
      Comp->SetWorldRotation(FRotator(0.f, Val.Yaw, 0.f));
      Comp->SetWorldScale3D(Val.Scale);
      Comp->SetVisibility(true);
    }
  }

  // 2. Position Bond highlights
  if (Driver->GetSim()) {
    USimWorldHex *Sim = Driver->GetSim();
    UStaticMesh *BondMesh = Placer ? Placer->BondPreviewMesh : nullptr;
    for (int32 i = 0; i < BondsInSelection.Num(); i++) {
      int32 BondIdx = BondsInSelection[i];
      UStaticMeshComponent *Comp = SelectionHighlightComponents[CompIdx++];

      if (BondMesh && Comp->GetStaticMesh() != BondMesh) {
        Comp->SetStaticMesh(BondMesh);
      }

      const USimWorldHex::FBond &B = Sim->Bonds[BondIdx];
      FHex AnchorB(B.Root.Q + B.dQ, B.Root.R + B.dR, B.Root.Level);
      FVector PosA = Driver->MapHexToWorld(B.Root);
      FVector PosB = Driver->MapHexToWorld(AnchorB);
      FVector Mid = (PosA + PosB) * 0.5f;

      FVector DirVec = (PosB - PosA).GetSafeNormal();
      float Yaw = FMath::RadiansToDegrees(FMath::Atan2(DirVec.Y, DirVec.X));

      Comp->SetWorldLocation(Mid + FVector(0, 0, ZOffset));
      Comp->SetWorldRotation(FRotator(0, Yaw, 0));

      FVector BScale(1.0f);
      if (CachedHexFactory)
        BScale = CachedHexFactory->BondScale;
      Comp->SetWorldScale3D(BScale);
      Comp->SetVisibility(true);
    }
  }
}
