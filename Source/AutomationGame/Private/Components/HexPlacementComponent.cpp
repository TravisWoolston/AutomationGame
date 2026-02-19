#include "Components/HexPlacementComponent.h"
#include "Actors/BasicArm.h"
#include "Actors/BasicWalker.h"
#include "Actors/HexFactoryActor.h"
#include "Actors/OrbSimDriver.h"
#include "Actors/RequestBeacon.h"
#include "Actors/TriggerTile.h"
#include "Animation/AnimInstance.h"
#include "Components/PoseableMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Data/OrbMaterialManager.h"
#include "DrawDebugHelpers.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Framework/GameManager.h"
#include "Framework/TestLevelDirector.h"
#include "GameFramework/Actor.h"
#include "GameFramework/PlayerController.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Sim/HexCoord.h"
#include "Sim/SimWorldHex.h"
#include "Sim/Tiles.h"
#include "Sim/WalkerTypes.h"

UHexPlacementComponent::UHexPlacementComponent() {
  PrimaryComponentTick.bCanEverTick = true;
}

// Added: helper to locate ATestLevelDirector
static ATestLevelDirector *FindTLD(UWorld *W) {
  if (!W)
    return nullptr;
  for (TActorIterator<ATestLevelDirector> It(W); It; ++It) {
    return *It;
  }
  return nullptr;
}

void UHexPlacementComponent::BeginPlay() {
  Super::BeginPlay();
  if (UWorld *W = GetWorld()) {
    Driver = AOrbSimDriver::Find(W);
    if (Driver)
      Sim = Driver->GetSim();
    PC = W->GetFirstPlayerController();
    // Cache HexFactoryActor for material access
    for (TActorIterator<AHexFactoryActor> It(W); It; ++It) {
      CachedHexFactory = *It;
      break;
    }
  }

  CachedTLD = FindTLD(GetWorld());
}

static void ReacquireSimIfNeeded(UWorld *W, AOrbSimDriver *&Driver,
                                 USimWorldHex *&Sim) {
  if (!Sim || !Driver) {
    Driver = AOrbSimDriver::Find(W);
    if (Driver)
      Sim = Driver->GetSim();
  }
}
static float ComputeDirYawFromNeighbor(const UHexPlacementComponent *Self,
                                       const FHex &Base, uint8 Dir) {
  if (!Self)
    return 0.f;
  const FHex Neighbor = HexUtil::Neighbor(Base, (EHexDir)(Dir % 6));
  const FVector A = Self->HexToWorld(Base);
  const FVector B = Self->HexToWorld(Neighbor);
  const FVector2D D(B.X - A.X, B.Y - A.Y);
  if (D.IsNearlyZero())
    return 0.f;
  return FMath::RadiansToDegrees(FMath::Atan2(D.Y, D.X));
}
void UHexPlacementComponent::TickComponent(float DeltaTime, enum ELevelTick,
                                           FActorComponentTickFunction *) {
  if ((!Driver || !Sim) && GetWorld())
    ReacquireSimIfNeeded(GetWorld(), Driver, Sim);

  if (PC) {
    UpdateHoverHex();
    UpdatePreviewVisual(DeltaTime);
  }

  if (!bUseInputPolling)
    return;
  if (!Sim || !PC)
    return;

  const bool bClickL = PC->IsInputKeyDown(EKeys::LeftMouseButton);
  const bool bClickR = PC->IsInputKeyDown(EKeys::RightMouseButton);

  FVector World;
  if (bClickL && !bWasClickingL && HitPlane(World))
    DoPlaceAt(World);
  if (bClickR && !bWasClickingR && HitPlane(World))
    DoRemoveAt(World);

  bWasClickingL = bClickL;
  bWasClickingR = bClickR;
}

AActor *UHexPlacementComponent::GetAnchor() const {
  if (bOverrideAnchor && AnchorOverride)
    return AnchorOverride;
  return Driver ? Driver->GetGridAnchor() : nullptr;
}

bool UHexPlacementComponent::HitPlane(FVector &OutWorld) const {
  if (!PC)
    return false;
  float X, Y;
  if (!PC->GetMousePosition(X, Y))
    return false;
  FVector Origin, Dir;
  if (!PC->DeprojectScreenPositionToWorld(X, Y, Origin, Dir))
    return false;

  const float Z = Driver
                      ? Driver->GetGridPlaneZ(GetOwner())
                      : (GetOwner() ? GetOwner()->GetActorLocation().Z : 0.f);

  if (FMath::IsNearlyZero(Dir.Z))
    return false;
  const float T = (Z - Origin.Z) / Dir.Z;
  if (T <= 0)
    return false;
  OutWorld = Origin + T * Dir;

  if (bDebug) {
    DrawDebugSphere(GetWorld(), OutWorld, 6.f, 12, FColor::Green, false, 0.2f);
  }
  return true;
}

void UHexPlacementComponent::DoPlaceAt(const FVector &World) {
  UWorld *W = GetWorld();
  if ((!Driver || !Sim) && W)
    ReacquireSimIfNeeded(W, Driver, Sim);
  if (!Sim) {
    UE_LOG(LogTemp, Warning,
           TEXT("HexPlacementComponent: Sim missing; cannot place."));
    return;
  }

  const float Tile = Driver ? Driver->GetTileSize() : Sim->TileSizeUU;
  const AActor *Anchor = GetAnchor();
  const FVector2D OriginXY = Anchor ? FVector2D(Anchor->GetActorLocation().X,
                                                Anchor->GetActorLocation().Y)
                                    : FVector2D::ZeroVector;

  const float Yaw = Driver ? Driver->GetGridYaw() : 0.f;
  const EHexOrientation Ori =
      Driver ? Driver->GetGridOrientation() : EHexOrientation::PointyTop;

  FVector2D P(World.X - OriginXY.X, World.Y - OriginXY.Y);
  const FVector2D Local = HexCoord::Rotate2D(P, -Yaw);
  const FHexAxial A = HexCoord::XYToAxialRound(Local, Tile, Ori);
  const FHex Root{A.Q, A.R, ActiveLevel};

  FTile T{};
  switch (PlaceKind) {
  case EPlaceKind::Input:
    T = FTile{ETileKind::Input, (uint8)GetCurrentInputOrbType()};
    break;
  case EPlaceKind::Output:
    T = FTile{ETileKind::Output, (uint8)GetCurrentOutputOrbType()};
    break;
  case EPlaceKind::ClusterOutput:
    T = FTile{ETileKind::ClusterOutput, (uint8)GetCurrentOutputOrbType()};
    break;
  case EPlaceKind::ClusterInput:
    T = FTile{ETileKind::ClusterInput, (uint8)GetCurrentInputOrbType()};
    break;
  case EPlaceKind::ClusterSend:
    T = FTile{ETileKind::ClusterSend, (uint8)GetCurrentOutputOrbType()};
    break;
  case EPlaceKind::Belt:
    T = FTile{ETileKind::Belt, (uint8)BeltDir};
    break;
  case EPlaceKind::Bond:
    T = FTile{ETileKind::Bond, (uint8)BeltDir};
    break;
  case EPlaceKind::Unbond:
    T = FTile{ETileKind::Unbond, (uint8)BeltDir};
    break;
  case EPlaceKind::HelixOutput: {
    // Check if clicking on an existing HelixOutput tile to enter pairing mode
    if (Sim) {
      int32 OutputGroupIndex = Sim->GetHelixOutputGroupIndexAt(Root);
      if (OutputGroupIndex >= 0) {
        // Clicked on existing helix output - enter pairing mode for HelixInput
        StartHelixInputPairing(OutputGroupIndex);
        if (GEngine) {
          GEngine->AddOnScreenDebugMessage(
              -1, 3.0f, FColor::Cyan,
              TEXT("Helix Output selected. Click to place paired Input tiles "
                   "(must be at least 3 tiles away)."));
        }
        return;
      }
    }
    T = FTile{ETileKind::HelixOutput, 0};
    break;
  }
  case EPlaceKind::HelixInput: {
    // Only valid if we're in pairing mode
    if (PendingHelixOutputGroupIndex < 0 || HelixInputLayout.Num() == 0) {
      if (GEngine) {
        GEngine->AddOnScreenDebugMessage(
            -1, 2.0f, FColor::Red,
            TEXT("Select a HelixOutput tile first to create paired input."));
      }
      return;
    }

    // Validate that all tiles in the set can be placed
    if (!CanPlaceHelixInputSetAt(Root)) {
      if (GEngine) {
        GEngine->AddOnScreenDebugMessage(
            -1, 2.0f, FColor::Orange,
            TEXT("Invalid location - must be far enough from output center "
                 "and all tiles must be placeable."));
      }
      return;
    }

    // Remove previously paired input tiles (if any)
    if (Sim->GetHelixGroups().IsValidIndex(PendingHelixOutputGroupIndex)) {
      // Copy array because PlaceTile will modify the source array via
      // UnregisterHelixInputTile
      TArray<FHex> OldTiles =
          Sim->GetHelixGroups()[PendingHelixOutputGroupIndex].PairedInputTiles;
      for (const FHex &OldHex : OldTiles) {
        Sim->PlaceTile(OldHex, FTile{ETileKind::Empty, 0});
      }
    }

    // Place all HelixInput tiles at once
    TArray<FHex> InputHexes = GetHelixInputPreviewHexes(Root);
    for (const FHex &InputHex : InputHexes) {
      FTile InputTile{ETileKind::HelixInput,
                      (uint8)PendingHelixOutputGroupIndex};
      Sim->PlaceTile(InputHex, InputTile);
      // We will call PairHelixInput below to register them all and set the
      // correct center
    }

    // Link the group to the new input tiles and explicitly set the center to
    // align with placement (Root)
    Sim->PairHelixInput(PendingHelixOutputGroupIndex, InputHexes, Root);

    if (GEngine) {
      GEngine->AddOnScreenDebugMessage(
          -1, 2.0f, FColor::Green,
          FString::Printf(TEXT("Placed %d HelixInput tiles centered at "
                               "(%d,%d) paired to output group %d"),
                          InputHexes.Num(), Root.Q, Root.R,
                          PendingHelixOutputGroupIndex));
    }

    // Exit pairing mode after successful placement
    CancelHelixInputPairing();
    return; // Skip normal tile placement flow
  }
  case EPlaceKind::Source:
    T = FTile{ETileKind::Source, 0};
    break;
  case EPlaceKind::Arm: {
    AActor *GMActor = nullptr;
    for (TActorIterator<AActor> It(GetWorld()); It; ++It)
      if (It->IsA<AGameManager>()) {
        GMActor = *It;
        break;
      }

    if (AGameManager *GM = Cast<AGameManager>(GMActor)) {
      GM->SpawnBasicArmAt(FHex{A.Q, A.R, ActiveLevel}, BeltDir % 6,
                          TArray<FArmInstr>(), true);
    } else {
      UE_LOG(LogTemp, Warning,
             TEXT("HexPlacementComponent: cannot spawn Arm (no GameManager)."));
    }

    // Scenario capture for arm
    if (bAutoScenarioCapture) {
      if (!CachedTLD)
        CachedTLD = FindTLD(GetWorld());
      if (CachedTLD) {
        CachedTLD->UpsertArmSpec(A.Q, A.R, BeltDir % 6, TArray<FArmInstr>(),
                                 true, ActiveLevel);
      }
    }
    return;
  }
  case EPlaceKind::Walker: {
    AActor *GMActor = nullptr;
    for (TActorIterator<AActor> It(GetWorld()); It; ++It)
      if (It->IsA<AGameManager>()) {
        GMActor = *It;
        break;
      }

    if (AGameManager *GM = Cast<AGameManager>(GMActor)) {
      FHex StartingLocation(A.Q, A.R, ActiveLevel);
      TArray<FWalkerInstr> WalkerProgram = GM->DefaultWalkerProgram;
      ABasicWalker *Walker = GM->SpawnBasicWalkerAt(FHex{A.Q, A.R, ActiveLevel},
                                                    WalkerProgram, true);
      if (Walker) {
        Walker->HomeHex = StartingLocation;
        WalkerProgram = Walker->Program;
      }

      // Scenario capture for walker
      if (bAutoScenarioCapture) {
        if (!CachedTLD)
          CachedTLD = FindTLD(GetWorld());
        if (CachedTLD) {
          CachedTLD->UpsertWalkerSpec(A.Q, A.R, StartingLocation, WalkerProgram,
                                      Walker ? Walker->bLoopProgram : true,
                                      ActiveLevel);
        }
      }
    } else {
      UE_LOG(
          LogTemp, Warning,
          TEXT("HexPlacementComponent: cannot spawn Walker (no GameManager)."));
    }
    return;
  }
  case EPlaceKind::RequestBeacon: {
    // Find GameManager
    AGameManager *GM = nullptr;
    for (TActorIterator<AGameManager> It(GetWorld()); It; ++It) {
      GM = *It;
      break;
    }

    if (!GM) {
      UE_LOG(
          LogTemp, Warning,
          TEXT("HexPlacementComponent: cannot spawn Beacon (no GameManager)."));
      return;
    }
    // Calculate rotation for Pickup to match grid orientation
    FRotator PickupRotation = FRotator::ZeroRotator;
    if (Driver) {
      PickupRotation.Yaw = Driver->GetGridYaw();
      if (Driver->GetGridOrientation() == EHexOrientation::FlatTop) {
        PickupRotation.Yaw += 30.f;
      }
    }

    if (BeaconPlacementStep == 0) // Pickup Beacon
    {
      // Check if we can place here (no arms)
      bool bHasArmAtLoc = false;
      if (Sim) {
        for (const FArmState &ArmState : Sim->Arms) {
          if (ArmState.BaseHex.Q == A.Q && ArmState.BaseHex.R == A.R) {
            bHasArmAtLoc = true;
            break;
          }
        }
      }
      if (bHasArmAtLoc) {
        UE_LOG(LogTemp, Warning,
               TEXT("HexPlacementComponent: Cannot place Beacon - arm present "
                    "at pickup location."));
        return;
      }

      // Spawn Pickup Beacon
      UClass *BeaconClass = GM->RequestBeaconClass
                                ? GM->RequestBeaconClass.Get()
                                : ARequestBeacon::StaticClass();
      FVector CenteredLocation = HexToWorld(Root);
      ARequestBeacon *Beacon = GetWorld()->SpawnActor<ARequestBeacon>(
          BeaconClass, CenteredLocation, PickupRotation);
      if (Beacon) {
        Beacon->Q = A.Q;
        Beacon->R = A.R;
        Beacon->Level = ActiveLevel;
        Beacon->SyncWorldLocation();
        Beacon->BeaconType = EBeaconType::Relocate;
        Beacon->BeaconRole = EBeaconRole::PickUp;
        Beacon->SetState(
            EBeaconState::NotReady); // Not ready until pair is complete

        // Scenario capture
        if (bAutoScenarioCapture) {
          if (!CachedTLD)
            CachedTLD = FindTLD(GetWorld());
          if (CachedTLD) {
            CachedTLD->UpsertBeaconSpec(A.Q, A.R, EBeaconType::Relocate,
                                        EBeaconRole::PickUp, PickupRotation.Yaw,
                                        ActiveLevel);
          }
        }

        PendingPickupBeacon = Beacon;
        BeaconPlacementStep = 1; // Move to DropOff step
        UE_LOG(
            LogTemp, Log,
            TEXT("Placed Pickup Beacon at (%d,%d). Place DropOff Beacon next."),
            A.Q, A.R);
      }
    } else if (BeaconPlacementStep == 1) // DropOff Beacon
    {
      // Spawn DropOff Beacon with same default rotation as Pickup
      // (User can rotate with BeltDir if needed, but default matches Pickup)
      FRotator Rotation = FRotator::ZeroRotator;
      if (Driver) {
        Rotation.Yaw = Driver->GetGridYaw();
        if (Driver->GetGridOrientation() == EHexOrientation::FlatTop) {
          Rotation.Yaw += 30.f;
        }
      }

      UClass *BeaconClass = GM->RequestBeaconClass
                                ? GM->RequestBeaconClass.Get()
                                : ARequestBeacon::StaticClass();
      FVector CenteredLocation = HexToWorld(Root);
      ARequestBeacon *Beacon = GetWorld()->SpawnActor<ARequestBeacon>(
          BeaconClass, CenteredLocation, Rotation);
      if (Beacon) {
        Beacon->Q = A.Q;
        Beacon->R = A.R;
        Beacon->Level = ActiveLevel;
        Beacon->SyncWorldLocation();
        Beacon->BeaconType = EBeaconType::Relocate;
        Beacon->BeaconRole = EBeaconRole::DropOff;
        Beacon->SetState(EBeaconState::Ready); // Ready now

        // Link beacons
        if (PendingPickupBeacon && IsValid(PendingPickupBeacon)) {
          Beacon->PairedBeacon = PendingPickupBeacon;
          PendingPickupBeacon->PairedBeacon = Beacon;
          // Removed: PendingPickupBeacon->SetState(EBeaconState::Ready);
          // Let CheckReadiness() handle state based on occupancy
          PendingPickupBeacon->UpdateVisuals(); // Force visual update
        } else {
          // If pickup beacon is gone, just destroy this one and reset
          Beacon->Destroy();
        }

        // Scenario capture
        if (bAutoScenarioCapture) {
          if (!CachedTLD)
            CachedTLD = FindTLD(GetWorld());
          if (CachedTLD) {
            CachedTLD->UpsertBeaconSpec(A.Q, A.R, EBeaconType::Relocate,
                                        EBeaconRole::DropOff, Rotation.Yaw,
                                        ActiveLevel);

            // Update Pickup beacon spec to save its pair relationship
            if (PendingPickupBeacon && IsValid(PendingPickupBeacon)) {
              CachedTLD->UpsertBeaconSpec(
                  PendingPickupBeacon->Q, PendingPickupBeacon->R,
                  EBeaconType::Relocate, EBeaconRole::PickUp,
                  PickupRotation.Yaw, ActiveLevel);
            }
          }
        }

        PendingPickupBeacon = nullptr;
        BeaconPlacementStep = 0; // Reset
        UE_LOG(LogTemp, Log,
               TEXT("Placed DropOff Beacon at (%d,%d). Pair complete."), A.Q,
               A.R);
      }
    }
    return;
  }
  case EPlaceKind::GatherScatter: {
    // Find GameManager
    AGameManager *GM = nullptr;
    for (TActorIterator<AGameManager> It(GetWorld()); It; ++It) {
      GM = *It;
      break;
    }

    if (!GM) {
      UE_LOG(LogTemp, Warning,
             TEXT("HexPlacementComponent: cannot spawn GatherScatter Beacon "
                  "(no GameManager)."));
      return;
    }

    // Calculate rotation to match grid orientation
    FRotator BeaconRotation = FRotator::ZeroRotator;
    if (Driver) {
      BeaconRotation.Yaw = Driver->GetGridYaw();
      if (Driver->GetGridOrientation() == EHexOrientation::FlatTop) {
        BeaconRotation.Yaw += 30.f;
      }
    }

    if (GatherScatterPlacementStep == 0) // Gather Beacon
    {
      // Check no arm at location
      bool bHasArmAtLoc = false;
      if (Sim) {
        for (const FArmState &ArmState : Sim->Arms) {
          if (ArmState.BaseHex.Q == A.Q && ArmState.BaseHex.R == A.R) {
            bHasArmAtLoc = true;
            break;
          }
        }
      }
      if (bHasArmAtLoc) {
        UE_LOG(LogTemp, Warning,
               TEXT("HexPlacementComponent: Cannot place Gather Beacon - arm "
                    "present."));
        return;
      }

      // Spawn Gather Beacon
      UClass *BeaconClass = GM->RequestBeaconClass
                                ? GM->RequestBeaconClass.Get()
                                : ARequestBeacon::StaticClass();
      FVector CenteredLocation = HexToWorld(Root);
      ARequestBeacon *Beacon = GetWorld()->SpawnActor<ARequestBeacon>(
          BeaconClass, CenteredLocation, BeaconRotation);
      if (Beacon) {
        Beacon->Q = A.Q;
        Beacon->R = A.R;
        Beacon->Level = ActiveLevel;
        Beacon->SyncWorldLocation();
        Beacon->BeaconType = EBeaconType::GatherScatter;
        Beacon->BeaconRole = EBeaconRole::Gather;
        Beacon->Radius = GatherScatterRadius;
        Beacon->MaxGatherCount = 10;
        // Set orb filter type from cycling
        Beacon->FilterOrbType = (uint8)GetCurrentGatherOrbType();
        Beacon->SetState(EBeaconState::NotReady);

        PendingGatherBeacon = Beacon;
        GatherScatterPlacementStep = 1; // Move to radius drag step
        UE_LOG(LogTemp, Log,
               TEXT("Placed Gather Beacon at (%d,%d). Hold Place to set "
                    "radius."),
               A.Q, A.R);
        if (GEngine) {
          GEngine->AddOnScreenDebugMessage(
              200, 3.0f, FColor::Yellow,
              TEXT("Gather placed. Hold Place key + drag mouse to set "
                   "radius."));
        }
      }
    } else if (GatherScatterPlacementStep == 2) // Scatter Beacon
    {
      // Spawn Scatter Beacon
      UClass *BeaconClass = GM->RequestBeaconClass
                                ? GM->RequestBeaconClass.Get()
                                : ARequestBeacon::StaticClass();
      FVector CenteredLocation = HexToWorld(Root);
      ARequestBeacon *Beacon = GetWorld()->SpawnActor<ARequestBeacon>(
          BeaconClass, CenteredLocation, BeaconRotation);
      if (Beacon) {
        Beacon->Q = A.Q;
        Beacon->R = A.R;
        Beacon->Level = ActiveLevel;
        Beacon->SyncWorldLocation();
        Beacon->BeaconType = EBeaconType::GatherScatter;
        Beacon->BeaconRole = EBeaconRole::Scatter;
        Beacon->Radius = GatherScatterRadius;
        // Copy orb filter from Gather beacon
        if (PendingGatherBeacon && IsValid(PendingGatherBeacon)) {
          Beacon->FilterOrbType = PendingGatherBeacon->FilterOrbType;
          Beacon->MaxGatherCount = PendingGatherBeacon->MaxGatherCount;
        }
        Beacon->SetState(EBeaconState::Ready);

        // Link beacons
        if (PendingGatherBeacon && IsValid(PendingGatherBeacon)) {
          Beacon->PairedBeacon = PendingGatherBeacon;
          PendingGatherBeacon->PairedBeacon = Beacon;
          PendingGatherBeacon->UpdateVisuals();
        } else {
          Beacon->Destroy();
          return;
        }

        PendingScatterBeacon = Beacon;
        GatherScatterPlacementStep = 3; // Move to scatter radius drag step
        UE_LOG(LogTemp, Log,
               TEXT("Placed Scatter Beacon at (%d,%d). Hold Place to set "
                    "radius."),
               A.Q, A.R);
        if (GEngine) {
          GEngine->AddOnScreenDebugMessage(
              201, 3.0f, FColor::Yellow,
              TEXT("Scatter placed. Hold Place key + drag mouse to set "
                   "radius."));
        }
      }
    }
    return;
  }
  case EPlaceKind::TriggerTile: {
    // Find GameManager
    AGameManager *GM = nullptr;
    for (TActorIterator<AGameManager> It(GetWorld()); It; ++It) {
      GM = *It;
      break;
    }

    if (!GM) {
      UE_LOG(LogTemp, Warning,
             TEXT("HexPlacementComponent: cannot spawn TriggerTile (no "
                  "GameManager)."));
      return;
    }

    if (TriggerTilePlacementStep == 0) // TriggerTile location
    {
      // Check for arms at this location (can't place tile itself on arms)
      bool bHasArmAtLoc = false;
      if (Sim) {
        for (const FArmState &ArmState : Sim->Arms) {
          if (ArmState.BaseHex.Q == A.Q && ArmState.BaseHex.R == A.R) {
            bHasArmAtLoc = true;
            break;
          }
        }
      }
      if (bHasArmAtLoc) {
        UE_LOG(LogTemp, Warning,
               TEXT("HexPlacementComponent: Cannot place TriggerTile - arm "
                    "present at tile location."));
        return;
      }

      // Calculate rotation to match grid orientation
      FRotator TileRotation = FRotator::ZeroRotator;
      if (Driver) {
        TileRotation.Yaw = Driver->GetGridYaw();
        if (Driver->GetGridOrientation() == EHexOrientation::FlatTop) {
          TileRotation.Yaw += 30.f;
        }
      }

      // Check if there is already a TriggerTile here
      ATriggerTile *ExistingTrigger = nullptr;
      if (W) {
        for (TActorIterator<ATriggerTile> It(W); It; ++It) {
          if (It->Q == A.Q && It->R == A.R && It->Level == ActiveLevel) {
            ExistingTrigger = *It;
            break;
          }
        }
      }

      if (ExistingTrigger) {
        PendingTriggerTile = ExistingTrigger;
        TriggerTilePlacementStep = 1;
        if (GEngine) {
          GEngine->AddOnScreenDebugMessage(
              -1, 3.0f, FColor::Cyan,
              TEXT("Selected existing TriggerTile. Click on a belt or arm to "
                   "toggle owner."));
        }
        return;
      }

      // Spawn TriggerTile (existing logic)
      UClass *TriggerClass = GM->TriggerTileClass ? GM->TriggerTileClass.Get()
                                                  : ATriggerTile::StaticClass();
      FVector CenteredLocation = HexToWorld(Root);
      ATriggerTile *TriggerTile = GetWorld()->SpawnActor<ATriggerTile>(
          TriggerClass, CenteredLocation, TileRotation);
      if (TriggerTile) {
        TriggerTile->Q = A.Q;
        TriggerTile->R = A.R;
        TriggerTile->Level = ActiveLevel;
        TriggerTile->SyncWorldLocation();
        TriggerTile->TriggerOrbType = GetCurrentTriggerOrbType();
        TriggerTile->Variant = CurrentTriggerVariant;
        TriggerTile->UpdateVisuals();

        PendingTriggerTile = TriggerTile;
        TriggerTilePlacementStep = 1; // Move to owner selection step

        // Save to scenario (initial creation with no owners)
        if (!CachedTLD)
          CachedTLD = FindTLD(GetWorld());
        if (CachedTLD) {
          CachedTLD->UpsertTriggerTileSpec(
              TriggerTile->Q, TriggerTile->R, TriggerTile->TriggerOrbType,
              TriggerTile->Variant, TriggerTile->GetActorRotation().Yaw,
              ActiveLevel);
        }
      }
    } else if (TriggerTilePlacementStep == 1) // Owner selection
    {
      // Check if clicked tile has a belt or arm
      bool bHasValidOwner = false;
      ETriggerTileOwnerType ValidOwnerType = ETriggerTileOwnerType::Arm;

      // Check for arms FIRST (precedence over belts)
      if (Sim) {
        for (const auto &ArmState : Sim->Arms) {
          if (ArmState.BaseHex == Root) {
            bHasValidOwner = true;
            ValidOwnerType = ETriggerTileOwnerType::Arm;
            break;
          }
        }
      }

      // Check for belts SECOND
      if (!bHasValidOwner && Sim && Sim->TileMap.Contains(Root)) {
        const FTile *ClickedTile = Sim->TileMap.Find(Root);
        if (ClickedTile && ClickedTile->Kind == ETileKind::Belt) {
          bHasValidOwner = true;
          ValidOwnerType = ETriggerTileOwnerType::Belt;
        }
      }

      if (!bHasValidOwner) {
        // Cancel placement - no valid owner selected
        UE_LOG(LogTemp, Warning,
               TEXT("HexPlacementComponent: No arm or belt at (%d,%d) - "
                    "canceling TriggerTile placement."),
               A.Q, A.R);
        if (GEngine) {
          GEngine->AddOnScreenDebugMessage(
              -1, 2.0f, FColor::Red,
              TEXT("No arm or belt at selected location. TriggerTile "
                   "placement "
                   "canceled."));
        }

        if (PendingTriggerTile && IsValid(PendingTriggerTile)) {
          PendingTriggerTile->Destroy();
          PendingTriggerTile = nullptr;
        }
        TriggerTilePlacementStep = 0;
        return;
      }

      // Valid owner found
      if (PendingTriggerTile && IsValid(PendingTriggerTile)) {
        PendingTriggerTile->ToggleOwner(A.Q, A.R, ActiveLevel, ValidOwnerType);

        UE_LOG(LogTemp, Log,
               TEXT("TriggerTile owner toggled for %s at (%d,%d) level %d."),
               ValidOwnerType == ETriggerTileOwnerType::Belt ? TEXT("belt")
                                                             : TEXT("arm"),
               A.Q, A.R, ActiveLevel);

        // Save to scenario via TestLevelDirector
        if (!CachedTLD)
          CachedTLD = FindTLD(GetWorld());
        if (CachedTLD) {
          CachedTLD->ToggleTriggerTileOwnerSpec(
              PendingTriggerTile->Q, PendingTriggerTile->R,
              PendingTriggerTile->Level, A.Q, A.R, ActiveLevel, ValidOwnerType);
        }

        if (GEngine) {
          GEngine->AddOnScreenDebugMessage(
              -1, 2.0f, FColor::Green,
              FString::Printf(TEXT("TriggerTile at (%d,%d) toggled owner %s at "
                                   "(%d,%d) level %d"),
                              PendingTriggerTile->Q, PendingTriggerTile->R,
                              ValidOwnerType == ETriggerTileOwnerType::Belt
                                  ? TEXT("belt")
                                  : TEXT("arm"),
                              A.Q, A.R, ActiveLevel));
        }
      }

      PendingTriggerTile = nullptr;
      TriggerTilePlacementStep = 0; // Reset
    }
    return;
  }
  default:
    UE_LOG(LogTemp, Warning,
           TEXT("HexPlacementComponent: PlaceKind unset; skip."));
    return;
  }

  // For outputs and cluster outputs, remove any existing tile first to avoid
  // overlap
  if (T.Kind == ETileKind::Output || T.Kind == ETileKind::ClusterOutput) {
    // Check if there's already a tile at this location
    if (Sim->TileMap.Contains(Root)) {
      // Remove the existing tile first
      Sim->PlaceTile(Root, FTile{ETileKind::Empty, 0});
    }
  }

  // Early out if placing identical tile (prevents redundant rebuilds during
  // "painting")
  if (Sim->TileMap.Contains(Root)) {
    const FTile *Existing = Sim->TileMap.Find(Root);
    if (Existing && Existing->Kind == T.Kind && Existing->Arg == T.Arg) {
      return;
    }
  }

  // Check if location is already occupied (except for Empty tiles which are
  // allowed to clear)
  if (T.Kind != ETileKind::Empty && Sim->TileMap.Contains(Root)) {
    const FTile *ExistingTile = Sim->TileMap.Find(Root);
    if (ExistingTile && ExistingTile->Kind != ETileKind::Empty) {
      // Allow Bond tiles to be placed on Input/Output/ClusterOutput tiles
      // (they will auto-convert or expand clusters)
      bool bAllowBondOnInputOutput =
          (T.Kind == ETileKind::Bond &&
           (ExistingTile->Kind == ETileKind::Input ||
            ExistingTile->Kind == ETileKind::Output ||
            ExistingTile->Kind == ETileKind::InputBond ||
            ExistingTile->Kind == ETileKind::OutputBond ||
            ExistingTile->Kind == ETileKind::ClusterInput ||
            ExistingTile->Kind == ETileKind::ClusterOutput));

      if (!bAllowBondOnInputOutput) {
        UE_LOG(LogTemp, Warning,
               TEXT("HexPlacementComponent: Cannot place %s at (%d,%d) - "
                    "location already occupied by %s"),
               *StaticEnum<ETileKind>()->GetNameStringByValue((int64)T.Kind),
               Root.Q, Root.R,
               *StaticEnum<ETileKind>()->GetNameStringByValue(
                   (int64)ExistingTile->Kind));
        if (GEngine)
          GEngine->AddOnScreenDebugMessage(
              -1, 1.5f, FColor::Red,
              FString::Printf(
                  TEXT("Cannot place %s - location occupied by %s"),
                  *StaticEnum<ETileKind>()->GetNameStringByValue((int64)T.Kind),
                  *StaticEnum<ETileKind>()->GetNameStringByValue(
                      (int64)ExistingTile->Kind)));
        return;
      }
    }
  }

  // Attempt placement
  const bool bPlaced = Sim->PlaceTile(Root, T);
  if (!bPlaced) {
    if (T.Kind == ETileKind::Bond) {
      const FHex Partner = HexUtil::Neighbor(Root, (EHexDir)(BeltDir % 6));
      UE_LOG(LogTemp, Warning,
             TEXT("HexPlacementComponent: Bond placement FAILED at Root(%d,%d) "
                  "Partner(%d,%d). Partner occupied by blocking tile."),
             Root.Q, Root.R, Partner.Q, Partner.R);
      if (GEngine)
        GEngine->AddOnScreenDebugMessage(
            -1, 1.5f, FColor::Red,
            FString::Printf(TEXT("Bond blocked (%d,%d)->(%d,%d)"), Root.Q,
                            Root.R, Partner.Q, Partner.R));
    } else {
      UE_LOG(LogTemp, Warning,
             TEXT("HexPlacementComponent: Placement failed at (%d,%d)."),
             Root.Q, Root.R);
    }
    return;
  }

  // Success
  if (bPlaced) {
    // Update Sim BeltTileOrbTypes for Belt placements
    if (T.Kind == ETileKind::Belt) {
      EOrbType BO = GetCurrentBeltOrbType();
      if (BO != EOrbType::None) {
        Sim->BeltTileOrbTypes.Add(Root, (uint8)BO);
      } else {
        Sim->BeltTileOrbTypes.Remove(Root);
      }
    } else {
      // If we replaced a belt with something else, clear the orb type filter
      Sim->BeltTileOrbTypes.Remove(Root);
    }

    // Scenario capture (tiles only)
    if (bAutoScenarioCapture) {
      if (!CachedTLD)
        CachedTLD = FindTLD(GetWorld());
      if (CachedTLD) {
        // For ClusterOutput tiles, save with orb type
        if (T.Kind == ETileKind::ClusterOutput) {
          CachedTLD->UpsertClusterOutputTileSpec(
              Root, 0, T.Arg); // Template ID = 0 (not used), T.Arg = orb type
        } else if (T.Kind == ETileKind::Belt) {
          CachedTLD->UpsertBeltTileSpec(Root, T.Arg,
                                        (uint8)GetCurrentBeltOrbType());

        } else {
          CachedTLD->UpsertTileSpec(Root, T.Kind, T.Arg);
        }
      }
    }
  }

  // Force GameManager to rebuild grid to reflect tile changes
  if (bPlaced) {
    if (UWorld *GameWorld = GetWorld()) {
      for (TActorIterator<AGameManager> It(GameWorld); It; ++It) {
        AGameManager *GameManager = *It;
        if (GameManager) {
          GameManager->Rebuild();
          break;
        }
      }
    }
  }

  // Success logging
  if (bDebug) {
    if (T.Kind == ETileKind::Belt) {
      UE_LOG(LogTemp, Log, TEXT("Placed Belt at Hex(%d,%d) Dir=%d"), Root.Q,
             Root.R, (int32)BeltDir);
    } else if (T.Kind == ETileKind::Bond) {
      const FHex Partner = HexUtil::Neighbor(Root, (EHexDir)(BeltDir % 6));
      UE_LOG(LogTemp, Log,
             TEXT("Placed Bond at Root(%d,%d) -> Partner(%d,%d) Dir=%d"),
             Root.Q, Root.R, Partner.Q, Partner.R, (int32)BeltDir);
    } else {
      const FString KindName =
          StaticEnum<ETileKind>()->GetNameStringByValue((int64)T.Kind);
      UE_LOG(LogTemp, Log, TEXT("Placed %s at Hex(%d,%d)"), *KindName, Root.Q,
             Root.R);
    }
  }
}

void UHexPlacementComponent::DoRemoveAt(const FVector &World) {
  UWorld *W = GetWorld();
  if ((!Driver || !Sim) && W)
    ReacquireSimIfNeeded(W, Driver, Sim);
  if (!Sim)
    return;

  const float Tile = Driver ? Driver->GetTileSize() : Sim->TileSizeUU;
  const AActor *Anchor = GetAnchor();
  const FVector2D OriginXY = Anchor ? FVector2D(Anchor->GetActorLocation().X,
                                                Anchor->GetActorLocation().Y)
                                    : FVector2D::ZeroVector;

  const float Yaw = Driver ? Driver->GetGridYaw() : 0.f;
  const EHexOrientation Ori =
      Driver ? Driver->GetGridOrientation() : EHexOrientation::PointyTop;

  FVector2D P(World.X - OriginXY.X, World.Y - OriginXY.Y);
  const FVector2D Local = HexCoord::Rotate2D(P, -Yaw);
  const FHexAxial A = HexCoord::XYToAxialRound(Local, Tile, Ori);

  const FHex Root{A.Q, A.R, ActiveLevel};
  Sim->PlaceTile(Root, FTile{ETileKind::Empty, 0});
  if (bDebug) {
    UE_LOG(LogTemp, Log,
           TEXT("HexPlacementComponent: Removed tile at Hex(%d,%d)"), A.Q, A.R);
  }

  // Remove walkers at this location
  if (W) {
    for (TActorIterator<ABasicWalker> It(W); It; ++It) {
      if (It->Q == A.Q && It->R == A.R) {
        // Remove from simulation first
        if (Sim && It->SimWalkerIndex >= 0 &&
            It->SimWalkerIndex < Sim->Walkers.Num()) {
          Sim->Walkers.RemoveAt(It->SimWalkerIndex);
          // Adjust indices for walkers after the removed one
          for (TActorIterator<ABasicWalker> OtherIt(W); OtherIt; ++OtherIt) {
            if (OtherIt->SimWalkerIndex > It->SimWalkerIndex) {
              OtherIt->SimWalkerIndex--;
            }
          }
        }
        It->Destroy();
        break;
      }
    }
  }

  // Remove arms at this location (existing code)
  if (W) {
    for (TActorIterator<ABasicArm> It(W); It; ++It) {
      if (It->Q == A.Q && It->R == A.R) {
        // Remove from simulation first
        if (Sim && It->SimArmIndex >= 0 && It->SimArmIndex < Sim->Arms.Num()) {
          Sim->Arms.RemoveAt(It->SimArmIndex);
          // Adjust indices for arms after the removed one
          for (TActorIterator<ABasicArm> OtherIt(W); OtherIt; ++OtherIt) {
            if (OtherIt->SimArmIndex > It->SimArmIndex) {
              OtherIt->SimArmIndex--;
            }
          }
        }
        It->Destroy();
        break;
      }
    }
  }

  if (bDebug && GEngine) {
    GEngine->AddOnScreenDebugMessage(
        -1, 1.5f, FColor::Yellow,
        FString::Printf(TEXT("Removed tile at Hex(%d,%d)"), A.Q, A.R));
  }

  // Scenario capture removal
  if (bAutoScenarioCapture) {
    if (!CachedTLD)
      CachedTLD = FindTLD(GetWorld());
    if (CachedTLD) {
      CachedTLD->RemoveTileSpec(FHex{A.Q, A.R, ActiveLevel});
      CachedTLD->RemoveArmSpec(A.Q, A.R,
                               ActiveLevel); // in case an arm spec was there
      CachedTLD->RemoveWalkerSpec(
          A.Q, A.R, ActiveLevel); // in case a walker spec was there
    }
  }
}

void UHexPlacementComponent::HandlePlace(bool bPressed) {
  if (!bPressed)
    return;
  FVector World;
  if (HitPlane(World))
    DoPlaceAt(World);
}

void UHexPlacementComponent::HandleRemove(bool bPressed) {
  if (!bPressed)
    return;
  FVector World;
  if (HitPlane(World))
    DoRemoveAt(World);
}

void UHexPlacementComponent::RotateCW() { BeltDir = (BeltDir + 5) % 6; }

void UHexPlacementComponent::RotateCCW() { BeltDir = (BeltDir + 1) % 6; }

void UHexPlacementComponent::SetPlaceKind(EPlaceKind NewKind) {
  if (PlaceKind == EPlaceKind::RequestBeacon &&
      NewKind != EPlaceKind::RequestBeacon) {
    // Switching away from beacon mode, cancel pending
    if (PendingPickupBeacon && IsValid(PendingPickupBeacon)) {
      PendingPickupBeacon->Destroy();
      PendingPickupBeacon = nullptr;
    }
    BeaconPlacementStep = 0;
  }

  if (PlaceKind == EPlaceKind::TriggerTile &&
      NewKind != EPlaceKind::TriggerTile) {
    // Switching away from trigger tile mode, cancel pending
    if (PendingTriggerTile && IsValid(PendingTriggerTile)) {
      PendingTriggerTile->Destroy();
      PendingTriggerTile = nullptr;
    }
    TriggerTilePlacementStep = 0;
  }

  // Clear helix pairing state when switching away from HelixInput mode
  if (PlaceKind == EPlaceKind::HelixInput &&
      NewKind != EPlaceKind::HelixInput) {
    CancelHelixInputPairing();
  }

  // Clear GatherScatter state when switching away
  if (PlaceKind == EPlaceKind::GatherScatter &&
      NewKind != EPlaceKind::GatherScatter) {
    PendingGatherBeacon = nullptr;
    PendingScatterBeacon = nullptr;
    GatherScatterPlacementStep = 0;
    bGatherRadiusDragging = false;
  }

  // Reset Belt orb type when switching to Belt
  if (NewKind == EPlaceKind::Belt) {
    CurrentBeltOrbTypeIndex = 0;
  }

  PlaceKind = NewKind;
}

void UHexPlacementComponent::CycleNextType() {
  // Get the list of valid orb types (those with assigned materials)
  TArray<EOrbType> ValidTypes = GetValidOrbTypes();
  if (ValidTypes.Num() == 0)
    return; // No valid types to cycle through

  // Cycle the appropriate type based on current placement mode
  if (PlaceKind == EPlaceKind::Input) {
    CurrentInputOrbTypeIndex =
        (CurrentInputOrbTypeIndex + 1) % ValidTypes.Num();
  } else if (PlaceKind == EPlaceKind::Output ||
             PlaceKind == EPlaceKind::ClusterOutput ||
             PlaceKind == EPlaceKind::ClusterSend) {
    CurrentOutputOrbTypeIndex =
        (CurrentOutputOrbTypeIndex + 1) % ValidTypes.Num();
  } else if (PlaceKind == EPlaceKind::Belt) {
    // Belts include 'None' (index 0) + valid types
    CurrentBeltOrbTypeIndex =
        (CurrentBeltOrbTypeIndex + 1) % (ValidTypes.Num() + 1);
  } else if (PlaceKind == EPlaceKind::ClusterInput) {
    // ClusterInputs cycle through orb types like regular Inputs
    CurrentInputOrbTypeIndex =
        (CurrentInputOrbTypeIndex + 1) % ValidTypes.Num();
  } else if (PlaceKind == EPlaceKind::TriggerTile) {
    // TriggerTiles include Void option for "any orb"
    TArray<EOrbType> TriggerTypes = ValidTypes;
    if (!TriggerTypes.Contains(EOrbType::Void)) {
      TriggerTypes.Add(EOrbType::Void);
    }
    CurrentTriggerOrbTypeIndex =
        (CurrentTriggerOrbTypeIndex + 1) % TriggerTypes.Num();
  } else if (PlaceKind == EPlaceKind::GatherScatter) {
    // GatherScatter cycles through orb types (0 = any/unfiltered)
    TArray<EOrbType> GatherTypes;
    GatherTypes.Add(EOrbType::None); // None = any orb
    GatherTypes.Append(ValidTypes);
    CurrentGatherOrbTypeIndex =
        (CurrentGatherOrbTypeIndex + 1) % GatherTypes.Num();
    if (GEngine) {
      EOrbType SelectedType = GatherTypes[CurrentGatherOrbTypeIndex];
      GEngine->AddOnScreenDebugMessage(
          201, 2.0f, FColor::Cyan,
          FString::Printf(TEXT("Gather Filter: %s"),
                          SelectedType == EOrbType::None
                              ? TEXT("Any")
                              : *UEnum::GetValueAsString(SelectedType)));
    }
  }
}

void UHexPlacementComponent::CycleTriggerVariant() {
  if (CurrentTriggerVariant == ETriggerVariant::Positive) {
    CurrentTriggerVariant = ETriggerVariant::Negative;
  } else {
    CurrentTriggerVariant = ETriggerVariant::Positive;
  }

  if (GEngine) {
    GEngine->AddOnScreenDebugMessage(
        -1, 2.0f, FColor::Cyan,
        FString::Printf(TEXT("Trigger Variant: %s"),
                        CurrentTriggerVariant == ETriggerVariant::Positive
                            ? TEXT("Positive")
                            : TEXT("Negative")));
  }
}

void UHexPlacementComponent::CyclePreviousType() {
  // Get the list of valid orb types (those with assigned materials)
  TArray<EOrbType> ValidTypes = GetValidOrbTypes();
  if (ValidTypes.Num() == 0)
    return; // No valid types to cycle through

  // Cycle the appropriate type based on current placement mode
  if (PlaceKind == EPlaceKind::Input) {
    CurrentInputOrbTypeIndex =
        (CurrentInputOrbTypeIndex - 1 + ValidTypes.Num()) % ValidTypes.Num();
  } else if (PlaceKind == EPlaceKind::Output ||
             PlaceKind == EPlaceKind::ClusterOutput ||
             PlaceKind == EPlaceKind::ClusterSend) {
    CurrentOutputOrbTypeIndex =
        (CurrentOutputOrbTypeIndex - 1 + ValidTypes.Num()) % ValidTypes.Num();
  } else if (PlaceKind == EPlaceKind::Belt) {
    // Belts include 'None' (index 0) + valid types
    int32 NumOptions = ValidTypes.Num() + 1;
    CurrentBeltOrbTypeIndex =
        (CurrentBeltOrbTypeIndex - 1 + NumOptions) % NumOptions;
  } else if (PlaceKind == EPlaceKind::ClusterInput) {
    // ClusterInputs cycle through orb types like regular Inputs
    CurrentInputOrbTypeIndex =
        (CurrentInputOrbTypeIndex - 1 + ValidTypes.Num()) % ValidTypes.Num();
  } else if (PlaceKind == EPlaceKind::TriggerTile) {
    // TriggerTiles include Void option for "any orb"
    TArray<EOrbType> TriggerTypes = ValidTypes;
    if (!TriggerTypes.Contains(EOrbType::Void)) {
      TriggerTypes.Add(EOrbType::Void);
    }
    CurrentTriggerOrbTypeIndex =
        (CurrentTriggerOrbTypeIndex - 1 + TriggerTypes.Num()) %
        TriggerTypes.Num();
  } else if (PlaceKind == EPlaceKind::GatherScatter) {
    // GatherScatter cycles through orb types (0 = any/unfiltered)
    TArray<EOrbType> GatherTypes;
    GatherTypes.Add(EOrbType::None); // None = any orb
    GatherTypes.Append(ValidTypes);
    CurrentGatherOrbTypeIndex =
        (CurrentGatherOrbTypeIndex - 1 + GatherTypes.Num()) % GatherTypes.Num();
    if (GEngine) {
      EOrbType SelectedType = GatherTypes[CurrentGatherOrbTypeIndex];
      GEngine->AddOnScreenDebugMessage(
          201, 2.0f, FColor::Cyan,
          FString::Printf(TEXT("Gather Filter: %s"),
                          SelectedType == EOrbType::None
                              ? TEXT("Any")
                              : *UEnum::GetValueAsString(SelectedType)));
    }
  }
}

void UHexPlacementComponent::CycleGlyphType() {
  // Cycle through Belt, Bond, Unbond
  if (PlaceKind == EPlaceKind::Belt) {
    PlaceKind = EPlaceKind::Bond;
  } else if (PlaceKind == EPlaceKind::Bond) {
    PlaceKind = EPlaceKind::Unbond;
  } else if (PlaceKind == EPlaceKind::Unbond) {
    PlaceKind = EPlaceKind::Belt;
  } else if (PlaceKind == EPlaceKind::TriggerTile) {
    CycleTriggerVariant();
  }
}

void UHexPlacementComponent::CycleInputOrbType() { CycleNextType(); }

EOrbType UHexPlacementComponent::GetCurrentGatherOrbType() const {
  TArray<EOrbType> GatherTypes;
  GatherTypes.Add(EOrbType::None); // None = any orb
  GatherTypes.Append(GetValidOrbTypes());
  if (CurrentGatherOrbTypeIndex >= 0 &&
      CurrentGatherOrbTypeIndex < GatherTypes.Num()) {
    return GatherTypes[CurrentGatherOrbTypeIndex];
  }
  return EOrbType::None;
}

TArray<EOrbType> UHexPlacementComponent::GetValidOrbTypes() const {
  TArray<EOrbType> ValidTypes;

  // Try to find UOrbMaterialManager in the world
  if (UWorld *World = GetWorld()) {
    for (TActorIterator<AActor> It(World); It; ++It) {
      if (UOrbMaterialManager *MaterialManager =
              (*It)->FindComponentByClass<UOrbMaterialManager>()) {
        // Check all possible orb types to see which ones have materials
        // assigned
        TArray<EOrbType> AllOrbTypes = {EOrbType::None,   EOrbType::BaseA,
                                        EOrbType::BaseB,  EOrbType::BaseC,
                                        EOrbType::MetalA, EOrbType::CatalystX,
                                        EOrbType::Void};

        for (EOrbType OrbType : AllOrbTypes) {
          // Check if this orb type has a material assigned in the map
          if (const UMaterialInterface *const *FoundMaterial =
                  MaterialManager->OrbTypeMaterials.Find(OrbType)) {
            // Only add if the material is not null and not the default
            // material
            if (*FoundMaterial != nullptr &&
                *FoundMaterial != MaterialManager->DefaultOrbMaterial) {
              ValidTypes.Add(OrbType);
            }
          }
        }
        break; // Found the material manager, no need to continue searching
      }
    }
  }

  // If no valid types found, fallback to at least BaseA (which will use
  // DefaultOrbMaterial)
  if (ValidTypes.Num() == 0) {
    ValidTypes.Add(EOrbType::BaseA);
  }

  return ValidTypes;
}

TArray<uint8> UHexPlacementComponent::GetValidClusterTemplateIds() const {
  TArray<uint8> ValidTemplateIds;

  if (Sim) {
    // Get the cluster templates map from SimWorldHex via public accessor
    const TMap<uint8, USimWorldHex::FClusterTemplate> &Templates =
        Sim->GetClusterTemplates();

    // Collect all registered template IDs
    for (const auto &TemplatePair : Templates) {
      ValidTemplateIds.Add(TemplatePair.Key);
    }

    // Sort by template ID for consistent ordering
    ValidTemplateIds.Sort();
  }

  return ValidTemplateIds;
}

EOrbType UHexPlacementComponent::GetCurrentInputOrbType() const {
  // Get the list of valid orb types (those with assigned materials)
  TArray<EOrbType> ValidTypes = GetValidOrbTypes();
  if (ValidTypes.Num() == 0) {
    return EOrbType::BaseA; // Fallback if no valid types
  }

  // Clamp the index to valid range
  int32 ClampedIndex =
      FMath::Clamp(CurrentInputOrbTypeIndex, 0, ValidTypes.Num() - 1);
  return ValidTypes[ClampedIndex];
}

EOrbType UHexPlacementComponent::GetCurrentOutputOrbType() const {
  TArray<EOrbType> ValidTypes = GetValidOrbTypes();
  if (ValidTypes.Num() == 0) {
    return EOrbType::BaseA; // Fallback if no valid types
  }

  int32 ClampedIndex =
      FMath::Clamp(CurrentOutputOrbTypeIndex, 0, ValidTypes.Num() - 1);
  return ValidTypes[ClampedIndex];
}

EOrbType UHexPlacementComponent::GetCurrentBeltOrbType() const {
  TArray<EOrbType> ValidTypes = GetValidOrbTypes();
  // Index 0 is "None" (All types)
  if (CurrentBeltOrbTypeIndex == 0) {
    return EOrbType::None;
  }

  if (ValidTypes.Num() == 0) {
    return EOrbType::None;
  }

  // Adjust index (1-based index maps to 0-based array)
  int32 ClampedIndex =
      FMath::Clamp(CurrentBeltOrbTypeIndex - 1, 0, ValidTypes.Num() - 1);
  return ValidTypes[ClampedIndex];
}

EOrbType UHexPlacementComponent::GetCurrentTriggerOrbType() const {
  // Get the list of valid orb types (those with assigned materials)
  // For TriggerTile, we also include Void type as a special "any orb" option
  TArray<EOrbType> ValidTypes = GetValidOrbTypes();

  // Ensure Void is included for TriggerTiles (allows "any orb" triggering)
  if (!ValidTypes.Contains(EOrbType::Void)) {
    ValidTypes.Add(EOrbType::Void);
  }

  if (ValidTypes.Num() == 0) {
    return EOrbType::BaseA; // Fallback if no valid types
  }

  // Clamp the index to valid range
  int32 ClampedIndex =
      FMath::Clamp(CurrentTriggerOrbTypeIndex, 0, ValidTypes.Num() - 1);
  return ValidTypes[ClampedIndex];
}

uint8 UHexPlacementComponent::GetCurrentClusterTemplateId() const {
  // Get the current orb type (ClusterInputs now cycle through orb types like
  // Inputs)
  EOrbType CurrentOrbType = GetCurrentInputOrbType();

  // Find a cluster template that matches this orb type
  // Prefer templates whose first orb matches the type, otherwise any template
  // containing this orb type
  UWorld *W = GetWorld();
  AOrbSimDriver *NonConstDriver = const_cast<AOrbSimDriver *>(Driver);
  USimWorldHex *NonConstSim = const_cast<USimWorldHex *>(Sim);
  if ((!NonConstDriver || !NonConstSim) && W) {
    ReacquireSimIfNeeded(W, NonConstDriver, NonConstSim);
    // Update the const pointers (this is safe because they're just cached
    // pointers)
    const_cast<UHexPlacementComponent *>(this)->Driver = NonConstDriver;
    const_cast<UHexPlacementComponent *>(this)->Sim = NonConstSim;
  }

  if (!NonConstSim)
    return 0; // Fallback if no sim

  const TMap<uint8, USimWorldHex::FClusterTemplate> &Templates =
      NonConstSim->GetClusterTemplates();

  // First pass: find templates whose first orb matches the orb type
  for (const auto &TemplatePair : Templates) {
    const USimWorldHex::FClusterTemplate &Template = TemplatePair.Value;
    if (Template.Orbs.Num() > 0 &&
        Template.Orbs[0].Type == (uint8)CurrentOrbType) {
      return TemplatePair.Key;
    }
  }

  // Second pass: find any template containing this orb type
  for (const auto &TemplatePair : Templates) {
    const USimWorldHex::FClusterTemplate &Template = TemplatePair.Value;
    for (const auto &Orb : Template.Orbs) {
      if (Orb.Type == (uint8)CurrentOrbType) {
        return TemplatePair.Key;
      }
    }
  }

  // Fallback: return first available template, or 0 if none
  TArray<uint8> ValidTemplates = GetValidClusterTemplateIds();
  if (ValidTemplates.Num() > 0) {
    return ValidTemplates[0];
  }

  return 0; // Fallback if no valid templates
}

void UHexPlacementComponent::SetClusterTemplateToId(uint8 TemplateId) {
  UWorld *W = GetWorld();
  if ((!Driver || !Sim) && W)
    ReacquireSimIfNeeded(W, Driver, Sim);
  if (!Sim)
    return;

  TArray<uint8> ValidTemplates = GetValidClusterTemplateIds();

  // Find the index of the desired template ID
  for (int32 i = 0; i < ValidTemplates.Num(); i++) {
    if (ValidTemplates[i] == TemplateId) {
      CurrentClusterTemplateIndex = i;
      UE_LOG(LogTemp, Log,
             TEXT("HexPlacementComponent: Set cluster template to ID %d (index "
                  "%d)"),
             TemplateId, i);
      return;
    }
  }

  UE_LOG(LogTemp, Warning,
         TEXT("HexPlacementComponent: Template ID %d not found in valid "
              "templates"),
         TemplateId);
}

void UHexPlacementComponent::EnsurePreviewComponent() {
  if (PreviewComp)
    return;

  AActor *Owner = GetOwner();
  if (!Owner)
    return;

  PreviewComp = NewObject<UStaticMeshComponent>(Owner);
  PreviewComp->SetupAttachment(Owner->GetRootComponent());
  PreviewComp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
  PreviewComp->SetMobility(EComponentMobility::Movable);
  PreviewComp->RegisterComponent();

  if (PreviewBaseMaterial) {
    PreviewMID = UMaterialInstanceDynamic::Create(PreviewBaseMaterial, this);
    PreviewComp->SetMaterial(0, PreviewMID);
  }
}

void UHexPlacementComponent::EnsureSkeletalPreviewComponent() {
  if (SkeletalPreviewComp)
    return;

  AActor *Owner = GetOwner();
  if (!Owner)
    return;

  SkeletalPreviewComp = NewObject<UPoseableMeshComponent>(Owner);
  SkeletalPreviewComp->SetupAttachment(Owner->GetRootComponent());
  SkeletalPreviewComp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
  SkeletalPreviewComp->SetMobility(EComponentMobility::Movable);
  SkeletalPreviewComp->SetCastShadow(false);
  SkeletalPreviewComp->SetHiddenInGame(false);
  SkeletalPreviewComp->bUseAsOccluder = false;
  SkeletalPreviewComp->RegisterComponent();

  UE_LOG(LogTemp, Log,
         TEXT("HexPlacementComponent: Created skeletal preview component - "
              "Owner: %s"),
         Owner ? *Owner->GetName() : TEXT("NULL"));
}

UStaticMesh *UHexPlacementComponent::SelectPreviewMesh() const {
  switch (PlaceKind) {
  case EPlaceKind::Input:
  case EPlaceKind::ClusterInput:
  case EPlaceKind::ClusterSend: {
    // For Input/ClusterInput/ClusterSend, use the actual tile mesh from
    // GameManager
    if (UWorld *World = GetWorld()) {
      for (TActorIterator<AGameManager> It(World); It; ++It) {
        // Prefer CustomInputTileMesh if available, otherwise use TileMesh
        if ((*It)->CustomInputTileMesh) {
          return (*It)->CustomInputTileMesh;
        } else if ((*It)->TileMesh) {
          return (*It)->TileMesh;
        }
      }
    }
    // Fallback to generic preview mesh
    return GenericTilePreviewMesh;
  }

  case EPlaceKind::Output:
  case EPlaceKind::ClusterOutput: {
    // For Output/ClusterOutput, use the actual tile mesh from GameManager
    if (UWorld *World = GetWorld()) {
      for (TActorIterator<AGameManager> It(World); It; ++It) {
        if ((*It)->TileMesh) {
          return (*It)->TileMesh;
        }
      }
    }
    // Fallback to generic preview mesh
    return GenericTilePreviewMesh;
  }

  case EPlaceKind::Belt:
    return BeltMeshPreview ? BeltMeshPreview : GenericTilePreviewMesh;

  case EPlaceKind::Arm:
    return ArmBasePreviewMesh ? ArmBasePreviewMesh : GenericTilePreviewMesh;

  case EPlaceKind::Walker:
    return ArmBasePreviewMesh ? ArmBasePreviewMesh : GenericTilePreviewMesh;

  case EPlaceKind::Bond:
    return BondPreviewMesh;

  case EPlaceKind::Unbond:
    return UnbondPreviewMesh ? UnbondPreviewMesh : BondPreviewMesh;

  case EPlaceKind::RequestBeacon: {
    if (UWorld *World = GetWorld()) {
      for (TActorIterator<AGameManager> It(World); It; ++It) {
        AGameManager *GameManager = *It;
        if (GameManager && GameManager->RequestBeaconClass) {
          ARequestBeacon *DefaultBeacon =
              GameManager->RequestBeaconClass
                  ->GetDefaultObject<ARequestBeacon>();
          if (DefaultBeacon && DefaultBeacon->MeshComp &&
              DefaultBeacon->MeshComp->GetStaticMesh()) {
            return DefaultBeacon->MeshComp->GetStaticMesh();
          }
        }
        break;
      }
    }
    // Fallback to generic
    return GenericTilePreviewMesh;
  }

  case EPlaceKind::TriggerTile: {
    if (UWorld *World = GetWorld()) {
      for (TActorIterator<AGameManager> It(World); It; ++It) {
        AGameManager *GameManager = *It;
        if (GameManager && GameManager->TriggerTileClass) {
          ATriggerTile *DefaultTrigger =
              GameManager->TriggerTileClass->GetDefaultObject<ATriggerTile>();
          if (DefaultTrigger) {
            UStaticMesh *DesiredMesh =
                (CurrentTriggerVariant == ETriggerVariant::Positive)
                    ? DefaultTrigger->PositiveMesh
                    : DefaultTrigger->NegativeMesh;
            if (DesiredMesh)
              return DesiredMesh;

            if (DefaultTrigger->MeshComp &&
                DefaultTrigger->MeshComp->GetStaticMesh()) {
              return DefaultTrigger->MeshComp->GetStaticMesh();
            }
          }
        }
        break;
      }
    }
    // Fallback to generic
    return GenericTilePreviewMesh;
  }

  case EPlaceKind::HelixOutput:
  case EPlaceKind::HelixInput:
    // For now, use generic tile mesh for helix tiles
    // TODO: Add dedicated helix tile mesh in GameManager
    return GenericTilePreviewMesh;

  default:
    return GenericTilePreviewMesh;
  }
}

USkeletalMesh *UHexPlacementComponent::SelectSkeletalPreviewMesh() const {
  switch (PlaceKind) {
  case EPlaceKind::Arm:
    if (bUseActualArmAssets) {
      if (UWorld *World = GetWorld()) {
        // First try to find an existing arm actor in the world (has BP
        // defaults)
        for (TActorIterator<ABasicArm> ArmIt(World); ArmIt; ++ArmIt) {
          ABasicArm *ExistingArm = *ArmIt;
          if (ExistingArm && ExistingArm->ArmSkeletalMesh) {
            UE_LOG(LogTemp, Log,
                   TEXT("HexPlacementComponent: Using skeletal mesh from "
                        "existing arm: %s"),
                   *ExistingArm->ArmSkeletalMesh->GetName());
            return ExistingArm->ArmSkeletalMesh;
          }
        }

        // If no existing arm, try to get from GameManager's BasicArmClass CDO
        for (TActorIterator<AGameManager> It(World); It; ++It) {
          AGameManager *GameManager = *It;
          if (GameManager && GameManager->BasicArmClass) {
            // For Blueprint classes, we need to check the actual CDO which has
            // BP values
            if (AActor *CDO =
                    GameManager->BasicArmClass->GetDefaultObject<AActor>()) {
              if (ABasicArm *ArmCDO = Cast<ABasicArm>(CDO)) {
                if (ArmCDO->ArmSkeletalMesh) {
                  UE_LOG(LogTemp, Log,
                         TEXT("HexPlacementComponent: Using arm skeletal mesh "
                              "from CDO: %s"),
                         *ArmCDO->ArmSkeletalMesh->GetName());
                  return ArmCDO->ArmSkeletalMesh;
                }
              }
            }

            // Last resort: try spawning a temporary instance to get defaults
            // This is needed because Blueprint-assigned defaults may not be in
            // CDO
            UE_LOG(LogTemp, Warning,
                   TEXT("HexPlacementComponent: CDO ArmSkeletalMesh is null. "
                        "Make sure ArmSkeletalMesh is set in the BasicArm "
                        "Blueprint class defaults."));
          } else {
            UE_LOG(LogTemp, Warning,
                   TEXT("HexPlacementComponent: GameManager or BasicArmClass "
                        "not found"));
          }
          break;
        }
      } else {
        UE_LOG(LogTemp, Warning, TEXT("HexPlacementComponent: No world found"));
      }
    }
    return nullptr;

  case EPlaceKind::Walker:
    if (bUseActualWalkerAssets) {
      if (UWorld *World = GetWorld()) {
        for (TActorIterator<AGameManager> It(World); It; ++It) {
          AGameManager *GameManager = *It;
          if (GameManager && GameManager->BasicWalkerClass) {
            ABasicWalker *DefaultWalker =
                GameManager->BasicWalkerClass->GetDefaultObject<ABasicWalker>();
            if (DefaultWalker && DefaultWalker->WalkerSkeletalMesh) {
              return DefaultWalker->WalkerSkeletalMesh;
            } else {
              UE_LOG(LogTemp, Warning,
                     TEXT("HexPlacementComponent: BasicWalker class found but "
                          "WalkerSkeletalMesh is null"));
            }
          } else {
            UE_LOG(LogTemp, Warning,
                   TEXT("HexPlacementComponent: GameManager or "
                        "BasicWalkerClass not found"));
          }
          break;
        }
      } else {
        UE_LOG(LogTemp, Warning, TEXT("HexPlacementComponent: No world found"));
      }
    }
    return nullptr;

  default:
    return nullptr;
  }
}

UStaticMesh *UHexPlacementComponent::GetActualBeltMesh() const {
  // First try the local preview mesh
  if (BeltMeshPreview) {
    return BeltMeshPreview;
  }

  // Fallback: try to find AHexFactoryActor in the world
  if (UWorld *World = GetWorld()) {
    for (TActorIterator<AHexFactoryActor> It(World); It; ++It) {
      AHexFactoryActor *FactoryActor = *It;
      if (FactoryActor && FactoryActor->BeltMesh) {
        // Cache the reference for future use
        const_cast<UHexPlacementComponent *>(this)->BeltMeshPreview =
            FactoryActor->BeltMesh;
        return FactoryActor->BeltMesh;
      }
    }
  }

  // Final fallback
  return GenericTilePreviewMesh;
}

UMaterialInterface *UHexPlacementComponent::GetActualBeltMaterial() const {
  // First try the local preview material
  if (BeltMaterialPreview) {
    return BeltMaterialPreview;
  }

  // Fallback: try to find AHexFactoryActor in the world
  if (UWorld *World = GetWorld()) {
    for (TActorIterator<AHexFactoryActor> It(World); It; ++It) {
      AHexFactoryActor *FactoryActor = *It;
      if (FactoryActor && FactoryActor->BeltMaterial) {
        // Cache the reference for future use
        const_cast<UHexPlacementComponent *>(this)->BeltMaterialPreview =
            FactoryActor->BeltMaterial;
        return FactoryActor->BeltMaterial;
      }
    }
  }

  // Final fallback
  return nullptr;
}

UStaticMesh *UHexPlacementComponent::GetActualOrbMesh() const {
  // Try to find UOrbMaterialManager in the world
  if (UWorld *World = GetWorld()) {
    for (TActorIterator<AActor> It(World); It; ++It) {
      if (UOrbMaterialManager *MaterialManager =
              (*It)->FindComponentByClass<UOrbMaterialManager>()) {
        UStaticMesh *OrbMesh = MaterialManager->GetOrbMesh();
        if (OrbMesh) {
          return OrbMesh;
        }
      }
    }
  }

  // Final fallback
  return GenericTilePreviewMesh;
}

UMaterialInterface *UHexPlacementComponent::GetActualOrbMaterial() const {
  // Try to find UOrbMaterialManager in the world
  if (UWorld *World = GetWorld()) {
    for (TActorIterator<AActor> It(World); It; ++It) {
      if (UOrbMaterialManager *MaterialManager =
              (*It)->FindComponentByClass<UOrbMaterialManager>()) {
        EOrbType CurrentType = GetCurrentInputOrbType();
        UMaterialInterface *OrbMaterial =
            MaterialManager->GetMaterialForOrbType(CurrentType);
        if (OrbMaterial) {
          return OrbMaterial;
        }
      }
    }
  }

  // Final fallback
  return nullptr;
}

bool UHexPlacementComponent::CanPlaceAt(const FHex &Hex) const {
  if (!Sim)
    return true;

  // Discovery Mode radius check
  if (UWorld *World = GetWorld()) {
    for (TActorIterator<AGameManager> It(World); It; ++It) {
      AGameManager *GM = *It;
      if (GM && GM->IsDiscoveryMode()) {
        int32 Dist = (FMath::Abs(Hex.Q) + FMath::Abs(Hex.Q + Hex.R) +
                      FMath::Abs(Hex.R)) /
                     2;
        if (Dist > GM->UnlockedGridRadius) {
          return false;
        }
      }
      break;
    }
  }

  // Block placement on planted walker feet (excluding holding feet which are
  // raised)
  for (const FWalkerState &W : Sim->Walkers) {
    if (!W.DFoot.bHoldingItem &&
        (W.DFoot.CurrentTile == Hex || W.DFoot.TargetTile == Hex))
      return false;
    if (!W.LFoot.bHoldingItem &&
        (W.LFoot.CurrentTile == Hex || W.LFoot.TargetTile == Hex))
      return false;
    if (!W.RFoot.bHoldingItem &&
        (W.RFoot.CurrentTile == Hex || W.RFoot.TargetTile == Hex))
      return false;
    if (!W.UFoot.bHoldingItem &&
        (W.UFoot.CurrentTile == Hex || W.UFoot.TargetTile == Hex))
      return false;
  }

  return true;
}

FVector UHexPlacementComponent::HexToWorld(const FHex &H) const {
  if (Driver) {
    return Driver->MapHexToWorld(H);
  }

  if (!Sim) {
    return LastHoverWorld;
  }

  const float Tile = Sim->TileSizeUU;
  const AActor *Anchor = GetAnchor();
  const FVector AnchorV =
      Anchor ? Anchor->GetActorLocation() : FVector::ZeroVector;

  const FHexAxial A(H.Q, H.R);
  const FVector2D LocalXY =
      HexCoord::AxialToXY(A, Tile, EHexOrientation::PointyTop);
  const float Z = (H.Level == 0) ? Sim->OrbZOffset : -Sim->OrbZOffset;

  return AnchorV + FVector(LocalXY.X, LocalXY.Y, Z);
}

void UHexPlacementComponent::UpdateHoverHex() {
  FVector World;
  if (!HitPlane(World)) {
    bHoverValid = false;
    bSmoothingInitialized = false;
    return;
  }

  LastHoverWorld = World;

  if (!Driver && !Sim) {
    UE_LOG(LogTemp, Warning,
           TEXT("HexPlacementComponent: UpdateHoverHex - No Driver or Sim "
                "available"));
    bHoverValid = false;
    bSmoothingInitialized = false;
    return;
  }

  if (Driver && Driver->IsSphereEnabled()) {
    FHex H;
    if (Driver->WorldToHex(World, H)) {
      HoverHex = H;
      bHoverValid = true;
      return;
    }
  }

  const float Tile =
      Driver ? Driver->GetTileSize() : (Sim ? Sim->TileSizeUU : 100.f);
  const AActor *Anchor = GetAnchor();
  const FVector2D OriginXY = Anchor ? FVector2D(Anchor->GetActorLocation().X,
                                                Anchor->GetActorLocation().Y)
                                    : FVector2D::ZeroVector;

  const float Yaw = Driver ? Driver->GetGridYaw() : 0.f;
  const EHexOrientation Ori =
      Driver ? Driver->GetGridOrientation() : EHexOrientation::PointyTop;

  FVector2D P(World.X - OriginXY.X, World.Y - OriginXY.Y);
  const FVector2D Local = HexCoord::Rotate2D(P, -Yaw);
  const FHexAxial A = HexCoord::XYToAxialRound(Local, Tile, Ori);

  HoverHex = FHex{A.Q, A.R, ActiveLevel};
  bHoverValid = true;
}

void UHexPlacementComponent::UpdatePreviewVisual(float DeltaTime) {
  // Disable preview if in blueprint mode
  if (bDisablePreview) {
    if (PreviewComp) {
      PreviewComp->SetVisibility(false);
    }
    if (SkeletalPreviewComp) {
      SkeletalPreviewComp->SetVisibility(false);
    }
    return;
  }

  if (!bHoverValid || !GetWorld())
    return;

  const bool bPlaceOK = CanPlaceAt(HoverHex);
  const float Tile =
      Driver ? Driver->GetTileSize() : (Sim ? Sim->TileSizeUU : 100.f);

  // Calculate the effective level for display (swapped if display is flipped)
  const bool bDisplayFlipped = Driver ? Driver->IsDisplayFlipped() : false;
  const int32 DisplayLevel = bDisplayFlipped ? (1 - ActiveLevel) : ActiveLevel;

  const FVector CenterA = HexToWorld(HoverHex) + FVector(0, 0, PreviewZOffset);

  if (PlaceKind != EPlaceKind::Walker) {
    // Start hidden by default - visibility will be controlled by GameManager
    // This block seems to be misplaced from a constructor.
    // SetVisibility(false);
    // SetCollisionEnabled(ECollisionEnabled::NoCollision);
    // SetCollisionProfileName(TEXT("NoCollision"));
    // CommandTextures.Empty();
    if (SkeletalPreviewComp) {
      SkeletalPreviewComp->SetVisibility(false);
    }
  }

  // If walker placement, try to use actual walker skeletal assets for preview
  if (PlaceKind == EPlaceKind::Walker) {
    USkeletalMesh *WalkerMesh = SelectSkeletalPreviewMesh();
    if (WalkerMesh && bUseActualWalkerAssets) {
      EnsureSkeletalPreviewComponent();
      if (PreviewComp) {
        PreviewComp->SetVisibility(false);
      }
      if (SkeletalPreviewComp) {
        SkeletalPreviewComp->SetVisibility(true);
        SkeletalPreviewComp->SetSkinnedAssetAndUpdate(WalkerMesh);

        // Smoothing
        FVector TargetLoc = CenterA;
        FRotator TargetRot = FRotator(DisplayLevel == 0 ? 0.f : 180.f,
                                      Driver ? Driver->GetGridYaw() : 0.f, 0.f);
        if (!bSmoothingInitialized) {
          SmoothedLocation = TargetLoc;
          SmoothedRotation = TargetRot;
          bSmoothingInitialized = true;
        } else {
          SmoothedLocation = FMath::VInterpTo(SmoothedLocation, TargetLoc,
                                              DeltaTime, SmoothingSpeed);
          SmoothedRotation = FMath::RInterpTo(SmoothedRotation, TargetRot,
                                              DeltaTime, SmoothingSpeed);
        }

        SkeletalPreviewComp->SetWorldLocation(SmoothedLocation);
        SkeletalPreviewComp->SetWorldRotation(SmoothedRotation);
        SkeletalPreviewComp->SetWorldScale3D(FVector(1.f));
      }
      return;
    } else if (SkeletalPreviewComp) {
      SkeletalPreviewComp->SetVisibility(false);
    }
  }

  // Outline (keep single hex for non-bond, two for bond/unbond, multiple for
  // ClusterOutput and HelixInput)
  if (PlaceKind == EPlaceKind::HelixInput && HelixInputLayout.Num() > 0) {
    // Draw all tiles in the HelixInput set
    const bool bSetOK = CanPlaceHelixInputSetAt(HoverHex);
    const FColor Col = bSetOK ? FColor(0, 200, 40) : FColor(200, 20, 20);
    TArray<FHex> PreviewHexes = GetHelixInputPreviewHexes(HoverHex);
    for (const FHex &PreviewHex : PreviewHexes) {
      const FVector PreviewCenter =
          HexToWorld(PreviewHex) + FVector(0, 0, PreviewZOffset);
      DrawDebugCircle(GetWorld(), PreviewCenter, Tile * 0.95f, 6, Col, false,
                      0.f, 0, 2.5f, FVector(1, 0, 0), FVector(0, 1, 0), false);
    }
    // Also draw the center marker
    DrawDebugCircle(GetWorld(), CenterA, Tile * 0.5f, 6, FColor::Cyan, false,
                    0.f, 0, 3.f, FVector(1, 0, 0), FVector(0, 1, 0), false);
  } else if (PlaceKind == EPlaceKind::GatherScatter) {
    FHex Center = HoverHex;
    int32 Radius = GatherScatterRadius;

    // Determine center based on placement step
    if ((GatherScatterPlacementStep == 1 || GatherScatterPlacementStep == 2) &&
        PendingGatherBeacon) {
      Center = FHex(PendingGatherBeacon->Q, PendingGatherBeacon->R,
                    PendingGatherBeacon->Level);
    } else if (GatherScatterPlacementStep == 3 && PendingScatterBeacon) {
      Center = FHex(PendingScatterBeacon->Q, PendingScatterBeacon->R,
                    PendingScatterBeacon->Level);
    }

    // Draw hexagonal outline if radius > 0
    // Draw hexagonal outline describing the assigned tiles if radius > 0
    if (Radius > 0) {
      TArray<FHex> RingHexes;
      FHex Current = Center;
      // Start at corner (Direction 4 * Radius)
      for (int k = 0; k < Radius; ++k) {
        Current = HexUtil::Neighbor(Current, (EHexDir)4);
      }

      // Walk the ring
      for (int i = 0; i < 6; ++i) {
        for (int j = 0; j < Radius; ++j) {
          RingHexes.Add(Current);
          Current = HexUtil::Neighbor(Current, (EHexDir)i);
        }
      }

      const FColor RadiusCol =
          (GatherScatterPlacementStep < 2) ? FColor::Cyan : FColor::Orange;
      const float LineThickness = 4.0f;
      // Use consistent Z height
      const float ZHeight = PreviewZOffset + 5.0f;

      for (const FHex &H : RingHexes) {
        FVector CenterH = HexToWorld(H);
        CenterH.Z += ZHeight;

        for (uint8 d = 0; d < 6; ++d) {
          FHex N = HexUtil::Neighbor(H, (EHexDir)d);
          if (HexUtil::Dist(Center, N) > Radius) {
            // This neighbor is outside the radius; draw the shared edge
            FVector CenterN = HexToWorld(N);
            CenterN.Z += ZHeight;

            FVector Midpoint = (CenterH + CenterN) * 0.5f;
            FVector DirToN = (CenterN - CenterH).GetSafeNormal();
            FVector Tangent =
                FVector::CrossProduct(FVector(0, 0, 1), DirToN).GetSafeNormal();
            float EdgeHalfLen = Tile * 0.5f;

            DrawDebugLine(GetWorld(), Midpoint - Tangent * EdgeHalfLen,
                          Midpoint + Tangent * EdgeHalfLen, RadiusCol, false,
                          0.f, 0, LineThickness);
          }
        }
      }
    }

    // Also draw individual hex circle at hover
    DrawDebugCircle(GetWorld(), CenterA, Tile * 0.95f, 6,
                    bPlaceOK ? FColor(0, 200, 40) : FColor(200, 20, 20), false,
                    0.f, 0, 2.f, FVector(1, 0, 0), FVector(0, 1, 0), false);
  } else if (PlaceKind != EPlaceKind::Bond && PlaceKind != EPlaceKind::Unbond) {
    DrawDebugCircle(GetWorld(), CenterA, Tile * 0.95f, 6,
                    bPlaceOK ? FColor(0, 200, 40) : FColor(200, 20, 20), false,
                    0.f, 0, 2.f, FVector(1, 0, 0), FVector(0, 1, 0), false);
  } else {
    const FHex NeighborHex =
        HexUtil::Neighbor(HoverHex, (EHexDir)(BeltDir % 6));
    const FVector CenterB =
        HexToWorld(NeighborHex) + FVector(0, 0, PreviewZOffset);
    const FColor Col = bPlaceOK ? FColor(0, 200, 40) : FColor(200, 20, 20);
    DrawDebugCircle(GetWorld(), CenterA, Tile * 0.95f, 6, Col, false, 0.f, 0,
                    2.f, FVector(1, 0, 0), FVector(0, 1, 0), false);
    DrawDebugCircle(GetWorld(), CenterB, Tile * 0.95f, 6, Col, false, 0.f, 0,
                    2.f, FVector(1, 0, 0), FVector(0, 1, 0), false);
  }

  // Direction arrow only for Belt/Arm or RequestBeacon (DropOff step)
  if (PlaceKind == EPlaceKind::Belt || PlaceKind == EPlaceKind::Arm ||
      (PlaceKind == EPlaceKind::RequestBeacon && BeaconPlacementStep == 1)) {
    const FHex NeighborHex =
        HexUtil::Neighbor(HoverHex, (EHexDir)(BeltDir % 6));
    const FVector CenterB =
        HexToWorld(NeighborHex) + FVector(0, 0, PreviewZOffset);
    const FVector DirVec = (CenterB - CenterA).GetSafeNormal();
    if (!DirVec.IsNearlyZero()) {
      const float ArrowLen = (CenterB - CenterA).Size() * 0.55f;
      const FVector Tip = CenterA + DirVec * ArrowLen;
      const FColor Col = bPlaceOK ? FColor(0, 200, 40) : FColor(200, 20, 20);
      DrawDebugLine(GetWorld(), CenterA, Tip, Col, false, 0.f, 0, 3.f);
      const FVector Side(-DirVec.Y, DirVec.X, 0.f);
      DrawDebugLine(GetWorld(), Tip,
                    Tip - DirVec * (ArrowLen * 0.25f) +
                        Side * (ArrowLen * 0.15f),
                    Col, false, 0.f, 0, 2.f);
      DrawDebugLine(GetWorld(), Tip,
                    Tip - DirVec * (ArrowLen * 0.25f) -
                        Side * (ArrowLen * 0.15f),
                    Col, false, 0.f, 0, 2.f);
    }
  }

  // Mesh preview (for all placeable tiles)
  if (PlaceKind == EPlaceKind::Input || PlaceKind == EPlaceKind::Output ||
      PlaceKind == EPlaceKind::ClusterOutput ||
      PlaceKind == EPlaceKind::ClusterInput || PlaceKind == EPlaceKind::Belt ||
      PlaceKind == EPlaceKind::Arm || PlaceKind == EPlaceKind::Walker ||
      PlaceKind == EPlaceKind::Bond || PlaceKind == EPlaceKind::Unbond ||
      PlaceKind == EPlaceKind::RequestBeacon ||
      PlaceKind == EPlaceKind::TriggerTile ||
      PlaceKind == EPlaceKind::ClusterSend ||
      PlaceKind == EPlaceKind::GatherScatter ||
      PlaceKind == EPlaceKind::Source) {

    // For now, use static mesh preview for arms (ArmBasePreviewMesh)
    if (PlaceKind == EPlaceKind::Arm) {
      EnsurePreviewComponent();
      if (!PreviewComp) {
        UE_LOG(
            LogTemp, Warning,
            TEXT("HexPlacementComponent: Failed to create preview component"));
        return;
      }

      UStaticMesh *Mesh = SelectPreviewMesh();
      if (!Mesh) {
        // UE_LOG(LogTemp, Warning,
        //        TEXT("HexPlacementComponent: No mesh selected for arm
        //        preview"));
        PreviewComp->SetVisibility(false);
        return;
      }

      if (PreviewComp->GetStaticMesh() != Mesh) {
        PreviewComp->SetStaticMesh(Mesh);
      }

      const uint8 EffectiveDir = BeltDir % 6;
      const float Yaw = ComputeDirYawFromNeighbor(this, HoverHex, EffectiveDir);

      // Apply arm preview facing fix
      float FixedYaw = FMath::UnwindDegrees(Yaw + ArmPreviewFacingFixDeg);

      FVector UseLoc = CenterA;
      FVector UseScale(PreviewScale);

      // Smoothing
      FVector TargetLoc = UseLoc;
      FRotator TargetRot =
          FRotator(DisplayLevel == 0 ? 0.f : 180.f, FixedYaw, 0.f);
      if (!bSmoothingInitialized) {
        SmoothedLocation = TargetLoc;
        SmoothedRotation = TargetRot;
        bSmoothingInitialized = true;
      } else {
        SmoothedLocation = FMath::VInterpTo(SmoothedLocation, TargetLoc,
                                            DeltaTime, SmoothingSpeed);
        SmoothedRotation = FMath::RInterpTo(SmoothedRotation, TargetRot,
                                            DeltaTime, SmoothingSpeed);
      }

      PreviewComp->SetWorldLocationAndRotation(SmoothedLocation,
                                               SmoothedRotation);
      PreviewComp->SetWorldScale3D(UseScale);
      PreviewComp->SetVisibility(true);

      // Set material for arm preview
      if (PreviewMID) {
        const FLinearColor Col = bPlaceOK ? FLinearColor(0.f, 0.9f, 0.f, 0.5f)
                                          : FLinearColor(0.9f, 0.f, 0.f, 0.6f);
        PreviewMID->SetVectorParameterValue(FName("Tint"), Col);
      }

      // Hide skeletal mesh component
      if (SkeletalPreviewComp) {
        SkeletalPreviewComp->SetVisibility(false);
      }

    } else {
      // Use static mesh preview for other types
      EnsurePreviewComponent();
      if (!PreviewComp)
        return;

      UStaticMesh *Mesh = SelectPreviewMesh();
      if (!Mesh) {
        PreviewComp->SetVisibility(false);
        return;
      }
      if (PreviewComp->GetStaticMesh() != Mesh)
        PreviewComp->SetStaticMesh(Mesh);

      const uint8 EffectiveDir = BeltDir % 6;
      const float Yaw = ComputeDirYawFromNeighbor(this, HoverHex, EffectiveDir);

      float FixedYaw = Yaw;
      if (PlaceKind == EPlaceKind::Arm) {
        FixedYaw = FMath::UnwindDegrees(FixedYaw + ArmPreviewFacingFixDeg);
      } else if (PlaceKind == EPlaceKind::Output ||
                 PlaceKind == EPlaceKind::ClusterOutput ||
                 PlaceKind == EPlaceKind::ClusterSend) {
        // For output tiles, we need to match the grid orientation
        // If the grid is using FlatTop orientation, rotate the tile mesh 30
        // degrees
        const EHexOrientation Ori =
            Driver ? Driver->GetGridOrientation() : EHexOrientation::PointyTop;
        if (Ori == EHexOrientation::FlatTop) {
          FixedYaw = FMath::UnwindDegrees(FixedYaw + 30.0f);
        }
      } else if (PlaceKind == EPlaceKind::Input ||
                 PlaceKind == EPlaceKind::ClusterInput) {
        // For input tiles, we need to match the grid orientation (same as
        // output) If the grid is using FlatTop orientation, rotate the tile
        // mesh 30 degrees
        const EHexOrientation Ori =
            Driver ? Driver->GetGridOrientation() : EHexOrientation::PointyTop;
        if (Ori == EHexOrientation::FlatTop) {
          FixedYaw = FMath::UnwindDegrees(FixedYaw + 30.0f);
        }
      } else if (PlaceKind == EPlaceKind::TriggerTile) {
        // For trigger tiles, match the grid orientation like input/output
        // tiles
        const EHexOrientation Ori =
            Driver ? Driver->GetGridOrientation() : EHexOrientation::PointyTop;
        if (Ori == EHexOrientation::FlatTop) {
          FixedYaw = FMath::UnwindDegrees(FixedYaw + 30.0f);
        }
      }

      FVector UseLoc = CenterA;
      FVector UseScale(PreviewScale);

      // ClusterOutput tiles use larger scale
      if (PlaceKind == EPlaceKind::ClusterOutput ||
          PlaceKind == EPlaceKind::ClusterSend) {
        // Get ClusterOutputScale from GameManager
        float ClusterScale = 1.3f; // Default
        if (UWorld *World = GetWorld()) {
          for (TActorIterator<AGameManager> It(World); It; ++It) {
            ClusterScale = (*It)->ClusterOutputScale;
            break;
          }
        }
        UseScale *= ClusterScale;
      }

      else if (PlaceKind == EPlaceKind::RequestBeacon &&
               BeaconPlacementStep == 1) {
        // DropOff beacon rotates
        // EffectiveDir already declared above
        FixedYaw = ComputeDirYawFromNeighbor(this, HoverHex, EffectiveDir);
      } else if (PlaceKind == EPlaceKind::RequestBeacon &&
                 BeaconPlacementStep == 0) {
        // Pickup beacon static (grid aligned)
        FixedYaw = 0.f;
        // ComputeDirYawFromNeighbor already handles GridYaw/etc relative to
        // world If we want default yaw (Direction 0), call with Dir 0 But
        // ComputeDirYawFromNeighbor is based on neighbor position relative to
        // center For Pickup, we just want to align with grid
        // ComputeDirYawFromNeighbor(0) returns angle to neighbor 0, which is
        // grid axis 0
        FixedYaw = ComputeDirYawFromNeighbor(this, HoverHex, 0);
      }

      // Scale preview
      if (PlaceKind == EPlaceKind::RequestBeacon) {
        // If using the generic mesh, scale it up to look taller
        if (Mesh == GenericTilePreviewMesh) {
          UseScale = FVector(PreviewScale * 1.0f, PreviewScale * 1.0f,
                             PreviewScale * 1.5f);
        }
        // If using a custom mesh (from BP), keep scale 1.0 (or PreviewScale)
      }

      if (PlaceKind == EPlaceKind::Bond || PlaceKind == EPlaceKind::Unbond) {
        // Midpoint between the two hex centers
        const FHex NeighborHex =
            HexUtil::Neighbor(HoverHex, (EHexDir)EffectiveDir);
        const FVector CenterB =
            HexToWorld(NeighborHex) + FVector(0, 0, PreviewZOffset);
        UseLoc = (CenterA + CenterB) * 0.5f;
        // Assume authored size fits two hexes; let user adjust in editor via
        // mesh or PreviewScale
        UseScale = FVector(PreviewScale);
      }

      // Smoothing
      FVector TargetLoc = UseLoc;
      FRotator TargetRot = FRotator(0.f, FixedYaw, 0.f);
      if (!bSmoothingInitialized) {
        SmoothedLocation = TargetLoc;
        SmoothedRotation = TargetRot;
        bSmoothingInitialized = true;
      } else {
        SmoothedLocation = FMath::VInterpTo(SmoothedLocation, TargetLoc,
                                            DeltaTime, SmoothingSpeed);
        SmoothedRotation = FMath::RInterpTo(SmoothedRotation, TargetRot,
                                            DeltaTime, SmoothingSpeed);
      }

      PreviewComp->SetWorldLocationAndRotation(SmoothedLocation,
                                               SmoothedRotation);
      PreviewComp->SetWorldScale3D(UseScale);
      PreviewComp->SetVisibility(true);

      // Set material based on placement type
      if (PlaceKind == EPlaceKind::Belt) {
        EOrbType CurrentType = GetCurrentBeltOrbType();
        if (CurrentType != EOrbType::None) {
          // Look up orb material
          UMaterialInterface *OrbMaterial = nullptr;
          if (Driver) {
            if (UOrbMaterialManager *MaterialManager =
                    Driver->FindComponentByClass<UOrbMaterialManager>()) {
              OrbMaterial = MaterialManager->GetMaterialForOrbType(CurrentType);
            }
          }

          if (OrbMaterial) {
            UMaterialInstanceDynamic *OutputMID =
                UMaterialInstanceDynamic::Create(OrbMaterial, this);
            if (OutputMID) {
              OutputMID->SetScalarParameterValue(FName("Opacity"), 0.5f);
              PreviewComp->SetMaterial(0, OutputMID);
            }
          }
        } else {
          // Restore default material (try CachedHexFactory first)
          UMaterialInterface *DefaultMat = BeltMaterialPreview;
          if (!DefaultMat && CachedHexFactory) {
            DefaultMat = CachedHexFactory->BeltMaterial;
          }
          if (DefaultMat) {
            PreviewComp->SetMaterial(0, DefaultMat);
          }
        }
      } else if (PlaceKind == EPlaceKind::Input ||
                 PlaceKind == EPlaceKind::ClusterInput ||
                 PlaceKind == EPlaceKind::Source) {
        // For input tiles and cluster input tiles, use the actual orb
        // material
        UMaterialInterface *OrbMaterial = GetActualOrbMaterial();
        if (OrbMaterial) {
          PreviewComp->SetMaterial(0, OrbMaterial);
        }
      } else if (PlaceKind == EPlaceKind::Output ||
                 PlaceKind == EPlaceKind::ClusterOutput ||
                 PlaceKind == EPlaceKind::ClusterSend) {
        // For output/cluster/send tiles, use the actual orb material with
        // half opacity
        EOrbType CurrentType = GetCurrentOutputOrbType();
        UMaterialInterface *OrbMaterial = GetActualOutputOrbMaterial();

        if (OrbMaterial) {
          // Create a NEW dynamic material instance for opacity control
          // (recreated each frame)
          UMaterialInstanceDynamic *OutputMID =
              UMaterialInstanceDynamic::Create(OrbMaterial, this);
          if (OutputMID) {
            // Set half opacity (0.5 alpha)
            OutputMID->SetScalarParameterValue(FName("Opacity"), 0.5f);
            PreviewComp->SetMaterial(0, OutputMID);
          } else {
            PreviewComp->SetMaterial(0, OrbMaterial);
          }
        }
      } else if (PlaceKind == EPlaceKind::RequestBeacon) {
        // Material (Both use PreviewBeaconPickupMaterial for base, decals
        // differentiate)
        UMaterialInterface *Mat = PreviewBeaconPickupMaterial;
        if (Mat) {
          PreviewComp->SetMaterial(0, Mat);
        } else if (PreviewMID) {
          // Fallback color: Green for Pickup, Blue for DropOff (if no
          // material set)
          const FLinearColor Col = (BeaconPlacementStep == 0)
                                       ? FLinearColor(0.f, 1.f, 0.f, 0.5f)
                                       : FLinearColor(0.f, 0.5f, 1.f, 0.5f);
          PreviewMID->SetVectorParameterValue(FName("Tint"), Col);
        }
      } else if (PlaceKind == EPlaceKind::TriggerTile) {
        // For TriggerTile, use the orb type material with transparency
        EOrbType CurrentType = GetCurrentTriggerOrbType();
        UMaterialInterface *OrbMaterial = nullptr;

        // Get material from OrbMaterialManager
        if (UWorld *World = GetWorld()) {
          for (TActorIterator<AActor> It(World); It; ++It) {
            if (UOrbMaterialManager *MaterialManager =
                    (*It)->FindComponentByClass<UOrbMaterialManager>()) {
              OrbMaterial = MaterialManager->GetMaterialForOrbType(CurrentType);
              break;
            }
          }
        }

        if (OrbMaterial) {
          UMaterialInstanceDynamic *TriggerMID =
              UMaterialInstanceDynamic::Create(OrbMaterial, this);
          if (TriggerMID) {
            // Set 60% opacity for trigger tile preview
            TriggerMID->SetScalarParameterValue(FName("Opacity"), 0.6f);
            PreviewComp->SetMaterial(0, TriggerMID);
          } else {
            PreviewComp->SetMaterial(0, OrbMaterial);
          }
        } else if (PreviewMID) {
          // Fallback: use tinted material
          // During step 0 (placement), use yellow; during step 1 (owner
          // selection), use cyan
          const FLinearColor Col =
              (TriggerTilePlacementStep == 0)
                  ? FLinearColor(1.0f, 0.8f, 0.f,
                                 0.6f) // Yellow-ish for placement
                  : FLinearColor(0.f, 0.8f, 1.0f,
                                 0.6f); // Cyan for owner selection
          PreviewMID->SetVectorParameterValue(FName("Tint"), Col);
        }
      } else if (PreviewMID) {
        // For other types, use the tinted material
        const FLinearColor Col = bPlaceOK ? FLinearColor(0.f, 0.9f, 0.f, 0.5f)
                                          : FLinearColor(0.9f, 0.f, 0.f, 0.6f);
        PreviewMID->SetVectorParameterValue(FName("Tint"), Col);
      }

      // Hide skeletal mesh component
      if (SkeletalPreviewComp) {
        SkeletalPreviewComp->SetVisibility(false);
      }
    }
  } else if (PreviewComp) {
    PreviewComp->SetVisibility(false);
  }
  if (SkeletalPreviewComp) {
    SkeletalPreviewComp->SetVisibility(false);
  }
}

UStaticMesh *UHexPlacementComponent::GetActualOutputOrbMesh() const {
  // Try to find GameManager for tile mesh
  if (UWorld *World = GetWorld()) {
    for (TActorIterator<AGameManager> It(World); It; ++It) {
      AGameManager *GameManager = *It;
      if (GameManager && GameManager->TileMesh) {
        return GameManager->TileMesh;
      }
    }
  }

  // Fallback to local tile mesh if available
  if (UWorld *World = GetWorld()) {
    for (TActorIterator<AHexFactoryActor> It(World); It; ++It) {
      AHexFactoryActor *FactoryActor = *It;
      if (FactoryActor && FactoryActor->TileMesh) {
        return FactoryActor->TileMesh;
      }
    }
  }

  // Final fallback
  return GenericTilePreviewMesh;
}

UMaterialInterface *UHexPlacementComponent::GetActualOutputOrbMaterial() const {
  // Try to find UOrbMaterialManager in the world
  if (UWorld *World = GetWorld()) {
    for (TActorIterator<AActor> It(World); It; ++It) {
      if (UOrbMaterialManager *MaterialManager =
              (*It)->FindComponentByClass<UOrbMaterialManager>()) {
        EOrbType CurrentType = GetCurrentOutputOrbType();
        UMaterialInterface *OrbMaterial =
            MaterialManager->GetMaterialForOrbType(CurrentType);
        if (OrbMaterial) {
          return OrbMaterial;
        }
      }
    }
  }

  // Final fallback
  return nullptr;
}

void UHexPlacementComponent::UpdateArmPreviewBoneRotations(float Yaw) {
  if (!SkeletalPreviewComp || !SkeletalPreviewComp->GetSkinnedAsset()) {
    return;
  }

  // Get bone names and rotation offsets from the actual BasicArm class
  FName RotatingBase2BoneName = TEXT("RotatingBase2");
  FName BaseGear7BoneName = TEXT("BaseGear7");
  FName Base1BoneName = TEXT("Base1");
  FRotator RotatingBase2RotationOffset = FRotator::ZeroRotator;
  FRotator BaseGear7RotationOffset = FRotator::ZeroRotator;

  if (bUseActualArmAssets) {
    // Get the actual bone names and offsets from BasicArm class blueprint
    if (UWorld *World = GetWorld()) {
      for (TActorIterator<AGameManager> It(World); It; ++It) {
        AGameManager *GameManager = *It;
        if (GameManager && GameManager->BasicArmClass) {
          ABasicArm *DefaultArm =
              GameManager->BasicArmClass->GetDefaultObject<ABasicArm>();
          if (DefaultArm) {
            RotatingBase2BoneName = DefaultArm->RotatingBase2BoneName;
            BaseGear7BoneName = DefaultArm->BaseGear7BoneName;
            Base1BoneName = DefaultArm->Base1BoneName;
            RotatingBase2RotationOffset =
                DefaultArm->RotatingBase2RotationOffset;
            BaseGear7RotationOffset = DefaultArm->BaseGear7RotationOffset;

            UE_LOG(LogTemp, Log,
                   TEXT("HexPlacementComponent: Using actual arm bone settings "
                        "- RotatingBase2: %s, BaseGear7: %s"),
                   *RotatingBase2BoneName.ToString(),
                   *BaseGear7BoneName.ToString());
          }
          break; // Only check the first GameManager found
        }
      }
    }
  }

  // Rotate RotatingBase2 bone (Y-axis rotation for facing direction)
  if (RotatingBase2BoneName != NAME_None) {
    FRotator RotatingBase2Rotation =
        FRotator(0.f, Yaw, 0.f) + RotatingBase2RotationOffset;
    SkeletalPreviewComp->SetBoneRotationByName(RotatingBase2BoneName,
                                               RotatingBase2Rotation,
                                               EBoneSpaces::ComponentSpace);
  }

  // Rotate BaseGear7 bone (opposite direction of RotatingBase2)
  if (BaseGear7BoneName != NAME_None) {
    FRotator BaseGear7Rotation =
        FRotator(0.f, -Yaw, 0.f) + BaseGear7RotationOffset;
    SkeletalPreviewComp->SetBoneRotationByName(
        BaseGear7BoneName, BaseGear7Rotation, EBoneSpaces::ComponentSpace);
  }

  // Base1 remains stationary (no rotation applied)
}

void UHexPlacementComponent::CancelPlacement() {
  // Reset RequestBeacon state
  BeaconPlacementStep = 0;
  PendingPickupBeacon = nullptr;

  // Reset TriggerTile state
  TriggerTilePlacementStep = 0;
  PendingTriggerTile = nullptr;

  // Reset HelixInput pairing state
  CancelHelixInputPairing();

  // Reset GatherScatter state
  GatherScatterPlacementStep = 0;
  PendingGatherBeacon = nullptr;
  PendingScatterBeacon = nullptr;
  bGatherRadiusDragging = false;
}

void UHexPlacementComponent::StartHelixInputPairing(int32 OutputGroupIndex) {
  if (!Sim || OutputGroupIndex < 0 ||
      OutputGroupIndex >= Sim->GetHelixGroups().Num()) {
    return;
  }

  PendingHelixOutputGroupIndex = OutputGroupIndex;
  PlaceKind = EPlaceKind::HelixInput;

  // Cache the relative hex offsets from the output group's center
  const USimWorldHex::FHelixOutputGroup &Group =
      Sim->GetHelixGroups()[OutputGroupIndex];
  HelixInputLayout.Empty();

  for (const FHex &MemberHex : Group.MemberTiles) {
    // Compute offset from center
    FHex Offset;
    Offset.Q = MemberHex.Q - Group.CenterHex.Q;
    Offset.R = MemberHex.R - Group.CenterHex.R;
    Offset.Level = 0; // Same level
    HelixInputLayout.Add(Offset);
  }

  if (GEngine) {
    GEngine->AddOnScreenDebugMessage(
        -1, 3.0f, FColor::Cyan,
        FString::Printf(TEXT("Entered HelixInput pairing mode: %d tiles. "
                             "Click to place paired input set."),
                        HelixInputLayout.Num()));
  }
}

void UHexPlacementComponent::CancelHelixInputPairing() {
  if (PendingHelixOutputGroupIndex >= 0) {
    PendingHelixOutputGroupIndex = -1;
    HelixInputLayout.Empty();
    // Only reset PlaceKind if we're actually in HelixInput mode
    // (SetPlaceKind handles this before calling us when switching modes)
    if (PlaceKind == EPlaceKind::HelixInput) {
      PlaceKind = EPlaceKind::HelixOutput;
    }
  }
}

TArray<FHex>
UHexPlacementComponent::GetHelixInputPreviewHexes(const FHex &Center) const {
  TArray<FHex> Result;
  for (const FHex &Offset : HelixInputLayout) {
    FHex TargetHex;
    TargetHex.Q = Center.Q + Offset.Q;
    TargetHex.R = Center.R + Offset.R;
    TargetHex.Level = Center.Level;
    Result.Add(TargetHex);
  }
  return Result;
}

bool UHexPlacementComponent::CanPlaceHelixInputSetAt(const FHex &Center) const {
  if (!Sim || PendingHelixOutputGroupIndex < 0 ||
      PendingHelixOutputGroupIndex >= Sim->GetHelixGroups().Num()) {
    return false;
  }

  const USimWorldHex::FHelixOutputGroup &OutputGroup =
      Sim->GetHelixGroups()[PendingHelixOutputGroupIndex];

  // Check minimum distance from output center to input center
  int32 Distance = OutputGroup.CenterHex.Distance(Center);
  if (Distance < USimWorldHex::FHelixOutputGroup::MinPairDistance) {
    return false;
  }

  // Get all input hex positions
  TArray<FHex> InputHexes = GetHelixInputPreviewHexes(Center);

  // Check that none of the input hexes overlap with output tiles
  for (const FHex &InputHex : InputHexes) {
    // Check against output group tiles
    if (OutputGroup.MemberTiles.Contains(InputHex)) {
      return false;
    }

    // Check that tile location is not already occupied by something blocking
    if (const FTile *ExistingTile = Sim->TileMap.Find(InputHex)) {
      if (ExistingTile->Kind != ETileKind::Empty) {
        // Allow overlap if it's an existing HelixInput tile belonging to the
        // same group (we're replacing it)
        bool bIsSelfOverlap = false;
        if (ExistingTile->Kind == ETileKind::HelixInput &&
            Sim->GetHelixGroups().IsValidIndex(PendingHelixOutputGroupIndex)) {
          const auto &Group =
              Sim->GetHelixGroups()[PendingHelixOutputGroupIndex];
          if (Group.PairedInputTiles.Contains(InputHex)) {
            bIsSelfOverlap = true;
          }
        }

        if (!bIsSelfOverlap) {
          return false;
        }
      }
    }

    // Check discovery mode radius
    if (!CanPlaceAt(InputHex)) {
      return false;
    }
  }

  return true;
}