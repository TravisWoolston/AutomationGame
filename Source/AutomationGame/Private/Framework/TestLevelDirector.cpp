#include "Framework/TestLevelDirector.h"
#include "Framework/GameTickManager.h"
#include "Actors/OrbSimDriver.h"
#include "Actors/BasicArm.h"
#include "Sim/SimWorldHex.h"
#include "Sim/HexCoord.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Kismet/GameplayStatics.h"
#include "Components/InstancedStaticMeshComponent.h"

ATestLevelDirector::ATestLevelDirector()
{
	PrimaryActorTick.bCanEverTick = false;
}

void ATestLevelDirector::BeginPlay()
{
	Super::BeginPlay();
	AutoFindOrSpawn();
	if (bAutoBuildOnBeginPlay) // header flag
	{
		BuildActiveScenario();
	}
}

void ATestLevelDirector::AutoFindOrSpawn()
{
	UWorld* W = GetWorld(); if (!W) return;

	// Find or spawn tick manager
	if (!TickManager)
	{
		for (TActorIterator<AGameTickManager> It(W); It; ++It) { TickManager = *It; break; }
	}
	if (!TickManager && TickManagerClass)
	{
		FActorSpawnParameters S; S.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		TickManager = W->SpawnActor<AGameTickManager>(TickManagerClass, GetActorTransform(), S);
		UE_LOG(LogTemp, Log, TEXT("TestLevelDirector: Spawned GameTickManager."));
	}

	// Find or spawn driver
	if (!Driver)
	{
		for (TActorIterator<AOrbSimDriver> It(W); It; ++It) { Driver = *It; break; }
	}
	if (!Driver && DriverClass)
	{
		FActorSpawnParameters S; S.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		Driver = W->SpawnActor<AOrbSimDriver>(DriverClass, GetActorTransform(), S);
		UE_LOG(LogTemp, Log, TEXT("TestLevelDirector: Spawned OrbSimDriver."));
	}
}

void ATestLevelDirector::BuildActiveScenario()
{
	ClearArms();

	if (!Scenarios.IsValidIndex(ActiveScenarioIndex))
	{
		UE_LOG(LogTemp, Warning, TEXT("TestLevelDirector: Invalid scenario index %d."), ActiveScenarioIndex);
		return;
	}

	AutoFindOrSpawn();
	const FScenarioSpec& S = Scenarios[ActiveScenarioIndex];
	ApplyScenario(S);

	if (S.bAutoPlay && TickManager)
	{
		TickManager->Play();
	}
}

void ATestLevelDirector::ClearArms()
{
	// Destroy actors we spawned
	for (TWeakObjectPtr<ABasicArm>& P : SpawnedArms)
	{
		if (P.IsValid())
		{
			P->Destroy();
		}
	}
	SpawnedArms.Reset();

	// Also clear the spawned refs in our authorable array (if present)
	if (Scenarios.IsValidIndex(ActiveScenarioIndex))
	{
		for (FArmSpawnSpec& Spec : Scenarios[ActiveScenarioIndex].Arms)
		{
			Spec.Spawned = nullptr;
		}
	}
}

AOrbSimDriver* ATestLevelDirector::FindDriver() const
{
	if (Driver) return Driver;
	if (UWorld* W = GetWorld())
	{
		for (TActorIterator<AOrbSimDriver> It(W); It; ++It) return *It;
	}
	return nullptr;
}

FVector ATestLevelDirector::HexToWorld(const FHex& H, const AOrbSimDriver* InDriver) const
{
	if (!InDriver) return FVector::ZeroVector;

	const float Tile = InDriver->GetTileSize();                 // driver exposes grid knobs
	const float YawDeg = InDriver->GetGridYaw();
	const EHexOrientation Ori = InDriver->GetGridOrientation();

	const FVector2D XY = HexCoord::AxialToXY(FHexAxial(H.Q, H.R), Tile, Ori);
	const FVector2D RotXY = HexCoord::Rotate2D(XY, YawDeg);

	const AActor* Anchor = InDriver->GetGridAnchor();
	const FVector AnchorLoc = Anchor ? Anchor->GetActorLocation() : FVector::ZeroVector;

	return AnchorLoc + FVector(RotXY.X, RotXY.Y, 0.f);
}

void ATestLevelDirector::ApplyScenario(const FScenarioSpec& S)
{
	AOrbSimDriver* D = FindDriver();
	if (!D)
	{
		UE_LOG(LogTemp, Error, TEXT("TestLevelDirector: No OrbSimDriver available; cannot apply scenario."));
		return;
	}

	USimWorldHex* Sim = D->GetSim();
	if (!Sim)
	{
		UE_LOG(LogTemp, Error, TEXT("TestLevelDirector: Driver has no Sim; cannot apply scenario."));
		return;
	}

	// Configure sim tile size from scenario
	if (!FMath::IsNearlyEqual(Sim->TileSizeUU, S.TileSizeUU))
	{
		Sim->TileSizeUU = S.TileSizeUU;
		Sim->TileRevision++; // nudge visuals to rebuild if your renderer listens to this
		UE_LOG(LogTemp, Log, TEXT("TestLevelDirector: Set Sim TileSizeUU = %.1f"), S.TileSizeUU);
	}

	// Place tiles
	for (const FPlacedTileSpec& T : S.Tiles)
	{
		const FTile TileToPlace{ T.Kind, T.Dir };
		Sim->PlaceTile(T.Hex, TileToPlace);
		UE_LOG(LogTemp, Log, TEXT("TestLevelDirector: Placed %s at Hex(%d,%d) Dir=%d"),
			*StaticEnum<ETileKind>()->GetNameStringByValue((int64)T.Kind), T.Hex.Q, T.Hex.R, (int32)T.Dir);
	}

	// Spawn arms using your blueprint
	if (!ArmBlueprint)
	{
		UE_LOG(LogTemp, Warning, TEXT("TestLevelDirector: ArmBlueprint not set; skipping arm spawns."));
		return;
	}

	UWorld* W = GetWorld(); if (!W) return;

	// We’ll mutate our stored array to keep Spawned refs in sync
	if (!Scenarios.IsValidIndex(ActiveScenarioIndex))
	{
		UE_LOG(LogTemp, Warning, TEXT("TestLevelDirector: Scenario array changed; cannot record spawned arms."));
	}

	for (int32 i = 0; i < S.Arms.Num(); ++i)
	{
		const FArmSpawnSpec& Spec = S.Arms[i];

		const FVector SpawnLoc = HexToWorld(FHex{ Spec.Q, Spec.R }, D);
		const FRotator SpawnRot(0.f, 0.f, 0.f);

		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

		ABasicArm* Arm = W->SpawnActor<ABasicArm>(ArmBlueprint, SpawnLoc, SpawnRot, Params);
		if (!Arm)
		{
			UE_LOG(LogTemp, Warning, TEXT("TestLevelDirector: Failed to spawn arm at Hex(%d,%d)."), Spec.Q, Spec.R);
			continue;
		}

		// Wire grid + pose
		Arm->Driver = D;
		Arm->Q = Spec.Q;
		Arm->R = Spec.R;
		Arm->FacingDir = Spec.Facing;

		// Program (use per-arm if provided, else director default)
		if (Spec.Program.Num() > 0)
		{
			Arm->SetProgram(Spec.Program, Spec.bLoopProgram);
		}
		else if (DefaultProgram.Num() > 0)
		{
			Arm->SetProgram(DefaultProgram, true);
		}

		// ensure it starts (if your ABasicArm defaults bRunning=false, call Start())
		Arm->Start();

		SpawnedArms.Add(Arm);

		// reflect back into editable array for convenience (if valid index)
		if (Scenarios.IsValidIndex(ActiveScenarioIndex))
		{
			Scenarios[ActiveScenarioIndex].Arms[i].Spawned = Arm;
		}

		UE_LOG(LogTemp, Log, TEXT("TestLevelDirector: Spawned arm at Hex(%d,%d) Facing=%d, ProgramLen=%d"),
			Spec.Q, Spec.R, (int32)Spec.Facing, Spec.Program.Num() > 0 ? Spec.Program.Num() : DefaultProgram.Num());
	}
}
