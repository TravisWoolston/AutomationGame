#include "Rendering/OrbRenderer.h"
#include "Actors/BasicArm.h"
#include "Actors/OrbSimDriver.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Data/OrbMaterialManager.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Framework/GameTickManager.h"
#include "GameFramework/Actor.h"
#include "Materials/MaterialInterface.h"
#include "Sim/HexCoord.h"
#include "Sim/HexTypes.h"
#include "Sim/SimWorldHex.h"
#include "UObject/ConstructorHelpers.h"

static FORCEINLINE int32 MakeSubOrbId(int32 ItemId, int32 SubIdx) {
  return (ItemId << 8) | (SubIdx & 0xFF);
}

// Optional utility to rotate a raw FVector2D (not hex axial) by yaw degrees.
static FORCEINLINE FVector2D RotateXY(const FVector2D &V, float YawDeg) {
  const float Rad = FMath::DegreesToRadians(YawDeg);
  const float C = FMath::Cos(Rad);
  const float S = FMath::Sin(Rad);
  return FVector2D(C * V.X - S * V.Y, S * V.X + C * V.Y);
}

// Helper for shortest yaw interpolation
static float LerpAngleDeg(float A, float B, float T) {
  float Delta = FMath::UnwindDegrees(B - A);
  // Shortest path
  if (Delta > 180.f)
    Delta -= 360.f;
  if (Delta < -180.f)
    Delta += 360.f;
  return FMath::UnwindDegrees(A + Delta * T);
}

// Compute arm base world position (without iterating arm length)
static FVector ComputeArmBaseWorld(const FArmState &S, const AActor *Anchor,
                                   float Tile, float GridYaw,
                                   EHexOrientation Ori) {
  const FVector AnchorV =
      Anchor ? Anchor->GetActorLocation() : FVector::ZeroVector;
  const FHexAxial A(S.BaseHex.Q, S.BaseHex.R);
  const FVector2D LocalXY = HexCoord::AxialToXY(A, Tile, Ori);
  const FVector2D RotXY = HexCoord::Rotate2D(LocalXY, GridYaw);
  return AnchorV + FVector(RotXY.X, RotXY.Y, 0.f);
}

// Returns grip world center (end effector) at a given yaw (continuous) for pure
// rotation (no extension animation yet)
FVector UOrbRenderer::ComputeArmGripAtYaw(const FArmState &S, float YawDeg,
                                          const AActor *Anchor, float Tile,
                                          float GridYaw,
                                          EHexOrientation Ori) const {
  const FVector Base = ComputeArmBaseWorld(S, Anchor, Tile, GridYaw, Ori);
  // Direction unit in local (grid-rotated) space
  const float Rad = FMath::DegreesToRadians(YawDeg);
  const FVector2D Dir(FMath::Cos(Rad), FMath::Sin(Rad));
  const float Reach = float(S.ArmLen) * Tile;
  return Base + FVector(Dir.X * Reach, Dir.Y * Reach, 0.f);
}

UOrbRenderer::UOrbRenderer() {
  PrimaryComponentTick.bCanEverTick = true;

  // Initialize bonded orb mesh to a small sphere by default
  static ConstructorHelpers::FObjectFinder<UStaticMesh> SphereMesh(
      TEXT("/Engine/BasicShapes/Sphere.Sphere"));
  static ConstructorHelpers::FObjectFinder<UMaterialInterface> BasicMat(
      TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
  if (!BondedOrbMesh && SphereMesh.Succeeded())
    BondedOrbMesh = SphereMesh.Object;
  if (!BondedOrbMaterial && BasicMat.Succeeded())
    BondedOrbMaterial = BasicMat.Object;
}

void UOrbRenderer::OnRegister() {
  Super::OnRegister();
  EnsureISM();
}

void UOrbRenderer::BeginPlay() {
  Super::BeginPlay();
  EnsureISM();

  if (UWorld *W = GetWorld()) {
    Driver = AOrbSimDriver::Find(W);
    if (Driver)
      Sim = Driver->GetSim();

    // Set tick dependency: OrbRenderer should tick AFTER OrbSimDriver
    // This ensures we read the updated alpha value each frame
    if (Driver) {
      PrimaryComponentTick.AddPrerequisite(Driver, Driver->PrimaryActorTick);
    }
  }

  RebuildCurrPos();
  PrevPos = CurrPos;
  PrevRot = CurrRot;
  PrevAlpha = Sim ? Sim->GetFrameAlpha() : 0.f;
}

void UOrbRenderer::EndPlay(const EEndPlayReason::Type EndPlayReason) {
  // Clean up any resources if needed
  CurrPos.Empty();
  PrevPos.Empty();
  CurrRot.Empty();
  PrevRot.Empty();
  OrbTypes.Empty();
  CurrArmFacing.Empty();
  PrevArmFacing.Empty();

  Super::EndPlay(EndPlayReason);
}

void UOrbRenderer::HandleSimStepComplete(float StepSeconds) {
  // This function can be used to respond to simulation step completion
  // Currently empty as the main logic is in TickComponent
}

void UOrbRenderer::EnsureISM() {
  AActor *Owner = GetOwner();
  if (!Owner)
    return;

  // Note: Debug logging removed for performance

  // Get material manager
  UOrbMaterialManager *MaterialManager = UOrbMaterialManager::FindInWorld(this);
  if (!MaterialManager)
    return;

  // Get the base mesh from material manager
  UStaticMesh *BaseMesh = MaterialManager->GetOrbMesh();
  if (!BaseMesh)
    return;

  // Create ISM components for each orb type
  TArray<EOrbType> AllOrbTypes = {EOrbType::None,   EOrbType::BaseA,
                                  EOrbType::BaseB,  EOrbType::BaseC,
                                  EOrbType::MetalA, EOrbType::CatalystX};

  for (EOrbType OrbType : AllOrbTypes) {
    // Skip if already created
    if (OrbTypeISMs.Contains(OrbType)) {
      // Note: Debug logging removed for performance
      continue;
    }

    // Create ISM component for this orb type
    FString ComponentName =
        FString::Printf(TEXT("OrbRenderer_ISM_%d"), (int32)OrbType);
    UInstancedStaticMeshComponent *ISM =
        NewObject<UInstancedStaticMeshComponent>(Owner, *ComponentName);
    ISM->RegisterComponent();

    // Note: Debug logging removed for performance

    if (Owner->GetRootComponent()) {
      ISM->AttachToComponent(Owner->GetRootComponent(),
                             FAttachmentTransformRules::KeepRelativeTransform);
    }

    // Set mesh and material for this orb type
    ISM->SetStaticMesh(BaseMesh);

    UMaterialInterface *TypeMaterial =
        MaterialManager->GetMaterialForOrbType(OrbType);
    if (TypeMaterial) {
      UE_LOG(LogTemp, Log,
             TEXT("EnsureISM: Setting material for orb type %d to %s"),
             (int32)OrbType, *TypeMaterial->GetName());
      ISM->SetMaterial(0, TypeMaterial);
      ISM->SetRenderCustomDepth(false);

      // Verify the material was set correctly
      UMaterialInterface *VerifyMaterial = ISM->GetMaterial(0);
      UE_LOG(LogTemp, Log,
             TEXT("EnsureISM: Verified material for orb type %d is %s"),
             (int32)OrbType,
             VerifyMaterial ? *VerifyMaterial->GetName() : TEXT("NULL"));
    } else {
      UE_LOG(LogTemp, Warning,
             TEXT("EnsureISM: No material found for orb type %d"),
             (int32)OrbType);
    }

    ISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);

    // Store in map
    OrbTypeISMs.Add(OrbType, ISM);
  }

  // Create bonded orb ISM
  BondedOrbISM = NewObject<UInstancedStaticMeshComponent>(
      Owner, TEXT("BondedOrbRenderer_ISM"));
  BondedOrbISM->RegisterComponent();

  if (Owner->GetRootComponent()) {
    BondedOrbISM->AttachToComponent(
        Owner->GetRootComponent(),
        FAttachmentTransformRules::KeepRelativeTransform);
  }

  if (BondedOrbMesh)
    BondedOrbISM->SetStaticMesh(BondedOrbMesh);
  if (BondedOrbMaterial) {
    BondedOrbISM->SetMaterial(0, BondedOrbMaterial);
    // Ensure the material renders as opaque (not transparent)
    BondedOrbISM->SetRenderCustomDepth(false);
  }
  BondedOrbISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
}

AActor *UOrbRenderer::GetAnchor() const {
  if (bOverrideAnchor && AnchorOverride)
    return AnchorOverride;
  return Driver ? Driver->GetGridAnchor() : nullptr;
}

void UOrbRenderer::TickComponent(float DeltaTime, enum ELevelTick,
                                 FActorComponentTickFunction *) {
  if ((!Driver || !Sim) && bAutoReacquire) {
    if (UWorld *W = GetWorld()) {
      if (!Driver)
        Driver = AOrbSimDriver::Find(W);
      if (Driver && !Sim)
        Sim = Driver->GetSim();
      if (Sim) {
        RebuildCurrPos();
        PrevPos = CurrPos;
        PrevRot = CurrRot;
      }

      // Set tick dependency if driver was just acquired
      if (Driver) {
        PrimaryComponentTick.AddPrerequisite(Driver, Driver->PrimaryActorTick);
      }
    }
  }
  if (!Driver || !Sim)
    return;

  // Get alpha from GameTickManager directly for proper synchronization
  float Alpha = 0.0f;
  if (AGameTickManager *TickManager = AGameTickManager::Get(this)) {
    Alpha = TickManager->GetStepAlpha();
  } else if (Sim) {
    Alpha = Sim->GetFrameAlpha(); // Fallback
  }
  const int32 Step = Sim->GetStepCounter();
  const int32 Revision = Sim->TileRevision;

  // Detect new fixed step via counter OR instantaneous item changes via
  // revision
  if (Step != LastSeenStepCounter || Revision != LastSeenTileRevision) {
    bool bStepChanged = (Step != LastSeenStepCounter);
    LastSeenStepCounter = Step;
    LastSeenTileRevision = Revision;

    // Trigger full ISM rebuild on step/revision change (orbs can be
    // added/removed/merged)
    bNeedFullRebuild = true;

    if (bStepChanged) {
      // Step change: preserve previous state for interpolation
      PrevArmFacing = CurrArmFacing;
      CurrArmFacing.Reset();

      // Save current positions as previous (these are from BEFORE the step)
      PrevPos = CurrPos;
      PrevRot = CurrRot;
    } else {
      // Instantaneous change (e.g. deletion): snap previous to current to avoid
      // ghosting
      PrevPos = CurrPos;
      PrevRot = CurrRot;
    }

    // Rebuild positions from the NEW sim state
    RebuildCurrPos();

    if (bStepChanged) {
      for (int32 ArmIdx = 0; ArmIdx < Sim->Arms.Num(); ++ArmIdx) {
        CurrArmFacing.Add(ArmIdx, (uint8)Sim->Arms[ArmIdx].Facing);
        if (!PrevArmFacing.Contains(ArmIdx))
          PrevArmFacing.Add(ArmIdx, CurrArmFacing[ArmIdx]);
      }
    }
  }

  // Draw debug alpha dial if enabled (BEFORE updating PrevAlpha so we can see
  // the difference)
  if (bShowAlphaDial) {
    DrawDebugAlphaDial(Alpha, Step);
  }

  // Update animated orbs
  Sim->UpdateAnimatedOrbs(DeltaTime);

  // Process pending bond formations
  Sim->ProcessPendingBondFormations(DeltaTime);

  RebuildInstances();
  RebuildBondedOrbs();

  // Update PrevAlpha at the END so it's correct for the next frame
  PrevAlpha = Alpha;
}

void UOrbRenderer::DrawDebugAlphaDial(float Alpha, int32 Step) {
  if (!GetWorld())
    return;

  APlayerController *PC = GetWorld()->GetFirstPlayerController();
  if (!PC)
    return;

  FVector CameraLocation;
  FRotator CameraRotation;
  PC->GetPlayerViewPoint(CameraLocation, CameraRotation);

  // === MAIN STATUS DISPLAY (follows camera for top-down view) ===
  // Position in top-right corner relative to camera
  FVector StatusPos = CameraLocation;
  StatusPos.X += 800.0f; // Right
  StatusPos.Y -= 600.0f; // Forward/back depending on camera
  StatusPos.Z = 50.0f;   // Fixed height above ground

  FString StatusText = FString::Printf(TEXT("STEP %d | α %.3f | Δα %.3f"), Step,
                                       Alpha, Alpha - PrevAlpha);
  DrawDebugString(GetWorld(), StatusPos, StatusText, nullptr, FColor::Cyan,
                  0.0f, true, 3.0f);

  // === ALPHA DIAL (circular progress indicator) ===
  FVector DialCenter = StatusPos + FVector(-100, 150, 0); // Near status
  float DialRadius = 60.0f;

  const int32 NumSegments = 32;
  for (int32 i = 0; i < NumSegments; ++i) {
    float Angle1 = (float)i / NumSegments * 2.0f * PI;
    float Angle2 = (float)(i + 1) / NumSegments * 2.0f * PI;

    FVector Point1 = DialCenter + FVector(FMath::Cos(Angle1) * DialRadius,
                                          FMath::Sin(Angle1) * DialRadius, 5);
    FVector Point2 = DialCenter + FVector(FMath::Cos(Angle2) * DialRadius,
                                          FMath::Sin(Angle2) * DialRadius, 5);

    DrawDebugLine(GetWorld(), Point1, Point2, FColor::White, false, -1.0f, 0,
                  2.0f);
  }

  float AngleRadians = Alpha * 2.0f * PI;
  FVector DialPos =
      DialCenter + FVector(FMath::Cos(AngleRadians) * DialRadius,
                           FMath::Sin(AngleRadians) * DialRadius, 5);

  bool bJustStepped = (Alpha < PrevAlpha);
  FColor IndicatorColor = bJustStepped ? FColor::Red : FColor::Green;
  DrawDebugSphere(GetWorld(), DialPos + FVector(0, 0, 5), 10.0f, 8,
                  IndicatorColor, false, -1.0f, 0, 3.0f);
  DrawDebugSphere(GetWorld(), DialCenter + FVector(0, 0, 5), 5.0f, 8,
                  FColor::Yellow, false, -1.0f, 0, 2.0f);

  // === PER-ORB VISUALIZATION (at each orb's location) ===
  if (Sim) {
    int32 OrbsVisualized = 0;
    const int32 MaxOrbsToShow = 15;

    for (const auto &KV : Sim->ItemMap) {
      const FItem &Item = KV.Value;
      const int32 OrbId = Item.Id;

      // Skip invalid IDs and clusters
      if (OrbId == 0 || Item.bIsCluster)
        continue;

      FVector *PrevP = PrevPos.Find(OrbId);
      FVector *CurrP = CurrPos.Find(OrbId);
      if (!PrevP || !CurrP)
        continue;

      float PosDelta = FVector::Dist(*PrevP, *CurrP);
      FVector InterpPos = FMath::Lerp(*PrevP, *CurrP, Alpha);

      // Determine orb state
      bool bHasMovement = PosDelta > 0.1f; // Any position change
      bool bIsOnBelt = false;
      const FTile *Tile = Sim->TileMap.Find(KV.Key);
      if (Tile && Tile->Kind == ETileKind::Belt) {
        bIsOnBelt = true;
      }

      // Color coding based on actual interpolation state:
      // Green = Has movement (good interpolation)
      // Red = No movement (stuck - PrevPos == CurrPos)
      // Cyan = Not on belt
      FColor MarkerColor;
      if (bIsOnBelt) {
        MarkerColor = bHasMovement ? FColor::Green : FColor::Red; // Red = STUCK
      } else {
        MarkerColor = FColor::Cyan;
      }

      // Draw interpolation visualization
      if (bHasMovement) {
        // Path line (prev → curr) - shows expected movement
        DrawDebugLine(GetWorld(), *PrevP + FVector(0, 0, 15),
                      *CurrP + FVector(0, 0, 15), FColor::Yellow, false, -1.0f,
                      0, 3.0f);

        // Endpoint markers
        DrawDebugSphere(GetWorld(), *PrevP + FVector(0, 0, 15), 8.0f, 6,
                        FColor::Red, false, -1.0f, 0, 2.0f);
        DrawDebugSphere(GetWorld(), *CurrP + FVector(0, 0, 15), 8.0f, 6,
                        FColor::Blue, false, -1.0f, 0, 2.0f);
      } else if (bIsOnBelt) {
        // NO MOVEMENT - draw warning indicator
        DrawDebugSphere(GetWorld(), InterpPos + FVector(0, 0, 15), 20.0f, 8,
                        FColor::Red, false, -1.0f, 0, 1.0f);
      }

      // Main position marker at interpolated location
      DrawDebugSphere(GetWorld(), InterpPos + FVector(0, 0, 15), 10.0f, 8,
                      MarkerColor, false, -1.0f, 0, 3.0f);

      // Debug info text above orb
      FString OrbInfo;
      if (bIsOnBelt) {
        FString State = bHasMovement ? TEXT("OK") : TEXT("STUCK");
        OrbInfo =
            FString::Printf(TEXT("%d\n%s Δ%.0f"), OrbId, *State, PosDelta);
      } else {
        OrbInfo = FString::Printf(TEXT("%d\nOFF"), OrbId);
      }

      DrawDebugString(GetWorld(), InterpPos + FVector(0, 0, 35), OrbInfo,
                      nullptr, MarkerColor, 0.0f, true, 1.1f);

      OrbsVisualized++;
      if (OrbsVisualized >= MaxOrbsToShow)
        break;
    }

    // Legend (positioned near dial)
    int32 TotalOrbs = Sim->ItemMap.Num();
    FString Legend =
        FString::Printf(TEXT("LEGEND:\n● Green = Interpolating OK\n● Red = "
                             "STUCK (no Δ)\n● Cyan = Off belt\n%d/%d orbs"),
                        OrbsVisualized, TotalOrbs);
    DrawDebugString(GetWorld(), DialCenter + FVector(150, 80, 0), Legend,
                    nullptr, FColor::White, 0.0f, true, 1.5f);
  }
}

void UOrbRenderer::RebuildCurrPos() {
  CurrPos.Reset();
  CurrRot.Reset();
  OrbTypes.Reset(); // Clear type cache
  if (!Sim)
    return;

  const AActor *Anchor = GetAnchor();
  const FVector AnchorV =
      Anchor ? Anchor->GetActorLocation() : FVector::ZeroVector;
  const float Tile = Driver ? Driver->GetTileSize() : Sim->TileSizeUU;
  const float Yaw = Driver ? Driver->GetGridYaw() : 0.f;
  const EHexOrientation Ori =
      Driver ? Driver->GetGridOrientation() : EHexOrientation::PointyTop;
  const bool bSphere = (Driver && Driver->IsSphereEnabled());

  auto ComputeUprightRot = [&](const FVector &WorldPos,
                               const FHex &H) -> FRotator {
    if (!bSphere)
      return FRotator::ZeroRotator;
    FVector Up = (WorldPos - AnchorV).GetSafeNormal();
    if (Up.IsNearlyZero())
      Up = FVector::UpVector;
    // Approx forward: direction toward axial +1 in direction D0 (east) mapped
    // on sphere
    FHex Neighbor = HexUtil::Neighbor(H, EHexDir::D0);
    FVector NeighborPos = Driver->MapHexToWorld(Neighbor);
    FVector Forward = (NeighborPos - WorldPos).GetSafeNormal();
    if (Forward.IsNearlyZero()) {
      Forward = FVector::CrossProduct(FVector::UpVector, Up).GetSafeNormal();
      if (Forward.IsNearlyZero())
        Forward = FVector::ForwardVector;
    }
    FVector Right = FVector::CrossProduct(Forward, Up).GetSafeNormal();
    Forward = FVector::CrossProduct(Up, Right).GetSafeNormal();
    FMatrix M(Forward, Right, Up, FVector::ZeroVector);
    return M.Rotator();
  };

  // Board items (not held) - just store tile centers, we'll apply belt progress
  // during rendering
  for (const auto &KV : Sim->ItemMap) {
    const FItem &Item = KV.Value;
    const int32 ItemId = Item.Id;
    if (ItemId == 0)
      continue;
    const FHex H = KV.Key;

    // Calculate base tile center position
    FVector TileWorld = Driver ? Driver->MapHexToWorld(H) : [&]() {
      const FHexAxial A(H.Q, H.R);
      const FVector2D LocalXY = HexCoord::AxialToXY(A, Tile, Ori);
      const FVector2D RotXY = HexCoord::Rotate2D(LocalXY, Yaw);
      const float Z = (H.Level == 0) ? (Sim ? Sim->OrbZOffset : 50.f)
                                     : -(Sim ? Sim->OrbZOffset : 50.f);
      return AnchorV + FVector(RotXY.X, RotXY.Y, Z);
    }();

    // Position is just the tile center - interpolation happens in
    // RebuildInstances() using Alpha No need for belt progress offset here!

    // Determine orb type for this item (base or cluster first orb)
    EOrbType ItemType = EOrbType::BaseA;
    if (Item.bIsCluster && Item.Orbs.Num() > 0) {
      ItemType = (EOrbType)Item.Orbs[0].Type;
    } else if (!Item.bIsCluster) {
      ItemType = Item.Single.Type;
    }

    if (!Item.bIsCluster) {
      CurrPos.Add(ItemId, TileWorld);
      OrbTypes.Add(ItemId, ItemType);
      if (bSphere)
        CurrRot.Add(ItemId, ComputeUprightRot(TileWorld, H));
    } else {
      // For clusters, apply the belt-adjusted center position to all sub-orbs
      for (int32 SubIdx = 0; SubIdx < Item.Orbs.Num(); ++SubIdx) {
        const FOrbData &Sub = Item.Orbs[SubIdx];
        const int32 SubId = MakeSubOrbId(ItemId, SubIdx);
        FVector SubWorld;
        if (bSphere) {
          FHex SubHex(H.Q + Sub.dQ, H.R + Sub.dR);
          SubWorld = Driver->MapHexToWorld(SubHex);
        } else {
          const FVector2D OffRot = RotateXY(Sub.Offset * Tile, Yaw);
          SubWorld = TileWorld + FVector(OffRot.X, OffRot.Y, 0.f);
        }
        CurrPos.Add(SubId, SubWorld);
        // Cache type for sub-orb
        OrbTypes.Add(SubId, (EOrbType)Sub.Type);
        if (bSphere)
          CurrRot.Add(SubId, ComputeUprightRot(
                                 SubWorld, FHex(H.Q + Sub.dQ, H.R + Sub.dR)));
      }
    }
  }

  // Held items are rendered by arm meshes, not by the renderer's ISM system
  // This prevents interference with held cluster rotation interpolation
  // The renderer only handles board items and animated orbs
}

void UOrbRenderer::RebuildInstances() {
  EnsureISM();
  if (!Driver || !Sim)
    return;

  // Early return if orbs should be hidden for debugging
  if (!bShowOrbs) {
    // Clear all ISM instances if hiding orbs
    for (auto &KV : OrbTypeISMs) {
      if (KV.Value) {
        KV.Value->ClearInstances();
      }
    }
    OrbIdToInstanceIndex.Empty();
    bNeedFullRebuild = true;
    return;
  }

  // Clear and rebuild ISMs when simulation step changes (orbs can be
  // added/removed)
  if (bNeedFullRebuild) {
    for (auto &KV : OrbTypeISMs) {
      if (KV.Value) {
        KV.Value->ClearInstances();
      }
    }
    OrbIdToInstanceIndex.Empty();
    AnimatedOrbToInstanceIndex.Empty(); // Also clear animated orb tracking
    bNeedFullRebuild = false;
  }

  // Get alpha from GameTickManager directly for proper synchronization
  float Alpha = 0.0f;
  if (AGameTickManager *TickManager = AGameTickManager::Get(this)) {
    Alpha = TickManager->GetStepAlpha();
  } else if (Sim) {
    Alpha = Sim->GetFrameAlpha(); // Fallback
  }

  // Build set of held item ids so we don't double render
  TSet<int32> HeldIds;
  for (const FArmState &S : Sim->Arms) {
    if (S.bHolding && S.Held.Id != 0) {
      HeldIds.Add(S.Held.Id);
      // Also add sub-orb IDs for clusters
      if (S.Held.bIsCluster) {
        for (const FOrbData &Sub : S.Held.Orbs) {
          if (Sub.ItemId > 0) {
            HeldIds.Add(Sub.ItemId);
          }
        }
      }
    }
  }

  // Build set of animated output item ids so we don't double render them
  // Input orbs should still be visible on input tiles during ascent
  TSet<int32> AnimatedOutputIds;
  for (const USimWorldHex::FAnimatedOrb &AnimatedOrb : Sim->GetAnimatedOrbs()) {
    if (AnimatedOrb.bActive && !AnimatedOrb.bIsInput) {
      AnimatedOutputIds.Add(AnimatedOrb.Item.Id);
    }
  }

  // Render animated orbs (inputs ascending, outputs descending) - these use sim
  // interpolation
  RenderAnimatedOrbs();

  // Legacy prev->curr interpolation path
  TSet<int32> AllIds;
  PrevPos.GetKeys(AllIds);
  for (const auto &K : CurrPos)
    AllIds.Add(K.Key);

  for (int32 Id : AllIds) {
    if (HeldIds.Contains(Id))
      continue;
    if (AnimatedOutputIds.Contains(Id))
      continue; // Skip output orbs that are being animated (descending)

    const FVector *A = PrevPos.Find(Id);
    const FVector *B = CurrPos.Find(Id);
    const FRotator *RotA = PrevRot.Find(Id);
    const FRotator *RotB = CurrRot.Find(Id);

    const FVector From = A ? *A : (B ? *B : FVector::ZeroVector);
    const FVector To = B ? *B : From;

    FVector P;
    // Check if this orb is currently held by an arm
    bool bHeldByArm = false;
    // FVector ArmBase;
    for (const FArmState &S : Sim->Arms) {
      if (S.bHolding && S.Held.Id != 0) {
        HeldIds.Add(S.Held.Id);
        if (S.Held.bIsCluster) {
          for (int32 i = 0; i < S.Held.Orbs.Num(); ++i) {
            HeldIds.Add(MakeSubOrbId(
                S.Held.Id, i)); // Skip sub‑orb ISM instances while held
          }
        }
      }
    }

    // Simple interpolation between previous and current positions
    // Belt progress offset is already baked into CurrPos by RebuildCurrPos()
    P = FMath::Lerp(From, To, Alpha);

    // Apply belt progress offset for continuous movement (calculated fresh each
    // frame)
    if (!bHeldByArm) {
      // Find what tile this orb is on
      int32 ParentId = (Id >> 8); // Extract parent from sub-orb ID
      if (ParentId == 0)
        ParentId = Id;

      for (const auto &KV : Sim->ItemMap) {
        if (KV.Value.Id == ParentId) {
          const FHex &ItemHex = KV.Key;
          const FTile *T = Sim->TileMap.Find(ItemHex);

          if (T && T->Kind == ETileKind::Belt) {
            float BeltProgress = Sim->GetBeltProgress(ParentId);

            if (BeltProgress > 0.0f) {
              // Calculate next tile position
              uint8 BeltDir = (uint8)(T->Arg & 0xFF);
              FHex NextHex = HexUtil::Neighbor(ItemHex, (EHexDir)(BeltDir % 6));

              const AActor *Anchor = GetAnchor();
              const FVector AnchorV =
                  Anchor ? Anchor->GetActorLocation() : FVector::ZeroVector;
              const float Tile =
                  Driver ? Driver->GetTileSize() : Sim->TileSizeUU;
              const float Yaw = Driver ? Driver->GetGridYaw() : 0.f;
              const EHexOrientation Ori = Driver ? Driver->GetGridOrientation()
                                                 : EHexOrientation::PointyTop;
              const bool bSphere = (Driver && Driver->IsSphereEnabled());

              FVector NextTileWorld =
                  Driver ? Driver->MapHexToWorld(NextHex) : [&]() {
                    const FHexAxial NextA(NextHex.Q, NextHex.R);
                    const FVector2D NextLocalXY =
                        HexCoord::AxialToXY(NextA, Tile, Ori);
                    const FVector2D NextRotXY =
                        HexCoord::Rotate2D(NextLocalXY, Yaw);
                    const float Z = (NextHex.Level == 0)
                                        ? (Sim ? Sim->OrbZOffset : 50.f)
                                        : -(Sim ? Sim->OrbZOffset : 50.f);
                    return AnchorV + FVector(NextRotXY.X, NextRotXY.Y, Z);
                  }();

              // Apply belt offset to the interpolated position
              P = FMath::Lerp(P, NextTileWorld, BeltProgress);
            }
          }
          break;
        }
      }
    }

    // Arm rotation arc (only if held and position changed by rotation)
    if (bHeldByArm) {
      // Find arm record
      const AActor *Anchor = GetAnchor();
      const float Tile = Driver ? Driver->GetTileSize() : Sim->TileSizeUU;
      const float GridYaw = Driver ? Driver->GetGridYaw() : 0.f;
      const EHexOrientation Ori =
          Driver ? Driver->GetGridOrientation() : EHexOrientation::PointyTop;

      for (int32 ArmIdx = 0; ArmIdx < Sim->Arms.Num(); ++ArmIdx) {
        const FArmState &S = Sim->Arms[ArmIdx];
        if (S.bHolding && S.Held.Id == Id) {
          const uint8 *PrevF = PrevArmFacing.Find(ArmIdx);
          const uint8 *CurrF = CurrArmFacing.Find(ArmIdx);
          if (PrevF && CurrF) {
            float YawA = DirToYaw(*PrevF);
            float YawB = DirToYaw(*CurrF);
            float YawI = LerpAngleDeg(YawA, YawB, Alpha);
            P = ComputeArmGripAtYaw(S, YawI, Anchor, Tile, GridYaw, Ori);
          }
          break;
        }
      }
    }

    FRotator RA = RotA ? *RotA : FRotator::ZeroRotator;
    FRotator RB = RotB ? *RotB : RA;
    FRotator R = FMath::Lerp(RA, RB, Alpha);

    // Get the orb type for this item using optimized lookup
    EOrbType OrbType = EOrbType::BaseA; // Default
    if (const EOrbType *FoundType = OrbTypes.Find(Id)) {
      OrbType = *FoundType;
    } else {
      // Fallback to searching if not in cache (should rarely happen)
      for (const auto &KV : Sim->ItemMap) {
        if (KV.Value.Id == Id) {
          if (KV.Value.bIsCluster && KV.Value.Orbs.Num() > 0) {
            OrbType = (EOrbType)KV.Value.Orbs[0].Type;
          } else if (!KV.Value.bIsCluster) {
            OrbType = KV.Value.Single.Type;
          }
          // Cache it for next time
          OrbTypes.Add(Id, OrbType);
          break;
        }
      }
    }

    // Prepare batch update or add new instance
    if (UInstancedStaticMeshComponent **ISMPtr = OrbTypeISMs.Find(OrbType)) {
      if (UInstancedStaticMeshComponent *ISM = *ISMPtr) {
        FTransform Transform(R, P, FVector(OrbScale));

        // Check if this orb already has an instance
        TMap<int32, int32> &InstanceMap =
            OrbIdToInstanceIndex.FindOrAdd(OrbType);
        int32 *ExistingIndex = InstanceMap.Find(Id);

        if (ExistingIndex && ISM->IsValidInstance(*ExistingIndex)) {
          // Update existing instance - bTeleport=false allows proper motion
          // vectors for TAA/motion blur
          ISM->UpdateInstanceTransform(*ExistingIndex, Transform, false, true,
                                       false);
        } else {
          // Add new instance immediately and track its index
          int32 NewIndex = ISM->AddInstance(Transform);
          InstanceMap.Add(Id, NewIndex);
        }
      }
    }
  }
}

float UOrbRenderer::DirToYaw(uint8 Dir) const {
  const float GridYaw = Driver ? Driver->GetGridYaw() : 0.f;
  return FMath::UnwindDegrees(GridYaw + 60.f * float(Dir % 6));
}

void UOrbRenderer::RebuildBondedOrbs() {
  // Bonded orb rendering is now handled by BasicArm for held clusters
  // This function is kept for potential future use with board-only bonds
  EnsureISM();
  if (!BondedOrbISM || !Driver || !Sim)
    return;

  BondedOrbISM->ClearInstances();
  if (!BondedOrbMesh || !bShowOrbs)
    return;

  // For now, only render bonds between board items (not held items)
  // Held cluster bonds are handled by BasicArm
  // Get alpha from GameTickManager directly for proper synchronization
  float Alpha = 0.0f;
  if (AGameTickManager *TickManager = AGameTickManager::Get(this)) {
    Alpha = TickManager->GetStepAlpha();
  } else if (Sim) {
    Alpha = Sim->GetFrameAlpha(); // Fallback
  }
  const AActor *Anchor = GetAnchor();
  const FVector AnchorV =
      Anchor ? Anchor->GetActorLocation() : FVector::ZeroVector;
  const float Tile = Driver ? Driver->GetTileSize() : Sim->TileSizeUU;
  const float Yaw = Driver ? Driver->GetGridYaw() : 0.f;
  const EHexOrientation Ori =
      Driver ? Driver->GetGridOrientation() : EHexOrientation::PointyTop;

  // Render bonded orb connections for board items only
  for (const auto &Bond : Sim->GetBonds()) {
    if (!Bond.bActive)
      continue;

    // Get positions of both bonded items
    FVector PosA, PosB;
    bool bFoundA = false, bFoundB = false;

    // Helper function to get interpolated position for board items only
    auto GetInterpolatedPosition = [&](int32 ItemId, FVector &OutPos) -> bool {
      // Only check position maps for board items
      const FVector *CurrPosItem = CurrPos.Find(ItemId);
      const FVector *PrevPosItem = PrevPos.Find(ItemId);

      if (CurrPosItem) {
        const FVector From = PrevPosItem ? *PrevPosItem : *CurrPosItem;
        OutPos = FMath::Lerp(From, *CurrPosItem, Alpha);
        return true;
      }
      return false;
    };

    // Get positions for both bonded items
    bFoundA = GetInterpolatedPosition(Bond.ItemA, PosA);
    bFoundB = GetInterpolatedPosition(Bond.ItemB, PosB);

    // If we found both positions, create a connection mesh
    if (bFoundA && bFoundB) {
      // Calculate midpoint and direction
      FVector Midpoint = (PosA + PosB) * 0.5f;
      FVector Direction = (PosB - PosA).GetSafeNormal();
      float Distance = FVector::Dist(PosA, PosB);

      // For a cylinder that's standing up by default, we need to rotate it to
      // align with the bond direction The cylinder's default "up" axis is Z, so
      // we need to rotate it so its Z-axis aligns with the bond direction
      FVector Up =
          Direction; // The bond direction becomes the new "up" for the cylinder
      FVector Right =
          FVector::CrossProduct(Up, FVector::UpVector).GetSafeNormal();
      if (Right.IsNearlyZero()) {
        // If the bond is vertical, use a different approach
        Right = FVector::RightVector;
      }
      FVector Forward = FVector::CrossProduct(Right, Up).GetSafeNormal();

      // Create rotation matrix from the three axes
      FMatrix RotationMatrix = FMatrix(Forward, Right, Up, FVector::ZeroVector);
      FRotator Rotation = FQuat(RotationMatrix).Rotator();

      // Scale the cylinder to fit the bond length
      // The cylinder's length is along its Z-axis, so we scale Z by the
      // distance
      FVector Scale = FVector(BondedOrbScale, BondedOrbScale, 1.f);

      // Add the bonded orb instance
      BondedOrbISM->AddInstance(FTransform(Rotation, Midpoint, Scale));
    }
  }

  // Also render bonds for animated orbs (outputs descending, inputs ascending)
  const TArray<USimWorldHex::FAnimatedOrb> &AnimatedOrbs =
      Sim->GetAnimatedOrbs();

  for (int32 AnimIdx = 0; AnimIdx < AnimatedOrbs.Num(); ++AnimIdx) {
    const USimWorldHex::FAnimatedOrb &AnimatedOrb = AnimatedOrbs[AnimIdx];
    if (!AnimatedOrb.bActive || !AnimatedOrb.bIsBonded)
      continue;

    // Find the partner animation
    int32 PartnerAnimIdx = -1;
    for (int32 i = 0; i < AnimatedOrbs.Num(); ++i) {
      if (AnimatedOrbs[i].bActive &&
          AnimatedOrbs[i].Item.Id == AnimatedOrb.PartnerItemId) {
        PartnerAnimIdx = i;
        break;
      }
    }

    if (PartnerAnimIdx == -1)
      continue;

    const USimWorldHex::FAnimatedOrb &PartnerOrb = AnimatedOrbs[PartnerAnimIdx];

    // Only render bond once per pair (from lower index to higher)
    if (AnimIdx > PartnerAnimIdx)
      continue;

    // Calculate positions for both orbs
    const FHexAxial A1(AnimatedOrb.Hex.Q, AnimatedOrb.Hex.R);
    const FVector2D LocalXY1 = HexCoord::AxialToXY(A1, Tile, Ori);
    const FVector2D RotXY1 = HexCoord::Rotate2D(LocalXY1, Yaw);
    const FVector BasePos1 = AnchorV + FVector(RotXY1.X, RotXY1.Y, 0.f);

    const FHexAxial A2(PartnerOrb.Hex.Q, PartnerOrb.Hex.R);
    const FVector2D LocalXY2 = HexCoord::AxialToXY(A2, Tile, Ori);
    const FVector2D RotXY2 = HexCoord::Rotate2D(LocalXY2, Yaw);
    const FVector BasePos2 = AnchorV + FVector(RotXY2.X, RotXY2.Y, 0.f);

    // Calculate animation progress for both orbs
    const int32 CurrentStep = Sim->GetStepCounter();

    auto CalcZOffset = [&](const USimWorldHex::FAnimatedOrb &Orb) -> float {
      float StepProgress = 0.f;
      const int32 AnimationDuration = (Orb.CompletionStep) - Orb.StartStep;

      if (CurrentStep < Orb.StartStep)
        StepProgress = 0.f;
      else if (CurrentStep >= Orb.CompletionStep)
        StepProgress = 1.f;
      else {
        const int32 StepsElapsed = CurrentStep - Orb.StartStep;
        StepProgress =
            (StepsElapsed + Alpha) / FMath::Max(1.0f, (float)AnimationDuration);
      }

      const float Progress = FMath::Clamp(StepProgress, 0.f, 1.f);

      // Maintain constant descent rate: 50 units per 3 ticks
      constexpr float DescentRatePerTick = 100.0f / 3.0f;
      const float TotalDistance = DescentRatePerTick * (float)AnimationDuration;

      if (Orb.bIsInput)
        return FMath::Lerp(-TotalDistance, 0.f, Progress);
      else
        return FMath::Lerp(0.f, -TotalDistance, Progress);
    };

    const FVector FinalPos1 =
        BasePos1 + FVector(0.f, 0.f, CalcZOffset(AnimatedOrb));
    const FVector FinalPos2 =
        BasePos2 + FVector(0.f, 0.f, CalcZOffset(PartnerOrb));

    // Calculate midpoint and direction for the bond mesh
    const FVector Midpoint = (FinalPos1 + FinalPos2) * 0.5f;
    const FVector Direction = (FinalPos2 - FinalPos1).GetSafeNormal();
    const float Distance = FVector::Dist(FinalPos1, FinalPos2);

    if (Distance > 0.1f) {
      // Use same rotation calculation as static bonds
      FVector Up = Direction;
      FVector Right =
          FVector::CrossProduct(Up, FVector::UpVector).GetSafeNormal();
      if (Right.IsNearlyZero()) {
        Right = FVector::RightVector;
      }
      FVector Forward = FVector::CrossProduct(Right, Up).GetSafeNormal();

      FMatrix RotationMatrix = FMatrix(Forward, Right, Up, FVector::ZeroVector);
      FRotator Rotation = FQuat(RotationMatrix).Rotator();

      // Use same scale as static bonds and held cluster bonds
      FVector Scale = FVector(BondedOrbScale, BondedOrbScale, 1.f);

      BondedOrbISM->AddInstance(FTransform(Rotation, Midpoint, Scale));
    }
  }
}

void UOrbRenderer::RenderAnimatedOrbs() {
  if (!Sim || !bShowOrbs)
    return;

  const TArray<USimWorldHex::FAnimatedOrb> &AnimatedOrbs =
      Sim->GetAnimatedOrbs();
  if (AnimatedOrbs.Num() == 0)
    return;

  const AActor *Anchor = GetAnchor();
  const FVector AnchorV =
      Anchor ? Anchor->GetActorLocation() : FVector::ZeroVector;
  const float Tile = Driver ? Driver->GetTileSize() : Sim->TileSizeUU;
  const float Yaw = Driver ? Driver->GetGridYaw() : 0.f;
  const EHexOrientation Ori =
      Driver ? Driver->GetGridOrientation() : EHexOrientation::PointyTop;
  // Get alpha from GameTickManager directly for proper synchronization
  float Alpha = 0.0f;
  if (AGameTickManager *TickManager = AGameTickManager::Get(this)) {
    Alpha = TickManager->GetStepAlpha();
  } else if (Sim) {
    Alpha = Sim->GetFrameAlpha(); // Fallback
  }

  for (int32 AnimIdx = 0; AnimIdx < AnimatedOrbs.Num(); ++AnimIdx) {
    const USimWorldHex::FAnimatedOrb &AnimatedOrb = AnimatedOrbs[AnimIdx];
    if (!AnimatedOrb.bActive)
      continue;

    // Calculate base position at the hex center
    const FHexAxial A(AnimatedOrb.Hex.Q, AnimatedOrb.Hex.R);
    const FVector2D LocalXY = HexCoord::AxialToXY(A, Tile, Ori);
    const FVector2D RotXY = HexCoord::Rotate2D(LocalXY, Yaw);
    const FVector BasePos = AnchorV + FVector(RotXY.X, RotXY.Y, 0.f);

    // Calculate animation progress (0.0 to 1.0)
    const int32 CurrentStep = Sim->GetStepCounter();
    // Interpolate during the animation window (from StartStep to
    // CompletionStep)
    float StepProgress = 0.f;

    const int32 AnimationDuration =
        AnimatedOrb.CompletionStep - AnimatedOrb.StartStep;

    if (CurrentStep < AnimatedOrb.StartStep) {
      StepProgress = 0.f; // Not started yet
    } else if (CurrentStep >= AnimatedOrb.CompletionStep) {
      StepProgress = 1.f; // Completed
    } else {
      // Calculate smooth progress based on step counter and frame alpha
      // This ensures constant animation rate regardless of step boundaries
      const int32 StepsElapsed = CurrentStep - AnimatedOrb.StartStep;
      StepProgress =
          (StepsElapsed + Alpha) / FMath::Max(1.0f, (float)AnimationDuration);
    }

    const float Progress = FMath::Clamp(StepProgress, 0.f, 1.f);

    // Calculate Z offset based on animation type and progress
    // Maintain constant descent rate: 50 units per 3 ticks
    constexpr float DescentRatePerTick = 100.0f / 3.0f;
    const float TotalDistance = DescentRatePerTick * (float)AnimationDuration;

    float ZOffset = 0.f;
    if (AnimatedOrb.bIsInput) {
      // Input: start below ground, ascend to tile level
      ZOffset = FMath::Lerp(-TotalDistance, 0.f, Progress);
    } else {
      // Output: start at tile level, descend below ground
      ZOffset = FMath::Lerp(0.f, -TotalDistance, Progress);
    }

    const FVector FinalPos = BasePos + FVector(0.f, 0.f, ZOffset);

    // Get the orb type
    EOrbType OrbType = EOrbType::BaseA; // Default
    if (AnimatedOrb.Item.bIsCluster && AnimatedOrb.Item.Orbs.Num() > 0) {
      OrbType = (EOrbType)AnimatedOrb.Item.Orbs[0].Type;
    } else if (!AnimatedOrb.Item.bIsCluster) {
      OrbType = AnimatedOrb.Item.Single.Type;
    }

    // Add or update animated orb instance
    if (UInstancedStaticMeshComponent **ISMPtr = OrbTypeISMs.Find(OrbType)) {
      if (UInstancedStaticMeshComponent *ISM = *ISMPtr) {
        FTransform Transform(FRotator::ZeroRotator, FinalPos,
                             FVector(OrbScale));

        // Check if this animated orb already has an instance
        TMap<int32, int32> &AnimInstanceMap =
            AnimatedOrbToInstanceIndex.FindOrAdd(OrbType);
        int32 *ExistingIndex = AnimInstanceMap.Find(AnimIdx);

        if (ExistingIndex && ISM->IsValidInstance(*ExistingIndex)) {
          // Update existing animated orb instance - bTeleport=false for proper
          // motion vectors
          ISM->UpdateInstanceTransform(*ExistingIndex, Transform, false, true,
                                       false);
        } else {
          // Add new instance immediately and track its index
          int32 NewIndex = ISM->AddInstance(Transform);
          AnimInstanceMap.Add(AnimIdx, NewIndex);
        }
      }
    }
  }

  // Animated bonds are now rendered in RebuildBondedOrbs() to avoid clearing
  // issues
}
