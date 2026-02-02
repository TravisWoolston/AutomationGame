#include "Actors/TriggerTile.h"
#include "Actors/OrbSimDriver.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SplineMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Data/OrbMaterialManager.h"
#include "Engine/TextureRenderTarget2D.h"
#include "EngineUtils.h"
#include "Framework/GameManager.h"
#include "Framework/GameTickManager.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Sim/HexTypes.h"
#include "Sim/SimWorldHex.h"
#include "Sim/Tiles.h"

ATriggerTile::ATriggerTile() {
  PrimaryActorTick.bCanEverTick = true;

  RootComp = CreateDefaultSubobject<USceneComponent>(TEXT("RootComp"));
  RootComponent = RootComp;

  MeshComp = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("MeshComp"));
  MeshComp->SetupAttachment(RootComp);
  MeshComp->SetCollisionEnabled(ECollisionEnabled::NoCollision);

  // Create wire component for visual connection to owner
  WireComp = CreateDefaultSubobject<USplineMeshComponent>(TEXT("WireComp"));
  WireComp->SetupAttachment(RootComp);
  WireComp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
  WireComp->SetMobility(EComponentMobility::Movable);
  // Use Z-axis as forward since engine cylinder is oriented along Z
  WireComp->SetForwardAxis(ESplineMeshAxis::Z);
  WireComp->SetVisibility(false); // Hidden until owner is set

  // Create wave pulse indicator component
  WavePulseComp =
      CreateDefaultSubobject<UStaticMeshComponent>(TEXT("WavePulseComp"));
  WavePulseComp->SetupAttachment(RootComp);
  WavePulseComp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
  WavePulseComp->SetMobility(EComponentMobility::Movable);
  WavePulseComp->SetVisibility(false); // Hidden until owner is set
  WavePulseComp->SetCastShadow(false);

  PositiveMesh = nullptr;
  NegativeMesh = nullptr;
}

void ATriggerTile::BeginPlay() {
  Super::BeginPlay();

  // Find Driver and Sim
  if (UWorld *World = GetWorld()) {
    Driver = AOrbSimDriver::Find(World);
    if (Driver) {
      Sim = Driver->GetSim();
    }

    // Initialize IsGridVisible from GameManager's current state and get mesh if
    // not set
    for (TActorIterator<AGameManager> It(World); It; ++It) {
      AGameManager *GM = *It;
      if (GM) {
        // If mesh isn't already set (e.g., from Blueprint), use GameManager's
        // TileMesh
        if (MeshComp && !MeshComp->GetStaticMesh() && GM->TileMesh) {
          MeshComp->SetStaticMesh(GM->TileMesh);
        }

        if (UInstancedStaticMeshComponent *ISM =
                GM->FindComponentByClass<UInstancedStaticMeshComponent>()) {
          SetIsGridVisible(ISM->IsVisible());
        }
        break;
      }
    }

    // Find GameTickManager for tick rate reference
    TickManager = AGameTickManager::Get(this);

    // Subscribe to simulation step completion to update signals immediately
    // after moves
    if (Driver) {
      Driver->OnSimStepComplete.AddDynamic(
          this, &ATriggerTile::HandleSimStepComplete);
    }
  }

  SyncWorldLocation();
  UpdateVisuals();
  UpdateWireVisual();
  CheckTriggerCondition();
}

// void ATriggerTile::Destroyed() {
//   if (Driver) {
//     Driver->OnSimStepComplete.RemoveAll(this);
//   }
//   Super::Destroyed();
// }

void ATriggerTile::HandleSimStepComplete(float StepSeconds) {
  if (Owners.Num() > 0) {
    CheckTriggerCondition();
  }
}

void ATriggerTile::Tick(float DeltaTime) {
  Super::Tick(DeltaTime);

  // Force wire update for first few frames to catch late Blueprint property
  // initialization
  if (bWireNeedsInitialUpdate && WireUpdateRetryCount < 10) {
    WireUpdateRetryCount++;

    // Check if WireMaterial is now set (from Blueprint) or if we just need to
    // refresh the geometry
    if ((WireMaterial && WireMaterial != LastWireMaterialUsed) ||
        WireUpdateRetryCount == 10) {
      if (WireMaterial && WireMaterial != LastWireMaterialUsed) {
        UE_LOG(
            LogTemp, Log,
            TEXT(
                "TriggerTile: Late wire material detected (%s), updating wire"),
            *WireMaterial->GetName());
        CachedWireMID = nullptr; // Force recreation
      }

      UpdateWireVisual();
      bWireNeedsInitialUpdate = false;
    }
  }

  // Only check trigger condition if we have at least one owner
  if (Owners.Num() > 0) {
    CheckTriggerCondition();
  }

  // Animate ripple based on GTM tick rate (updates every engine tick)
  if (bEnableWavePulse && bHasOwner && Driver && TickManager) {
    // Get GTM's current ticks per second
    float GTMTicksPerSecond = TickManager->TicksPerSecond;

    // Advance ripple position based on GTM tick rate
    // WavePulseSpeed = 1.0 means it takes 1 GTM tick to travel from start to
    // end So we advance by: (DeltaTime * TicksPerSecond) / WavePulseSpeed
    float AdvanceRate =
        (DeltaTime * GTMTicksPerSecond) / FMath::Max(0.001f, WavePulseSpeed);
    WavePulseT += AdvanceRate;

    if (WavePulseT >= 1.0f) {
      WavePulseT -= FMath::Floor(WavePulseT); // Wrap around for continuous loop
      RippleIntensity = 0.0f; // Reset intensity when starting new cycle
    }

    // Calculate fade in/out based on position
    // Convert fade ticks to normalized position (0-1)
    float TotalTravelTicks = WavePulseSpeed;
    float FadeInEnd = (RippleFadeInTicks / TotalTravelTicks);
    float FadeOutStart = 1.0f - (RippleFadeOutTicks / TotalTravelTicks);

    if (WavePulseT < FadeInEnd) {
      // Fading in
      RippleIntensity = (FadeInEnd > 0.0f)
                            ? FMath::Clamp(WavePulseT / FadeInEnd, 0.0f, 1.0f)
                            : 1.0f;
    } else if (WavePulseT > FadeOutStart) {
      // Fading out
      float FadeOutRange = (RippleFadeOutTicks / TotalTravelTicks);
      RippleIntensity =
          (FadeOutRange > 0.0f)
              ? FMath::Clamp((1.0f - WavePulseT) / FadeOutRange, 0.0f, 1.0f)
              : 1.0f;
    } else {
      // Full intensity in the middle
      RippleIntensity = 1.0f;
    }
  }

  // Update visuals every frame
  UpdateRippleVisuals();
}

void ATriggerTile::UpdateRippleVisuals() {
  if (!bEnableWavePulse || Owners.Num() == 0 || !Driver) {
    // Disable effects when wave pulse is disabled or no owners
    for (auto &KV : WavePulseComponents) {
      if (KV.Value)
        KV.Value->SetVisibility(false);
    }
    return;
  }

  // Draw ripple for each owner
  for (const FTriggerOwner &TOwner : Owners) {
    EnsureComponentsForOwner(TOwner);

    // Calculate start and end positions (at grid level)
    FVector StartPos = GetActorLocation();
    FHex OwnerHex(TOwner.Q, TOwner.R, TOwner.Level);
    FVector EndPos = Driver->MapHexToWorld(OwnerHex);

    // Calculate current ripple position at grid level
    FVector RipplePos = FMath::Lerp(StartPos, EndPos, WavePulseT);

    // Draw ripple to render target
    if (UWorld *World = GetWorld()) {
      AGameManager *GM = nullptr;
      for (TActorIterator<AGameManager> It(World); It; ++It) {
        GM = *It;
        break;
      }

      if (GM) {
        if (!GM->RippleActive)
          continue;

        UTextureRenderTarget2D *RT = GM->GetRippleRenderTarget();
        UMaterialInterface *DrawMaterial = GM->RippleDrawMaterial;

        if (RT && RippleIntensity > 0.0f && RT->IsValidLowLevel() &&
            DrawMaterial && DrawMaterial->IsValidLowLevel()) {
          if (!RippleDrawMID) {
            UMaterialInterface *BaseMat = DrawMaterial;
            if (UMaterialInstanceDynamic *ExistingMID =
                    Cast<UMaterialInstanceDynamic>(BaseMat)) {
              BaseMat = ExistingMID->Parent;
            }
            if (BaseMat && BaseMat->IsValidLowLevel()) {
              RippleDrawMID = UMaterialInstanceDynamic::Create(BaseMat, this);
            }
          }

          if (RippleDrawMID && RippleDrawMID->IsValidLowLevel()) {
            FVector2D RTUV = GM->WorldPositionToRippleRTUV(RipplePos);
            FLinearColor EncodedColor(RippleIntensity,
                                      bCurrentSignal ? 1.0f : 0.0f, 0.0f, 1.0f);
            float RTWorldSize = GM->RippleRenderTargetWorldSize;
            if (RTWorldSize > 0.0f) {
              RippleDrawMID->SetVectorParameterValue(
                  FName("RippleCenter"),
                  FLinearColor(RTUV.X, RTUV.Y, 0.0f, 1.0f));
              RippleDrawMID->SetVectorParameterValue(FName("RippleColor"),
                                                     EncodedColor);
              RippleDrawMID->SetScalarParameterValue(
                  FName("RippleRadius"), RippleRadius / RTWorldSize);
              RippleDrawMID->SetScalarParameterValue(
                  FName("RippleFalloff"), RippleFalloff / RTWorldSize);

              if (World->WorldType == EWorldType::Game ||
                  World->WorldType == EWorldType::PIE) {
                UKismetRenderingLibrary::DrawMaterialToRenderTarget(
                    World, RT, RippleDrawMID);
              }
            }
          }
        }
      }
    }

    // Update pulse mesh if enabled
    UStaticMeshComponent *PulseComp = WavePulseComponents.FindRef(TOwner);
    if (PulseComp && WavePulseMesh) {
      FVector MeshPos = RipplePos;
      MeshPos.Z += WireHeightOffset + WavePulseHeightOffset;

      PulseComp->SetWorldLocation(MeshPos);
      PulseComp->SetWorldScale3D(FVector(WavePulseScale));

      if (!PulseComp->GetStaticMesh()) {
        PulseComp->SetStaticMesh(WavePulseMesh);
      }

      if (!WavePulseMID && PulseComp->GetStaticMesh()) {
        UMaterialInterface *BaseMat = WavePulseMaterial;
        if (!BaseMat)
          BaseMat = PulseComp->GetStaticMesh()->GetMaterial(0);
        if (BaseMat) {
          if (UMaterialInstanceDynamic *ExistingMID =
                  Cast<UMaterialInstanceDynamic>(BaseMat))
            BaseMat = ExistingMID->Parent;
          if (BaseMat)
            WavePulseMID = UMaterialInstanceDynamic::Create(BaseMat, this);
        }
      }

      if (WavePulseMID) {
        FLinearColor PulseColor =
            bCurrentSignal ? PositiveWireColor : NegativeWireColor;
        FLinearColor EmissiveColor =
            PulseColor * WavePulseEmissiveIntensity * RippleIntensity;

        WavePulseMID->SetVectorParameterValue(FName("BaseColor"),
                                              PulseColor * RippleIntensity);
        WavePulseMID->SetVectorParameterValue(FName("Color"),
                                              PulseColor * RippleIntensity);
        WavePulseMID->SetVectorParameterValue(FName("Tint"),
                                              PulseColor * RippleIntensity);
        WavePulseMID->SetVectorParameterValue(FName("EmissiveColor"),
                                              EmissiveColor);
        PulseComp->SetMaterial(0, WavePulseMID);
      }
      PulseComp->SetVisibility(RippleIntensity > 0.0f);
    }
  }

  // Hide components for owners no longer in the list (if any cleanup needed)
  // For now EnsureComponentsForOwner handles creation.
}

void ATriggerTile::CheckTriggerCondition() {
  // Ensure we have Sim reference
  if (!Sim && !Driver) {
    if (UWorld *World = GetWorld()) {
      Driver = AOrbSimDriver::Find(World);
      if (Driver) {
        Sim = Driver->GetSim();
      }
    }
  }

  if (!Sim)
    return;

  // Check tile content at Q, R
  FHex Location(Q, R, Level);
  bool bMatchingOrb = false;

  // Check for any orb at this location (board or held)
  EOrbType ActualType = Sim->GetOrbTypeAtLocation(Location);

  if (ActualType != EOrbType::None) {
    if (TriggerOrbType == EOrbType::Void) {
      // Void type: any orb triggers positive signal
      bMatchingOrb = true;
    } else {
      // Check if type matches
      bMatchingOrb = (ActualType == TriggerOrbType);
    }
  }

  // Update signal state if changed
  bool bFinalSignal = bMatchingOrb;
  if (Variant == ETriggerVariant::Negative) {
    bFinalSignal = !bMatchingOrb;
  }
  SetSignal(bFinalSignal, true); // Force sync on condition check
}

void ATriggerTile::SetSignal(bool bNewSignal, bool bForceSync) {
  if (bCurrentSignal != bNewSignal || bForceSync) {
    bCurrentSignal = bNewSignal;
    OnSignalChanged.Broadcast(bCurrentSignal);

    // Update the simulation for all owners
    if (Sim) {
      FHex TriggerHex(Q, R, Level);
      for (const FTriggerOwner &TOwner : Owners) {
        FHex OwnerHex(TOwner.Q, TOwner.R, TOwner.Level);
        if (TOwner.Type == ETriggerTileOwnerType::Belt) {
          Sim->SetBeltTriggerSignal(TriggerHex, OwnerHex, bCurrentSignal);
        } else {
          Sim->SetArmTriggerSignal(TriggerHex, OwnerHex, bCurrentSignal);
        }
      }
    }

    // Update visuals to reflect signal state
    UpdateVisuals();
    UpdateWireVisual();
  }
}

void ATriggerTile::OnConstruction(const FTransform &Transform) {
  Super::OnConstruction(Transform);

  // Try to set mesh from GameManager if not already set
  if (MeshComp && !MeshComp->GetStaticMesh()) {
    if (UWorld *World = GetWorld()) {
      for (TActorIterator<AGameManager> It(World); It; ++It) {
        AGameManager *GM = *It;
        if (GM && GM->TileMesh) {
          MeshComp->SetStaticMesh(GM->TileMesh);
          break;
        }
      }
    }
  }

  SyncWorldLocation();
  UpdateVisuals();
  UpdateWireVisual();
}

void ATriggerTile::SetIsGridVisible(bool bVisible) {
  if (bIsGridVisible != bVisible) {
    bIsGridVisible = bVisible;
    OnGridVisibilityChanged.Broadcast(bVisible);
  }
}

void ATriggerTile::ToggleOwner(int32 InOwnerQ, int32 InOwnerR,
                               int32 InOwnerLevel,
                               ETriggerTileOwnerType InType) {
  FTriggerOwner NewOwner;
  NewOwner.Q = InOwnerQ;
  NewOwner.R = InOwnerR;
  NewOwner.Level = InOwnerLevel;
  NewOwner.Type = InType;

  if (Owners.Contains(NewOwner)) {
    RemoveOwner(InOwnerQ, InOwnerR, InOwnerLevel, InType);
  } else {
    AddOwner(InOwnerQ, InOwnerR, InOwnerLevel, InType);
  }
}

void ATriggerTile::AddOwner(int32 InOwnerQ, int32 InOwnerR, int32 InOwnerLevel,
                            ETriggerTileOwnerType InType) {
  if (!Sim) {
    if (UWorld *World = GetWorld()) {
      Driver = AOrbSimDriver::Find(World);
      if (Driver)
        Sim = Driver->GetSim();
    }
  }

  FTriggerOwner NewOwner;
  NewOwner.Q = InOwnerQ;
  NewOwner.R = InOwnerR;
  NewOwner.Level = InOwnerLevel;
  NewOwner.Type = InType;

  if (!Owners.Contains(NewOwner)) {
    Owners.Add(NewOwner);
    bHasOwner = true;

    // Ensure components exist for visualization
    EnsureComponentsForOwner(NewOwner);

    // Initialize simulation signal
    if (Sim) {
      FHex TriggerHex(Q, R, Level);
      FHex OwnerHex(InOwnerQ, InOwnerR, InOwnerLevel);
      if (InType == ETriggerTileOwnerType::Belt) {
        Sim->SetBeltTriggerSignal(TriggerHex, OwnerHex, bCurrentSignal);
      } else {
        Sim->SetArmTriggerSignal(TriggerHex, OwnerHex, bCurrentSignal);
      }
    }

    UE_LOG(LogTemp, Log,
           TEXT("TriggerTile at (%d,%d) added owner %s at (%d,%d) level %d"), Q,
           R,
           InType == ETriggerTileOwnerType::Belt ? TEXT("belt") : TEXT("arm"),
           InOwnerQ, InOwnerR, InOwnerLevel);

    UpdateWireVisual();
  }
}

void ATriggerTile::RemoveOwner(int32 InOwnerQ, int32 InOwnerR,
                               int32 InOwnerLevel,
                               ETriggerTileOwnerType InType) {
  FTriggerOwner Target;
  Target.Q = InOwnerQ;
  Target.R = InOwnerR;
  Target.Level = InOwnerLevel;
  Target.Type = InType;

  if (Owners.Contains(Target)) {
    // Clear simulation signal
    if (Sim) {
      FHex TriggerHex(Q, R, Level);
      FHex OwnerHex(InOwnerQ, InOwnerR, InOwnerLevel);
      if (InType == ETriggerTileOwnerType::Belt) {
        Sim->ClearBeltTriggerSignal(TriggerHex, OwnerHex);
      } else {
        Sim->ClearArmTriggerSignal(TriggerHex, OwnerHex);
      }
    }

    // Remove components
    if (USplineMeshComponent *Wire = WireComponents.FindRef(Target)) {
      Wire->DestroyComponent();
      WireComponents.Remove(Target);
    }
    if (UStaticMeshComponent *Pulse = WavePulseComponents.FindRef(Target)) {
      Pulse->DestroyComponent();
      WavePulseComponents.Remove(Target);
    }

    Owners.Remove(Target);
    bHasOwner = Owners.Num() > 0;

    UE_LOG(LogTemp, Log,
           TEXT("TriggerTile at (%d,%d) removed owner %s at (%d,%d) level %d"),
           Q, R,
           InType == ETriggerTileOwnerType::Belt ? TEXT("belt") : TEXT("arm"),
           InOwnerQ, InOwnerR, InOwnerLevel);

    UpdateWireVisual();
  }
}

void ATriggerTile::SetOwnerBelt(int32 InOwnerQ, int32 InOwnerR) {
  AddOwner(InOwnerQ, InOwnerR, Level, ETriggerTileOwnerType::Belt);
}

void ATriggerTile::UpdateVisuals() {
  if (!MeshComp)
    return;

  // Set the correct mesh based on variant
  UStaticMesh *DesiredMesh =
      (Variant == ETriggerVariant::Positive) ? PositiveMesh : NegativeMesh;

  // If specific variant mesh is not available, try to get from GameManager (for
  // PositiveMesh)
  if (!DesiredMesh) {
    if (UWorld *World = GetWorld()) {
      for (TActorIterator<AGameManager> It(World); It; ++It) {
        AGameManager *GM = *It;
        if (GM && GM->TileMesh) {
          if (Variant == ETriggerVariant::Positive) {
            DesiredMesh = GM->TileMesh;
          }
          // If we're on Negative variant and no NegativeMesh is provided,
          // we could fallback to the same mesh or something else.
          // For now let's just use GM->TileMesh as a fallback for both if
          // DesiredMesh is null.
          if (!DesiredMesh)
            DesiredMesh = GM->TileMesh;
          break;
        }
      }
    }
  }

  if (DesiredMesh && MeshComp->GetStaticMesh() != DesiredMesh) {
    MeshComp->SetStaticMesh(DesiredMesh);
  }

  // Priority 1: Use explicit signal state materials if provided
  if (bCurrentSignal && PositiveSignalMaterial) {
    MeshComp->SetMaterial(0, PositiveSignalMaterial);
    return;
  } else if (!bCurrentSignal && NegativeSignalMaterial) {
    MeshComp->SetMaterial(0, NegativeSignalMaterial);
    return;
  }

  // Priority 2: Use TranslucentBaseMaterial with orb type color tint
  if (TranslucentBaseMaterial) {
    UMaterialInstanceDynamic *MID =
        UMaterialInstanceDynamic::Create(TranslucentBaseMaterial, this);
    if (MID) {
      // Get the orb type color from the material manager
      FLinearColor OrbColor = FLinearColor::White;
      if (UWorld *World = GetWorld()) {
        UOrbMaterialManager *MaterialManager =
            UOrbMaterialManager::FindInWorld(this);
        if (MaterialManager) {
          // Try to get color from the orb type material
          UMaterialInterface *TypeMaterial =
              MaterialManager->GetMaterialForOrbType(TriggerOrbType);
          if (TypeMaterial) {
            // Try to extract base color from the material
            // Use material-specific colors based on orb type
            switch (TriggerOrbType) {
            case EOrbType::BaseA:
              OrbColor = FLinearColor(0.8f, 0.2f, 0.2f);
              break; // Red
            case EOrbType::BaseB:
              OrbColor = FLinearColor(0.2f, 0.8f, 0.2f);
              break; // Green
            case EOrbType::BaseC:
              OrbColor = FLinearColor(0.2f, 0.2f, 0.8f);
              break; // Blue
            case EOrbType::MetalA:
              OrbColor = FLinearColor(0.7f, 0.7f, 0.7f);
              break; // Silver
            case EOrbType::CatalystX:
              OrbColor = FLinearColor(0.8f, 0.6f, 0.2f);
              break; // Gold
            case EOrbType::Void:
              OrbColor = FLinearColor(0.5f, 0.0f, 0.8f);
              break; // Purple (any orb)
            default:
              OrbColor = FLinearColor::White;
              break;
            }
          }
        }
      }

      // Apply color and opacity to the translucent material
      MID->SetVectorParameterValue(FName("BaseColor"), OrbColor);
      MID->SetVectorParameterValue(FName("Color"),
                                   OrbColor); // Alternative parameter name
      MID->SetVectorParameterValue(FName("Tint"),
                                   OrbColor); // Another common name
      MID->SetScalarParameterValue(FName("Opacity"), TriggerTileOpacity);

      MeshComp->SetMaterial(0, MID);
      return;
    }
  }

  // Priority 3: Fallback - create translucent version of orb material
  if (UWorld *World = GetWorld()) {
    UOrbMaterialManager *MaterialManager =
        UOrbMaterialManager::FindInWorld(this);
    if (MaterialManager) {
      UMaterialInterface *TypeMaterial =
          MaterialManager->GetMaterialForOrbType(TriggerOrbType);
      if (TypeMaterial) {
        UMaterialInstanceDynamic *MID =
            UMaterialInstanceDynamic::Create(TypeMaterial, this);
        if (MID) {
          // Try to set opacity (material must support this parameter)
          MID->SetScalarParameterValue(FName("Opacity"), TriggerTileOpacity);
          MeshComp->SetMaterial(0, MID);
          return;
        }
      }
    }
  }
}

void ATriggerTile::UpdateWireVisual() {
  for (const FTriggerOwner &TOwner : Owners) {
    EnsureComponentsForOwner(TOwner);
    USplineMeshComponent *Comp = WireComponents.FindRef(TOwner);
    if (!Comp)
      continue;

    Comp->SetVisibility(true);

    FVector StartPos = GetActorLocation();
    FHex OwnerHex(TOwner.Q, TOwner.R, TOwner.Level);
    FVector EndPos = Driver ? Driver->MapHexToWorld(OwnerHex) : StartPos;

    StartPos.Z += WireHeightOffset;
    EndPos.Z += WireHeightOffset;

    FVector LocalStart = GetActorTransform().InverseTransformPosition(StartPos);
    FVector LocalEnd = GetActorTransform().InverseTransformPosition(EndPos);

    FVector ToEnd = LocalEnd - LocalStart;
    float Distance = ToEnd.Size();
    FVector HorizontalDir = ToEnd.GetSafeNormal();

    float TangentLength = Distance * 0.5f;
    FVector StartTangent = HorizontalDir * TangentLength;
    StartTangent.Z -= WireSagAmount;
    FVector EndTangent = HorizontalDir * TangentLength;
    EndTangent.Z -= WireSagAmount;

    Comp->SetStartAndEnd(LocalStart, StartTangent, LocalEnd, EndTangent);
    FVector2D WireScale(WireRadius, WireRadius);
    Comp->SetStartScale(WireScale);
    Comp->SetEndScale(WireScale);

    if (!Comp->GetStaticMesh() && WireMesh) {
      Comp->SetStaticMesh(WireMesh);
    }
    if (!Comp->GetStaticMesh()) {
      UStaticMesh *DefaultCylinder = LoadObject<UStaticMesh>(
          nullptr, TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
      if (DefaultCylinder)
        Comp->SetStaticMesh(DefaultCylinder);
    }

    FLinearColor WireColor =
        bCurrentSignal ? PositiveWireColor : NegativeWireColor;
    UMaterialInterface *DesiredBaseMaterial =
        WireMaterial ? WireMaterial : Comp->GetStaticMesh()->GetMaterial(0);

    bool bNeedNewMID = !CachedWireMID ||
                       (WireMaterial && LastWireMaterialUsed != WireMaterial);
    if (bNeedNewMID && DesiredBaseMaterial) {
      UMaterialInterface *BaseMaterial = DesiredBaseMaterial;
      if (UMaterialInstanceDynamic *ExistingMID =
              Cast<UMaterialInstanceDynamic>(BaseMaterial))
        BaseMaterial = ExistingMID->Parent;
      if (BaseMaterial) {
        CachedWireMID = UMaterialInstanceDynamic::Create(BaseMaterial, this);
        LastWireMaterialUsed = DesiredBaseMaterial;
      }
    }

    if (CachedWireMID) {
      CachedWireMID->SetVectorParameterValue(FName("BaseColor"), WireColor);
      CachedWireMID->SetVectorParameterValue(FName("Color"), WireColor);
      CachedWireMID->SetVectorParameterValue(FName("Tint"), WireColor);
      CachedWireMID->SetVectorParameterValue(FName("EmissiveColor"), WireColor);
      Comp->SetMaterial(0, CachedWireMID);
    } else if (DesiredBaseMaterial) {
      Comp->SetMaterial(0, DesiredBaseMaterial);
    }
  }

  // Cleanup wires for owners no longer present (already handled in
  // RemoveOwner, but good for safety)
}

void ATriggerTile::Destroyed() {
  if (Driver) {
    Driver->OnSimStepComplete.RemoveAll(this);
  }
  Super::Destroyed();

  // Clean up all trigger signals in simulation
  if (Sim) {
    FHex TriggerHex(Q, R, Level);
    for (const FTriggerOwner &TOwner : Owners) {
      FHex OwnerHex(TOwner.Q, TOwner.R, TOwner.Level);
      if (TOwner.Type == ETriggerTileOwnerType::Belt) {
        Sim->ClearBeltTriggerSignal(TriggerHex, OwnerHex);
      } else {
        Sim->ClearArmTriggerSignal(TriggerHex, OwnerHex);
      }
    }
  }
}

void ATriggerTile::SetOwnerArm(int32 InOwnerQ, int32 InOwnerR) {
  AddOwner(InOwnerQ, InOwnerR, Level, ETriggerTileOwnerType::Arm);
}

void ATriggerTile::SyncWorldLocation() {
  if (!Driver) {
    if (UWorld *World = GetWorld()) {
      Driver = AOrbSimDriver::Find(World);
    }
  }

  if (Driver) {
    FVector NewLocation = Driver->MapHexToWorld(FHex(Q, R, Level));
    SetActorLocation(NewLocation);
    UpdateWireVisual();
  }
}

void ATriggerTile::EnsureComponentsForOwner(const FTriggerOwner &TOwner) {
  if (!WireComponents.Contains(TOwner)) {
    CreateWireComponent(TOwner);
  }
  if (!WavePulseComponents.Contains(TOwner)) {
    CreateWavePulseComponent(TOwner);
  }
}

UStaticMeshComponent *
ATriggerTile::CreateWavePulseComponent(const FTriggerOwner &TOwner) {
  UStaticMeshComponent *NewComp =
      NewObject<UStaticMeshComponent>(this, NAME_None, RF_Transient);
  if (NewComp) {
    NewComp->SetMobility(EComponentMobility::Movable);
    NewComp->AttachToComponent(
        RootComp, FAttachmentTransformRules::KeepRelativeTransform);
    NewComp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    NewComp->SetCastShadow(false);
    if (WavePulseMesh) {
      NewComp->SetStaticMesh(WavePulseMesh);
    }
    NewComp->RegisterComponent();
    WavePulseComponents.Add(TOwner, NewComp);
  }
  return NewComp;
}

USplineMeshComponent *
ATriggerTile::CreateWireComponent(const FTriggerOwner &TOwner) {
  USplineMeshComponent *NewComp =
      NewObject<USplineMeshComponent>(this, NAME_None, RF_Transient);
  if (NewComp) {
    NewComp->SetMobility(EComponentMobility::Movable);
    NewComp->AttachToComponent(
        RootComp, FAttachmentTransformRules::KeepRelativeTransform);
    NewComp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    NewComp->SetForwardAxis(ESplineMeshAxis::Z);
    if (WireMesh) {
      NewComp->SetStaticMesh(WireMesh);
    }
    NewComp->RegisterComponent();
    WireComponents.Add(TOwner, NewComp);
  }
  return NewComp;
}
