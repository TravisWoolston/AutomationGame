#include "Framework/TestLevelDirector.h"
#include "Framework/GameTickManager.h"
#include "Actors/OrbSimDriver.h"
#include "Sim/SimWorldHex.h"
#include "Engine/World.h"
#include "EngineUtils.h" // TActorIterator
#include "AGLogChannels.h"

ATestLevelDirector::ATestLevelDirector()
{
	PrimaryActorTick.bCanEverTick = false;
}

void ATestLevelDirector::BeginPlay()
{
	Super::BeginPlay();
	AutoFindOrSpawn();

	if (Scenarios.IsValidIndex(ActiveScenarioIndex))
	{
		ApplyScenario(Scenarios[ActiveScenarioIndex]);
	}
	// TestLevelDirector.cpp (after you place both Inputs, before/after spawning arms is okay)
	if (Driver && Driver->GetSim())
	{
		Driver->GetSim()->StepSim();  // spawns items on the Input tiles immediately

#if !(UE_BUILD_SHIPPING)
		FItem Tmp;
		UE_LOG(LogTemp, Log, TEXT("Seed step: Input L=%d, R=%d"),
			Driver->GetSim()->PeekItemAt(FHex(-1, 0), Tmp) ? 1 : 0,
			Driver->GetSim()->PeekItemAt(FHex(1, 0), Tmp) ? 1 : 0);
#endif
	}

}

void ATestLevelDirector::AutoFindOrSpawn()
{
	UWorld* W = GetWorld();
	if (!W) return;

	// Tick manager
	if (!TickManager)
	{
		for (TActorIterator<AGameTickManager> It(W); It; ++It) { TickManager = *It; break; }
		if (!TickManager && TickManagerClass)
		{
			TickManager = W->SpawnActor<AGameTickManager>(TickManagerClass, FTransform::Identity);
		}
	}

	// Driver
	if (!Driver)
	{
		for (TActorIterator<AOrbSimDriver> It(W); It; ++It) { Driver = *It; break; }
		if (!Driver && DriverClass)
		{
			Driver = W->SpawnActor<AOrbSimDriver>(DriverClass, FTransform::Identity);
		}
	}
}

void ATestLevelDirector::BuildActiveScenario()
{
	if (Scenarios.IsValidIndex(ActiveScenarioIndex))
	{
		ApplyScenario(Scenarios[ActiveScenarioIndex]);
	}
}

void ATestLevelDirector::ClearArms()
{
	if (!GetWorld()) return;

	for (FScenarioSpec& Sc : Scenarios)
	{
		for (FArmSpawnSpec& S : Sc.Arms)
		{
			if (S.Spawned)
			{
				S.Spawned->Destroy();
				S.Spawned = nullptr;
			}
		}
	}
}
// TestLevelDirector.cpp  (where you currently construct ArmState then call Sim->AddArm)

static TArray<FOp> ConvertProgramToOps(const TArray<FArmInstr>& Program)
{
	TArray<FOp> Out;
	for (const FArmInstr& I : Program)
	{
		switch (I.Cmd)
		{
		case EArmCmd::NoOp:      Out.Add({ ECmd::Wait, 1 }); break;
		case EArmCmd::Grab:      Out.Add({ ECmd::Grab, 0 }); break;
		case EArmCmd::Drop:      Out.Add({ ECmd::Drop, 0 }); break;
		case EArmCmd::RotateCW:  Out.Add({ ECmd::RotateR, (uint8)FMath::Max(1, I.Arg) }); break;
		case EArmCmd::RotateCCW: Out.Add({ ECmd::RotateL, (uint8)FMath::Max(1, I.Arg) }); break;
		case EArmCmd::Wait:      Out.Add({ ECmd::Wait,    (uint8)FMath::Clamp(I.Arg, 0, 255) }); break;
		case EArmCmd::Reset:     Out.Add({ ECmd::Reset, 0 }); break;
		default:                 Out.Add({ ECmd::Wait, 1 }); break;
		}
	}
	return Out;
}



void ATestLevelDirector::ApplyScenario(const FScenarioSpec& S)
{
	UWorld* W = GetWorld();
	if (!W || !Driver) return;

	// Sim tiles
	if (USimWorldHex* Sim = Driver->GetSim())
	{
		for (const FPlacedTileSpec& P : S.Tiles)
		{
			FTile T;
			T.Kind = P.Kind;
			T.Arg = P.Dir;
			Sim->PlaceTile(P.Hex, T);
			UE_LOG(LogTemp, Log, TEXT("TestLevelDirector: Placed %s at Hex(%d,%d) Dir=%d"),
				*StaticEnum<ETileKind>()->GetNameStringByValue((int64)P.Kind),
				P.Hex.Q, P.Hex.R, (int32)P.Dir);
		}
	}

	// Arms
	for (FArmSpawnSpec& A : const_cast<TArray<FArmSpawnSpec>&>(S.Arms))
	{
		if (!ArmClass)
		{
			UE_LOG(LogTemp, Warning, TEXT("TestLevelDirector: ArmClass not set; skipping arm spawn."));
			continue;
		}

		FTransform Tm;
		Tm.SetLocation(Driver->GetGridAnchor() ? Driver->GetGridAnchor()->GetActorLocation() : FVector::ZeroVector);
		A.Spawned = W->SpawnActorDeferred<ABasicArm>(ArmClass, Tm, nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (!A.Spawned) continue;

		A.Spawned->InitializeOnGrid(Driver, A.Q, A.R, A.Facing);
		A.Spawned->SetProgram(A.Program, A.bLoopProgram);

		A.Spawned->FinishSpawning(Tm);
		A.Spawned->Start();

		UE_LOG(LogTemp, Log, TEXT("TestLevelDirector: Spawned arm at Hex(%d,%d) Facing=%d, ProgramLen=%d"),
			A.Q, A.R, (int32)A.Facing, A.Program.Num());

		// After spawning and initializing the arm actor:
		if (USimWorldHex* Sim = Driver->GetSim())
		{
			FArmState ArmState;
			ArmState.BaseHex = FHex(A.Q, A.R);
			ArmState.Facing = static_cast<EHexDir>(A.Facing);
			//ArmState.Tape = A.Program; // Assuming FArmInstr is compatible with FOp or you have a conversion
			ArmState.HomeFacing = ArmState.Facing;
			ArmState.HomeArmLen = 1; // Set as needed
			ArmState.HomeTrackIdx = 0; // Set as needed
			ArmState.ArmLen = 1; // Set as needed
			ArmState.TrackIdx = 0; // Set as needed
			ArmState.PC = 0;
			ArmState.bHolding = false;
			// Set other fields as needed

			ArmState.Tape = ConvertProgramToOps(A.Program);

			int32 ArmIndex = Sim->AddArm(ArmState);
			UE_LOG(LogTemp, Warning, TEXT("AddArm: Arm added at index %d, total arms now %d"), ArmIndex, Sim->Arms.Num());
		}
	}

	// Auto-play tick manager if requested
	if (TickManager && S.bAutoPlay)
	{
		TickManager->Play();
		UE_LOG(LogTemp, Log, TEXT("GameTickManager: Play()"));
	}
}
