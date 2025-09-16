#include "Actors/HexFactoryActor.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Actors/OrbSimDriver.h"
#include "Actors/HexBoardActor.h"
#include "Sim/SimWorldHex.h"
#include "Sim/HexCoord.h"
#include "Sim/OrbTypes.h"
#include "Data/OrbMaterialManager.h"
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
	TileISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	InputTileISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("InputTileISM"));
	InputTileISM->SetupAttachment(Root);
	InputTileISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	OutputTileISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("OutputTileISM"));
	OutputTileISM->SetupAttachment(Root);
	OutputTileISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	BeltTileISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("BeltTileISM"));
	BeltTileISM->SetupAttachment(Root);
	BeltTileISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	BondTileISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("BondTileISM"));
	BondTileISM->SetupAttachment(Root);
	BondTileISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	BeltISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("BeltISM"));
	BeltISM->SetupAttachment(Root);
	BeltISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	BondISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("BondISM"));
	BondISM->SetupAttachment(Root);
	BondISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	// Create type-specific output ISM components
	OutputNoneISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("OutputNoneISM"));
	OutputNoneISM->SetupAttachment(Root);
	OutputNoneISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	OutputBaseAISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("OutputBaseAISM"));
	OutputBaseAISM->SetupAttachment(Root);
	OutputBaseAISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	OutputBaseBISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("OutputBaseBISM"));
	OutputBaseBISM->SetupAttachment(Root);
	OutputBaseBISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	OutputBaseCISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("OutputBaseCISM"));
	OutputBaseCISM->SetupAttachment(Root);
	OutputBaseCISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	OutputMetalAISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("OutputMetalAISM"));
	OutputMetalAISM->SetupAttachment(Root);
	OutputMetalAISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	OutputCatalystXISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("OutputCatalystXISM"));
	OutputCatalystXISM->SetupAttachment(Root);
	OutputCatalystXISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
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
				Comp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			}
			Comp->SetVisibility(true, true);
		};
	EnsureISM(TileISM, TEXT("TileISM_Runtime"));
	EnsureISM(InputTileISM, TEXT("InputTileISM_Runtime"));
	EnsureISM(OutputTileISM, TEXT("OutputTileISM_Runtime"));
	EnsureISM(BeltTileISM, TEXT("BeltTileISM_Runtime"));
	EnsureISM(BondTileISM, TEXT("BondTileISM_Runtime"));
	EnsureISM(BeltISM, TEXT("BeltISM_Runtime"));
	EnsureISM(BondISM, TEXT("BondISM_Runtime"));
	
	// Ensure type-specific output ISM components
	EnsureISM(OutputNoneISM, TEXT("OutputNoneISM_Runtime"));
	EnsureISM(OutputBaseAISM, TEXT("OutputBaseAISM_Runtime"));
	EnsureISM(OutputBaseBISM, TEXT("OutputBaseBISM_Runtime"));
	EnsureISM(OutputBaseCISM, TEXT("OutputBaseCISM_Runtime"));
	EnsureISM(OutputMetalAISM, TEXT("OutputMetalAISM_Runtime"));
	EnsureISM(OutputCatalystXISM, TEXT("OutputCatalystXISM_Runtime"));
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

void AHexFactoryActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!Sim) return;

	const bool bBoardChanged = (Board && Board->BoardRevision != LastSeenBoardRevision);
	if (!bAutoRefresh && !bBoardChanged) return;

	if (Sim->TileRevision != LastSeenRevision || bBoardChanged)
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
	UE_LOG(LogTemp, Log, TEXT("HexFactoryActor::RebuildInternal: Starting rebuild"));
	
	EnsureComponents();
	
	// Reacquire Sim reference in case it was created after our BeginPlay
	if (!Sim && Driver)
	{
		Sim = Driver->GetSim();
		UE_LOG(LogTemp, Log, TEXT("HexFactoryActor::RebuildInternal: Reacquired Sim=%s"), Sim ? TEXT("found") : TEXT("null"));
	}
	
	if (!Sim) 
	{
		UE_LOG(LogTemp, Warning, TEXT("HexFactoryActor::RebuildInternal: Sim is null after reacquisition attempt"));
		return;
	}

	LastSeenRevision = Sim->TileRevision;
	LastSeenBoardRevision = Board ? Board->BoardRevision : -1;

	// Set up tile ISM components
	if (TileMesh) 
	{
		TileISM->SetStaticMesh(TileMesh);
		InputTileISM->SetStaticMesh(TileMesh);
		OutputTileISM->SetStaticMesh(TileMesh);
		BeltTileISM->SetStaticMesh(TileMesh);
		BondTileISM->SetStaticMesh(TileMesh);
	}
	
	if (TileMaterial) TileISM->SetMaterial(0, TileMaterial);
	if (InputTileMaterial) InputTileISM->SetMaterial(0, InputTileMaterial);
	if (OutputTileMaterial) OutputTileISM->SetMaterial(0, OutputTileMaterial);
	if (BeltTileMaterial) BeltTileISM->SetMaterial(0, BeltTileMaterial);
	if (BondTileMaterial) BondTileISM->SetMaterial(0, BondTileMaterial);

	if (BeltMesh) BeltISM->SetStaticMesh(BeltMesh);
	if (BeltMaterial) BeltISM->SetMaterial(0, BeltMaterial);
	if (BondMesh) BondISM->SetStaticMesh(BondMesh);
	if (BondMaterial) BondISM->SetMaterial(0, BondMaterial);

	// Clear all instances
	TileISM->ClearInstances();
	InputTileISM->ClearInstances();
	OutputTileISM->ClearInstances();
	BeltTileISM->ClearInstances();
	BondTileISM->ClearInstances();
	BeltISM->ClearInstances();
	BondISM->ClearInstances();

	RebuildTiles();
	RebuildBelts();
	RebuildBonds();   // NEW
	RebuildOutputs(); // NEW: Type-specific output rendering
	
	UE_LOG(LogTemp, Log, TEXT("HexFactoryActor::RebuildInternal: Rebuild complete"));
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
			if (InputTileISM) InputTileISM->AddInstance(Transform);
			break;
		case ETileKind::Output:
			if (OutputTileISM) OutputTileISM->AddInstance(Transform);
			break;
		case ETileKind::Belt:
			if (BeltTileISM) BeltTileISM->AddInstance(Transform);
			break;
		case ETileKind::Bond:
			if (BondTileISM) BondTileISM->AddInstance(Transform);
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

	auto ForwardYaw = [&](const FHex& Base, uint8 Dir)->float
		{
			const FHex Fwd = HexUtil::Neighbor(Base, (EHexDir)(Dir % 6));
			const FVector2D Axy = HexCoord::Rotate2D(HexCoord::AxialToXY(FHexAxial(Base.Q, Base.R), Tile, Ori), GridYaw);
			const FVector2D Bxy = HexCoord::Rotate2D(HexCoord::AxialToXY(FHexAxial(Fwd.Q, Fwd.R), Tile, Ori), GridYaw);
			const FVector2D Delta = Bxy - Axy;
			return Delta.IsNearlyZero() ? 0.f : FMath::RadiansToDegrees(FMath::Atan2(Delta.Y, Delta.X));
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
			BeltISM->AddInstance(FTransform(FRotator(0.f, Yaw, 0.f), Loc, BeltScale));
			if (bAlwaysDrawBeltArrows)
				DrawBeltArrow(Loc, Tile, Yaw);
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("HexFactoryActor::RebuildBelts: BeltMesh is null"));
		}
	}
	
	UE_LOG(LogTemp, Log, TEXT("HexFactoryActor::RebuildBelts: Found %d belts, BeltMesh=%s, BeltISM=%s"), 
		BeltCount, BeltMesh ? TEXT("exists") : TEXT("null"), BeltISM ? TEXT("exists") : TEXT("null"));
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
		if (T.Kind != ETileKind::Bond) continue;

		BondCount++;
		const uint8 Dir = (uint8)(T.Arg & 0xFF);
		const FHex N = HexUtil::Neighbor(H, (EHexDir)(Dir % 6));

		// World positions of each hex center
		const FVector2D AxyLocal = HexCoord::AxialToXY(FHexAxial(H.Q, H.R), Tile, Ori);
		const FVector2D BxyLocal = HexCoord::AxialToXY(FHexAxial(N.Q, N.R), Tile, Ori);
		const FVector2D Axy = HexCoord::Rotate2D(AxyLocal, GridYaw);
		const FVector2D Bxy = HexCoord::Rotate2D(BxyLocal, GridYaw);
		const FVector WorldA = AnchorV + FVector(Axy.X, Axy.Y, BondZOffset);
		const FVector WorldB = AnchorV + FVector(Bxy.X, Bxy.Y, BondZOffset);
		const FVector Mid = (WorldA + WorldB) * 0.5f;
		const FVector2D Delta2D(Bxy - Axy);
		const float Yaw = Delta2D.IsNearlyZero()
			? 0.f
			: FMath::RadiansToDegrees(FMath::Atan2(Delta2D.Y, Delta2D.X));

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
	
	UE_LOG(LogTemp, Log, TEXT("HexFactoryActor::RebuildBonds: Found %d bonds, BondMesh=%s, BondISM=%s"), 
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
	
	UE_LOG(LogTemp, Log, TEXT("HexFactoryActor::RebuildOutputs: Found OrbMaterialManager: %s"), MaterialManager ? *MaterialManager->GetName() : TEXT("null"));
	
	// Get the base orb mesh from material manager
	UStaticMesh* BaseOrbMesh = MaterialManager->GetOrbMesh();
	if (!BaseOrbMesh)
	{
		UE_LOG(LogTemp, Warning, TEXT("HexFactoryActor::RebuildOutputs: No orb mesh found in material manager. Please assign OrbMesh in the OrbMaterialManager component."));
		return;
	}
	
	UE_LOG(LogTemp, Log, TEXT("HexFactoryActor::RebuildOutputs: Found material manager and orb mesh"));
	
	const float Tile = Driver ? Driver->GetTileSize() : 100.f;
	const float GridYaw = Driver ? Driver->GetGridYaw() : 0.f;
	const EHexOrientation Ori = Driver ? Driver->GetGridOrientation() : EHexOrientation::PointyTop;
	const AActor* Anchor = GetAnchor();
	const FVector AnchorV = Anchor ? Anchor->GetActorLocation() : FVector::ZeroVector;
	
	// Set up all output ISM components with the base orb mesh and type-specific materials
	TArray<EOrbType> AllOrbTypes = {
		EOrbType::None,
		EOrbType::BaseA,
		EOrbType::BaseB,
		EOrbType::BaseC,
		EOrbType::MetalA,
		EOrbType::CatalystX
	};
	
	for (EOrbType OrbType : AllOrbTypes)
	{
		UInstancedStaticMeshComponent* OutputISM = GetOutputISMForType(OrbType);
		if (OutputISM)
		{
			// Set the base orb mesh
			OutputISM->SetStaticMesh(BaseOrbMesh);
			
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
			
			UE_LOG(LogTemp, Log, TEXT("HexFactoryActor::RebuildOutputs: Set up ISM for type %d with mesh %s and material %s"), 
				(int32)OrbType, 
				BaseOrbMesh ? TEXT("valid") : TEXT("null"),
				TypeMaterial ? TEXT("valid") : TEXT("null"));
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("HexFactoryActor::RebuildOutputs: No ISM found for type %d"), (int32)OrbType);
		}
	}
	
	int32 OutputCount = 0;

	for (const auto& KV : Sim->TileMap)
	{
		const FHex& H = KV.Key;
		const FTile& T = KV.Value;
		if (T.Kind != ETileKind::Output) continue;

		OutputCount++;
		const FVector2D LocalXY = HexCoord::AxialToXY(FHexAxial(H.Q, H.R), Tile, Ori);
		const FVector2D RotXY = HexCoord::Rotate2D(LocalXY, GridYaw);
		const FVector Loc = AnchorV + FVector(RotXY.X, RotXY.Y, 0.f);
		const FTransform Transform(FRotator::ZeroRotator, Loc, FVector(1.f));

		// Get the appropriate ISM for this output type
		EOrbType OutputType = (EOrbType)T.Arg;
		UInstancedStaticMeshComponent* OutputISM = GetOutputISMForType(OutputType);
		
		if (OutputISM)
		{
			OutputISM->AddInstance(Transform);
			UE_LOG(LogTemp, Log, TEXT("HexFactoryActor::RebuildOutputs: Added output instance at (%d,%d) with type %d"), 
				H.Q, H.R, (int32)OutputType);
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("HexFactoryActor::RebuildOutputs: No ISM found for output type %d at (%d,%d)"), 
				(int32)OutputType, H.Q, H.R);
		}
	}
	
	UE_LOG(LogTemp, Log, TEXT("HexFactoryActor::RebuildOutputs: Found %d outputs"), OutputCount);
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
	default: return OutputBaseAISM; // Fallback
	}
}

