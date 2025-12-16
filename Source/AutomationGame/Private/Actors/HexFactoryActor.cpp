#include "Actors/HexFactoryActor.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Components/BoxComponent.h"
#include "ProceduralMeshComponent.h"
#include "Actors/OrbSimDriver.h"
#include "Actors/HexBoardActor.h"
#include "Sim/SimWorldHex.h"
#include "Sim/HexCoord.h"
#include "Sim/OrbTypes.h"
#include "Data/OrbMaterialManager.h"
#include "Framework/GameManager.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "DrawDebugHelpers.h"

AHexFactoryActor::AHexFactoryActor()
{
	PrimaryActorTick.bCanEverTick = true;

	USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);

	TileISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("TileISM"));
	TileISM->SetupAttachment(Root);
	TileISM->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	TileISM->SetCollisionResponseToAllChannels(ECollisionResponse::ECR_Block);
	TileISM->SetCanEverAffectNavigation(false);

	InputTileISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("InputTileISM"));
	InputTileISM->SetupAttachment(Root);
	InputTileISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	InputTileISM->SetCanEverAffectNavigation(false);

	OutputTileISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("OutputTileISM"));
	OutputTileISM->SetupAttachment(Root);
	OutputTileISM->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	OutputTileISM->SetCollisionResponseToAllChannels(ECollisionResponse::ECR_Block);
	OutputTileISM->SetCanEverAffectNavigation(false);

	BeltTileISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("BeltTileISM"));
	BeltTileISM->SetupAttachment(Root);
	BeltTileISM->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	BeltTileISM->SetCollisionResponseToAllChannels(ECollisionResponse::ECR_Block);
	BeltTileISM->SetCanEverAffectNavigation(false);

	BondTileISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("BondTileISM"));
	BondTileISM->SetupAttachment(Root);
	BondTileISM->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	BondTileISM->SetCollisionResponseToAllChannels(ECollisionResponse::ECR_Block);
	BondTileISM->SetCanEverAffectNavigation(false);

	UnbondTileISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("UnbondTileISM"));
	UnbondTileISM->SetupAttachment(Root);
	UnbondTileISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	UnbondTileISM->SetCanEverAffectNavigation(false);

	BeltISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("BeltISM"));
	BeltISM->SetupAttachment(Root);
	BeltISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	BeltISM->SetCanEverAffectNavigation(false);

	BeltReadyISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("BeltReadyISM"));
	BeltReadyISM->SetupAttachment(Root);
	BeltReadyISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	BeltReadyISM->SetCanEverAffectNavigation(false);

	BeltWaitingISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("BeltWaitingISM"));
	BeltWaitingISM->SetupAttachment(Root);
	BeltWaitingISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	BeltWaitingISM->SetCanEverAffectNavigation(false);

	BondISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("BondISM"));
	BondISM->SetupAttachment(Root);
	BondISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	BondISM->SetCanEverAffectNavigation(false);

	UnbondISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("UnbondISM"));
	UnbondISM->SetupAttachment(Root);
	UnbondISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	UnbondISM->SetCanEverAffectNavigation(false);

	// Create type-specific output ISM components
	OutputNoneISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("OutputNoneISM"));
	OutputNoneISM->SetupAttachment(Root);
	OutputNoneISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	OutputNoneISM->SetCanEverAffectNavigation(false);

	OutputBaseAISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("OutputBaseAISM"));
	OutputBaseAISM->SetupAttachment(Root);
	OutputBaseAISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	OutputBaseAISM->SetCanEverAffectNavigation(false);

	OutputBaseBISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("OutputBaseBISM"));
	OutputBaseBISM->SetupAttachment(Root);
	OutputBaseBISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	OutputBaseBISM->SetCanEverAffectNavigation(false);

	OutputBaseCISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("OutputBaseCISM"));
	OutputBaseCISM->SetupAttachment(Root);
	OutputBaseCISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	OutputBaseCISM->SetCanEverAffectNavigation(false);

	OutputMetalAISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("OutputMetalAISM"));
	OutputMetalAISM->SetupAttachment(Root);
	OutputMetalAISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	OutputMetalAISM->SetCanEverAffectNavigation(false);

	OutputCatalystXISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("OutputCatalystXISM"));
	OutputCatalystXISM->SetupAttachment(Root);
	OutputCatalystXISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	OutputCatalystXISM->SetCanEverAffectNavigation(false);

	OutputVoidISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("OutputVoidISM"));
	OutputVoidISM->SetupAttachment(Root);
	OutputVoidISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	OutputVoidISM->SetCanEverAffectNavigation(false);

	// Type-specific input ISM components
	InputNoneISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("InputNoneISM"));
	InputNoneISM->SetupAttachment(Root);
	InputNoneISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	InputNoneISM->SetCanEverAffectNavigation(false);

	InputBaseAISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("InputBaseAISM"));
	InputBaseAISM->SetupAttachment(Root);
	InputBaseAISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	InputBaseAISM->SetCanEverAffectNavigation(false);

	InputBaseBISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("InputBaseBISM"));
	InputBaseBISM->SetupAttachment(Root);
	InputBaseBISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	InputBaseBISM->SetCanEverAffectNavigation(false);

	InputBaseCISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("InputBaseCISM"));
	InputBaseCISM->SetupAttachment(Root);
	InputBaseCISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	InputBaseCISM->SetCanEverAffectNavigation(false);

	InputMetalAISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("InputMetalAISM"));
	InputMetalAISM->SetupAttachment(Root);
	InputMetalAISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	InputMetalAISM->SetCanEverAffectNavigation(false);

	InputCatalystXISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("InputCatalystXISM"));
	InputCatalystXISM->SetupAttachment(Root);
	InputCatalystXISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	InputCatalystXISM->SetCanEverAffectNavigation(false);

	InputVoidISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("InputVoidISM"));
	InputVoidISM->SetupAttachment(Root);
	InputVoidISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	InputVoidISM->SetCanEverAffectNavigation(false);

	// Create collision plane for efficient line traces
	GridCollisionPlane = CreateDefaultSubobject<UBoxComponent>(TEXT("GridCollisionPlane"));
	GridCollisionPlane->SetupAttachment(Root);
	GridCollisionPlane->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	GridCollisionPlane->SetCollisionResponseToAllChannels(ECollisionResponse::ECR_Block);
	GridCollisionPlane->SetBoxExtent(FVector(50000.f, 50000.f, 1.f)); // Large plane, 2 units thick
	GridCollisionPlane->SetRelativeLocation(FVector(0.f, 0.f, 0.f)); // Will be positioned at grid Z level
	GridCollisionPlane->SetVisibility(false); // Invisible, collision only
	GridCollisionPlane->SetHiddenInGame(true);
}

void AHexFactoryActor::EnsureComponents()
{
	if (!GetRootComponent())
	{
		USceneComponent* Root = NewObject<USceneComponent>(this, TEXT("Root_Runtime"));
		Root->RegisterComponent();
		SetRootComponent(Root);
	}
	auto EnsureISM = [&](UInstancedStaticMeshComponent*& Comp, const TCHAR* Name)
		{
			if (!Comp)
			{
				Comp = NewObject<UInstancedStaticMeshComponent>(this, Name);
				Comp->SetupAttachment(GetRootComponent());
				Comp->RegisterComponent();
				// Disable navigation to prevent ensure failures when clearing instances
				Comp->SetCanEverAffectNavigation(false);
				// Enable collision for tile ISMs to support line traces
			if (Comp == TileISM || Comp == OutputTileISM || Comp == BeltTileISM || Comp == BondTileISM || Comp == UnbondTileISM)
			{
				Comp->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
				Comp->SetCollisionResponseToAllChannels(ECollisionResponse::ECR_Block);
			}
			else
			{
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
	
	// Ensure procedural mesh for merged outputs
	if (!MergedOutputMesh)
	{
		MergedOutputMesh = NewObject<UProceduralMeshComponent>(this, TEXT("MergedOutputMesh_Runtime"));
		MergedOutputMesh->SetupAttachment(GetRootComponent());
		MergedOutputMesh->RegisterComponent();
		MergedOutputMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	}
	MergedOutputMesh->SetVisibility(true, true);
	
	// Ensure InputTilesMesh component exists
	if (!InputTilesMesh)
	{
		InputTilesMesh = NewObject<UProceduralMeshComponent>(this, TEXT("InputTilesMesh_Runtime"));
		InputTilesMesh->SetupAttachment(GetRootComponent());
		InputTilesMesh->RegisterComponent();
		InputTilesMesh->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
		InputTilesMesh->SetCollisionResponseToAllChannels(ECollisionResponse::ECR_Block);
	}
	InputTilesMesh->SetVisibility(true, true);
	
	// Ensure grid collision plane exists
	if (!GridCollisionPlane)
	{
		GridCollisionPlane = NewObject<UBoxComponent>(this, TEXT("GridCollisionPlane_Runtime"));
		GridCollisionPlane->SetupAttachment(GetRootComponent());
		GridCollisionPlane->RegisterComponent();
		GridCollisionPlane->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
		GridCollisionPlane->SetCollisionResponseToAllChannels(ECollisionResponse::ECR_Block);
		GridCollisionPlane->SetBoxExtent(FVector(50000.f, 50000.f, 1.f)); // Large plane, 2 units thick
		GridCollisionPlane->SetRelativeLocation(FVector(0.f, 0.f, 0.f));
		GridCollisionPlane->SetVisibility(false);
		GridCollisionPlane->SetHiddenInGame(true);
	}
	
	// Update collision plane position to match grid Z level
	UpdateGridCollisionPlane();
}

void AHexFactoryActor::UpdateGridCollisionPlane()
{
	if (!GridCollisionPlane)
	{
		return;
	}
	
	const AActor* Anchor = GetAnchor();
	if (!Anchor)
	{
		return;
	}
	
	// Position the collision plane at the grid's Z level (anchor's Z)
	// Use world space positioning - detach temporarily to set world location, then reattach
	// This ensures the plane is at the exact Z level of the grid
	const FVector AnchorLocation = Anchor->GetActorLocation();
	
	// Detach to set world location directly
	GridCollisionPlane->DetachFromComponent(FDetachmentTransformRules::KeepWorldTransform);
	GridCollisionPlane->SetWorldLocation(FVector(AnchorLocation.X, AnchorLocation.Y, AnchorLocation.Z));
	
	// Reattach to root (this will convert world location to relative)
	GridCollisionPlane->AttachToComponent(GetRootComponent(), FAttachmentTransformRules::KeepWorldTransform);
}

AActor* AHexFactoryActor::GetAnchor() const
{
	return Driver ? Driver->GetGridAnchor() : nullptr;
}

void AHexFactoryActor::BeginPlay()
{
	Super::BeginPlay();
	EnsureComponents();

	if (UWorld* W = GetWorld())
	{
		Driver = AOrbSimDriver::Find(W);
		if (Driver) 
		{
			Sim = Driver->GetSim();
			UE_LOG(LogTemp, Log, TEXT("HexFactoryActor::BeginPlay: Driver=%s Sim=%s"), 
				Driver ? TEXT("found") : TEXT("null"), Sim ? TEXT("found") : TEXT("null"));
			
			// Subscribe to simulation step completion for belt material updates
			Driver->OnSimStepComplete.AddDynamic(this, &AHexFactoryActor::HandleSimStepComplete);
			UE_LOG(LogTemp, Log, TEXT("HexFactoryActor::BeginPlay: Bound to OrbSimDriver::OnSimStepComplete"));
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("HexFactoryActor::BeginPlay: No OrbSimDriver found"));
		}
		
		for (TActorIterator<AHexBoardActor> It(W); It; ++It)
		{
			Board = *It;
			UE_LOG(LogTemp, Log, TEXT("HexFactoryActor::BeginPlay: Found HexBoardActor"));
			break;
		}
	}

	RebuildInternal();
}

void AHexFactoryActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (Driver)
	{
		Driver->OnSimStepComplete.RemoveDynamic(this, &AHexFactoryActor::HandleSimStepComplete);
	}
	Super::EndPlay(EndPlayReason);
}

void AHexFactoryActor::HandleSimStepComplete(float StepSeconds)
{
	// Update belt materials immediately after simulation has stepped
	UpdateBeltMaterials();
}

void AHexFactoryActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	
	// Update collision plane position if anchor has moved
	static FVector LastAnchorLocation = FVector::ZeroVector;
	const AActor* Anchor = GetAnchor();
	if (Anchor)
	{
		const FVector CurrentAnchorLocation = Anchor->GetActorLocation();
		if (!CurrentAnchorLocation.Equals(LastAnchorLocation, 1.0f)) // Update if moved more than 1 unit
		{
			UpdateGridCollisionPlane();
			LastAnchorLocation = CurrentAnchorLocation;
		}
	}
	
	if (!Sim) return;

	const bool bBoardChanged = (Board && Board->BoardRevision != LastSeenBoardRevision);
	const bool bTilesChanged = (Sim->TileRevision != LastSeenRevision);
	
	// Handle full rebuilds when tiles or board change
	if (bTilesChanged || bBoardChanged)
	{
		UE_LOG(LogTemp, Log, TEXT("HexFactoryActor::Tick: Triggering rebuild - TileRevision=%d LastSeen=%d BoardChanged=%d"), 
			Sim->TileRevision, LastSeenRevision, bBoardChanged ? 1 : 0);
		RebuildInternal();
	}
}

void AHexFactoryActor::Rebuild()
{
	// Reacquire Sim reference in case it was created after our BeginPlay
	if (!Sim && Driver)
	{
		Sim = Driver->GetSim();
		UE_LOG(LogTemp, Log, TEXT("HexFactoryActor::Rebuild: Reacquired Sim=%s"), Sim ? TEXT("found") : TEXT("null"));
	}
	RebuildInternal();
}

FVector AHexFactoryActor::HexToWorld(const FHex& H) const
{
	const AActor* Anchor = GetAnchor();
	const FVector AnchorV = Anchor ? Anchor->GetActorLocation() : FVector::ZeroVector;
	const float Tile = Driver ? Driver->GetTileSize() : 100.f;
	const float GridYaw = Driver ? Driver->GetGridYaw() : 0.f;
	const EHexOrientation Ori = Driver ? Driver->GetGridOrientation() : EHexOrientation::PointyTop;

	const FVector2D LocalXY = HexCoord::AxialToXY(FHexAxial(H.Q, H.R), Tile, Ori);
	const FVector2D RotXY = HexCoord::Rotate2D(LocalXY, GridYaw);
	return AnchorV + FVector(RotXY.X, RotXY.Y, 0.f);
}

void AHexFactoryActor::RebuildInternal()
{
	// Note: Debug logging removed for performance
	
	EnsureComponents();
	
	// Update collision plane position when grid is rebuilt
	UpdateGridCollisionPlane();
	
	// Reacquire Sim reference in case it was created after our BeginPlay
	if (!Sim && Driver)
	{
		Sim = Driver->GetSim();
		// Note: Debug logging removed for performance
	}
	
	if (!Sim) 
	{
		UE_LOG(LogTemp, Warning, TEXT("HexFactoryActor::RebuildInternal: Sim is null after reacquisition attempt"));
		return;
	}

	LastSeenRevision = Sim->TileRevision;
	LastSeenBoardRevision = Board ? Board->BoardRevision : -1;

	// Set up tile ISM components
	// Get the tile mesh from GameManager (same as Output tiles use)
	UStaticMesh* BaseTileMesh = TileMesh;
	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<AGameManager> It(World); It; ++It)
		{
			AGameManager* GameManager = *It;
			if (GameManager && GameManager->TileMesh)
			{
				BaseTileMesh = GameManager->TileMesh;
				break;
			}
		}
	}
	
	if (BaseTileMesh) 
	{
		TileISM->SetStaticMesh(BaseTileMesh);
		// Input and ClusterInput tiles are rendered via procedural mesh (no ISM needed)
		OutputTileISM->SetStaticMesh(BaseTileMesh);
		BeltTileISM->SetStaticMesh(BaseTileMesh);
		BondTileISM->SetStaticMesh(BaseTileMesh);
	}
	
	if (TileMaterial) TileISM->SetMaterial(0, TileMaterial);
	
	// Input and ClusterInput tiles use procedural meshes with cutouts (handled in RebuildInputTiles)
	// No ISM material setup needed for Input tiles
	
	if (OutputTileMaterial) OutputTileISM->SetMaterial(0, OutputTileMaterial);
	if (BeltTileMaterial) BeltTileISM->SetMaterial(0, BeltTileMaterial);
	if (BondTileMaterial) BondTileISM->SetMaterial(0, BondTileMaterial);

	if (BeltMesh) 
	{
		BeltReadyISM->SetStaticMesh(BeltMesh);
		BeltWaitingISM->SetStaticMesh(BeltMesh);
		// Legacy BeltISM only used if new materials aren't set
		BeltISM->SetStaticMesh(BeltMesh);
	}
	// Only set up the new material ISMs if materials are provided
	if (BeltReadyMaterial) BeltReadyISM->SetMaterial(0, BeltReadyMaterial);
	if (BeltWaitingMaterial) BeltWaitingISM->SetMaterial(0, BeltWaitingMaterial);
	// Legacy fallback
	if (BeltMaterial) BeltISM->SetMaterial(0, BeltMaterial);
	if (BondMesh) BondISM->SetStaticMesh(BondMesh);
	if (BondMaterial) BondISM->SetMaterial(0, BondMaterial);
	
	// Set up unbond mesh - use BondMesh as fallback if UnbondMesh not assigned
	if (UnbondMesh) 
	{
		UnbondISM->SetStaticMesh(UnbondMesh);
	}
	else if (BondMesh)
	{
		// Fallback to bond mesh if no unbond mesh is set
		UnbondISM->SetStaticMesh(BondMesh);
	}
	
	// Set up unbond material - use BondMaterial as fallback if UnbondMaterial not assigned
	if (UnbondMaterial) 
	{
		UnbondISM->SetMaterial(0, UnbondMaterial);
	}
	else if (BondMaterial)
	{
		// Fallback to bond material if no unbond material is set
		UnbondISM->SetMaterial(0, BondMaterial);
	}

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

	RebuildTiles();
	RebuildBelts();
	RebuildBonds();   // NEW
	RebuildOutputs(); // NEW: Type-specific output rendering
	RebuildInputTiles(); // Render Input/ClusterInput tiles using ISM with orb-type materials
	
	// Note: Debug logging removed for performance
}

void AHexFactoryActor::RebuildTiles()
{
	if (!Sim) return;
	
	const float Tile = Driver ? Driver->GetTileSize() : 100.f;
	const float GridYaw = Driver ? Driver->GetGridYaw() : 0.f;
	const EHexOrientation Ori = Driver ? Driver->GetGridOrientation() : EHexOrientation::PointyTop;
	const AActor* Anchor = GetAnchor();
	const FVector AnchorV = Anchor ? Anchor->GetActorLocation() : FVector::ZeroVector;

	// Helper function to add tile instance to appropriate ISM
	auto AddTileInstance = [&](const FHex& H, ETileKind TileKind)
	{
		const FVector2D LocalXY = HexCoord::AxialToXY(FHexAxial(H.Q, H.R), Tile, Ori);
		const FVector2D RotXY = HexCoord::Rotate2D(LocalXY, GridYaw);
		const FVector Loc = AnchorV + FVector(RotXY.X, RotXY.Y, 0.f);
		const FTransform Transform(FRotator::ZeroRotator, Loc, FVector(1.f));
		
		switch (TileKind)
		{
		case ETileKind::Input:
		case ETileKind::ClusterInput:
			// Input and ClusterInput tiles are rendered via procedural mesh (with cutout)
			// Skip ISM rendering for these - they're handled in RebuildInputTiles()
			break;
		case ETileKind::Output:
			// Output tiles are handled by RebuildOutputs() with type-specific materials
			break;
		case ETileKind::Belt:
		case ETileKind::Bond:
		case ETileKind::Unbond:
			// Belts and bonds don't use base tile meshes - only their glyph meshes
			// Skip tile mesh rendering for these
			break;
		default:
			// Empty tiles or unknown types go to the default TileISM
			if (TileISM) TileISM->AddInstance(Transform);
			break;
		}
	};

	// Render tiles from the simulation
	for (const auto& KV : Sim->TileMap)
	{
		const FHex& H = KV.Key;
		const FTile& T = KV.Value;
		
		// Skip output tiles - they're handled by RebuildOutputs()
		if (T.Kind != ETileKind::Output)
		{
			AddTileInstance(H, T.Kind);
		}
	}

	// Also render empty tiles from the board if it exists
	if (Board)
	{
		for (const FHex& H : Board->GetAllHexes())
		{
			// Only add if not already in simulation
			if (!Sim->TileMap.Contains(H))
			{
				AddTileInstance(H, ETileKind::Empty);
			}
		}
	}
}

void AHexFactoryActor::DrawBeltArrow(const FVector& Center, float TileRadiusUU, float YawDeg)
{
	const float Len = TileRadiusUU * DebugArrowLenScale;
	const FVector Dir = FRotator(0.f, YawDeg, 0.f).RotateVector(FVector(1, 0, 0));
	const FVector A = Center + FVector(0, 0, BeltZOffset + 2.f);
	const FVector B = A + Dir * Len;
	DrawDebugDirectionalArrow(GetWorld(), A, B, 16.f, FColor::Yellow, false, 1.0f, 0, DebugArrowThickness);
}

void AHexFactoryActor::DrawBondArrow(const FVector& A, const FVector& B)
{
	DrawDebugLine(GetWorld(), A + FVector(0, 0, BondZOffset + 2.f), B + FVector(0, 0, BondZOffset + 2.f), FColor::Cyan, false, 1.0f, 0, 2.f);
}

void AHexFactoryActor::RebuildBelts()
{
	if (!Sim) 
	{
		UE_LOG(LogTemp, Warning, TEXT("HexFactoryActor::RebuildBelts: Sim is null"));
		return;
	}
	
	const float Tile = Driver ? Driver->GetTileSize() : 100.f;
	const float GridYaw = Driver ? Driver->GetGridYaw() : 0.f;
	const EHexOrientation Ori = Driver ? Driver->GetGridOrientation() : EHexOrientation::PointyTop;
	const AActor* Anchor = GetAnchor();
	const FVector AnchorV = Anchor ? Anchor->GetActorLocation() : FVector::ZeroVector;
	
	int32 BeltCount = 0;
	int32 ReadyCount = 0;
	int32 WaitingCount = 0;
	int32 EmptyCount = 0;

	auto ForwardYaw = [&](const FHex& Base, uint8 Dir)->float
		{
			const FHex Fwd = HexUtil::Neighbor(Base, (EHexDir)(Dir % 6));
			const FVector2D Axy = HexCoord::Rotate2D(HexCoord::AxialToXY(FHexAxial(Base.Q, Base.R), Tile, Ori), GridYaw);
			const FVector2D Bxy = HexCoord::Rotate2D(HexCoord::AxialToXY(FHexAxial(Fwd.Q, Fwd.R), Tile, Ori), GridYaw);
			const FVector2D Delta = Bxy - Axy;
			return Delta.IsNearlyZero() ? 0.f : FMath::RadiansToDegrees(FMath::Atan2(Delta.Y, Delta.X));
		};

	enum class EBeltState : uint8 { Empty, Ready, Waiting };

	// Helper function to check belt state
	auto GetBeltState = [&](const FHex& BeltHex, uint8 BeltDir) -> EBeltState
		{
			// Check if there's an item on this belt
			const FItem* BeltItem = Sim->ItemMap.Find(BeltHex);
			if (!BeltItem || BeltItem->Id == 0)
			{
				// No item on belt - belt is empty
				return EBeltState::Empty;
			}

			// There's an item on the belt - check if it can move forward
			const FHex DestHex = HexUtil::Neighbor(BeltHex, (EHexDir)(BeltDir % 6));
			
			// Check if destination is occupied
			if (Sim->ItemMap.Contains(DestHex))
			{
				// Destination is occupied - belt is waiting
				return EBeltState::Waiting;
			}

			// Check if destination is blocked by held items
			for (const FArmState& Arm : Sim->Arms)
			{
				if (!Arm.bHolding) continue;

				// Calculate arm grip position
				FHex GripHex = Arm.BaseHex;
				for (int i = 0; i < Arm.ArmLen; ++i)
					GripHex = HexUtil::Neighbor(GripHex, Arm.Facing);

				if (GripHex == DestHex)
				{
					// Arm is holding an item at the destination - belt is waiting
					return EBeltState::Waiting;
				}

				// Check for cluster items
				if (Arm.Held.bIsCluster)
				{
					for (const FOrbData& Sub : Arm.Held.Orbs)
					{
						FHex Local(Sub.dQ, Sub.dR);
						// Apply rotation based on arm facing and grab facing
						const uint8 cur = (uint8)Arm.Facing % 6;
						const int8 grab = (int8)Arm.Held.GrabFacing;
						const uint8 facingDelta = (grab >= 0)
							? (uint8)(((int32)cur - (int32)(grab % 6) + 6) % 6)
							: cur;
						const uint8 ccw = (uint8)((6 - facingDelta) % 6);

						// Rotate the local offset
						for (uint8 i = 0; i < (ccw % 6); ++i)
						{
							int32 q = Local.Q;
							int32 r = Local.R;
							Local.Q = -r;
							Local.R = q + r;
						}

						FHex WorldPos(GripHex.Q + Local.Q, GripHex.R + Local.R);
						if (WorldPos == DestHex)
						{
							// Part of held cluster is at destination - belt is waiting
							return EBeltState::Waiting;
						}
					}
				}
			}

			// Destination is clear - belt is ready to move
			return EBeltState::Ready;
		};

	for (const auto& KV : Sim->TileMap)
	{
		const FHex& H = KV.Key;
		const FTile& T = KV.Value;
		if (T.Kind != ETileKind::Belt) continue;

		BeltCount++;
		const FVector2D LocalXY = HexCoord::AxialToXY(FHexAxial(H.Q, H.R), Tile, Ori);
		const FVector2D RotXY = HexCoord::Rotate2D(LocalXY, GridYaw);
		const FVector Loc = AnchorV + FVector(RotXY.X, RotXY.Y, BeltZOffset);
		const float Yaw = ForwardYaw(H, (uint8)(T.Arg & 0xFF));

		if (BeltMesh)
		{
			const FTransform Transform(FRotator(0.f, Yaw, 0.f), Loc, BeltScale);
			
			// Use new material system if materials are set, otherwise fall back to legacy
			if (BeltReadyMaterial && BeltWaitingMaterial)
			{
				const uint8 BeltDir = (uint8)(T.Arg & 0xFF);
				EBeltState State = GetBeltState(H, BeltDir);
				
				switch (State)
				{
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
			else
			{
				// Fallback to legacy single-material rendering
				BeltISM->AddInstance(Transform);
			}
			
			if (bAlwaysDrawBeltArrows)
				DrawBeltArrow(Loc, Tile, Yaw);
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("HexFactoryActor::RebuildBelts: BeltMesh is null"));
		}
	}
	
	UE_LOG(LogTemp, Log, TEXT("HexFactoryActor::RebuildBelts: Found %d belts (%d empty, %d ready, %d waiting), BeltMesh=%s"), 
		BeltCount, EmptyCount, ReadyCount, WaitingCount, BeltMesh ? TEXT("exists") : TEXT("null"));
}

void AHexFactoryActor::UpdateBeltMaterials()
{
	if (!Sim || !BeltMesh) return;
	
	// Only update if new materials are set, otherwise do nothing (legacy system in use)
	if (!BeltReadyMaterial || !BeltWaitingMaterial) return;
	
	// Clear all belt material ISMs
	BeltISM->ClearInstances();
	BeltReadyISM->ClearInstances();
	BeltWaitingISM->ClearInstances();
	
	const float Tile = Driver ? Driver->GetTileSize() : 100.f;
	const float GridYaw = Driver ? Driver->GetGridYaw() : 0.f;
	const EHexOrientation Ori = Driver ? Driver->GetGridOrientation() : EHexOrientation::PointyTop;
	const AActor* Anchor = GetAnchor();
	const FVector AnchorV = Anchor ? Anchor->GetActorLocation() : FVector::ZeroVector;
	
	int32 EmptyCount = 0;
	int32 ReadyCount = 0;
	int32 WaitingCount = 0;

	auto ForwardYaw = [&](const FHex& Base, uint8 Dir)->float
		{
			const FHex Fwd = HexUtil::Neighbor(Base, (EHexDir)(Dir % 6));
			const FVector2D Axy = HexCoord::Rotate2D(HexCoord::AxialToXY(FHexAxial(Base.Q, Base.R), Tile, Ori), GridYaw);
			const FVector2D Bxy = HexCoord::Rotate2D(HexCoord::AxialToXY(FHexAxial(Fwd.Q, Fwd.R), Tile, Ori), GridYaw);
			const FVector2D Delta = Bxy - Axy;
			return Delta.IsNearlyZero() ? 0.f : FMath::RadiansToDegrees(FMath::Atan2(Delta.Y, Delta.X));
		};

	enum class EBeltState : uint8 { Empty, Ready, Waiting };

	// Helper function to check belt state
	auto GetBeltState = [&](const FHex& BeltHex, uint8 BeltDir) -> EBeltState
		{
			// Check if there's an item on this belt
			const FItem* BeltItem = Sim->ItemMap.Find(BeltHex);
			if (!BeltItem || BeltItem->Id == 0)
			{
				// No item on belt - belt is empty
				return EBeltState::Empty;
			}

			// There's an item on the belt - check if it can move forward
			const FHex DestHex = HexUtil::Neighbor(BeltHex, (EHexDir)(BeltDir % 6));
			
			// Check if destination is occupied
			if (Sim->ItemMap.Contains(DestHex))
			{
				// Destination is occupied - belt is waiting
				return EBeltState::Waiting;
			}

			// Check if destination is blocked by held items
			for (const FArmState& Arm : Sim->Arms)
			{
				if (!Arm.bHolding) continue;

				// Calculate arm grip position
				FHex GripHex = Arm.BaseHex;
				for (int i = 0; i < Arm.ArmLen; ++i)
					GripHex = HexUtil::Neighbor(GripHex, Arm.Facing);

				if (GripHex == DestHex)
				{
					// Arm is holding an item at the destination - belt is waiting
					return EBeltState::Waiting;
				}

				// Check for cluster items
				if (Arm.Held.bIsCluster)
				{
					for (const FOrbData& Sub : Arm.Held.Orbs)
					{
						FHex Local(Sub.dQ, Sub.dR);
						// Apply rotation based on arm facing and grab facing
						const uint8 cur = (uint8)Arm.Facing % 6;
						const int8 grab = (int8)Arm.Held.GrabFacing;
						const uint8 facingDelta = (grab >= 0)
							? (uint8)(((int32)cur - (int32)(grab % 6) + 6) % 6)
							: cur;
						const uint8 ccw = (uint8)((6 - facingDelta) % 6);

						// Rotate the local offset
						for (uint8 i = 0; i < (ccw % 6); ++i)
						{
							int32 q = Local.Q;
							int32 r = Local.R;
							Local.Q = -r;
							Local.R = q + r;
						}

						FHex WorldPos(GripHex.Q + Local.Q, GripHex.R + Local.R);
						if (WorldPos == DestHex)
						{
							// Part of held cluster is at destination - belt is waiting
							return EBeltState::Waiting;
						}
					}
				}
			}

			// Destination is clear - belt is ready to move
			return EBeltState::Ready;
		};

	for (const auto& KV : Sim->TileMap)
	{
		const FHex& H = KV.Key;
		const FTile& T = KV.Value;
		if (T.Kind != ETileKind::Belt) continue;

		const FVector2D LocalXY = HexCoord::AxialToXY(FHexAxial(H.Q, H.R), Tile, Ori);
		const FVector2D RotXY = HexCoord::Rotate2D(LocalXY, GridYaw);
		const FVector Loc = AnchorV + FVector(RotXY.X, RotXY.Y, BeltZOffset);
		const float Yaw = ForwardYaw(H, (uint8)(T.Arg & 0xFF));

		const uint8 BeltDir = (uint8)(T.Arg & 0xFF);
		EBeltState State = GetBeltState(H, BeltDir);
		
		const FTransform Transform(FRotator(0.f, Yaw, 0.f), Loc, BeltScale);
		
		switch (State)
		{
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

void AHexFactoryActor::RebuildBonds()
{
	if (!Sim) 
	{
		UE_LOG(LogTemp, Warning, TEXT("HexFactoryActor::RebuildBonds: Sim is null"));
		return;
	}
	
	const float Tile = Driver ? Driver->GetTileSize() : 100.f;
	const float GridYaw = Driver ? Driver->GetGridYaw() : 0.f;
	const EHexOrientation Ori = Driver ? Driver->GetGridOrientation() : EHexOrientation::PointyTop;
	const AActor* Anchor = GetAnchor();
	const FVector AnchorV = Anchor ? Anchor->GetActorLocation() : FVector::ZeroVector;
	
	int32 BondCount = 0;

	for (const auto& KV : Sim->TileMap)
	{
		const FHex& H = KV.Key;
		const FTile& T = KV.Value;
		if (T.Kind != ETileKind::Bond && T.Kind != ETileKind::Unbond) continue;

		BondCount++;
		const uint8 Dir = (uint8)(T.Arg & 0xFF);
		const FHex N = HexUtil::Neighbor(H, (EHexDir)(Dir % 6));

		// World positions of each hex center
		const FVector2D AxyLocal = HexCoord::AxialToXY(FHexAxial(H.Q, H.R), Tile, Ori);
		const FVector2D BxyLocal = HexCoord::AxialToXY(FHexAxial(N.Q, N.R), Tile, Ori);
		const FVector2D Axy = HexCoord::Rotate2D(AxyLocal, GridYaw);
		const FVector2D Bxy = HexCoord::Rotate2D(BxyLocal, GridYaw);
		const FVector WorldA = AnchorV + FVector(Axy.X, Axy.Y, T.Kind == ETileKind::Bond ? BondZOffset : UnbondZOffset);
		const FVector WorldB = AnchorV + FVector(Bxy.X, Bxy.Y, T.Kind == ETileKind::Bond ? BondZOffset : UnbondZOffset);
		const FVector Mid = (WorldA + WorldB) * 0.5f;
		const FVector2D Delta2D(Bxy - Axy);
		const float Yaw = Delta2D.IsNearlyZero()
			? 0.f
			: FMath::RadiansToDegrees(FMath::Atan2(Delta2D.Y, Delta2D.X));

		if (T.Kind == ETileKind::Bond)
		{
			if (BondMesh)
			{
				BondISM->AddInstance(FTransform(FRotator(0.f, Yaw, 0.f), Mid, BondScale));
				if (bDrawBondDebugArrows)
					DrawBondArrow(WorldA, WorldB);
			}
			else
			{
				UE_LOG(LogTemp, Warning, TEXT("HexFactoryActor::RebuildBonds: BondMesh is null"));
			}
		}
		else if (T.Kind == ETileKind::Unbond)
		{
			// Use UnbondMesh if available, otherwise fallback to BondMesh
			UStaticMesh* MeshToUse = UnbondMesh ? UnbondMesh : BondMesh;
			if (MeshToUse)
			{
				UnbondISM->AddInstance(FTransform(FRotator(0.f, Yaw, 0.f), Mid, UnbondScale));
				if (bDrawBondDebugArrows)
					DrawBondArrow(WorldA, WorldB);
			}
			else
			{
				UE_LOG(LogTemp, Warning, TEXT("HexFactoryActor::RebuildBonds: Neither UnbondMesh nor BondMesh is available"));
			}
		}
	}
	
	UE_LOG(LogTemp, Log, TEXT("HexFactoryActor::RebuildBonds: Found %d bonds/unbonds, BondMesh=%s, BondISM=%s"), 
		BondCount, BondMesh ? TEXT("exists") : TEXT("null"), BondISM ? TEXT("exists") : TEXT("null"));
}

void AHexFactoryActor::RebuildOutputs()
{
	if (!Sim) 
	{
		UE_LOG(LogTemp, Warning, TEXT("HexFactoryActor::RebuildOutputs: Sim is null"));
		return;
	}
	
	// Get material manager
	UOrbMaterialManager* MaterialManager = UOrbMaterialManager::FindInWorld(this);
	if (!MaterialManager)
	{
		UE_LOG(LogTemp, Warning, TEXT("HexFactoryActor::RebuildOutputs: No OrbMaterialManager found in world. Please add an OrbMaterialManagerActor to the level."));
		return;
	}
	
	// Note: Debug logging removed for performance
	
	// Get the tile mesh from GameManager instead of orb mesh
	UStaticMesh* BaseTileMesh = nullptr;
	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<AGameManager> It(World); It; ++It)
		{
			AGameManager* GameManager = *It;
			if (GameManager && GameManager->TileMesh)
			{
				BaseTileMesh = GameManager->TileMesh;
				// Note: Debug logging removed for performance
				break;
			}
		}
	}
	
	if (!BaseTileMesh)
	{
		UE_LOG(LogTemp, Warning, TEXT("HexFactoryActor::RebuildOutputs: No tile mesh found in GameManager. Please assign TileMesh in the GameManager."));
		// Fallback to local tile mesh
		if (TileMesh)
		{
			BaseTileMesh = TileMesh;
			// Note: Debug logging removed for performance
		}
		else
		{
			UE_LOG(LogTemp, Error, TEXT("HexFactoryActor::RebuildOutputs: No tile mesh available"));
			return;
		}
	}
	
	// UE_LOG(LogTemp, Log, TEXT("HexFactoryActor::RebuildOutputs: Using tile mesh for outputs"));
	
	const float Tile = Driver ? Driver->GetTileSize() : 100.f;
	const float GridYaw = Driver ? Driver->GetGridYaw() : 0.f;
	const EHexOrientation Ori = Driver ? Driver->GetGridOrientation() : EHexOrientation::PointyTop;
	const AActor* Anchor = GetAnchor();
	const FVector AnchorV = Anchor ? Anchor->GetActorLocation() : FVector::ZeroVector;
	
	// Set up all output ISM components with the tile mesh and type-specific materials
	TArray<EOrbType> AllOrbTypes = {
		EOrbType::None,
		EOrbType::BaseA,
		EOrbType::BaseB,
		EOrbType::BaseC,
		EOrbType::MetalA,
		EOrbType::CatalystX,
		EOrbType::Void
	};
	
	for (EOrbType OrbType : AllOrbTypes)
	{
		UInstancedStaticMeshComponent* OutputISM = GetOutputISMForType(OrbType);
		if (OutputISM)
		{
			// Set the tile mesh
			OutputISM->SetStaticMesh(BaseTileMesh);
			
			// Get the material for this orb type
			UMaterialInterface* TypeMaterial = MaterialManager->GetMaterialForOrbType(OrbType);
			if (TypeMaterial)
			{
				// Create a dynamic material instance for half opacity
				UMaterialInstanceDynamic* OutputMID = UMaterialInstanceDynamic::Create(TypeMaterial, this);
				if (OutputMID)
				{
					// Set half opacity (0.5 alpha)
					OutputMID->SetScalarParameterValue(FName("Opacity"), 0.5f);
					OutputISM->SetMaterial(0, OutputMID);
				}
				else
				{
					OutputISM->SetMaterial(0, TypeMaterial);
				}
			}
			
			// Clear instances for this type
			OutputISM->ClearInstances();
			
			// UE_LOG(LogTemp, Log, TEXT("HexFactoryActor::RebuildOutputs: Set up ISM for type %d with mesh %s and material %s"), 
			// 	(int32)OrbType, 
			// 	BaseTileMesh ? TEXT("valid") : TEXT("null"),
			// 	TypeMaterial ? TEXT("valid") : TEXT("null"));
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("HexFactoryActor::RebuildOutputs: No ISM found for type %d"), (int32)OrbType);
		}
	}
	
	// Get ClusterOutput scale from GameManager
	float ClusterScale = 1.3f; // Default
	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<AGameManager> It(World); It; ++It)
		{
			ClusterScale = (*It)->ClusterOutputScale;
			break;
		}
	}
	
	int32 OutputCount = 0;
	
	// Render all output tiles (both regular Output and ClusterOutput)
	for (const auto& KV : Sim->TileMap)
	{
		const FHex& H = KV.Key;
		const FTile& T = KV.Value;
		
		// Handle both Output and ClusterOutput tiles
		if (T.Kind == ETileKind::Output || T.Kind == ETileKind::ClusterOutput)
		{
			OutputCount++;
			const FVector2D LocalXY = HexCoord::AxialToXY(FHexAxial(H.Q, H.R), Tile, Ori);
			const FVector2D RotXY = HexCoord::Rotate2D(LocalXY, GridYaw);
			const FVector Loc = AnchorV + FVector(RotXY.X, RotXY.Y, 0.f);
			
			// ClusterOutput tiles are scaled larger than regular Output tiles
			float Scale = (T.Kind == ETileKind::ClusterOutput) ? ClusterScale : 1.0f;
			const FTransform Transform(FRotator::ZeroRotator, Loc, FVector(Scale));

			// Get orb type - for Output it's in Arg, for ClusterOutput it's also in Arg
			EOrbType OutputType = (EOrbType)T.Arg;

			// Get the appropriate ISM for this output type
			UInstancedStaticMeshComponent* OutputISM = GetOutputISMForType(OutputType);
			
			if (OutputISM)
			{
				OutputISM->AddInstance(Transform);
			}
			else
			{
				UE_LOG(LogTemp, Warning, TEXT("HexFactoryActor::RebuildOutputs: No ISM found for output type %d at (%d,%d)"), 
					(int32)OutputType, H.Q, H.R);
			}
		}
	}
	
	UE_LOG(LogTemp, Log, TEXT("HexFactoryActor::RebuildOutputs: Rendered %d output tiles"), OutputCount);
}

UInstancedStaticMeshComponent* AHexFactoryActor::GetOutputISMForType(EOrbType OrbType)
{
	switch (OrbType)
	{
	case EOrbType::None: return OutputNoneISM;
	case EOrbType::BaseA: return OutputBaseAISM;
	case EOrbType::BaseB: return OutputBaseBISM;
	case EOrbType::BaseC: return OutputBaseCISM;
	case EOrbType::MetalA: return OutputMetalAISM;
	case EOrbType::CatalystX: return OutputCatalystXISM;
	case EOrbType::Void: return OutputVoidISM;
	default: return OutputBaseAISM; // Fallback
	}
}

UInstancedStaticMeshComponent* AHexFactoryActor::GetInputISMForType(EOrbType OrbType)
{
	switch (OrbType)
	{
	case EOrbType::None: return InputNoneISM;
	case EOrbType::BaseA: return InputBaseAISM;
	case EOrbType::BaseB: return InputBaseBISM;
	case EOrbType::BaseC: return InputBaseCISM;
	case EOrbType::MetalA: return InputMetalAISM;
	case EOrbType::CatalystX: return InputCatalystXISM;
	case EOrbType::Void: return InputVoidISM;
	default: return InputBaseAISM; // Fallback
	}
}

void AHexFactoryActor::GenerateMergedHexMesh(const FVector& Hex1Center, const FVector& Hex2Center, float HexRadius,
	UMaterialInterface* Mat1, UMaterialInterface* Mat2, int32 SectionIndex)
{
	if (!MergedOutputMesh) return;
	
	// Generate vertices for a merged hex shape
	// For pointy-top hexagons, we need 6 vertices per hex plus shared vertices
	TArray<FVector> Vertices;
	TArray<int32> Triangles;
	TArray<FVector> Normals;
	TArray<FVector2D> UV0;
	TArray<FColor> VertexColors;
	TArray<FProcMeshTangent> Tangents;
	

	
	// Generate Hex1 vertices
	for (int32 i = 0; i < 6; i++)
	{
		float Angle = i * UE_PI / 3.0f;
		FVector LocalPos = FVector(FMath::Cos(Angle) * HexRadius, FMath::Sin(Angle) * HexRadius, 0);
		Vertices.Add(Hex1Center + LocalPos);
		Normals.Add(FVector::UpVector);
		UV0.Add(FVector2D(0.25f, 0.5f)); // UV for first material
		VertexColors.Add(FColor::White);
		Tangents.Add(FProcMeshTangent(1, 0, 0));
	}
	
	// Generate Hex2 vertices
	for (int32 i = 0; i < 6; i++)
	{
		float Angle = i * UE_PI / 3.0f;
		FVector LocalPos = FVector(FMath::Cos(Angle) * HexRadius, FMath::Sin(Angle) * HexRadius, 0);
		Vertices.Add(Hex2Center + LocalPos);
		Normals.Add(FVector::UpVector);
		UV0.Add(FVector2D(0.75f, 0.5f)); // UV for second material
		VertexColors.Add(FColor::White);
		Tangents.Add(FProcMeshTangent(1, 0, 0));
	}
	
	// Generate triangles for Hex1 (fan triangulation from center)
	// REVERSED winding order for correct facing direction (counter-clockwise when viewed from above)
	FVector Hex1CenterVert = Hex1Center;
	int32 Hex1CenterIdx = Vertices.Add(Hex1CenterVert);
	Normals.Add(FVector::UpVector);
	UV0.Add(FVector2D(0.25f, 0.5f));
	VertexColors.Add(FColor::White);
	Tangents.Add(FProcMeshTangent(1, 0, 0));
	
	for (int32 i = 0; i < 6; i++)
	{
		int32 Next = (i + 1) % 6;
		// Reversed winding: center -> next -> current (instead of center -> current -> next)
		Triangles.Add(Hex1CenterIdx);
		Triangles.Add(Next);
		Triangles.Add(i);
	}
	
	// Generate triangles for Hex2 (fan triangulation from center)
	// REVERSED winding order for correct facing direction
	FVector Hex2CenterVert = Hex2Center;
	int32 Hex2CenterIdx = Vertices.Add(Hex2CenterVert);
	Normals.Add(FVector::UpVector);
	UV0.Add(FVector2D(0.75f, 0.5f));
	VertexColors.Add(FColor::White);
	Tangents.Add(FProcMeshTangent(1, 0, 0));
	
	for (int32 i = 0; i < 6; i++)
	{
		int32 Next = (i + 1) % 6;
		// Reversed winding: center -> next -> current
		Triangles.Add(Hex2CenterIdx);
		Triangles.Add(6 + Next);
		Triangles.Add(6 + i);
	}
	
	// Find the shared edge vertices and create bridging triangles
	// The shared edge is where the two hexes touch
	// For adjacent hexes, we need to find which vertices are closest
	float MinDist1 = FLT_MAX, MinDist2 = FLT_MAX;
	int32 SharedIdx1_A = -1, SharedIdx1_B = -1;
	int32 SharedIdx2_A = -1, SharedIdx2_B = -1;
	
	// Find the two closest vertex pairs between the hexes
	for (int32 i = 0; i < 6; i++)
	{
		for (int32 j = 0; j < 6; j++)
		{
			float Dist = FVector::Dist(Vertices[i], Vertices[6 + j]);
			if (Dist < MinDist1)
			{
				MinDist2 = MinDist1;
				SharedIdx2_A = SharedIdx1_A;
				SharedIdx2_B = SharedIdx1_B;
				
				MinDist1 = Dist;
				SharedIdx1_A = i;
				SharedIdx1_B = 6 + j;
			}
			else if (Dist < MinDist2)
			{
				MinDist2 = Dist;
				SharedIdx2_A = i;
				SharedIdx2_B = 6 + j;
			}
		}
	}
	
	// Create bridging quads between the shared edges
	// REVERSED winding order for correct facing direction
	if (SharedIdx1_A >= 0 && SharedIdx2_A >= 0)
	{
		// Bridge quad (two triangles) - reversed winding
		Triangles.Add(SharedIdx1_A);
		Triangles.Add(SharedIdx2_A);
		Triangles.Add(SharedIdx1_B);
		
		Triangles.Add(SharedIdx2_A);
		Triangles.Add(SharedIdx2_B);
		Triangles.Add(SharedIdx1_B);
	}
	
	// Create the mesh section
	MergedOutputMesh->CreateMeshSection(SectionIndex, Vertices, Triangles, Normals, UV0, VertexColors, Tangents, true);
	
	// Set materials (for now, use Mat1 for the whole mesh - can be enhanced to use both)
	if (Mat1)
	{
		MergedOutputMesh->SetMaterial(SectionIndex, Mat1);
	}
}

void AHexFactoryActor::GenerateHexWithHole(const FVector& Center, float HexRadius, float HoleRadius,
	UMaterialInterface* Material, int32 SectionIndex)
{
	if (!InputTilesMesh) return;
	
	TArray<FVector> Vertices;
	TArray<int32> Triangles;
	TArray<FVector> Normals;
	TArray<FVector2D> UV0;
	TArray<FColor> VertexColors;
	TArray<FProcMeshTangent> Tangents;
	
	const int32 CircleSegments = 32; // Number of segments for the circular hole
	
	// Generate outer hexagon vertices (6 vertices)
	for (int32 i = 0; i < 6; i++)
	{
		float Angle = i * UE_PI / 3.0f;
		FVector LocalPos = FVector(FMath::Cos(Angle) * HexRadius, FMath::Sin(Angle) * HexRadius, 0);
		Vertices.Add(Center + LocalPos);
		Normals.Add(FVector::UpVector);
		// UV mapping: outer edge of hex
		FVector2D UV = FVector2D(0.5f + 0.5f * FMath::Cos(Angle), 0.5f + 0.5f * FMath::Sin(Angle));
		UV0.Add(UV);
		VertexColors.Add(FColor::White);
		Tangents.Add(FProcMeshTangent(1, 0, 0));
	}
	
	// Generate inner circle vertices (for the hole) - reverse order for proper winding
	for (int32 i = 0; i < CircleSegments; i++)
	{
		// Reverse order for proper winding (counter-clockwise when viewed from above)
		float Angle = (CircleSegments - 1 - i) * 2.0f * UE_PI / CircleSegments;
		FVector LocalPos = FVector(FMath::Cos(Angle) * HoleRadius, FMath::Sin(Angle) * HoleRadius, 0);
		Vertices.Add(Center + LocalPos);
		Normals.Add(FVector::UpVector);
		// UV mapping: inner edge of circle
		FVector2D UV = FVector2D(0.5f + (HoleRadius / HexRadius) * 0.5f * FMath::Cos(Angle), 
		                          0.5f + (HoleRadius / HexRadius) * 0.5f * FMath::Sin(Angle));
		UV0.Add(UV);
		VertexColors.Add(FColor::White);
		Tangents.Add(FProcMeshTangent(1, 0, 0));
	}
	
	// Create triangles connecting hex vertices to circle vertices
	// Form a ring by connecting each hex edge to the corresponding circle arc
	for (int32 HexIdx = 0; HexIdx < 6; HexIdx++)
	{
		int32 NextHexIdx = (HexIdx + 1) % 6;
		int32 HexVertexIdx = HexIdx;
		int32 NextHexVertexIdx = NextHexIdx;
		
		// Calculate which circle segments correspond to this hex edge
		// Each hex edge spans 60 degrees (UE_PI/3), divide circle segments equally
		int32 SegmentsPerEdge = CircleSegments / 6;
		int32 StartCircleIdx = HexIdx * SegmentsPerEdge;
		
		// Connect hex edge to circle arc with triangles
		for (int32 i = 0; i < SegmentsPerEdge; i++)
		{
			int32 CircleIdx1 = 6 + ((StartCircleIdx + i) % CircleSegments);
			int32 CircleIdx2 = 6 + ((StartCircleIdx + i + 1) % CircleSegments);
			
			// Create two triangles forming a quad between hex edge and circle arc
			// For correct front-facing (viewed from above), use counter-clockwise winding
			// Triangle 1: hex vertex -> circle segment 1 -> next hex vertex
			Triangles.Add(HexVertexIdx);
			Triangles.Add(CircleIdx1);
			Triangles.Add(NextHexVertexIdx);
			
			// Triangle 2: next hex vertex -> circle segment 1 -> circle segment 2
			Triangles.Add(NextHexVertexIdx);
			Triangles.Add(CircleIdx1);
			Triangles.Add(CircleIdx2);
		}
	}
	
	// Create the mesh section
	InputTilesMesh->CreateMeshSection(SectionIndex, Vertices, Triangles, Normals, UV0, VertexColors, Tangents, true);
	
	// Set material
	if (Material)
	{
		InputTilesMesh->SetMaterial(SectionIndex, Material);
	}
}

void AHexFactoryActor::RebuildInputTiles()
{
	if (!Sim) return;
	
	// Get material manager
	UOrbMaterialManager* MaterialManager = UOrbMaterialManager::FindInWorld(this);
	if (!MaterialManager)
	{
		UE_LOG(LogTemp, Warning, TEXT("HexFactoryActor::RebuildInputTiles: No OrbMaterialManager found in world."));
		return;
	}
	
	// Get the custom input tile mesh from GameManager, or fallback to regular tile mesh
	UStaticMesh* BaseTileMesh = nullptr;
	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<AGameManager> It(World); It; ++It)
		{
			AGameManager* GameManager = *It;
			if (GameManager)
			{
				// Prefer CustomInputTileMesh if available, otherwise use TileMesh
				if (GameManager->CustomInputTileMesh)
				{
					BaseTileMesh = GameManager->CustomInputTileMesh;
				}
				else if (GameManager->TileMesh)
				{
					BaseTileMesh = GameManager->TileMesh;
				}
				break;
			}
		}
	}
	
	if (!BaseTileMesh)
	{
		// Fallback to local tile mesh
		if (TileMesh)
		{
			BaseTileMesh = TileMesh;
		}
		else
		{
			UE_LOG(LogTemp, Error, TEXT("HexFactoryActor::RebuildInputTiles: No tile mesh available"));
			return;
		}
	}
	
	const float Tile = Driver ? Driver->GetTileSize() : 100.f;
	const float GridYaw = Driver ? Driver->GetGridYaw() : 0.f;
	const EHexOrientation Ori = Driver ? Driver->GetGridOrientation() : EHexOrientation::PointyTop;
	const AActor* Anchor = GetAnchor();
	const FVector AnchorV = Anchor ? Anchor->GetActorLocation() : FVector::ZeroVector;
	
	// Set up all input ISM components with the custom input tile mesh and type-specific materials
	TArray<EOrbType> AllOrbTypes = {
		EOrbType::None,
		EOrbType::BaseA,
		EOrbType::BaseB,
		EOrbType::BaseC,
		EOrbType::MetalA,
		EOrbType::CatalystX,
		EOrbType::Void
	};
	
	for (EOrbType OrbType : AllOrbTypes)
	{
		UInstancedStaticMeshComponent* InputISM = GetInputISMForType(OrbType);
		if (InputISM)
		{
			// Set the custom input tile mesh
			InputISM->SetStaticMesh(BaseTileMesh);
			
			// Get the material for this orb type
			UMaterialInterface* TypeMaterial = MaterialManager->GetMaterialForOrbType(OrbType);
			if (TypeMaterial)
			{
				// Create a dynamic material instance for half opacity (like Output tiles)
				UMaterialInstanceDynamic* InputMID = UMaterialInstanceDynamic::Create(TypeMaterial, this);
				if (InputMID)
				{
					// Set half opacity (0.5 alpha) to match Output tiles
					InputMID->SetScalarParameterValue(FName("Opacity"), 0.5f);
					InputISM->SetMaterial(0, InputMID);
				}
				else
				{
					InputISM->SetMaterial(0, TypeMaterial);
				}
			}
			
			// Clear instances for this type
			InputISM->ClearInstances();
		}
	}
	
	// Collect all Input and ClusterInput tiles and add them to the appropriate ISM
	for (const auto& KV : Sim->TileMap)
	{
		const FHex& H = KV.Key;
		const FTile& T = KV.Value;
		
		if (T.Kind == ETileKind::Input || T.Kind == ETileKind::ClusterInput)
		{
			const FVector2D LocalXY = HexCoord::AxialToXY(FHexAxial(H.Q, H.R), Tile, Ori);
			const FVector2D RotXY = HexCoord::Rotate2D(LocalXY, GridYaw);
			const FVector Loc = AnchorV + FVector(RotXY.X, RotXY.Y, 0.f);
			
			// Get orb type from tile Arg (same for Input and ClusterInput, like ClusterOutput)
			EOrbType OrbType = (EOrbType)T.Arg;
			
			// Add instance to the appropriate ISM for this orb type
			UInstancedStaticMeshComponent* InputISM = GetInputISMForType(OrbType);
			if (InputISM)
			{
				InputISM->AddInstance(FTransform(FRotator::ZeroRotator, Loc, FVector(1.f)));
			}
		}
	}
}

