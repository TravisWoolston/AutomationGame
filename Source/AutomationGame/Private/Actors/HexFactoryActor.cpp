#include "Actors/HexFactoryActor.h"
#include "Actors/HexBoardActor.h"
#include "Actors/OrbSimDriver.h"
#include "Components/BoxComponent.h"
#include "Components/HexPlacementComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Data/OrbMaterialManager.h"
#include "DrawDebugHelpers.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Framework/GameManager.h"
#include "Rendering/OrbRenderer.h"
#include "Sim/HexCoord.h"
#include "Sim/OrbTypes.h"
#include "Sim/SimWorldHex.h"

AHexFactoryActor::AHexFactoryActor() {
  PrimaryActorTick.bCanEverTick = true;

  USceneComponent *Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
  SetRootComponent(Root);

  TileISM =
      CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("TileISM"));
  TileISM->SetupAttachment(Root);
  TileISM->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
  TileISM->SetCollisionResponseToAllChannels(ECollisionResponse::ECR_Block);
  TileISM->SetCanEverAffectNavigation(false);

  InputTileISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(
      TEXT("InputTileISM"));
  InputTileISM->SetupAttachment(Root);
  InputTileISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
  InputTileISM->SetCanEverAffectNavigation(false);

  OutputTileISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(
      TEXT("OutputTileISM"));
  OutputTileISM->SetupAttachment(Root);
  OutputTileISM->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
  OutputTileISM->SetCollisionResponseToAllChannels(
      ECollisionResponse::ECR_Block);
  OutputTileISM->SetCanEverAffectNavigation(false);

  BeltTileISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(
      TEXT("BeltTileISM"));
  BeltTileISM->SetupAttachment(Root);
  BeltTileISM->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
  BeltTileISM->SetCollisionResponseToAllChannels(ECollisionResponse::ECR_Block);
  BeltTileISM->SetCanEverAffectNavigation(false);

  BondTileISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(
      TEXT("BondTileISM"));
  BondTileISM->SetupAttachment(Root);
  BondTileISM->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
  BondTileISM->SetCollisionResponseToAllChannels(ECollisionResponse::ECR_Block);
  BondTileISM->SetCanEverAffectNavigation(false);

  UnbondTileISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(
      TEXT("UnbondTileISM"));
  UnbondTileISM->SetupAttachment(Root);
  UnbondTileISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
  UnbondTileISM->SetCanEverAffectNavigation(false);

  BeltISM =
      CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("BeltISM"));
  BeltISM->SetupAttachment(Root);
  BeltISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
  BeltISM->SetCanEverAffectNavigation(false);

  BeltReadyISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(
      TEXT("BeltReadyISM"));
  BeltReadyISM->SetupAttachment(Root);
  BeltReadyISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
  BeltReadyISM->SetCanEverAffectNavigation(false);

  BeltWaitingISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(
      TEXT("BeltWaitingISM"));
  BeltWaitingISM->SetupAttachment(Root);
  BeltWaitingISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
  BeltWaitingISM->SetCanEverAffectNavigation(false);

  BondISM =
      CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("BondISM"));
  BondISM->SetupAttachment(Root);
  BondISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
  BondISM->SetCanEverAffectNavigation(false);

  UnbondISM =
      CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("UnbondISM"));
  UnbondISM->SetupAttachment(Root);
  UnbondISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
  UnbondISM->SetCanEverAffectNavigation(false);

  // Create type-specific output ISM components
  OutputNoneISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(
      TEXT("OutputNoneISM"));
  OutputNoneISM->SetupAttachment(Root);
  OutputNoneISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
  OutputNoneISM->SetCanEverAffectNavigation(false);

  OutputBaseAISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(
      TEXT("OutputBaseAISM"));
  OutputBaseAISM->SetupAttachment(Root);
  OutputBaseAISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
  OutputBaseAISM->SetCanEverAffectNavigation(false);

  OutputBaseBISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(
      TEXT("OutputBaseBISM"));
  OutputBaseBISM->SetupAttachment(Root);
  OutputBaseBISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
  OutputBaseBISM->SetCanEverAffectNavigation(false);

  OutputBaseCISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(
      TEXT("OutputBaseCISM"));
  OutputBaseCISM->SetupAttachment(Root);
  OutputBaseCISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
  OutputBaseCISM->SetCanEverAffectNavigation(false);

  OutputMetalAISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(
      TEXT("OutputMetalAISM"));
  OutputMetalAISM->SetupAttachment(Root);
  OutputMetalAISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
  OutputMetalAISM->SetCanEverAffectNavigation(false);

  OutputCatalystXISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(
      TEXT("OutputCatalystXISM"));
  OutputCatalystXISM->SetupAttachment(Root);
  OutputCatalystXISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
  OutputCatalystXISM->SetCanEverAffectNavigation(false);

  OutputVoidISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(
      TEXT("OutputVoidISM"));
  OutputVoidISM->SetupAttachment(Root);
  OutputVoidISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
  OutputVoidISM->SetCanEverAffectNavigation(false);

  // Type-specific input ISM components
  InputNoneISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(
      TEXT("InputNoneISM"));
  InputNoneISM->SetupAttachment(Root);
  InputNoneISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
  InputNoneISM->SetCanEverAffectNavigation(false);

  InputBaseAISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(
      TEXT("InputBaseAISM"));
  InputBaseAISM->SetupAttachment(Root);
  InputBaseAISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
  InputBaseAISM->SetCanEverAffectNavigation(false);

  InputBaseBISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(
      TEXT("InputBaseBISM"));
  InputBaseBISM->SetupAttachment(Root);
  InputBaseBISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
  InputBaseBISM->SetCanEverAffectNavigation(false);

  InputBaseCISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(
      TEXT("InputBaseCISM"));
  InputBaseCISM->SetupAttachment(Root);
  InputBaseCISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
  InputBaseCISM->SetCanEverAffectNavigation(false);

  InputMetalAISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(
      TEXT("InputMetalAISM"));
  InputMetalAISM->SetupAttachment(Root);
  InputMetalAISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
  InputMetalAISM->SetCanEverAffectNavigation(false);

  InputCatalystXISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(
      TEXT("InputCatalystXISM"));
  InputCatalystXISM->SetupAttachment(Root);
  InputCatalystXISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
  InputCatalystXISM->SetCanEverAffectNavigation(false);

  InputVoidISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(
      TEXT("InputVoidISM"));
  InputVoidISM->SetupAttachment(Root);
  InputVoidISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
  InputVoidISM->SetCanEverAffectNavigation(false);

  // Create collision plane for efficient line traces
  GridCollisionPlane =
      CreateDefaultSubobject<UBoxComponent>(TEXT("GridCollisionPlane"));
  GridCollisionPlane->SetupAttachment(Root);
  GridCollisionPlane->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
  GridCollisionPlane->SetCollisionResponseToAllChannels(
      ECollisionResponse::ECR_Block);
  GridCollisionPlane->SetBoxExtent(
      FVector(50000.f, 50000.f, 1.f)); // Large plane, 2 units thick
  GridCollisionPlane->SetRelativeLocation(
      FVector(0.f, 0.f, 0.f)); // Will be positioned at grid Z level
  GridCollisionPlane->SetVisibility(false); // Invisible, collision only
  GridCollisionPlane->SetHiddenInGame(true);

  // Helix output ISM components
  HelixTileISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(
      TEXT("HelixTileISM"));
  HelixTileISM->SetupAttachment(Root);
  HelixTileISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
  HelixTileISM->SetCanEverAffectNavigation(false);

  HelixOrbISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(
      TEXT("HelixOrbISM"));
  HelixOrbISM->SetupAttachment(Root);
  HelixOrbISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
  HelixOrbISM->SetCanEverAffectNavigation(false);

  HelixBondISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(
      TEXT("HelixBondISM"));
  HelixBondISM->SetupAttachment(Root);
  HelixBondISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
  HelixBondISM->SetCanEverAffectNavigation(false);

  // Per-type helix orb ISM components
  HelixOrbNoneISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(
      TEXT("HelixOrbNoneISM"));
  HelixOrbNoneISM->SetupAttachment(Root);
  HelixOrbNoneISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
  HelixOrbNoneISM->SetCanEverAffectNavigation(false);

  HelixOrbBaseAISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(
      TEXT("HelixOrbBaseAISM"));
  HelixOrbBaseAISM->SetupAttachment(Root);
  HelixOrbBaseAISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
  HelixOrbBaseAISM->SetCanEverAffectNavigation(false);

  HelixOrbBaseBISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(
      TEXT("HelixOrbBaseBISM"));
  HelixOrbBaseBISM->SetupAttachment(Root);
  HelixOrbBaseBISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
  HelixOrbBaseBISM->SetCanEverAffectNavigation(false);

  HelixOrbBaseCISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(
      TEXT("HelixOrbBaseCISM"));
  HelixOrbBaseCISM->SetupAttachment(Root);
  HelixOrbBaseCISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
  HelixOrbBaseCISM->SetCanEverAffectNavigation(false);

  HelixOrbMetalAISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(
      TEXT("HelixOrbMetalAISM"));
  HelixOrbMetalAISM->SetupAttachment(Root);
  HelixOrbMetalAISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
  HelixOrbMetalAISM->SetCanEverAffectNavigation(false);

  HelixOrbCatalystXISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(
      TEXT("HelixOrbCatalystXISM"));
  HelixOrbCatalystXISM->SetupAttachment(Root);
  HelixOrbCatalystXISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
  HelixOrbCatalystXISM->SetCanEverAffectNavigation(false);

  HelixOrbVoidISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(
      TEXT("HelixOrbVoidISM"));
  HelixOrbVoidISM->SetupAttachment(Root);
  HelixOrbVoidISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
  HelixOrbVoidISM->SetCanEverAffectNavigation(false);
}

void AHexFactoryActor::EnsureComponents() {
  if (!GetRootComponent()) {
    USceneComponent *Root =
        NewObject<USceneComponent>(this, TEXT("Root_Runtime"));
    Root->RegisterComponent();
    SetRootComponent(Root);
  }
  auto EnsureISM = [&](UInstancedStaticMeshComponent *&Comp,
                       const TCHAR *Name) {
    if (!Comp) {
      Comp = NewObject<UInstancedStaticMeshComponent>(this, Name);
      Comp->SetupAttachment(GetRootComponent());
      Comp->RegisterComponent();
      // Disable navigation to prevent ensure failures when clearing instances
      Comp->SetCanEverAffectNavigation(false);
      // Enable collision for tile ISMs to support line traces
      if (Comp == TileISM || Comp == OutputTileISM || Comp == BeltTileISM ||
          Comp == BondTileISM || Comp == UnbondTileISM) {
        Comp->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
        Comp->SetCollisionResponseToAllChannels(ECollisionResponse::ECR_Block);
      } else {
        Comp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
      }
    }
    Comp->SetVisibility(true, true);
  };
  EnsureISM(TileISM, TEXT("TileISM_Runtime"));
  EnsureISM(InputTileISM, TEXT("InputTileISM_Runtime"));
  EnsureISM(OutputTileISM, TEXT("OutputTileISM_Runtime"));
  EnsureISM(BeltTileISM, TEXT("BeltTileISM_Runtime"));
  EnsureISM(BondTileISM, TEXT("BondTileISM_Runtime"));
  EnsureISM(UnbondTileISM, TEXT("UnbondTileISM_Runtime"));
  EnsureISM(BeltISM, TEXT("BeltISM_Runtime"));
  EnsureISM(BeltReadyISM, TEXT("BeltReadyISM_Runtime"));
  EnsureISM(BeltWaitingISM, TEXT("BeltWaitingISM_Runtime"));
  EnsureISM(BondISM, TEXT("BondISM_Runtime"));
  EnsureISM(UnbondISM, TEXT("UnbondISM_Runtime"));

  EnsureISM(HighlightTileISM, TEXT("HighlightTileISM_Runtime"));
  EnsureISM(HighlightBeltISM, TEXT("HighlightBeltISM_Runtime"));
  EnsureISM(HighlightBondISM, TEXT("HighlightBondISM_Runtime"));
  EnsureISM(HighlightUnbondISM, TEXT("HighlightUnbondISM_Runtime"));

  // Ensure type-specific output ISM components
  EnsureISM(OutputNoneISM, TEXT("OutputNoneISM_Runtime"));
  EnsureISM(OutputBaseAISM, TEXT("OutputBaseAISM_Runtime"));
  EnsureISM(OutputBaseBISM, TEXT("OutputBaseBISM_Runtime"));
  EnsureISM(OutputBaseCISM, TEXT("OutputBaseCISM_Runtime"));
  EnsureISM(OutputMetalAISM, TEXT("OutputMetalAISM_Runtime"));
  EnsureISM(OutputCatalystXISM, TEXT("OutputCatalystXISM_Runtime"));
  EnsureISM(OutputVoidISM, TEXT("OutputVoidISM_Runtime"));

  // Ensure type-specific input ISM components
  EnsureISM(InputNoneISM, TEXT("InputNoneISM_Runtime"));
  EnsureISM(InputBaseAISM, TEXT("InputBaseAISM_Runtime"));
  EnsureISM(InputBaseBISM, TEXT("InputBaseBISM_Runtime"));
  EnsureISM(InputBaseCISM, TEXT("InputBaseCISM_Runtime"));
  EnsureISM(InputMetalAISM, TEXT("InputMetalAISM_Runtime"));
  EnsureISM(InputCatalystXISM, TEXT("InputCatalystXISM_Runtime"));
  EnsureISM(InputVoidISM, TEXT("InputVoidISM_Runtime"));

  // Ensure helix output ISM components
  EnsureISM(HelixTileISM, TEXT("HelixTileISM_Runtime"));
  EnsureISM(HelixOrbISM, TEXT("HelixOrbISM_Runtime"));
  EnsureISM(HelixBondISM, TEXT("HelixBondISM_Runtime"));

  // Ensure type-specific helix orb ISMs
  EnsureISM(HelixOrbNoneISM, TEXT("HelixOrbNoneISM_Runtime"));
  EnsureISM(HelixOrbBaseAISM, TEXT("HelixOrbBaseAISM_Runtime"));
  EnsureISM(HelixOrbBaseBISM, TEXT("HelixOrbBaseBISM_Runtime"));
  EnsureISM(HelixOrbBaseCISM, TEXT("HelixOrbBaseCISM_Runtime"));
  EnsureISM(HelixOrbMetalAISM, TEXT("HelixOrbMetalAISM_Runtime"));
  EnsureISM(HelixOrbCatalystXISM, TEXT("HelixOrbCatalystXISM_Runtime"));
  EnsureISM(HelixOrbVoidISM, TEXT("HelixOrbVoidISM_Runtime"));

  InputTileISM->SetVisibility(true, true);

  // Ensure grid collision plane exists
  if (!GridCollisionPlane) {
    GridCollisionPlane =
        NewObject<UBoxComponent>(this, TEXT("GridCollisionPlane_Runtime"));
    GridCollisionPlane->SetupAttachment(GetRootComponent());
    GridCollisionPlane->RegisterComponent();
    GridCollisionPlane->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
    GridCollisionPlane->SetCollisionResponseToAllChannels(
        ECollisionResponse::ECR_Block);
    GridCollisionPlane->SetBoxExtent(
        FVector(50000.f, 50000.f, 1.f)); // Large plane, 2 units thick
    GridCollisionPlane->SetRelativeLocation(FVector(0.f, 0.f, 0.f));
    GridCollisionPlane->SetVisibility(false);
    GridCollisionPlane->SetHiddenInGame(true);
  }

  // Update collision plane position to match grid Z level
  UpdateGridCollisionPlane();
}

void AHexFactoryActor::UpdateGridCollisionPlane() {
  if (!GridCollisionPlane) {
    return;
  }

  const AActor *Anchor = GetAnchor();
  if (!Anchor) {
    return;
  }

  // Position the collision plane at the grid's Z level (anchor's Z)
  // Use world space positioning - detach temporarily to set world location,
  // then reattach This ensures the plane is at the exact Z level of the grid
  const FVector AnchorLocation = Anchor->GetActorLocation();

  // Detach to set world location directly
  GridCollisionPlane->DetachFromComponent(
      FDetachmentTransformRules::KeepWorldTransform);
  GridCollisionPlane->SetWorldLocation(
      FVector(AnchorLocation.X, AnchorLocation.Y, AnchorLocation.Z));

  // Reattach to root (this will convert world location to relative)
  GridCollisionPlane->AttachToComponent(
      GetRootComponent(), FAttachmentTransformRules::KeepWorldTransform);
}

AActor *AHexFactoryActor::GetAnchor() const {
  return Driver ? Driver->GetGridAnchor() : nullptr;
}

void AHexFactoryActor::BeginPlay() {
  Super::BeginPlay();
  EnsureComponents();

  if (UWorld *W = GetWorld()) {
    Driver = AOrbSimDriver::Find(W);
    if (Driver) {
      Sim = Driver->GetSim();
      UE_LOG(LogTemp, Log, TEXT("HexFactoryActor::BeginPlay: Driver=%s Sim=%s"),
             Driver ? TEXT("found") : TEXT("null"),
             Sim ? TEXT("found") : TEXT("null"));

      // Subscribe to simulation step completion for belt material updates
      Driver->OnSimStepComplete.AddDynamic(
          this, &AHexFactoryActor::HandleSimStepComplete);
      UE_LOG(LogTemp, Log,
             TEXT("HexFactoryActor::BeginPlay: Bound to "
                  "OrbSimDriver::OnSimStepComplete"));
    } else {
      UE_LOG(LogTemp, Warning,
             TEXT("HexFactoryActor::BeginPlay: No OrbSimDriver found"));
    }

    for (TActorIterator<AHexBoardActor> It(W); It; ++It) {
      Board = *It;
      UE_LOG(LogTemp, Log,
             TEXT("HexFactoryActor::BeginPlay: Found HexBoardActor"));
      break;
    }
  }

  RebuildInternal();
}

void AHexFactoryActor::EndPlay(const EEndPlayReason::Type EndPlayReason) {
  if (Driver) {
    Driver->OnSimStepComplete.RemoveDynamic(
        this, &AHexFactoryActor::HandleSimStepComplete);
  }
  Super::EndPlay(EndPlayReason);
}

void AHexFactoryActor::HandleSimStepComplete(float StepSeconds) {
  // Update belt materials immediately after simulation has stepped
  UpdateBeltMaterials();
}

void AHexFactoryActor::Tick(float DeltaSeconds) {
  Super::Tick(DeltaSeconds);
  UpdateHelixAnimation(DeltaSeconds);

  // Update collision plane position if anchor has moved
  static FVector LastAnchorLocation = FVector::ZeroVector;
  const AActor *Anchor = GetAnchor();
  if (Anchor) {
    const FVector CurrentAnchorLocation = Anchor->GetActorLocation();
    if (!CurrentAnchorLocation.Equals(LastAnchorLocation,
                                      1.0f)) // Update if moved more than 1 unit
    {
      UpdateGridCollisionPlane();
      LastAnchorLocation = CurrentAnchorLocation;
    }
  }

  if (!Sim)
    return;

  const bool bBoardChanged =
      (Board && Board->BoardRevision != LastSeenBoardRevision);
  const bool bTilesChanged = (Sim->TileRevision != LastSeenRevision);

  // Handle full rebuilds when tiles or board change
  if (bTilesChanged || bBoardChanged) {
    UE_LOG(LogTemp, Log,
           TEXT("HexFactoryActor::Tick: Triggering rebuild - TileRevision=%d "
                "LastSeen=%d BoardChanged=%d"),
           Sim->TileRevision, LastSeenRevision, bBoardChanged ? 1 : 0);
    RebuildInternal();
  }

  // Update helix animation continuously
  UpdateHelixAnimation(DeltaSeconds);
}

void AHexFactoryActor::Rebuild() {
  // Reacquire Sim reference in case it was created after our BeginPlay
  if (!Sim && Driver) {
    Sim = Driver->GetSim();
    UE_LOG(LogTemp, Log, TEXT("HexFactoryActor::Rebuild: Reacquired Sim=%s"),
           Sim ? TEXT("found") : TEXT("null"));
  }
  RebuildInternal();
}

void AHexFactoryActor::SetInputHighlight(bool bEnable) {
  uint32 Stencil = bEnable ? 1 : 0;
  if (InputTileISM) {
    InputTileISM->SetRenderCustomDepth(bEnable);
    InputTileISM->SetCustomDepthStencilValue(Stencil);
  }
  if (InputNoneISM) {
    InputNoneISM->SetRenderCustomDepth(bEnable);
    InputNoneISM->SetCustomDepthStencilValue(Stencil);
  }
  if (InputBaseAISM) {
    InputBaseAISM->SetRenderCustomDepth(bEnable);
    InputBaseAISM->SetCustomDepthStencilValue(Stencil);
  }
  if (InputBaseBISM) {
    InputBaseBISM->SetRenderCustomDepth(bEnable);
    InputBaseBISM->SetCustomDepthStencilValue(Stencil);
  }
  if (InputBaseCISM) {
    InputBaseCISM->SetRenderCustomDepth(bEnable);
    InputBaseCISM->SetCustomDepthStencilValue(Stencil);
  }
  if (InputMetalAISM) {
    InputMetalAISM->SetRenderCustomDepth(bEnable);
    InputMetalAISM->SetCustomDepthStencilValue(Stencil);
  }
  if (InputCatalystXISM) {
    InputCatalystXISM->SetRenderCustomDepth(bEnable);
    InputCatalystXISM->SetCustomDepthStencilValue(Stencil);
  }
  if (InputVoidISM) {
    InputVoidISM->SetRenderCustomDepth(bEnable);
    InputVoidISM->SetCustomDepthStencilValue(Stencil);
  }
}

void AHexFactoryActor::SetOutputHighlight(bool bEnable) {
  uint32 Stencil = bEnable ? 1 : 0;
  if (OutputTileISM) {
    OutputTileISM->SetRenderCustomDepth(bEnable);
    OutputTileISM->SetCustomDepthStencilValue(Stencil);
  }
  if (OutputNoneISM) {
    OutputNoneISM->SetRenderCustomDepth(bEnable);
    OutputNoneISM->SetCustomDepthStencilValue(Stencil);
  }
  if (OutputBaseAISM) {
    OutputBaseAISM->SetRenderCustomDepth(bEnable);
    OutputBaseAISM->SetCustomDepthStencilValue(Stencil);
  }
  if (OutputBaseBISM) {
    OutputBaseBISM->SetRenderCustomDepth(bEnable);
    OutputBaseBISM->SetCustomDepthStencilValue(Stencil);
  }
  if (OutputBaseCISM) {
    OutputBaseCISM->SetRenderCustomDepth(bEnable);
    OutputBaseCISM->SetCustomDepthStencilValue(Stencil);
  }
  if (OutputMetalAISM) {
    OutputMetalAISM->SetRenderCustomDepth(bEnable);
    OutputMetalAISM->SetCustomDepthStencilValue(Stencil);
  }
  if (OutputCatalystXISM) {
    OutputCatalystXISM->SetRenderCustomDepth(bEnable);
    OutputCatalystXISM->SetCustomDepthStencilValue(Stencil);
  }
  if (OutputVoidISM) {
    OutputVoidISM->SetRenderCustomDepth(bEnable);
    OutputVoidISM->SetCustomDepthStencilValue(Stencil);
  }
}

void AHexFactoryActor::SetBeltHighlight(bool bEnable) {
  uint32 Stencil = bEnable ? 1 : 0;
  if (BeltISM) {
    BeltISM->SetRenderCustomDepth(bEnable);
    BeltISM->SetCustomDepthStencilValue(Stencil);
  }
  if (BeltReadyISM) {
    BeltReadyISM->SetRenderCustomDepth(bEnable);
    BeltReadyISM->SetCustomDepthStencilValue(Stencil);
  }
  if (BeltWaitingISM) {
    BeltWaitingISM->SetRenderCustomDepth(bEnable);
    BeltWaitingISM->SetCustomDepthStencilValue(Stencil);
  }
  if (BeltTileISM) {
    BeltTileISM->SetRenderCustomDepth(bEnable);
    BeltTileISM->SetCustomDepthStencilValue(Stencil);
  }
}

void AHexFactoryActor::SetBondHighlight(bool bEnable) {
  uint32 Stencil = bEnable ? 1 : 0;
  if (BondTileISM) {
    BondTileISM->SetRenderCustomDepth(bEnable);
    BondTileISM->SetCustomDepthStencilValue(Stencil);
  }
  if (BondISM) {
    BondISM->SetRenderCustomDepth(bEnable);
    BondISM->SetCustomDepthStencilValue(Stencil);
  }
}

void AHexFactoryActor::SetUnbondHighlight(bool bEnable) {
  uint32 Stencil = bEnable ? 1 : 0;
  if (UnbondTileISM) {
    UnbondTileISM->SetRenderCustomDepth(bEnable);
    UnbondTileISM->SetCustomDepthStencilValue(Stencil);
  }
  if (UnbondISM) {
    UnbondISM->SetRenderCustomDepth(bEnable);
    UnbondISM->SetCustomDepthStencilValue(Stencil);
  }
}

void AHexFactoryActor::SetViewMode(EViewMode Mode) {
  bool bOrbMode = (Mode == EViewMode::OrbView);

  // Components that should be hidden in Orb View
  TArray<UPrimitiveComponent *> ComponentsToHide;
  ComponentsToHide.Add(TileISM);
  ComponentsToHide.Add(BeltTileISM);
  ComponentsToHide.Add(BondTileISM);
  ComponentsToHide.Add(UnbondTileISM);
  ComponentsToHide.Add(BeltISM);
  ComponentsToHide.Add(BeltReadyISM);
  ComponentsToHide.Add(BeltWaitingISM);
  ComponentsToHide.Add(BondISM);
  ComponentsToHide.Add(UnbondISM);

  for (auto *Comp : ComponentsToHide) {
    if (Comp) {
      Comp->SetVisibility(!bOrbMode, true);
    }
  }

  // Input/Output tiles should STAY visible in Orb Mode
  if (InputTileISM)
    InputTileISM->SetVisibility(true, true);
  if (OutputTileISM)
    OutputTileISM->SetVisibility(true, true);

  // Type-specific ISMs
  TArray<UInstancedStaticMeshComponent *> IOISMs = {
      OutputNoneISM,     OutputBaseAISM,     OutputBaseBISM, OutputBaseCISM,
      OutputMetalAISM,   OutputCatalystXISM, OutputVoidISM,  InputNoneISM,
      InputBaseAISM,     InputBaseBISM,      InputBaseCISM,  InputMetalAISM,
      InputCatalystXISM, InputVoidISM};

  for (auto *ISM : IOISMs) {
    if (ISM) {
      ISM->SetVisibility(true, true);
    }
  }
}

FVector AHexFactoryActor::HexToWorld(const FHex &H) const {
  const AActor *Anchor = GetAnchor();
  const FVector AnchorV =
      Anchor ? Anchor->GetActorLocation() : FVector::ZeroVector;
  const float Tile = Driver ? Driver->GetTileSize() : 100.f;
  const float GridYaw = Driver ? Driver->GetGridYaw() : 0.f;
  const EHexOrientation Ori =
      Driver ? Driver->GetGridOrientation() : EHexOrientation::PointyTop;

  const FVector2D LocalXY = HexCoord::AxialToXY(FHexAxial(H.Q, H.R), Tile, Ori);
  const FVector2D RotXY = HexCoord::Rotate2D(LocalXY, GridYaw);
  const float Z =
      (Sim && H.Level != 0) ? -0.0f : 0.f; // Tiles are currently at 0
  // Wait, if I'm placing an orb, it should have offset. But HexFactoryActor
  // places tiles.
  return AnchorV + FVector(RotXY.X, RotXY.Y, 0.f);
}

void AHexFactoryActor::UpdateGlobalHighlight(EPlaceKind Kind, bool bEnable,
                                             int32 Level, EOrbType OrbType) {
  bHighlightEnabled = bEnable;
  CurrentHighlightKind = Kind;
  HighlightLevel = Level;
  HighlightOrbType = OrbType;

  if (HighlightTileISM)
    HighlightTileISM->ClearInstances();
  if (HighlightBeltISM)
    HighlightBeltISM->ClearInstances();
  if (HighlightBondISM)
    HighlightBondISM->ClearInstances();
  if (HighlightUnbondISM)
    HighlightUnbondISM->ClearInstances();

  if (!bEnable || !Sim)
    return;

  const float Tile = Driver ? Driver->GetTileSize() : 100.f;
  const float GridYaw = Driver ? Driver->GetGridYaw() : 0.f;
  const EHexOrientation Ori =
      Driver ? Driver->GetGridOrientation() : EHexOrientation::PointyTop;
  const AActor *Anchor = GetAnchor();
  const FVector AnchorV =
      Anchor ? Anchor->GetActorLocation() : FVector::ZeroVector;

  auto GetYawToNeighbor = [&](const FHex &Start, uint8 Dir) -> float {
    FHex Neighbor = HexUtil::Neighbor(Start, (EHexDir)(Dir % 6));
    FVector PosA = Driver->MapHexToWorld(Start);
    FVector PosB = Driver->MapHexToWorld(Neighbor);
    FVector DirVec = (PosB - PosA).GetSafeNormal();
    return DirVec.IsNearlyZero()
               ? 0.f
               : FMath::RadiansToDegrees(FMath::Atan2(DirVec.Y, DirVec.X));
  };

  for (const auto &KV : Sim->TileMap) {
    const FHex &H = KV.Key;
    const FTile &T = KV.Value;

    // Filter by level
    if (H.Level != Level)
      continue;

    bool bMatches = false;
    UInstancedStaticMeshComponent *TargetISM = nullptr;
    FTransform Transform = FTransform::Identity;

    const FVector2D LocalXY =
        HexCoord::AxialToXY(FHexAxial(H.Q, H.R), Tile, Ori);
    const FVector2D RotXY = HexCoord::Rotate2D(LocalXY, GridYaw);
    const float Z = 0.f;
    const FVector Loc = AnchorV + FVector(RotXY.X, RotXY.Y, Z);
    const int32 DisplayLevel = GetDisplayLevel(H.Level);
    const float Pitch = (DisplayLevel == 0) ? 0.f : 180.f;

    switch (Kind) {
    case EPlaceKind::Belt:
      if (T.Kind == ETileKind::Belt) {
        bMatches = true;
        TargetISM = HighlightBeltISM;
        float Yaw = GetYawToNeighbor(H, T.Arg);
        Transform = FTransform(FRotator(Pitch, Yaw, 0.f), Loc, BeltScale);
      }
      break;

    case EPlaceKind::Input:
    case EPlaceKind::ClusterInput:
    case EPlaceKind::ClusterSend:
      if (T.Kind == ETileKind::Input || T.Kind == ETileKind::ClusterInput ||
          T.Kind == ETileKind::ClusterSend ||
          T.Kind == ETileKind::DiscoveryClusterInput) {
        // Only match if OrbType matches
        if (T.Arg == (uint8)OrbType) {
          bMatches = true;
          TargetISM = HighlightTileISM;
          Transform = FTransform(FRotator(Pitch, 0.f, 0.f), Loc, FVector(1.f));
        }
      }
      break;

    case EPlaceKind::Output:
    case EPlaceKind::ClusterOutput:
      if (T.Kind == ETileKind::Output || T.Kind == ETileKind::ClusterOutput ||
          T.Kind == ETileKind::DiscoveryClusterOutput) {
        // Only match if OrbType matches
        if (T.Arg == (uint8)OrbType) {
          bMatches = true;
          TargetISM = HighlightTileISM;
          Transform = FTransform(FRotator(Pitch, 0.f, 0.f), Loc, FVector(1.f));
        }
      }
      break;

    case EPlaceKind::Bond:
      if (T.Kind == ETileKind::Bond) {
        bMatches = true;
        TargetISM = HighlightBondISM;
        FVector CenterA = Driver->MapHexToWorld(H);
        FVector CenterB =
            Driver->MapHexToWorld(HexUtil::Neighbor(H, (EHexDir)(T.Arg % 6)));
        FVector Mid = (CenterA + CenterB) * 0.5f;
        FVector DirVec = (CenterB - CenterA).GetSafeNormal();
        float Yaw = FMath::RadiansToDegrees(FMath::Atan2(DirVec.Y, DirVec.X));
        Transform = FTransform(FRotator(Pitch, Yaw, 0.f), Mid, BondScale);
      }
      break;

    case EPlaceKind::Unbond:
      if (T.Kind == ETileKind::Unbond) {
        bMatches = true;
        TargetISM = HighlightUnbondISM;
        FVector CenterA = Driver->MapHexToWorld(H);
        FVector CenterB =
            Driver->MapHexToWorld(HexUtil::Neighbor(H, (EHexDir)(T.Arg % 6)));
        FVector Mid = (CenterA + CenterB) * 0.5f;
        FVector DirVec = (CenterB - CenterA).GetSafeNormal();
        float Yaw = FMath::RadiansToDegrees(FMath::Atan2(DirVec.Y, DirVec.X));
        Transform = FTransform(FRotator(Pitch, Yaw, 0.f), Mid, UnbondScale);
      }
      break;

    case EPlaceKind::HelixOutput:
      if (T.Kind == ETileKind::HelixOutput) {
        bMatches = true;
        TargetISM = HighlightTileISM;
        Transform = FTransform(FRotator(Pitch, 0.f, 0.f), Loc, FVector(1.f));
      }
      break;

    default:
      break;
    }

    if (bMatches && TargetISM) {
      TargetISM->AddInstance(Transform);
    }
  }
}

void AHexFactoryActor::RebuildInternal() {
  // Note: Debug logging removed for performance

  EnsureComponents();

  // Update collision plane position when grid is rebuilt
  UpdateGridCollisionPlane();

  // Reacquire Sim reference in case it was created after our BeginPlay
  if (!Sim && Driver) {
    Sim = Driver->GetSim();
    // Note: Debug logging removed for performance
  }

  if (!Sim) {
    UE_LOG(LogTemp, Warning,
           TEXT("HexFactoryActor::RebuildInternal: Sim is null after "
                "reacquisition attempt"));
    return;
  }

  LastSeenRevision = Sim->TileRevision;
  LastSeenBoardRevision = Board ? Board->BoardRevision : -1;

  // Set up tile ISM components
  // Get the tile mesh from GameManager (same as Output tiles use)
  UStaticMesh *BaseTileMesh = TileMesh;
  if (UWorld *World = GetWorld()) {
    for (TActorIterator<AGameManager> It(World); It; ++It) {
      AGameManager *GameManager = *It;
      if (GameManager && GameManager->TileMesh) {
        BaseTileMesh = GameManager->TileMesh;
        break;
      }
    }
  }

  if (BaseTileMesh) {
    TileISM->SetStaticMesh(BaseTileMesh);
    // Input and ClusterInput tiles are rendered via procedural mesh (no ISM
    // needed)
    OutputTileISM->SetStaticMesh(BaseTileMesh);
    BeltTileISM->SetStaticMesh(BaseTileMesh);
    BondTileISM->SetStaticMesh(BaseTileMesh);
  }

  if (TileMaterial)
    TileISM->SetMaterial(0, TileMaterial);

  // Input and ClusterInput tiles use procedural meshes with cutouts (handled in
  // RebuildInputTiles) No ISM material setup needed for Input tiles

  if (OutputTileMaterial)
    OutputTileISM->SetMaterial(0, OutputTileMaterial);
  if (BeltTileMaterial)
    BeltTileISM->SetMaterial(0, BeltTileMaterial);
  if (BondTileMaterial)
    BondTileISM->SetMaterial(0, BondTileMaterial);

  if (BeltMesh) {
    BeltReadyISM->SetStaticMesh(BeltMesh);
    BeltWaitingISM->SetStaticMesh(BeltMesh);
    // Legacy BeltISM only used if new materials aren't set
    BeltISM->SetStaticMesh(BeltMesh);
  }
  // Only set up the new material ISMs if materials are provided
  if (BeltReadyMaterial)
    BeltReadyISM->SetMaterial(0, BeltReadyMaterial);
  if (BeltWaitingMaterial)
    BeltWaitingISM->SetMaterial(0, BeltWaitingMaterial);
  // Legacy fallback
  if (BeltMaterial)
    BeltISM->SetMaterial(0, BeltMaterial);
  if (BondMesh)
    BondISM->SetStaticMesh(BondMesh);
  if (BondMaterial)
    BondISM->SetMaterial(0, BondMaterial);

  // Set up unbond mesh - use BondMesh as fallback if UnbondMesh not assigned
  if (UnbondMesh) {
    UnbondISM->SetStaticMesh(UnbondMesh);
  } else if (BondMesh) {
    // Fallback to bond mesh if no unbond mesh is set
    UnbondISM->SetStaticMesh(BondMesh);
  }

  // Set up unbond material - use BondMaterial as fallback if UnbondMaterial not
  // assigned
  if (UnbondMaterial) {
    UnbondISM->SetMaterial(0, UnbondMaterial);
  } else if (BondMaterial) {
    UnbondISM->SetMaterial(0, BondMaterial);
  }

  // Set up meshes for highlight ISMs
  if (BaseTileMesh) {
    HighlightTileISM->SetStaticMesh(BaseTileMesh);
    HighlightBeltISM->SetStaticMesh(BaseTileMesh);
  }
  if (BeltMesh) {
    HighlightBeltISM->SetStaticMesh(BeltMesh);
  }
  if (BondMesh) {
    HighlightBondISM->SetStaticMesh(BondMesh);
  }
  if (UnbondMesh) {
    HighlightUnbondISM->SetStaticMesh(UnbondMesh);
  } else if (BondMesh) {
    HighlightUnbondISM->SetStaticMesh(BondMesh);
  }

  // Setup highlight ISMs for outlining
  auto SetupHighlightISM = [](UInstancedStaticMeshComponent *Comp) {
    if (Comp) {
      Comp->SetRenderCustomDepth(true);
      Comp->SetCustomDepthStencilValue(1); // Outline color
      Comp->SetRenderInMainPass(false);    // Only see outline
      Comp->SetVisibility(true);
    }
  };
  SetupHighlightISM(HighlightTileISM);
  SetupHighlightISM(HighlightBeltISM);
  SetupHighlightISM(HighlightBondISM);
  SetupHighlightISM(HighlightUnbondISM);

  // Clear all instances
  TileISM->ClearInstances();
  InputTileISM->ClearInstances();
  OutputTileISM->ClearInstances();
  BeltTileISM->ClearInstances();
  BondTileISM->ClearInstances();
  UnbondTileISM->ClearInstances();
  BeltISM->ClearInstances();
  BeltReadyISM->ClearInstances();
  BeltWaitingISM->ClearInstances();
  BondISM->ClearInstances();
  UnbondISM->ClearInstances();

  HighlightTileISM->ClearInstances();
  HighlightBeltISM->ClearInstances();
  HighlightBondISM->ClearInstances();
  HighlightUnbondISM->ClearInstances();

  RebuildTiles();
  RebuildBelts();
  RebuildBonds();       // NEW
  RebuildOutputs();     // NEW: Type-specific output rendering
  RebuildInputTiles();  // Render Input/ClusterInput tiles using ISM with
                        // orb-type materials
  RebuildHelixOutput(); // NEW: Helix output tiles and rungs

  // Update global highlight if enabled
  if (bHighlightEnabled) {
    UpdateGlobalHighlight(CurrentHighlightKind, bHighlightEnabled,
                          HighlightLevel, HighlightOrbType);
  }

  // Note: Debug logging removed for performance
}

void AHexFactoryActor::RebuildTiles() {
  if (!Sim)
    return;

  const float Tile = Driver ? Driver->GetTileSize() : 100.f;
  const float GridYaw = Driver ? Driver->GetGridYaw() : 0.f;
  const EHexOrientation Ori =
      Driver ? Driver->GetGridOrientation() : EHexOrientation::PointyTop;
  const AActor *Anchor = GetAnchor();
  const FVector AnchorV =
      Anchor ? Anchor->GetActorLocation() : FVector::ZeroVector;

  // Helper function to add tile instance to appropriate ISM
  auto AddTileInstance = [&](const FHex &H, ETileKind TileKind) {
    const FVector2D LocalXY =
        HexCoord::AxialToXY(FHexAxial(H.Q, H.R), Tile, Ori);
    const FVector2D RotXY = HexCoord::Rotate2D(LocalXY, GridYaw);
    const float Z = 0.f; // Tiles stay at Z=0 but flip
    const FVector Loc = AnchorV + FVector(RotXY.X, RotXY.Y, Z);
    const int32 DisplayLevel = GetDisplayLevel(H.Level);
    const float Pitch = (DisplayLevel == 0) ? 0.f : 180.f;
    const FTransform Transform(FRotator(Pitch, 0.f, 0.f), Loc, FVector(1.f));

    switch (TileKind) {
    case ETileKind::Input:
    case ETileKind::ClusterInput:
    case ETileKind::ClusterSend:
    case ETileKind::DiscoveryClusterInput:
      // Input/ClusterInput/ClusterSend tiles are rendered via procedural mesh
      // or specific ISM Skip ISM rendering for these - they're handled in
      // RebuildInputTiles()
      break;
    case ETileKind::Output:
    case ETileKind::ClusterOutput:
    case ETileKind::DiscoveryClusterOutput:
    case ETileKind::HelixOutput:
    case ETileKind::HelixInput:
      // Output tiles are handled by RebuildOutputs() or RebuildHelixOutput()
      // with type-specific materials
      break;
    case ETileKind::Belt:
      if (BeltTileISM)
        BeltTileISM->AddInstance(Transform);
      break;
    case ETileKind::Bond:
    case ETileKind::Unbond:
      // Bonds don't use base tile meshes - only their glyph meshes
      // Skip tile mesh rendering for these
      break;
    case ETileKind::Source:
      // Source tiles are rendered via RebuildSourceAnimation() to handle the
      // "hole" and "printing" effect
      break;
    default:
      // Empty tiles or unknown types go to the default TileISM
      if (TileISM)
        TileISM->AddInstance(Transform);
      break;
    }
  };

  // Render tiles from the simulation
  for (const auto &KV : Sim->TileMap) {
    AddTileInstance(KV.Key, KV.Value.Kind);
  }

  // Also render empty tiles from the board if it exists
  if (Board) {
    for (const FHex &H : Board->GetAllHexes()) {
      // Only add if not already in simulation
      if (!Sim->TileMap.Contains(H)) {
        AddTileInstance(H, ETileKind::Empty);
      }
    }
  }
}

void AHexFactoryActor::DrawBeltArrow(const FVector &Center, float TileRadiusUU,
                                     float YawDeg) {
  const float Len = TileRadiusUU * DebugArrowLenScale;
  const FVector Dir = FRotator(0.f, YawDeg, 0.f).RotateVector(FVector(1, 0, 0));
  const FVector A = Center + FVector(0, 0, BeltZOffset + 2.f);
  const FVector B = A + Dir * Len;
  DrawDebugDirectionalArrow(GetWorld(), A, B, 16.f, FColor::Yellow, false, 1.0f,
                            0, DebugArrowThickness);
}

void AHexFactoryActor::DrawBondArrow(const FVector &A, const FVector &B) {
  DrawDebugLine(GetWorld(), A + FVector(0, 0, BondZOffset + 2.f),
                B + FVector(0, 0, BondZOffset + 2.f), FColor::Cyan, false, 1.0f,
                0, 2.f);
}

void AHexFactoryActor::RebuildBelts() {
  if (!Sim) {
    UE_LOG(LogTemp, Warning,
           TEXT("HexFactoryActor::RebuildBelts: Sim is null"));
    return;
  }

  const float Tile = Driver ? Driver->GetTileSize() : 100.f;
  const float GridYaw = Driver ? Driver->GetGridYaw() : 0.f;
  const EHexOrientation Ori =
      Driver ? Driver->GetGridOrientation() : EHexOrientation::PointyTop;
  const AActor *Anchor = GetAnchor();
  const FVector AnchorV =
      Anchor ? Anchor->GetActorLocation() : FVector::ZeroVector;

  int32 BeltCount = 0;
  int32 ReadyCount = 0;
  int32 WaitingCount = 0;
  int32 EmptyCount = 0;

  auto ForwardYaw = [&](const FHex &Base, uint8 Dir) -> float {
    const FHex Fwd = HexUtil::Neighbor(Base, (EHexDir)(Dir % 6));
    const FVector2D Axy = HexCoord::Rotate2D(
        HexCoord::AxialToXY(FHexAxial(Base.Q, Base.R), Tile, Ori), GridYaw);
    const FVector2D Bxy = HexCoord::Rotate2D(
        HexCoord::AxialToXY(FHexAxial(Fwd.Q, Fwd.R), Tile, Ori), GridYaw);
    const FVector2D Delta = Bxy - Axy;
    return Delta.IsNearlyZero()
               ? 0.f
               : FMath::RadiansToDegrees(FMath::Atan2(Delta.Y, Delta.X));
  };

  enum class EBeltState : uint8 { Empty, Ready, Waiting };

  // Helper function to check belt state
  auto GetBeltState = [&](const FHex &BeltHex, uint8 BeltDir) -> EBeltState {
    // Check if there's an item on this belt
    const FItem *BeltItem = Sim->ItemMap.Find(BeltHex);
    if (!BeltItem || BeltItem->Id == 0) {
      // No item on belt - belt is empty
      return EBeltState::Empty;
    }

    // There's an item on the belt - check if it can move forward
    const FHex DestHex = HexUtil::Neighbor(BeltHex, (EHexDir)(BeltDir % 6));

    // Check if destination is occupied
    if (Sim->ItemMap.Contains(DestHex)) {
      // Destination is occupied - belt is waiting
      return EBeltState::Waiting;
    }

    // Check if destination is blocked by held items
    for (const FArmState &Arm : Sim->Arms) {
      if (!Arm.bHolding)
        continue;

      // Calculate arm grip position
      FHex GripHex = Arm.BaseHex;
      for (int i = 0; i < Arm.ArmLen; ++i)
        GripHex = HexUtil::Neighbor(GripHex, Arm.Facing);

      if (GripHex == DestHex) {
        // Arm is holding an item at the destination - belt is waiting
        return EBeltState::Waiting;
      }

      // Check for cluster items
      if (Arm.Held.bIsCluster) {
        for (const FOrbData &Sub : Arm.Held.Orbs) {
          FHex Local(Sub.dQ, Sub.dR);
          // Apply rotation based on arm facing and grab facing
          const uint8 cur = (uint8)Arm.Facing % 6;
          const int8 grab = (int8)Arm.Held.GrabFacing;
          const uint8 facingDelta =
              (grab >= 0) ? (uint8)(((int32)cur - (int32)(grab % 6) + 6) % 6)
                          : cur;
          const uint8 ccw = (uint8)((6 - facingDelta) % 6);

          // Rotate the local offset
          for (uint8 i = 0; i < (ccw % 6); ++i) {
            int32 q = Local.Q;
            int32 r = Local.R;
            Local.Q = -r;
            Local.R = q + r;
          }

          FHex WorldPos(GripHex.Q + Local.Q, GripHex.R + Local.R);
          if (WorldPos == DestHex) {
            // Part of held cluster is at destination - belt is waiting
            return EBeltState::Waiting;
          }
        }
      }
    }

    // Destination is clear - belt is ready to move
    return EBeltState::Ready;
  };

  for (const auto &KV : Sim->TileMap) {
    const FHex &H = KV.Key;
    const FTile &T = KV.Value;
    if (T.Kind != ETileKind::Belt)
      continue;

    BeltCount++;
    const FVector2D LocalXY =
        HexCoord::AxialToXY(FHexAxial(H.Q, H.R), Tile, Ori);
    const FVector2D RotXY = HexCoord::Rotate2D(LocalXY, GridYaw);
    const int32 DisplayLevel = GetDisplayLevel(H.Level);
    const float ZOffset = (DisplayLevel == 0) ? BeltZOffset : -BeltZOffset;
    const FVector Loc = AnchorV + FVector(RotXY.X, RotXY.Y, ZOffset);
    // Yaw needs 180° offset on upside-down grid to compensate for pitch flip
    const float Yaw = ForwardYaw(H, (uint8)(T.Arg & 0xFF)) +
                      (DisplayLevel == 1 ? 180.f : 0.f);
    const float Pitch = (DisplayLevel == 0) ? 0.f : 180.f;

    if (BeltMesh) {
      const FTransform Transform(FRotator(Pitch, Yaw, 0.f), Loc, BeltScale);

      // Use new material system if materials are set, otherwise fall back to
      // legacy
      if (BeltReadyMaterial && BeltWaitingMaterial) {
        const uint8 BeltDir = (uint8)(T.Arg & 0xFF);
        EBeltState State = GetBeltState(H, BeltDir);

        switch (State) {
        case EBeltState::Empty:
          // Belt has no orbs - use base material
          BeltISM->AddInstance(Transform);
          EmptyCount++;
          break;
        case EBeltState::Ready:
          // Belt has orbs that can move - use ready material
          BeltReadyISM->AddInstance(Transform);
          ReadyCount++;
          break;
        case EBeltState::Waiting:
          // Belt has orbs that are blocked - use waiting material
          BeltWaitingISM->AddInstance(Transform);
          WaitingCount++;
          break;
        }
      } else {
        // Fallback to legacy single-material rendering
        BeltISM->AddInstance(Transform);
      }

      if (bAlwaysDrawBeltArrows)
        DrawBeltArrow(Loc, Tile, Yaw);
    } else {
      UE_LOG(LogTemp, Warning,
             TEXT("HexFactoryActor::RebuildBelts: BeltMesh is null"));
    }
  }

  UE_LOG(LogTemp, Log,
         TEXT("HexFactoryActor::RebuildBelts: Found %d belts (%d empty, %d "
              "ready, %d waiting), BeltMesh=%s"),
         BeltCount, EmptyCount, ReadyCount, WaitingCount,
         BeltMesh ? TEXT("exists") : TEXT("null"));
}

void AHexFactoryActor::UpdateBeltMaterials() {
  if (!Sim || !BeltMesh)
    return;

  // Only update if new materials are set, otherwise do nothing (legacy system
  // in use)
  if (!BeltReadyMaterial || !BeltWaitingMaterial)
    return;

  // Clear all belt material ISMs
  BeltISM->ClearInstances();
  BeltReadyISM->ClearInstances();
  BeltWaitingISM->ClearInstances();

  const float Tile = Driver ? Driver->GetTileSize() : 100.f;
  const float GridYaw = Driver ? Driver->GetGridYaw() : 0.f;
  const EHexOrientation Ori =
      Driver ? Driver->GetGridOrientation() : EHexOrientation::PointyTop;
  const AActor *Anchor = GetAnchor();
  const FVector AnchorV =
      Anchor ? Anchor->GetActorLocation() : FVector::ZeroVector;

  int32 EmptyCount = 0;
  int32 ReadyCount = 0;
  int32 WaitingCount = 0;

  auto ForwardYaw = [&](const FHex &Base, uint8 Dir) -> float {
    const FHex Fwd = HexUtil::Neighbor(Base, (EHexDir)(Dir % 6));
    const FVector2D Axy = HexCoord::Rotate2D(
        HexCoord::AxialToXY(FHexAxial(Base.Q, Base.R), Tile, Ori), GridYaw);
    const FVector2D Bxy = HexCoord::Rotate2D(
        HexCoord::AxialToXY(FHexAxial(Fwd.Q, Fwd.R), Tile, Ori), GridYaw);
    const FVector2D Delta = Bxy - Axy;
    return Delta.IsNearlyZero()
               ? 0.f
               : FMath::RadiansToDegrees(FMath::Atan2(Delta.Y, Delta.X));
  };

  enum class EBeltState : uint8 { Empty, Ready, Waiting };

  // Helper function to check belt state
  auto GetBeltState = [&](const FHex &BeltHex, uint8 BeltDir) -> EBeltState {
    // Check if there's an item on this belt
    const FItem *BeltItem = Sim->ItemMap.Find(BeltHex);
    if (!BeltItem || BeltItem->Id == 0) {
      // No item on belt - belt is empty
      return EBeltState::Empty;
    }

    // There's an item on the belt - check if it can move forward
    const FHex DestHex = HexUtil::Neighbor(BeltHex, (EHexDir)(BeltDir % 6));

    // Check if destination is occupied
    if (Sim->ItemMap.Contains(DestHex)) {
      // Destination is occupied - belt is waiting
      return EBeltState::Waiting;
    }

    // Check if destination is blocked by held items
    for (const FArmState &Arm : Sim->Arms) {
      if (!Arm.bHolding)
        continue;

      // Calculate arm grip position
      FHex GripHex = Arm.BaseHex;
      for (int i = 0; i < Arm.ArmLen; ++i)
        GripHex = HexUtil::Neighbor(GripHex, Arm.Facing);

      if (GripHex == DestHex) {
        // Arm is holding an item at the destination - belt is waiting
        return EBeltState::Waiting;
      }

      // Check for cluster items
      if (Arm.Held.bIsCluster) {
        for (const FOrbData &Sub : Arm.Held.Orbs) {
          FHex Local(Sub.dQ, Sub.dR);
          // Apply rotation based on arm facing and grab facing
          const uint8 cur = (uint8)Arm.Facing % 6;
          const int8 grab = (int8)Arm.Held.GrabFacing;
          const uint8 facingDelta =
              (grab >= 0) ? (uint8)(((int32)cur - (int32)(grab % 6) + 6) % 6)
                          : cur;
          const uint8 ccw = (uint8)((6 - facingDelta) % 6);

          // Rotate the local offset
          for (uint8 i = 0; i < (ccw % 6); ++i) {
            int32 q = Local.Q;
            int32 r = Local.R;
            Local.Q = -r;
            Local.R = q + r;
          }

          FHex WorldPos(GripHex.Q + Local.Q, GripHex.R + Local.R);
          if (WorldPos == DestHex) {
            // Part of held cluster is at destination - belt is waiting
            return EBeltState::Waiting;
          }
        }
      }
    }

    // Destination is clear - belt is ready to move
    return EBeltState::Ready;
  };

  for (const auto &KV : Sim->TileMap) {
    const FHex &H = KV.Key;
    const FTile &T = KV.Value;
    if (T.Kind != ETileKind::Belt)
      continue;

    const FVector2D LocalXY =
        HexCoord::AxialToXY(FHexAxial(H.Q, H.R), Tile, Ori);
    const FVector2D RotXY = HexCoord::Rotate2D(LocalXY, GridYaw);
    const int32 DisplayLevel = GetDisplayLevel(H.Level);
    const float ZOffset = (DisplayLevel == 0) ? BeltZOffset : -BeltZOffset;
    const FVector Loc = AnchorV + FVector(RotXY.X, RotXY.Y, ZOffset);
    // Yaw needs 180° offset on upside-down grid to compensate for pitch flip
    const float Yaw = ForwardYaw(H, (uint8)(T.Arg & 0xFF)) +
                      (DisplayLevel == 1 ? 180.f : 0.f);
    const float Pitch = (DisplayLevel == 0) ? 0.f : 180.f;

    const uint8 BeltDir = (uint8)(T.Arg & 0xFF);
    EBeltState State = GetBeltState(H, BeltDir);

    const FTransform Transform(FRotator(Pitch, Yaw, 0.f), Loc, BeltScale);

    switch (State) {
    case EBeltState::Empty:
      // Belt has no orbs - use base material
      BeltISM->AddInstance(Transform);
      EmptyCount++;
      break;
    case EBeltState::Ready:
      // Belt has orbs that can move - use ready material
      BeltReadyISM->AddInstance(Transform);
      ReadyCount++;
      break;
    case EBeltState::Waiting:
      // Belt has orbs that are blocked - use waiting material
      BeltWaitingISM->AddInstance(Transform);
      WaitingCount++;
      break;
    }
  }
}

void AHexFactoryActor::RebuildBonds() {
  if (!Sim) {
    UE_LOG(LogTemp, Warning,
           TEXT("HexFactoryActor::RebuildBonds: Sim is null"));
    return;
  }

  const float Tile = Driver ? Driver->GetTileSize() : 100.f;
  const float GridYaw = Driver ? Driver->GetGridYaw() : 0.f;
  const EHexOrientation Ori =
      Driver ? Driver->GetGridOrientation() : EHexOrientation::PointyTop;
  const AActor *Anchor = GetAnchor();
  const FVector AnchorV =
      Anchor ? Anchor->GetActorLocation() : FVector::ZeroVector;

  int32 BondCount = 0;

  for (const auto &KV : Sim->TileMap) {
    const FHex &H = KV.Key;
    const FTile &T = KV.Value;
    if (T.Kind != ETileKind::Bond && T.Kind != ETileKind::Unbond)
      continue;

    BondCount++;
    const uint8 Dir = (uint8)(T.Arg & 0xFF);
    const FHex N = HexUtil::Neighbor(H, (EHexDir)(Dir % 6));

    // World positions of each hex center
    const FVector2D AxyLocal =
        HexCoord::AxialToXY(FHexAxial(H.Q, H.R), Tile, Ori);
    const FVector2D BxyLocal =
        HexCoord::AxialToXY(FHexAxial(N.Q, N.R), Tile, Ori);
    const FVector2D Axy = HexCoord::Rotate2D(AxyLocal, GridYaw);
    const FVector2D Bxy = HexCoord::Rotate2D(BxyLocal, GridYaw);
    const int32 DisplayLevel = GetDisplayLevel(H.Level);
    const float ZOffset =
        (DisplayLevel == 0)
            ? (T.Kind == ETileKind::Bond ? BondZOffset : UnbondZOffset)
            : -(T.Kind == ETileKind::Bond ? BondZOffset : UnbondZOffset);
    const FVector WorldA = AnchorV + FVector(Axy.X, Axy.Y, ZOffset);
    const FVector WorldB = AnchorV + FVector(Bxy.X, Bxy.Y, ZOffset);
    const FVector Mid = (WorldA + WorldB) * 0.5f;
    const FVector2D Delta2D(Bxy - Axy);
    const float Yaw =
        Delta2D.IsNearlyZero()
            ? 0.f
            : FMath::RadiansToDegrees(FMath::Atan2(Delta2D.Y, Delta2D.X));

    const float Pitch = (DisplayLevel == 0) ? 0.f : 180.f;
    if (T.Kind == ETileKind::Bond) {
      if (BondMesh) {
        BondISM->AddInstance(
            FTransform(FRotator(Pitch, Yaw, 0.f), Mid, BondScale));
        if (bDrawBondDebugArrows)
          DrawBondArrow(WorldA, WorldB);
      } else {
        UE_LOG(LogTemp, Warning,
               TEXT("HexFactoryActor::RebuildBonds: BondMesh is null"));
      }
    } else if (T.Kind == ETileKind::Unbond) {
      // Use UnbondMesh if available, otherwise fallback to BondMesh
      UStaticMesh *MeshToUse = UnbondMesh ? UnbondMesh : BondMesh;
      if (MeshToUse) {
        UnbondISM->AddInstance(
            FTransform(FRotator(Pitch, Yaw, 0.f), Mid, UnbondScale));
        if (bDrawBondDebugArrows)
          DrawBondArrow(WorldA, WorldB);
      } else {
        UE_LOG(LogTemp, Warning,
               TEXT("HexFactoryActor::RebuildBonds: Neither UnbondMesh nor "
                    "BondMesh is available"));
      }
    }
  }

  UE_LOG(LogTemp, Log,
         TEXT("HexFactoryActor::RebuildBonds: Found %d bonds/unbonds, "
              "BondMesh=%s, BondISM=%s"),
         BondCount, BondMesh ? TEXT("exists") : TEXT("null"),
         BondISM ? TEXT("exists") : TEXT("null"));
}

void AHexFactoryActor::RebuildOutputs() {
  if (!Sim) {
    UE_LOG(LogTemp, Warning,
           TEXT("HexFactoryActor::RebuildOutputs: Sim is null"));
    return;
  }

  // Get material manager
  UOrbMaterialManager *MaterialManager = UOrbMaterialManager::FindInWorld(this);
  if (!MaterialManager) {
    UE_LOG(
        LogTemp, Warning,
        TEXT("HexFactoryActor::RebuildOutputs: No OrbMaterialManager found in "
             "world. Please add an OrbMaterialManagerActor to the level."));
    return;
  }

  const float Tile = Driver ? Driver->GetTileSize() : 100.f;
  const float GridYaw = Driver ? Driver->GetGridYaw() : 0.f;
  const EHexOrientation Ori =
      Driver ? Driver->GetGridOrientation() : EHexOrientation::PointyTop;
  const AActor *Anchor = GetAnchor();
  const FVector AnchorV =
      Anchor ? Anchor->GetActorLocation() : FVector::ZeroVector;

  // Note: Debug logging removed for performance

  // Get the tile mesh from GameManager instead of orb mesh
  UStaticMesh *BaseTileMesh = nullptr;
  if (UWorld *World = GetWorld()) {
    for (TActorIterator<AGameManager> It(World); It; ++It) {
      AGameManager *GameManager = *It;
      if (GameManager && GameManager->TileMesh) {
        BaseTileMesh = GameManager->TileMesh;
        // Note: Debug logging removed for performance
        break;
      }
    }
  }

  if (!BaseTileMesh) {
    UE_LOG(LogTemp, Warning,
           TEXT("HexFactoryActor::RebuildOutputs: No tile mesh found in "
                "GameManager. Please assign TileMesh in the GameManager."));
    // Fallback to local tile mesh
    if (TileMesh) {
      BaseTileMesh = TileMesh;
      // Note: Debug logging removed for performance
    } else {
      UE_LOG(LogTemp, Error,
             TEXT("HexFactoryActor::RebuildOutputs: No tile mesh available"));
      return;
    }
  }

  // UE_LOG(LogTemp, Log, TEXT("HexFactoryActor::RebuildOutputs: Using tile mesh
  // for outputs"));

  // Set up all output ISM components with the tile mesh and type-specific
  // materials
  TArray<EOrbType> AllOrbTypes = {
      EOrbType::None,   EOrbType::BaseA,     EOrbType::BaseB, EOrbType::BaseC,
      EOrbType::MetalA, EOrbType::CatalystX, EOrbType::Void};

  for (EOrbType OrbType : AllOrbTypes) {
    UInstancedStaticMeshComponent *OutputISM = GetOutputISMForType(OrbType);
    if (OutputISM) {
      // Set the tile mesh
      OutputISM->SetStaticMesh(BaseTileMesh);

      // Get the material for this orb type
      UMaterialInterface *TypeMaterial =
          MaterialManager->GetMaterialForOrbType(OrbType);
      if (TypeMaterial) {
        // Create a dynamic material instance for half opacity
        UMaterialInstanceDynamic *OutputMID =
            UMaterialInstanceDynamic::Create(TypeMaterial, this);
        if (OutputMID) {
          // Set half opacity (0.5 alpha)
          OutputMID->SetScalarParameterValue(FName("Opacity"), 0.5f);
          OutputISM->SetMaterial(0, OutputMID);
        } else {
          OutputISM->SetMaterial(0, TypeMaterial);
        }
      }

      // Clear instances for this type
      OutputISM->ClearInstances();

      // UE_LOG(LogTemp, Log, TEXT("HexFactoryActor::RebuildOutputs: Set up ISM
      // for type %d with mesh %s and material %s"), 	(int32)OrbType,
      // 	BaseTileMesh ? TEXT("valid") : TEXT("null"),
      // 	TypeMaterial ? TEXT("valid") : TEXT("null"));
    } else {
      UE_LOG(LogTemp, Warning,
             TEXT("HexFactoryActor::RebuildOutputs: No ISM found for type %d"),
             (int32)OrbType);
    }
  }

  // Get ClusterOutput scale from GameManager
  float ClusterScale = 1.3f; // Default
  if (UWorld *World = GetWorld()) {
    for (TActorIterator<AGameManager> It(World); It; ++It) {
      ClusterScale = (*It)->ClusterOutputScale;
      break;
    }
  }

  int32 OutputCount = 0;

  // Render all output tiles (both regular Output and ClusterOutput)
  for (const auto &KV : Sim->TileMap) {
    const FHex &H = KV.Key;
    const FTile &T = KV.Value;

    // Handle Output and ClusterOutput tiles (NOT DiscoveryClusterInput - that's
    // handled by RebuildInputTiles)
    if (T.Kind == ETileKind::Output || T.Kind == ETileKind::ClusterOutput ||
        T.Kind == ETileKind::DiscoveryClusterOutput) {
      OutputCount++;
      const FVector2D LocalXY =
          HexCoord::AxialToXY(FHexAxial(H.Q, H.R), Tile, Ori);
      const FVector2D RotXY = HexCoord::Rotate2D(LocalXY, GridYaw);
      const FVector Loc = AnchorV + FVector(RotXY.X, RotXY.Y, 0.f);

      // ClusterOutput tiles are scaled larger than regular Output tiles
      float Scale = (T.Kind == ETileKind::ClusterOutput ||
                     T.Kind == ETileKind::DiscoveryClusterOutput)
                        ? ClusterScale
                        : 1.0f;
      const int32 DisplayLevel = GetDisplayLevel(H.Level);
      const FTransform Transform(
          FRotator(DisplayLevel == 0 ? 0.f : 180.f, 0.f, 0.f), Loc,
          FVector(Scale));

      // Get orb type - all output types now store OrbType directly in Arg
      EOrbType OutputType = (EOrbType)T.Arg;

      // Get the appropriate ISM for this output type
      UInstancedStaticMeshComponent *OutputISM =
          GetOutputISMForType(OutputType);

      if (OutputISM) {
        OutputISM->AddInstance(Transform);
      } else {
        UE_LOG(LogTemp, Warning,
               TEXT("HexFactoryActor::RebuildOutputs: No ISM found for output "
                    "type %d at (%d,%d)"),
               (int32)OutputType, H.Q, H.R);
      }
    }
  }

  UE_LOG(LogTemp, Log,
         TEXT("HexFactoryActor::RebuildOutputs: Rendered %d output tiles"),
         OutputCount);
}

UInstancedStaticMeshComponent *
AHexFactoryActor::GetOutputISMForType(EOrbType OrbType) {
  switch (OrbType) {
  case EOrbType::None:
    return OutputNoneISM;
  case EOrbType::BaseA:
    return OutputBaseAISM;
  case EOrbType::BaseB:
    return OutputBaseBISM;
  case EOrbType::BaseC:
    return OutputBaseCISM;
  case EOrbType::MetalA:
    return OutputMetalAISM;
  case EOrbType::CatalystX:
    return OutputCatalystXISM;
  case EOrbType::Void:
    return OutputVoidISM;
  default:
    return OutputBaseAISM; // Fallback
  }
}

UInstancedStaticMeshComponent *
AHexFactoryActor::GetInputISMForType(EOrbType OrbType) {
  switch (OrbType) {
  case EOrbType::None:
    return InputNoneISM;
  case EOrbType::BaseA:
    return InputBaseAISM;
  case EOrbType::BaseB:
    return InputBaseBISM;
  case EOrbType::BaseC:
    return InputBaseCISM;
  case EOrbType::MetalA:
    return InputMetalAISM;
  case EOrbType::CatalystX:
    return InputCatalystXISM;
  case EOrbType::Void:
    return InputVoidISM;
  default:
    return InputBaseAISM; // Fallback
  }
}

UInstancedStaticMeshComponent *
AHexFactoryActor::GetHelixOrbISMForType(EOrbType OrbType) {
  switch (OrbType) {
  case EOrbType::None:
    return HelixOrbNoneISM;
  case EOrbType::BaseA:
    return HelixOrbBaseAISM;
  case EOrbType::BaseB:
    return HelixOrbBaseBISM;
  case EOrbType::BaseC:
    return HelixOrbBaseCISM;
  case EOrbType::MetalA:
    return HelixOrbMetalAISM;
  case EOrbType::CatalystX:
    return HelixOrbCatalystXISM;
  case EOrbType::Void:
    return HelixOrbVoidISM;
  default:
    return HelixOrbBaseAISM; // Fallback
  }
}

void AHexFactoryActor::RebuildInputTiles() {
  if (!Sim)
    return;

  // Get material manager
  UOrbMaterialManager *MaterialManager = UOrbMaterialManager::FindInWorld(this);
  if (!MaterialManager) {
    UE_LOG(LogTemp, Warning,
           TEXT("HexFactoryActor::RebuildInputTiles: No OrbMaterialManager "
                "found in world."));
    return;
  }

  // Get the custom input tile mesh from GameManager, or fallback to regular
  // tile mesh
  UStaticMesh *BaseTileMesh = nullptr;
  if (UWorld *World = GetWorld()) {
    for (TActorIterator<AGameManager> It(World); It; ++It) {
      AGameManager *GameManager = *It;
      if (GameManager) {
        // Prefer CustomInputTileMesh if available, otherwise use TileMesh
        if (GameManager->CustomInputTileMesh) {
          BaseTileMesh = GameManager->CustomInputTileMesh;
        } else if (GameManager->TileMesh) {
          BaseTileMesh = GameManager->TileMesh;
        }
        break;
      }
    }
  }

  if (!BaseTileMesh) {
    // Fallback to local tile mesh
    if (TileMesh) {
      BaseTileMesh = TileMesh;
    } else {
      UE_LOG(
          LogTemp, Error,
          TEXT("HexFactoryActor::RebuildInputTiles: No tile mesh available"));
      return;
    }
  }

  const float Tile = Driver ? Driver->GetTileSize() : 100.f;
  const float GridYaw = Driver ? Driver->GetGridYaw() : 0.f;
  const EHexOrientation Ori =
      Driver ? Driver->GetGridOrientation() : EHexOrientation::PointyTop;
  const AActor *Anchor = GetAnchor();
  const FVector AnchorV =
      Anchor ? Anchor->GetActorLocation() : FVector::ZeroVector;

  // Set up all input ISM components with the custom input tile mesh and
  // type-specific materials
  TArray<EOrbType> AllOrbTypes = {
      EOrbType::None,   EOrbType::BaseA,     EOrbType::BaseB, EOrbType::BaseC,
      EOrbType::MetalA, EOrbType::CatalystX, EOrbType::Void};

  for (EOrbType OrbType : AllOrbTypes) {
    UInstancedStaticMeshComponent *InputISM = GetInputISMForType(OrbType);
    if (InputISM) {
      // Set the custom input tile mesh
      InputISM->SetStaticMesh(BaseTileMesh);

      // Get the material for this orb type
      UMaterialInterface *TypeMaterial =
          MaterialManager->GetMaterialForOrbType(OrbType);
      if (TypeMaterial) {
        // Create a dynamic material instance for half opacity (like Output
        // tiles)
        UMaterialInstanceDynamic *InputMID =
            UMaterialInstanceDynamic::Create(TypeMaterial, this);
        if (InputMID) {
          // Set half opacity (0.5 alpha) to match Output tiles
          InputMID->SetScalarParameterValue(FName("Opacity"), 0.5f);
          InputISM->SetMaterial(0, InputMID);
        } else {
          InputISM->SetMaterial(0, TypeMaterial);
        }
      }

      // Clear instances for this type
      InputISM->ClearInstances();
    }
  }

  // Collect all Input and ClusterInput tiles and add them to the appropriate
  // ISM
  for (const auto &KV : Sim->TileMap) {
    const FHex &H = KV.Key;
    const FTile &T = KV.Value;

    if (T.Kind == ETileKind::Input || T.Kind == ETileKind::ClusterInput ||
        T.Kind == ETileKind::ClusterSend ||
        T.Kind == ETileKind::DiscoveryClusterInput) {
      const FVector2D LocalXY =
          HexCoord::AxialToXY(FHexAxial(H.Q, H.R), Tile, Ori);
      const FVector2D RotXY = HexCoord::Rotate2D(LocalXY, GridYaw);
      const FVector Loc = AnchorV + FVector(RotXY.X, RotXY.Y, 0.f);

      // Get orb type
      EOrbType OrbType = EOrbType::BaseA;
      if (T.Kind == ETileKind::Input || T.Kind == ETileKind::ClusterSend ||
          T.Kind == ETileKind::ClusterInput ||
          T.Kind == ETileKind::DiscoveryClusterInput) {
        // All input types store OrbType directly in Arg
        OrbType = (EOrbType)T.Arg;
      }

      // Add instance to the appropriate ISM for this orb type
      UInstancedStaticMeshComponent *InputISM = GetInputISMForType(OrbType);
      if (InputISM) {
        const int32 DisplayLevel = GetDisplayLevel(H.Level);
        InputISM->AddInstance(
            FTransform(FRotator(DisplayLevel == 0 ? 0.f : 180.f, 0.f, 0.f), Loc,
                       FVector(1.f)));
      }
    }
  }
}

int32 AHexFactoryActor::GetDisplayLevel(int32 Level) const {
  if (Driver && Driver->IsDisplayFlipped()) {
    return 1 - Level;
  }
  return Level;
}
AHexFactoryActor *AHexFactoryActor::FindInWorld(UWorld *World) {
  if (!World)
    return nullptr;
  for (TActorIterator<AHexFactoryActor> It(World); It; ++It) {
    return *It;
  }
  return nullptr;
}

// ============================================================
// HELIX OUTPUT RENDERING
// ============================================================

void AHexFactoryActor::RebuildHelixOutput() {
  // Clear existing helix instances
  if (HelixTileISM)
    HelixTileISM->ClearInstances();
  if (HelixOrbISM)
    HelixOrbISM->ClearInstances();
  if (HelixBondISM)
    HelixBondISM->ClearInstances();

  // Clear per-type helix orb ISMs
  if (HelixOrbNoneISM)
    HelixOrbNoneISM->ClearInstances();
  if (HelixOrbBaseAISM)
    HelixOrbBaseAISM->ClearInstances();
  if (HelixOrbBaseBISM)
    HelixOrbBaseBISM->ClearInstances();
  if (HelixOrbBaseCISM)
    HelixOrbBaseCISM->ClearInstances();
  if (HelixOrbMetalAISM)
    HelixOrbMetalAISM->ClearInstances();
  if (HelixOrbCatalystXISM)
    HelixOrbCatalystXISM->ClearInstances();
  if (HelixOrbVoidISM)
    HelixOrbVoidISM->ClearInstances();

  if (!Sim || !Driver)
    return;

  const float TileSize = Driver->GetTileSize();
  const float GridYaw = Driver->GetGridYaw();
  const EHexOrientation Ori = Driver->GetGridOrientation();
  const AActor *Anchor = GetAnchor();
  const FVector AnchorV =
      Anchor ? Anchor->GetActorLocation() : FVector::ZeroVector;

  // Build a lookup map of board item IDs to world positions for bond rendering
  TMap<int32, FVector> BoardItemPositions;
  for (const auto &KV : Sim->ItemMap) {
    const FHex &H = KV.Key;
    const FItem &Item = KV.Value;
    if (Item.Id == 0)
      continue;

    const FVector2D LocalXY =
        HexCoord::AxialToXY(FHexAxial(H.Q, H.R), TileSize, Ori);
    const FVector2D RotXY = HexCoord::Rotate2D(LocalXY, GridYaw);
    const int32 DisplayLevel = GetDisplayLevel(H.Level);
    const float BoardZ =
        (DisplayLevel == 0) ? 0.f : 100.f; // Match orb renderer

    BoardItemPositions.Add(Item.Id,
                           AnchorV + FVector(RotXY.X, RotXY.Y, BoardZ));

    // Also add sub-orb positions for clusters
    if (Item.bIsCluster) {
      for (int32 i = 0; i < Item.Orbs.Num(); ++i) {
        const FOrbData &Sub = Item.Orbs[i];
        const FVector2D SubLocalXY = HexCoord::AxialToXY(
            FHexAxial(H.Q + Sub.dQ, H.R + Sub.dR), TileSize, Ori);
        const FVector2D SubRotXY = HexCoord::Rotate2D(SubLocalXY, GridYaw);
        BoardItemPositions.Add(MakeSubOrbId(Item.Id, i),
                               AnchorV +
                                   FVector(SubRotXY.X, SubRotXY.Y, BoardZ));
      }
    }
  }

  // Set up helix tile mesh if available
  if (HelixTileMesh && HelixTileISM) {
    HelixTileISM->SetStaticMesh(HelixTileMesh);
    if (HelixTileMaterial) {
      HelixTileISM->SetMaterial(0, HelixTileMaterial);
    }
  }

  // Use orb mesh for helix orbs (fallback to tile mesh if not specified)
  UStaticMesh *OrbMeshToUse = HelixOrbMesh ? HelixOrbMesh : TileMesh;
  UStaticMesh *BondMeshToUse = HelixBondMesh ? HelixBondMesh : BondMesh;
  UMaterialInterface *BondMaterialToUse = HelixBondMaterial;

  // Track the base scale and assumed mesh length for the bond
  FVector BaseBondScale = HelixBondScale;
  float MeshBaseLength = TileSize;

  // Try to find OrbRenderer to use board bond assets if they are set
  if (UWorld *World = GetWorld()) {
    for (TActorIterator<AActor> It(World); It; ++It) {
      if (UOrbRenderer *OrbRenderer =
              It->FindComponentByClass<UOrbRenderer>()) {
        if (OrbRenderer->BondedOrbMesh) {
          BondMeshToUse = OrbRenderer->BondedOrbMesh;
          // The board bond mesh (BondedOrbMesh) is assumed to be 100 units long
          MeshBaseLength = 100.f;
          // Use the board bond's thickness scale (Z-aligned for orb bonds)
          BaseBondScale = FVector(OrbRenderer->BondedOrbScale,
                                  OrbRenderer->BondedOrbScale, 1.0f);
        }
        if (OrbRenderer->BondedOrbMaterial) {
          BondMaterialToUse = OrbRenderer->BondedOrbMaterial;
        }
        break;
      }
    }
  }

  // Get material manager for per-type materials
  UOrbMaterialManager *MaterialManager = UOrbMaterialManager::FindInWorld(this);

  // Set up per-type helix orb ISMs with orb mesh and type-specific materials
  TArray<EOrbType> AllOrbTypes = {
      EOrbType::None,   EOrbType::BaseA,     EOrbType::BaseB, EOrbType::BaseC,
      EOrbType::MetalA, EOrbType::CatalystX, EOrbType::Void};

  for (EOrbType OrbType : AllOrbTypes) {
    UInstancedStaticMeshComponent *HelixOrbTypedISM =
        GetHelixOrbISMForType(OrbType);
    if (HelixOrbTypedISM && OrbMeshToUse) {
      HelixOrbTypedISM->SetStaticMesh(OrbMeshToUse);

      // Get per-type material
      if (MaterialManager) {
        UMaterialInterface *TypeMaterial =
            MaterialManager->GetMaterialForOrbType(OrbType);
        if (TypeMaterial) {
          HelixOrbTypedISM->SetMaterial(0, TypeMaterial);
        }
      }
    }
  }

  if (BondMeshToUse && HelixBondISM) {
    HelixBondISM->SetStaticMesh(BondMeshToUse);
    if (BondMaterialToUse) {
      HelixBondISM->SetMaterial(0, BondMaterialToUse);
    }
  }

  // Render helix output and input tiles
  for (const auto &TileKV : Sim->TileMap) {
    const FHex &H = TileKV.Key;
    const FTile &T = TileKV.Value;

    if (T.Kind != ETileKind::HelixOutput && T.Kind != ETileKind::HelixInput)
      continue;

    // Place tile mesh at helix output location
    if (HelixTileISM && HelixTileMesh) {
      const FVector2D LocalXY =
          HexCoord::AxialToXY(FHexAxial(H.Q, H.R), TileSize, Ori);
      const FVector2D RotXY = HexCoord::Rotate2D(LocalXY, GridYaw);
      const int32 DisplayLevel = GetDisplayLevel(H.Level);
      const float Pitch = (DisplayLevel == 0) ? 0.f : 180.f;
      const FVector Loc = AnchorV + FVector(RotXY.X, RotXY.Y, 0.f);

      HelixTileISM->AddInstance(
          FTransform(FRotator(Pitch, 0.f, 0.f), Loc, FVector(1.f)));
    }
  }

  // Render helix rungs (the spiraling cluster elevator)
  const TArray<USimWorldHex::FHelixOutputGroup> &HelixGroups =
      Sim->GetHelixGroups();

  // Get helix direction from GameManager
  float ZDirectionMult = 1.f; // Default: extends upward
  if (UWorld *World = GetWorld()) {
    for (TActorIterator<AGameManager> It(World); It; ++It) {
      if (AGameManager *GM = *It) {
        ZDirectionMult = GM->bHelixExtendsUp ? 1.f : -1.f;
        break;
      }
    }
  }

  for (const USimWorldHex::FHelixOutputGroup &Group : HelixGroups) {
    if (Group.MemberTiles.Num() == 0)
      continue;

    const FVector HelixCenter = Group.CenterWorld;
    const float IdleRotation = Group.HelixRotationOffset;

    // Render each cluster rung in the helix (newest at bottom, oldest at top)
    for (int32 RungIdx = 0; RungIdx < Group.Rungs.Num(); ++RungIdx) {
      const USimWorldHex::FHelixRung &Rung = Group.Rungs[RungIdx];

      // The cluster rotates around the helix center as it rises
      const float RungRotation = GridYaw + Rung.RotationDegrees + IdleRotation;
      const float RungAngleRad = FMath::DegreesToRadians(RungRotation);
      const float CosRot = FMath::Cos(RungAngleRad);
      const float SinRot = FMath::Sin(RungAngleRad);

      // Render each orb in the cluster
      for (int32 OrbIdx = 0; OrbIdx < Rung.Orbs.Num(); ++OrbIdx) {
        const FOrbData &OrbData = Rung.Orbs[OrbIdx];

        // Calculate orb position relative to cluster center
        // Use hex-to-XY conversion for proper spacing
        const FVector2D OrbLocalXY = HexCoord::AxialToXY(
            FHexAxial(OrbData.dQ, OrbData.dR), TileSize, Ori);

        // Rotate the local position by the rung rotation
        FVector OrbOffset;
        OrbOffset.X = OrbLocalXY.X * CosRot - OrbLocalXY.Y * SinRot;
        OrbOffset.Y = OrbLocalXY.X * SinRot + OrbLocalXY.Y * CosRot;
        OrbOffset.Z = Rung.ZOffset * ZDirectionMult; // Apply direction

        const FVector OrbWorld = HelixCenter + OrbOffset;

        // NEW: specific handling for just-spawned items to interpolate from
        // their previous board position
        FVector FinalOrbWorld = OrbWorld;
        if (Rung.SpawnTick == Sim->StepCounter && Sim) {
          // We are in the entry step. The item is logically in the helix (at
          // Z=0), but visually we want to slide it from where it was on the
          // board.
          if (OrbData.PreviousBoardHex != FHex(0, 0, 0)) {
            const FVector PrevWorldPos =
                Sim->HexCenterToWorld(OrbData.PreviousBoardHex);

            // Interpolate
            FinalOrbWorld =
                FMath::Lerp(PrevWorldPos, OrbWorld, Sim->FrameAlpha);
          }
        }

        // Use per-type helix orb ISM for correct mesh and material
        EOrbType OrbType = static_cast<EOrbType>(OrbData.Type);
        if (UInstancedStaticMeshComponent *TypedISM =
                GetHelixOrbISMForType(OrbType)) {
          TypedISM->AddInstance(FTransform(FRotator(0.f, RungRotation, 0.f),
                                           FinalOrbWorld, HelixOrbScale));
        }
      }

      // Render bonds between orbs in this cluster
      for (const TPair<int32, int32> &Bond : Rung.Bonds) {
        if (Bond.Key < 0 || Bond.Key >= Rung.Orbs.Num())
          continue;
        if (Bond.Value < 0 || Bond.Value >= Rung.Orbs.Num())
          continue;

        const FOrbData &OrbA = Rung.Orbs[Bond.Key];
        const FOrbData &OrbB = Rung.Orbs[Bond.Value];

        // Calculate world positions for both orbs
        const FVector2D OrbALocalXY =
            HexCoord::AxialToXY(FHexAxial(OrbA.dQ, OrbA.dR), TileSize, Ori);
        const FVector2D OrbBLocalXY =
            HexCoord::AxialToXY(FHexAxial(OrbB.dQ, OrbB.dR), TileSize, Ori);

        FVector PosA = HelixCenter +
                       FVector(OrbALocalXY.X * CosRot - OrbALocalXY.Y * SinRot,
                               OrbALocalXY.X * SinRot + OrbALocalXY.Y * CosRot,
                               Rung.ZOffset * ZDirectionMult);
        FVector PosB = HelixCenter +
                       FVector(OrbBLocalXY.X * CosRot - OrbBLocalXY.Y * SinRot,
                               OrbBLocalXY.X * SinRot + OrbBLocalXY.Y * CosRot,
                               Rung.ZOffset * ZDirectionMult);

        // Apply interpolation for just-spawned rungs
        if (Rung.SpawnTick == Sim->StepCounter && Sim) {
          if (OrbA.PreviousBoardHex != FHex(0, 0, 0)) {
            PosA = FMath::Lerp(Sim->HexCenterToWorld(OrbA.PreviousBoardHex),
                               PosA, Sim->FrameAlpha);
          }
          if (OrbB.PreviousBoardHex != FHex(0, 0, 0)) {
            PosB = FMath::Lerp(Sim->HexCenterToWorld(OrbB.PreviousBoardHex),
                               PosB, Sim->FrameAlpha);
          }
        }

        // Bond mesh positioned at midpoint, oriented toward the other orb
        FVector Mid = (PosA + PosB) * 0.5f;
        FVector Dir = (PosB - PosA).GetSafeNormal();
        float BondLength = (PosB - PosA).Size() / MeshBaseLength;

        if (HelixBondISM) {
          FVector BondScaleFinal = BaseBondScale;
          // Scale along Z if using board assets (100 units), otherwise X
          if (MeshBaseLength == 100.f)
            BondScaleFinal.Z *= BondLength;
          else
            BondScaleFinal.X *= BondLength;

          // Orientation: Mesh Z (Up) -> Dir if board assets, otherwise Mesh X
          FRotator Rot;
          if (MeshBaseLength == 100.f) {
            FVector Up = Dir;
            FVector Right =
                FVector::CrossProduct(Up, FVector::UpVector).GetSafeNormal();
            if (Right.IsNearlyZero())
              Right = FVector::RightVector;
            FVector Forward = FVector::CrossProduct(Right, Up).GetSafeNormal();
            Rot = FRotationMatrix::MakeFromXY(Forward, Right).Rotator();
          } else {
            Rot = Dir.Rotation();
          }

          HelixBondISM->AddInstance(FTransform(Rot, Mid, BondScaleFinal));
        }
      }

      // Render vertical bonds between this rung and the one above/previous
      if (RungIdx + 1 < Group.Rungs.Num()) {
        const USimWorldHex::FHelixRung &PrevRung = Group.Rungs[RungIdx + 1];
        const float PrevRungRotation =
            GridYaw + PrevRung.RotationDegrees + IdleRotation;
        const float PrevRungAngleRad =
            FMath::DegreesToRadians(PrevRungRotation);
        const float PrevCosRot = FMath::Cos(PrevRungAngleRad);
        const float PrevSinRot = FMath::Sin(PrevRungAngleRad);

        for (const TPair<int32, int32> &Bond : Rung.BondsToPrevious) {
          if (Bond.Key < 0 || Bond.Key >= Rung.Orbs.Num())
            continue;
          if (Bond.Value < 0 || Bond.Value >= PrevRung.Orbs.Num())
            continue;

          const FOrbData &OrbA = Rung.Orbs[Bond.Key];
          const FOrbData &OrbB = PrevRung.Orbs[Bond.Value];

          FVector2D OrbALocalXY =
              HexCoord::AxialToXY(FHexAxial(OrbA.dQ, OrbA.dR), TileSize, Ori);
          FVector2D OrbBLocalXY =
              HexCoord::AxialToXY(FHexAxial(OrbB.dQ, OrbB.dR), TileSize, Ori);

          FVector PosA =
              HelixCenter +
              FVector(OrbALocalXY.X * CosRot - OrbALocalXY.Y * SinRot,
                      OrbALocalXY.X * SinRot + OrbALocalXY.Y * CosRot,
                      Rung.ZOffset * ZDirectionMult);
          FVector PosB =
              HelixCenter +
              FVector(OrbBLocalXY.X * PrevCosRot - OrbBLocalXY.Y * PrevSinRot,
                      OrbBLocalXY.X * PrevSinRot + OrbBLocalXY.Y * PrevCosRot,
                      PrevRung.ZOffset * ZDirectionMult);

          // Interpolate the new orb's position for vertical bonds
          if (Rung.SpawnTick == Sim->StepCounter && Sim) {
            if (OrbA.PreviousBoardHex != FHex(0, 0, 0)) {
              PosA = FMath::Lerp(Sim->HexCenterToWorld(OrbA.PreviousBoardHex),
                                 PosA, Sim->FrameAlpha);
            }
          }

          FVector Mid = (PosA + PosB) * 0.5f;
          FVector Dir = (PosB - PosA).GetSafeNormal();
          float BondLength = (PosB - PosA).Size() / MeshBaseLength;

          if (HelixBondISM) {
            FVector BondScaleFinal = BaseBondScale;
            if (MeshBaseLength == 100.f)
              BondScaleFinal.Z *= BondLength;
            else
              BondScaleFinal.X *= BondLength;

            FRotator Rot;
            if (MeshBaseLength == 100.f) {
              FVector Up = Dir;
              FVector Right =
                  FVector::CrossProduct(Up, FVector::UpVector).GetSafeNormal();
              if (Right.IsNearlyZero())
                Right = FVector::RightVector;
              FVector Forward =
                  FVector::CrossProduct(Right, Up).GetSafeNormal();
              Rot = FRotationMatrix::MakeFromXY(Forward, Right).Rotator();
            } else {
              Rot = Dir.Rotation();
            }
            HelixBondISM->AddInstance(FTransform(Rot, Mid, BondScaleFinal));
          }
        }
      }

      // Render bonds connecting items in this rung to items still on the board
      for (const TPair<int32, int32> &Bond : Rung.BondsToBoard) {
        if (Bond.Key < 0 || Bond.Key >= Rung.Orbs.Num())
          continue;

        const FOrbData &OrbA = Rung.Orbs[Bond.Key];
        const int32 BoardId = Bond.Value;

        if (const FVector *PosBPtr = BoardItemPositions.Find(BoardId)) {
          // Calculate orb position relative to helix
          const FVector2D OrbALocalXY =
              HexCoord::AxialToXY(FHexAxial(OrbA.dQ, OrbA.dR), TileSize, Ori);
          FVector PosA =
              HelixCenter +
              FVector(OrbALocalXY.X * CosRot - OrbALocalXY.Y * SinRot,
                      OrbALocalXY.X * SinRot + OrbALocalXY.Y * CosRot,
                      Rung.ZOffset * ZDirectionMult);

          // Apply interpolation for just-spawned rungs (helix-side orb)
          if (Rung.SpawnTick == Sim->StepCounter && Sim) {
            if (OrbA.PreviousBoardHex != FHex(0, 0, 0)) {
              PosA = FMath::Lerp(Sim->HexCenterToWorld(OrbA.PreviousBoardHex),
                                 PosA, Sim->FrameAlpha);
            }
          }
          FVector PosB = *PosBPtr;

          FVector Mid = (PosA + PosB) * 0.5f;
          FVector Dir = (PosB - PosA).GetSafeNormal();
          float BondLength = (PosB - PosA).Size() / MeshBaseLength;

          if (HelixBondISM) {
            FVector BondScaleFinal = BaseBondScale;
            if (MeshBaseLength == 100.f)
              BondScaleFinal.Z *= BondLength;
            else
              BondScaleFinal.X *= BondLength;

            FRotator Rot;
            if (MeshBaseLength == 100.f) {
              FVector Up = Dir;
              FVector Right =
                  FVector::CrossProduct(Up, FVector::UpVector).GetSafeNormal();
              if (Right.IsNearlyZero())
                Right = FVector::RightVector;
              FVector Forward =
                  FVector::CrossProduct(Right, Up).GetSafeNormal();
              Rot = FRotationMatrix::MakeFromXY(Forward, Right).Rotator();
            } else {
              Rot = Dir.Rotation();
            }
            HelixBondISM->AddInstance(FTransform(Rot, Mid, BondScaleFinal));
          }
        }
      }
    }
  }
}

void AHexFactoryActor::UpdateHelixAnimation(float DeltaSeconds) {
  if (!Sim)
    return;

  // Update helix group animations (synced with GTM)
  Sim->UpdateHelixGroups(DeltaSeconds);
  Sim->UpdateSourceGroups(DeltaSeconds);

  // ALWAYS rebuild to update visuals (rotation and elevator effect)
  RebuildHelixOutput();
  RebuildSourceAnimation(); // NEW: Update source animations
}

void AHexFactoryActor::RebuildSourceAnimation() {
  if (!Sim || !Driver)
    return;

  const float TileSize = Driver->GetTileSize();
  const float GridYaw = Driver->GetGridYaw();
  const EHexOrientation Ori = Driver->GetGridOrientation();
  const AActor *Anchor = GetAnchor();
  const FVector AnchorV =
      Anchor ? Anchor->GetActorLocation() : FVector::ZeroVector;

  // Constant descent/ascent rate matching Helix Spacing of 150 units per 20
  // ticks (7.5 units/tick)
  // Speed is calculated per-group below based on GenerationInterval
  const int32 DelayTicks =
      150; // Sync with SimWorldHex_Source.cpp (hardcoded 150)

  // Twist config: Degrees per unit depth
  // Helix: 36 degrees per 150 units -> 0.24 deg/unit
  const float TwistPerUnit = 36.0f / 150.0f;

  for (const auto &KV : Sim->SourceGroups) {
    const FSourceGroup &Group = KV.Value;

    // Calculate Group Center (World Space) for rotation pivot
    FVector GroupCenter = FVector::ZeroVector;
    if (Group.SortedTiles.Num() > 0) {
      for (const FHex &H : Group.SortedTiles) {
        GroupCenter += Driver->MapHexToWorld(H);
      }
      GroupCenter /= (float)Group.SortedTiles.Num();
    } else {
      continue;
    }

    // Render pending rows (ascending orbs)
    // ZOffset is updated in Sim->UpdateSourceGroups(DeltaTime)
    for (int32 k = 0; k < Group.PendingRows.Num(); ++k) {
      const FSourceGroupItem &Row = Group.PendingRows[k];

      float ZOffset = Row.ZOffset;

      // Don't render if at surface (will be handled by board item)
      // Though overlapping frame might happen, it's safer to hide if Z >= 0
      if (ZOffset >= 0.0f)
        continue;

      // Calculate Twist Angle (0 at surface, increasing as we go down)
      float TwistAngle =
          ZOffset * TwistPerUnit; // Negative Z * pos factor = Negative Angle

      // Render orbs in this row
      for (int32 i = 0; i < Row.OrbTypes.Num(); ++i) {
        if (i >= Group.SortedTiles.Num())
          break;

        uint8 TypeId = Row.OrbTypes[i];
        if (TypeId == 0)
          continue; // None/Hole

        EOrbType OrbType = (EOrbType)TypeId;
        FHex Hex = Group.SortedTiles[i];

        // Calculate Target Grid Position (Where it wants to be at Surface)
        FVector GridPos = Driver->MapHexToWorld(Hex);

        // Apply Twist around GroupCenter
        FVector RelPos = GridPos - GroupCenter;
        FVector TwistedRelPos =
            RelPos.RotateAngleAxis(TwistAngle, FVector::UpVector);

        FVector FinalPos = GroupCenter + TwistedRelPos + FVector(0, 0, ZOffset);

        // Rotation of the orb itself?
        // Helix orbs rotate with the group.
        // Base Rotation = GridYaw + TwistAngle.
        FRotator FinalRot = FRotator(0.f, GridYaw + TwistAngle, 0.f);

        // Add Instance
        if (UInstancedStaticMeshComponent *ISM =
                GetHelixOrbISMForType(OrbType)) {
          ISM->AddInstance(FTransform(FinalRot, FinalPos, HelixOrbScale));
        }

        // --- Render Vertical Bonds (to Deeper Row k+1) ---
        if (k + 1 < Group.PendingRows.Num()) {
          const FSourceGroupItem &NextRow = Group.PendingRows[k + 1];
          if (i < NextRow.OrbTypes.Num() && NextRow.OrbTypes[i] != 0) {
            // Determine Position of NextRow Orb (Deeper)
            float NextZ = NextRow.ZOffset;
            float NextAngle = NextZ * TwistPerUnit;

            FVector NextRel = GridPos - GroupCenter; // Same grid pos
            FVector NextTwisted =
                NextRel.RotateAngleAxis(NextAngle, FVector::UpVector);
            FVector NextPos = GroupCenter + NextTwisted + FVector(0, 0, NextZ);

            // Draw Bond FinalPos -> NextPos
            FVector Mid = (FinalPos + NextPos) * 0.5f;
            FVector Dir = (NextPos - FinalPos).GetSafeNormal();
            float Dist = FVector::Dist(FinalPos, NextPos);

            if (HelixBondISM) {
              FRotator Rot = Dir.Rotation();
              FVector Scale = HelixBondScale;
              Scale.X = Dist / 100.0f;
              HelixBondISM->AddInstance(FTransform(Rot, Mid, Scale));
            }
          }
        }

        // --- Render Connection to Board (only for shallowest Row k=0) ---
        if (k == 0) {
          // Check if there is an item currently on the board at this location
          const FItem *BoardItem = Sim->ItemMap.Find(Hex);
          if (BoardItem && (uint8)BoardItem->Single.Type != 0) {
            FVector BoardPos = GridPos; // Default Z=0

            // If this is a newly spawned item, use the shared helper to get the
            // precise handoff path

            // NOTE: The previous hardcoded logic caused jumps if OrbZOffset !=
            // 0 or if math drifted. Using Driver->GetHelixHandoffTransform
            // ensures perfect alignment with OrbRenderer.
            if (Sim->SourceTileItemIds.Contains(BoardItem->Id)) {
              FTransform StartTM = Driver->GetHelixHandoffTransform(Hex, Group);
              FVector StartPos = StartTM.GetLocation();
              FVector EndPos = Driver->MapHexToWorld(Hex);

              BoardPos = FMath::Lerp(StartPos, EndPos, Sim->FrameAlpha);
            }

            // Draw connect FinalPos (Helix Row 1) -> BoardPos (Board Item / Row
            // 0)
            FVector Mid = (FinalPos + BoardPos) * 0.5f;
            FVector Dir = (BoardPos - FinalPos).GetSafeNormal();
            float Dist = FVector::Dist(FinalPos, BoardPos);

            if (HelixBondISM) {
              FRotator Rot = Dir.Rotation();
              FVector Scale = HelixBondScale;
              Scale.X = Dist / 100.0f;
              HelixBondISM->AddInstance(FTransform(Rot, Mid, Scale));
            }
          }
        }
      }

      // Render bonds between adjacent orbs in this row
      for (int32 i = 0; i < Row.OrbTypes.Num() - 1; ++i) {
        if (i + 1 >= Group.SortedTiles.Num())
          break;

        uint8 T1 = Row.OrbTypes[i];
        uint8 T2 = Row.OrbTypes[i + 1];

        if (T1 != 0 && T2 != 0) {
          // Calculate Target Positions
          FHex H1 = Group.SortedTiles[i];
          FHex H2 = Group.SortedTiles[i + 1];

          if (HexUtil::Dist(H1, H2) != 1)
            continue;

          FVector2D XY1 =
              HexCoord::AxialToXY(FHexAxial(H1.Q, H1.R), TileSize, Ori);
          FVector2D RotXY1 = HexCoord::Rotate2D(XY1, GridYaw);
          FVector P1_Grid = AnchorV + FVector(RotXY1.X, RotXY1.Y, 0);

          FVector2D XY2 =
              HexCoord::AxialToXY(FHexAxial(H2.Q, H2.R), TileSize, Ori);
          FVector2D RotXY2 = HexCoord::Rotate2D(XY2, GridYaw);
          FVector P2_Grid = AnchorV + FVector(RotXY2.X, RotXY2.Y, 0);

          // Apply Twist
          FVector Rel1 = P1_Grid - GroupCenter;
          FVector P1_Twisted =
              GroupCenter +
              Rel1.RotateAngleAxis(TwistAngle, FVector::UpVector) +
              FVector(0, 0, ZOffset);

          FVector Rel2 = P2_Grid - GroupCenter;
          FVector P2_Twisted =
              GroupCenter +
              Rel2.RotateAngleAxis(TwistAngle, FVector::UpVector) +
              FVector(0, 0, ZOffset);

          FVector Mid = (P1_Twisted + P2_Twisted) * 0.5f;
          FVector Dir = (P2_Twisted - P1_Twisted).GetSafeNormal();
          float Dist = FVector::Dist(P1_Twisted, P2_Twisted);

          if (HelixBondISM) {
            FRotator Rot = Dir.Rotation();

            // Scale X to match distance
            FVector Scale = HelixBondScale;
            Scale.X = Dist / 100.0f;

            HelixBondISM->AddInstance(FTransform(Rot, Mid, Scale));
          }
        }
      }
    }
  }
}
