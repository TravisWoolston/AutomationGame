#include "Framework/GameManager.h"
#include "Framework/GameTickManager.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Actors/BasicArm.h"
#include "Actors/OrbSimDriver.h"
#include "Sim/HexCoord.h"
#include "Sim/SimWorldHex.h"          // <-- ADDED for USimWorldHex / FArmState
#include "DrawDebugHelpers.h"

AGameManager::AGameManager()
{
	PrimaryActorTick.bCanEverTick = true; // needed for auto-recenter
}

void AGameManager::BeginPlay()
{
	Super::BeginPlay();
	AutoFindRefs();
	TryLoadInitial();
	EnsureISM();
	Rebuild();

	const FVector AnchorLoc = GridAnchor ? GridAnchor->GetActorLocation() : GetActorLocation();
	LastGridCenter = AnchorLoc;

	UE_LOG(LogTemp, Log, TEXT("GameManager::BeginPlay - TickManager=%s, OrbSimDriver=%s, Anchor=(%.1f,%.1f,%.1f), TileSize=%.1f, Radius=%d"),
		TickManager ? *TickManager->GetName() : TEXT("null"),
		OrbSimDriver ? *OrbSimDriver->GetName() : TEXT("null"),
		AnchorLoc.X, AnchorLoc.Y, AnchorLoc.Z, TileSizeUU, Radius);
}

void AGameManager::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!bAutoExpandAtRuntime) return;

	const FVector AnchorLoc = GridAnchor ? GridAnchor->GetActorLocation() : GetActorLocation();
	const float Dist = FVector::Dist2D(LastGridCenter, AnchorLoc);
	if (Dist > RecenterThresholdUU)
	{
		UE_LOG(LogTemp, Verbose, TEXT("GameManager::Tick - recentering grid (%.1f uu)"), Dist);
		Rebuild();
		LastGridCenter = AnchorLoc;
	}
}

#if WITH_EDITOR
void AGameManager::OnConstruction(const FTransform&)
{
	EnsureISM();
	Rebuild();
}
#endif

void AGameManager::AutoFindRefs()
{
	UWorld* W = GetWorld(); if (!W) return;

	if (!TickManager)
		for (TActorIterator<AGameTickManager> It(W); It; ++It) { TickManager = *It; break; }

	if (!OrbSimDriver)
		for (TActorIterator<AOrbSimDriver> It(W); It; ++It) { OrbSimDriver = *It; break; }  // <-- typed

	if (!OrbSimDriver && OrbSimDriverClass)
	{
		FActorSpawnParameters S; S.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		OrbSimDriver = W->SpawnActor<AOrbSimDriver>(OrbSimDriverClass, GetActorTransform(), S); // <-- typed
	}
	
	// Sync OrbSimDriver parameters with GameManager (GameManager is the source of truth)
	SyncOrbSimDriverParameters();
}

void AGameManager::SetGridRadius(int32 NewRadius)
{
	Radius = FMath::Max(1, NewRadius);
	UE_LOG(LogTemp, Log, TEXT("GameManager::SetGridRadius - Radius=%d"), Radius);
	Rebuild();
}

void AGameManager::SetTileMeshAndMaterial(UStaticMesh* Mesh, UMaterialInterface* Mat)
{
	TileMesh = Mesh;
	TileMaterial = Mat;
	UE_LOG(LogTemp, Log, TEXT("GameManager::SetTileMeshAndMaterial - Mesh=%s, Mat=%s"),
		TileMesh ? *TileMesh->GetName() : TEXT("null"),
		TileMaterial ? *TileMaterial->GetName() : TEXT("null"));
	Rebuild();
}

void AGameManager::TryLoadInitial()
{
	if (InitialMapToLoad.IsEmpty() || !OrbSimDriver) return;
	const FString Cmd = FString::Printf(TEXT("LoadMap %s"), *InitialMapToLoad);
	if (!OrbSimDriver->CallFunctionByNameWithArguments(*Cmd, *GLog, nullptr, true))
	{
		UE_LOG(LogTemp, Warning, TEXT("GameManager: OrbSimDriver didn't accept Exec 'LoadMap'."));
	}
	else
	{
		UE_LOG(LogTemp, Log, TEXT("GameManager::TryLoadInitial - Requested map '%s'"), *InitialMapToLoad);
	}
}

void AGameManager::EnsureISM()
{
	if (!ISM)
	{
		ISM = FindComponentByClass<UInstancedStaticMeshComponent>();
		if (!ISM)
		{
			ISM = NewObject<UInstancedStaticMeshComponent>(this, TEXT("GridISM"));
			ISM->SetupAttachment(GetRootComponent());
			ISM->RegisterComponent();
		}
		ISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	}
}

void AGameManager::BuildGridInstances()
{
	if (!ISM) return;
	ISM->ClearInstances();
	if (TileMesh) ISM->SetStaticMesh(TileMesh);
	if (TileMaterial) ISM->SetMaterial(0, TileMaterial);

	const float S = FMath::Max(1.f, TileSizeUU);
	const FVector Origin = GridAnchor ? GridAnchor->GetActorLocation() : GetActorLocation();

	const auto ToWorld = [S, Origin, this](int32 Q, int32 R)->FVector
		{
			// Use the same coordinate conversion as HexCoord::AxialToXY
			const FVector2D XY = HexCoord::AxialToXY(FHexAxial(Q, R), S, GridOrientation);
			const FVector2D RotXY = HexCoord::Rotate2D(XY, GridYawDeg);
			return Origin + FVector(RotXY.X, RotXY.Y, 0.f);
		};

	// Get simulation state to avoid rendering grid tiles where other tiles are placed
	USimWorldHex* Sim = nullptr;
	if (OrbSimDriver)
	{
		Sim = OrbSimDriver->GetSim();
	}

	for (int r = -Radius; r <= Radius; ++r)
	{
		for (int q = -Radius; q <= Radius; ++q)
		{
			const int s = -q - r;
			if (FMath::Max3(FMath::Abs(q), FMath::Abs(r), FMath::Abs(s)) > Radius) continue;
			
			// Skip rendering grid tile only if there's an output tile placed here in the simulation
			if (Sim && Sim->TileMap.Contains(FHex(q, r)))
			{
				const FTile& PlacedTile = Sim->TileMap[FHex(q, r)];
				if (PlacedTile.Kind == ETileKind::Output)
				{
					continue; // Skip grid tile for outputs only
				}
			}
			
			const FVector P = ToWorld(q, r);
			ISM->AddInstance(FTransform(FRotator::ZeroRotator, P, FVector(1)));
		}
	}
}

void AGameManager::Rebuild()
{
	EnsureISM();
	BuildGridInstances();
	// Sync OrbSimDriver whenever we rebuild the grid
	SyncOrbSimDriverParameters();
}

// Local helper to mirror TestLevelDirector conversion so runtime-placed arms work too.
static TArray<FOp> ConvertProgramToOps(const TArray<FArmInstr>& Program)
{
	TArray<FOp> Out;
	if (Program.Num() == 0)
	{
		Out.Add({ ECmd::Wait, 1 });
		return Out;
	}
	for (const FArmInstr& I : Program)
	{
		switch (I.Cmd)
		{
		case EArmCmd::NoOp:      Out.Add({ ECmd::Wait, 1 }); break;
		case EArmCmd::Grab:      Out.Add({ ECmd::Grab, 0 }); break;
		case EArmCmd::Drop:      Out.Add({ ECmd::Drop, 0 }); break;
		case EArmCmd::RotateCW:  Out.Add({ ECmd::RotateR, (uint8)FMath::Max(1, I.Arg) }); break;
		case EArmCmd::RotateCCW: Out.Add({ ECmd::RotateL, (uint8)FMath::Max(1, I.Arg) }); break;
		case EArmCmd::Wait:      Out.Add({ ECmd::Wait, (uint8)FMath::Clamp(I.Arg, 0, 255) }); break;
		case EArmCmd::Reset:     Out.Add({ ECmd::Reset, 0 }); break;
		case EArmCmd::Extend:    Out.Add({ ECmd::Extend,  (uint8)FMath::Clamp(I.Arg == 0 ? 1 : I.Arg, 0, 255) }); break;  // NEW
		case EArmCmd::Retract:   Out.Add({ ECmd::Retract, (uint8)FMath::Clamp(I.Arg == 0 ? 1 : I.Arg, 0, 255) }); break;  // NEW
		default:                 Out.Add({ ECmd::Wait, 1 }); break;
		}
	}
	return Out;
}

ABasicArm* AGameManager::SpawnBasicArmAt(const FHex& Hex, uint8 Facing, const TArray<FArmInstr>& Program, bool bLoop)
{
	if (!BasicArmClass)
	{
		UE_LOG(LogTemp, Warning, TEXT("GameManager: BasicArmClass not set; cannot spawn."));
		return nullptr;
	}
	if (!OrbSimDriver)
	{
		UE_LOG(LogTemp, Warning, TEXT("GameManager: OrbSimDriver not set; cannot compute grid transform for arm."));
		return nullptr;
	}

	// Compute world from grid
	const float Tile = OrbSimDriver->GetTileSize();
	const float GridYaw = OrbSimDriver->GetGridYaw();
	const EHexOrientation Ori = OrbSimDriver->GetGridOrientation();
	const AActor* Anchor = OrbSimDriver->GetGridAnchor();
	const FVector AnchorLoc = Anchor ? Anchor->GetActorLocation() : FVector::ZeroVector;

	const FVector2D XY = HexCoord::AxialToXY(FHexAxial(Hex.Q, Hex.R), Tile, Ori);
	const FVector2D RotXY = HexCoord::Rotate2D(XY, GridYaw);
	const FVector   Loc = AnchorLoc + FVector(RotXY.X, RotXY.Y, 0.f);
	const float     Yaw = GridYaw + 60.f * float(Facing % 6);

	FActorSpawnParameters S; S.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	ABasicArm* Arm = GetWorld()->SpawnActor<ABasicArm>(BasicArmClass, FTransform(FRotator(0, Yaw, 0), Loc), S);
	if (!Arm) return nullptr;

	// Initialize actor-side data
	Arm->Driver = OrbSimDriver;
	Arm->Q = Hex.Q;
	Arm->R = Hex.R;
	Arm->FacingDir = Facing % 6;
	Arm->StartingRotation = Facing % 6;
	Arm->StartingReach = Arm->ReachTiles; // Use current ReachTiles as starting reach
	Arm->SetProgram(Program, bLoop);

	// --- Add to simulation so it actually runs ---
	if (USimWorldHex* Sim = OrbSimDriver->GetSim())
	{
		FArmState ArmState;
		ArmState.BaseHex = Hex;
		ArmState.Facing = static_cast<EHexDir>(Facing % 6);
		ArmState.HomeFacing = ArmState.Facing;
		ArmState.HomeArmLen = FMath::Clamp(Arm->ReachTiles, 1, 3);        // NEW
		ArmState.HomeTrackIdx = 0;
		ArmState.ArmLen = ArmState.HomeArmLen;                            // NEW
		ArmState.TrackIdx = 0;
		ArmState.PC = 0;
		ArmState.bHolding = false;
		ArmState.WaitCounter = 0;                                         // (if field exists) NEW
		ArmState.Tape = ConvertProgramToOps(Program);

		const int32 ArmIndex = Sim->AddArm(ArmState);
		Arm->SimArmIndex = ArmIndex;
		Arm->Start();

		UE_LOG(LogTemp, Warning, TEXT("GameManager: Spawned + registered arm at Hex(%d,%d) Facing=%d SimIndex=%d TapeLen=%d"),
			Hex.Q, Hex.R, (int32)ArmState.Facing, ArmIndex, ArmState.Tape.Num());
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("GameManager: Arm spawned visually but Sim is null; program will not run."));
	}

	return Arm;
}

void AGameManager::SyncOrbSimDriverParameters()
{
	if (!OrbSimDriver) return;
	
	// GameManager is the source of truth - sync OrbSimDriver to match
	OrbSimDriver->SetGridParameters(GridOrientation, GridYawDeg, TileSizeUU, GridAnchor);
	
	UE_LOG(LogTemp, Log, TEXT("GameManager: Synced OrbSimDriver parameters - Orientation=%d Yaw=%.1f TileSize=%.1f"),
		(int32)GridOrientation, GridYawDeg, TileSizeUU);
}
