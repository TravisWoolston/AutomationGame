#include "Framework/TestLevelDirector.h"
#include "Framework/GameTickManager.h"
#include "Actors/OrbSimDriver.h"
#include "Sim/SimWorldHex.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "AGLogChannels.h"
#include "Actors/BasicArm.h"

#if WITH_EDITOR
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "Editor/UnrealEdEngine.h"
#endif

// Renamed to avoid redefinition clash with identical static function in GameManager.cpp when unity build groups files.
static TArray<FOp> ConvertProgramToOps_TLD(const TArray<FArmInstr>& Program)
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

// Helper: copy any edited preview arm data (Program, loop flag, facing, reach) back into the scenario spec
static void SyncPreviewEditsIntoScenario(FScenarioSpec& Scenario)
{
	for (FArmSpawnSpec& ArmSpec : Scenario.Arms)
	{
		if (ArmSpec.Spawned)
		{
			// Pull current live actor data back into the spec so re-apply uses edits
			ArmSpec.Program = ArmSpec.Spawned->Program;        // assumes Program is public/UPROPERTY
			ArmSpec.bLoopProgram = ArmSpec.Spawned->bLoopProgram;
			ArmSpec.Facing = ArmSpec.Spawned->FacingDir;
			//ArmSpec.ReachTiles = ArmSpec.Spawned->ReachTiles;     // if your spec tracks reach
			UE_LOG(LogTemp, Verbose, TEXT("TestLevelDirector: Synced preview arm edits (Hex=%d,%d) ProgramLen=%d Facing=%d Loop=%d"),
				ArmSpec.Q, ArmSpec.R, ArmSpec.Program.Num(), (int32)ArmSpec.Facing, ArmSpec.bLoopProgram ? 1 : 0);
		}
	}
}

ATestLevelDirector::ATestLevelDirector()
{
	PrimaryActorTick.bCanEverTick = false;
#if WITH_EDITOR
	if (GIsEditor)
	{
		FEditorDelegates::PreBeginPIE.AddUObject(this, &ATestLevelDirector::HandlePreBeginPIE);
	}
#endif
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

void ATestLevelDirector::ApplyScenario(const FScenarioSpec& S)
{
	// NEW: capture any preview edits before we destroy preview actors
	FScenarioSpec& MutableScenario = const_cast<FScenarioSpec&>(S);
	SyncPreviewEditsIntoScenario(MutableScenario);

	ClearArms(); // (existing call)

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

			ArmState.Tape = ConvertProgramToOps_TLD(A.Program);

			// After adding the arm to the simulation:
			int32 ArmIndex = Sim->AddArm(ArmState);
			UE_LOG(LogTemp, Warning, TEXT("AddArm: Arm added at index %d, total arms now %d"), ArmIndex, Sim->Arms.Num());

			// Set SimArmIndex on the visual arm
			A.Spawned->SimArmIndex = ArmIndex;
			A.Spawned->SetProgram(A.Program, A.bLoopProgram); // now pushes correctly
			A.Spawned->Start();
		}
	}

	// Auto-play tick manager if requested
	if (TickManager && S.bAutoPlay)
	{
		TickManager->Play();
		UE_LOG(LogTemp, Log, TEXT("GameTickManager: Play()"));
	}
}

// Example for arms
void ATestLevelDirector::SpawnScenarioPreview()
{
	ClearArms(); // <-- Add this line

	UWorld* World = GetWorld();
	if (!Driver) return;

	for (FArmSpawnSpec& ArmSpec : Scenarios[ActiveScenarioIndex].Arms)
	{
		if (!ArmSpec.Spawned)
		{
			FVector WorldPos = GetWorldPositionForHex(ArmSpec.Q, ArmSpec.R);
			FRotator WorldRot(0.f, Driver->GetGridYaw() + 60.f * (ArmSpec.Facing % 6), 0.f);

			FTransform Tm(WorldRot, WorldPos);
			ArmSpec.Spawned = World->SpawnActor<ABasicArm>(ArmClass, Tm);
		}
	}
}

FVector ATestLevelDirector::GetWorldPositionForHex(int32 Q, int32 R) const
{
	if (!Driver) return FVector::ZeroVector;
	const AActor* Anchor = Driver->GetGridAnchor();
	const FVector Origin = Anchor ? Anchor->GetActorLocation() : FVector::ZeroVector;
	const float TileSize = Driver->GetTileSize();
	const EHexOrientation Orientation = Driver->GetGridOrientation();
	const float GridYaw = Driver->GetGridYaw();

	// Convert axial (Q, R) to local XY
	const FVector2D LocalXY = HexCoord::AxialToXY(FHexAxial(Q, R), TileSize, Orientation);
	// Rotate by grid yaw
	const FVector2D RotXY = HexCoord::Rotate2D(LocalXY, GridYaw);

	// Final world position
	return Origin + FVector(RotXY.X, RotXY.Y, 0.f);
}

void ATestLevelDirector::CommitPreviewEdits()
{
	if (!Scenarios.IsValidIndex(ActiveScenarioIndex))
	{
		UE_LOG(LogTemp, Warning, TEXT("CommitPreviewEdits: Invalid ActiveScenarioIndex."));
		return;
	}
	FScenarioSpec& Scenario = Scenarios[ActiveScenarioIndex];
	SyncPreviewEditsIntoScenario(Scenario);
	// Mark owning package/map dirty so changes persist after restart
	if (UPackage* Pkg = GetOutermost())
	{
		Pkg->SetDirtyFlag(true);
	}
	UE_LOG(LogTemp, Log, TEXT("CommitPreviewEdits: Saved edits for Scenario '%s' (%d arms)."),
		*Scenario.Name.ToString(), Scenario.Arms.Num());
}

// NEW: live patch helper
void ATestLevelDirector::RefreshSpawnedArmsFromScenario(int32 ScenarioIndex)
{
	if (!Scenarios.IsValidIndex(ScenarioIndex) || !bAutoPatchSpawnedArms) return;

	FScenarioSpec& Spec = Scenarios[ScenarioIndex];
	for (FArmSpawnSpec& ArmSpec : Spec.Arms)
	{
		if (!ArmSpec.Spawned) continue;

		ABasicArm* Arm = ArmSpec.Spawned;
		// Patch Program / Loop
		if (Arm->Program != ArmSpec.Program || Arm->bLoopProgram != ArmSpec.bLoopProgram)
		{
			Arm->SetProgram(ArmSpec.Program, ArmSpec.bLoopProgram);
			UE_LOG(LogTemp, Verbose, TEXT("TestLevelDirector: Patched arm program at (%d,%d) Len=%d Loop=%d"),
				ArmSpec.Q, ArmSpec.R, ArmSpec.Program.Num(), ArmSpec.bLoopProgram ? 1 : 0);
		}
		// Patch facing (and grid placement if moved)
		bool bPlacementChanged = (Arm->Q != ArmSpec.Q) || (Arm->R != ArmSpec.R) || (Arm->FacingDir != ArmSpec.Facing);
		if (bPlacementChanged)
		{
			const uint8 NewFacing = ArmSpec.Facing % 6;
			Arm->InitializeOnGrid(Driver, ArmSpec.Q, ArmSpec.R, NewFacing);
			UE_LOG(LogTemp, Verbose, TEXT("TestLevelDirector: Patched arm placement (%d,%d) Facing=%d"),
				ArmSpec.Q, ArmSpec.R, (int32)NewFacing);
		}
	}
}

#if WITH_EDITOR
void ATestLevelDirector::HandlePreBeginPIE(bool /*bIsSimulating*/)
{
	// Auto commit any preview edits so they are serialized into Scenario spec before ApplyScenario in BeginPlay
	if (!IsValid(this)) return;
	CommitPreviewEdits();
}

void ATestLevelDirector::PostEditChangeProperty(FPropertyChangedEvent& Event)
{
	Super::PostEditChangeProperty(Event);

	// If the Scenarios array (or nested fields) changed, patch spawned preview arms
	if (Event.MemberProperty &&
		Event.MemberProperty->GetFName() == GET_MEMBER_NAME_CHECKED(ATestLevelDirector, Scenarios))
	{
		RefreshSpawnedArmsFromScenario(ActiveScenarioIndex);
	}
}
#endif

void ATestLevelDirector::ReapplyActiveScenario()
{
	if (Scenarios.IsValidIndex(ActiveScenarioIndex))
	{
		ApplyScenario(Scenarios[ActiveScenarioIndex]);
		UE_LOG(LogTemp, Log, TEXT("Reapplied scenario '%s'"), *Scenarios[ActiveScenarioIndex].Name.ToString());
	}
}
