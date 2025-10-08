#include "Actors/HexFactoryActor.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SceneComponent.h"
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

	UnbondTileISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("UnbondTileISM"));
	UnbondTileISM->SetupAttachment(Root);
	UnbondTileISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	BeltISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("BeltISM"));
	BeltISM->SetupAttachment(Root);
	BeltISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	BeltReadyISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("BeltReadyISM"));
	BeltReadyISM->SetupAttachment(Root);
	BeltReadyISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	BeltWaitingISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("BeltWaitingISM"));
	BeltWaitingISM->SetupAttachment(Root);
	BeltWaitingISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	BondISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("BondISM"));
	BondISM->SetupAttachment(Root);
	BondISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	UnbondISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("UnbondISM"));
	UnbondISM->SetupAttachment(Root);
	UnbondISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);

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
		case ETileKind::Unbond:
			if (UnbondTileISM) UnbondTileISM->AddInstance(Transform); // Use separate unbond tile background
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
	
	UE_LOG(LogTemp, Log, TEXT("HexFactoryActor::RebuildOutputs: Using tile mesh for outputs"));
	
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
		EOrbType::CatalystX
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
			
			UE_LOG(LogTemp, Log, TEXT("HexFactoryActor::RebuildOutputs: Set up ISM for type %d with mesh %s and material %s"), 
				(int32)OrbType, 
				BaseTileMesh ? TEXT("valid") : TEXT("null"),
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

