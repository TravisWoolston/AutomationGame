#include "Framework/TestLevelDirector.h"
#include "Framework/GameTickManager.h"
#include "Framework/GameManager.h"
#include "Actors/OrbSimDriver.h"
#include "Actors/HexFactoryActor.h"
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

// Destroy an actor immediately in editor/PIE, otherwise defer to runtime Destroy().
static void DestroyActorNow(AActor* Actor)
{
	if (!Actor) return;
#if WITH_EDITOR
	if (UWorld* W = Actor->GetWorld())
	{
		if (W->IsEditorWorld() || W->IsPlayInEditor())
		{
			W->EditorDestroyActor(Actor, /*bShouldModifyLevel*/true);
			return;
		}
	}
#endif
	Actor->Destroy();
}

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
		case EArmCmd::Extend:    Out.Add({ ECmd::Extend,  (uint8)FMath::Clamp(I.Arg == 0 ? 1 : I.Arg, 0, 255) }); break;   // NEW
		case EArmCmd::Retract:   Out.Add({ ECmd::Retract, (uint8)FMath::Clamp(I.Arg == 0 ? 1 : I.Arg, 0, 255) }); break;   // NEW
		default:                 Out.Add({ ECmd::Wait, 1 }); break;
		}
	}
	return Out;
}

ATestLevelDirector::ATestLevelDirector()
{
	PrimaryActorTick.bCanEverTick = false;
#if WITH_EDITOR
	if (GIsEditor)
	{
		FEditorDelegates::PreBeginPIE.AddUObject(this, &ATestLevelDirector::HandlePreBeginPIE);
		FEditorDelegates::EndPIE.AddUObject(this, &ATestLevelDirector::HandleEndPIE);
	}
#endif
}

void ATestLevelDirector::BeginDestroy()
{
	Super::BeginDestroy();
#if WITH_EDITOR
	if (GIsEditor)
	{
		FEditorDelegates::PreBeginPIE.RemoveAll(this);
		FEditorDelegates::EndPIE.RemoveAll(this);
	}
#endif
}

void ATestLevelDirector::BeginPlay()
{
	Super::BeginPlay();
	AutoFindOrSpawn();
	
	// Wait one frame to ensure GameManager has completed its initialization and synced parameters
	GetWorld()->GetTimerManager().SetTimerForNextTick(this, &ATestLevelDirector::DelayedScenarioApplication);
}

void ATestLevelDirector::DelayedScenarioApplication()
{
	if (Scenarios.IsValidIndex(ActiveScenarioIndex))
	{
		ApplyScenario(Scenarios[ActiveScenarioIndex]);
	}
	if (Driver && Driver->GetSim())
	{
		Driver->GetSim()->StepSim();
	}
	
	// Force any HexFactoryActor instances to rebuild their visuals
	if (UWorld* W = GetWorld())
	{
		for (TActorIterator<AHexFactoryActor> It(W); It; ++It)
		{
			AHexFactoryActor* Factory = *It;
			if (Factory)
			{
				UE_LOG(LogTemp, Log, TEXT("TestLevelDirector: Forcing HexFactoryActor rebuild"));
				Factory->Rebuild();
			}
		}
		
		// Force GameManager to rebuild grid to reflect scenario tiles
		for (TActorIterator<AGameManager> It(W); It; ++It)
		{
			AGameManager* GameManager = *It;
			if (GameManager)
			{
				UE_LOG(LogTemp, Log, TEXT("TestLevelDirector: Forcing GameManager rebuild"));
				GameManager->Rebuild();
				break;
			}
		}
	}
}

void ATestLevelDirector::AutoFindOrSpawn()
{
	UWorld* W = GetWorld();
	if (!W) return;

	if (!TickManager)
	{
		for (TActorIterator<AGameTickManager> It(W); It; ++It) { TickManager = *It; break; }
		if (!TickManager && TickManagerClass)
			TickManager = W->SpawnActor<AGameTickManager>(TickManagerClass, FTransform::Identity);
	}

	if (!Driver)
	{
		for (TActorIterator<AOrbSimDriver> It(W); It; ++It) { Driver = *It; break; }
		if (!Driver && DriverClass)
			Driver = W->SpawnActor<AOrbSimDriver>(DriverClass, FTransform::Identity);
	}
}

void ATestLevelDirector::BuildActiveScenario()
{
	if (!Scenarios.IsValidIndex(ActiveScenarioIndex)) return;
	ResetSimForActiveScenario();
	ApplyScenario(Scenarios[ActiveScenarioIndex]);
}

void ATestLevelDirector::ResetSimForActiveScenario()
{
	if (!Scenarios.IsValidIndex(ActiveScenarioIndex)) return;

	// FIRST: Save current simulation state (including manual arms/tiles) to the active scenario
	// This preserves any manually placed content as part of the scenario
	if (Driver && Driver->GetSim() && Scenarios.IsValidIndex(ActiveScenarioIndex))
	{
		UE_LOG(LogTemp, Warning, TEXT("TestLevelDirector: Saving current simulation state to scenario %d before reset"), ActiveScenarioIndex);
		SyncAllRuntimeArmEdits();  // Capture any program/facing changes from visual arms
		SyncPreviewEditsIntoScenarioFiltered(Scenarios[ActiveScenarioIndex]); // Capture tiles and arm positions
	}

	// Stop ticking while we rebuild (optional safeguard)
	if (TickManager)
	{
		// If you have an explicit Stop() function, call it; otherwise ignore.
		// TickManager->Stop();
	}

	// Destroy any spawned arms across scenarios (ApplyScenario will spawn fresh)
	ClearArms();

	// Hard purge: destroy any residual arms not tracked in specs (runtime placed, etc.)
	{
		UWorld* W = GetWorld();
		if (W)
		{
			TArray<ABasicArm*> ToDestroy;
			for (TActorIterator<ABasicArm> It(W); It; ++It)
			{
				ToDestroy.Add(*It);
			}
			for (ABasicArm* Arm : ToDestroy)
			{
				if (Arm) 
				{
					UE_LOG(LogTemp, Warning, TEXT("TestLevelDirector: Destroying arm at (%d,%d) SimIndex=%d during scenario reset"), 
						Arm->Q, Arm->R, Arm->SimArmIndex);
					DestroyActorNow(Arm);
				}
			}
		}
	}

	if (Driver && Driver->GetSim())
	{
		USimWorldHex* Sim = Driver->GetSim();
		const FScenarioSpec& Spec = Scenarios[ActiveScenarioIndex];

		// Reinitialize simulation (clears tiles, items, arms, bonds, etc.)
		Sim->Initialize(Spec.GridRadius);
		Sim->TileSizeUU = Spec.TileSizeUU;
		
		// Ensure sim gets the correct grid parameters from OrbSimDriver
		const FVector AnchorLoc = Driver->GetGridAnchor() ? Driver->GetGridAnchor()->GetActorLocation() : FVector::ZeroVector;
		Sim->SetWorldTransform(AnchorLoc, Driver->GetGridYaw(), Driver->GetGridOrientation());
	}
}

void ATestLevelDirector::ClearArms()
{
	if (!GetWorld()) return;
	for (FScenarioSpec& Sc : Scenarios)
		for (FArmSpawnSpec& S : Sc.Arms)
			if (S.Spawned)
			{
				DestroyActorNow(S.Spawned);
				S.Spawned = nullptr;
			}
}

void ATestLevelDirector::ApplyScenario(const FScenarioSpec& S)
{
	// Note: We already synced current state to scenario in ResetSimForActiveScenario()
	// Don't sync here as it would overwrite the scenario we're trying to load
	ClearArms();

	UWorld* W = GetWorld();
	if (!W || !Driver) return;

	if (USimWorldHex* Sim = Driver->GetSim())
	{
		for (const FPlacedTileSpec& P : S.Tiles)
		{
			FTile T; T.Kind = P.Kind; T.Arg = P.Dir;
			Sim->PlaceTile(P.Hex, T);
		}
	}

	for (FArmSpawnSpec& A : const_cast<TArray<FArmSpawnSpec>&>(S.Arms))
	{
		if (!ArmClass) continue;

		// Spawn deferred so we can finish configuration before BeginPlay-style logic
		FTransform Tm;
		Tm.SetLocation(Driver->GetGridAnchor() ? Driver->GetGridAnchor()->GetActorLocation() : FVector::ZeroVector);

		A.Spawned = W->SpawnActorDeferred<ABasicArm>(ArmClass, Tm, nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (!A.Spawned) continue;

		// Basic spatial init (does not start execution)
		A.Spawned->InitializeOnGrid(Driver, A.Q, A.R, A.Facing);

		int32 ArmIndex = INDEX_NONE;
		if (USimWorldHex* Sim = Driver->GetSim())
		{
			// Build initial sim-side arm state (tape comes from scenario program)
			FArmState ArmState;
			ArmState.BaseHex = FHex(A.Q, A.R);
			ArmState.Facing = (EHexDir)(A.Facing % 6);
			ArmState.HomeFacing = ArmState.Facing;
			ArmState.HomeArmLen = 1;
			ArmState.HomeTrackIdx = 0;
			ArmState.ArmLen = 1;
			ArmState.TrackIdx = 0;
			ArmState.PC = 0;
			ArmState.bHolding = false;
			ArmState.Tape = ConvertProgramToOps_TLD(A.Program);

			ArmIndex = Sim->AddArm(ArmState);
			A.Spawned->SimArmIndex = ArmIndex;
			UE_LOG(LogTemp, Warning, TEXT("TestLevelDirector: Added scenario arm at (%d,%d) SimIndex=%d"), 
				A.Q, A.R, ArmIndex);
		}

		// Finish spawn now that SimArmIndex is valid
		A.Spawned->FinishSpawning(Tm);

		// Apply program AFTER SimArmIndex is assigned so SetProgram can push immediately
		A.Spawned->SetProgram(A.Program, A.bLoopProgram);

		// Now start execution
		A.Spawned->Start();

		// If your SetProgram implementation does NOT push to sim, uncomment:
		// A.Spawned->PushProgramToSim();
	}

	if (TickManager && S.bAutoPlay)
	{
		TickManager->Play();
	}
}

void ATestLevelDirector::SpawnScenarioPreview()
{
	ClearArms();
	UWorld* World = GetWorld();
	if (!Driver) return;

	for (FArmSpawnSpec& ArmSpec : Scenarios[ActiveScenarioIndex].Arms)
	{
		if (!ArmSpec.Spawned && ArmClass)
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

	const FVector2D LocalXY = HexCoord::AxialToXY(FHexAxial(Q, R), TileSize, Orientation);
	const FVector2D RotXY = HexCoord::Rotate2D(LocalXY, GridYaw);
	return Origin + FVector(RotXY.X, RotXY.Y, 0.f);
}

void ATestLevelDirector::CommitPreviewEdits()
{
	if (!Scenarios.IsValidIndex(ActiveScenarioIndex)) return;
	FScenarioSpec& Scenario = Scenarios[ActiveScenarioIndex];
	SyncPreviewEditsIntoScenarioFiltered(Scenario);
	if (UPackage* Pkg = GetOutermost())
		Pkg->SetDirtyFlag(true);
}

void ATestLevelDirector::RefreshSpawnedArmsFromScenario(int32 ScenarioIndex)
{
	if (!Scenarios.IsValidIndex(ScenarioIndex) || !bAutoPatchSpawnedArms) return;
	FScenarioSpec& Spec = Scenarios[ScenarioIndex];
	for (FArmSpawnSpec& ArmSpec : Spec.Arms)
	{
		if (!ArmSpec.Spawned) continue;
		ABasicArm* Arm = ArmSpec.Spawned;

		if (Arm->Program != ArmSpec.Program || Arm->bLoopProgram != ArmSpec.bLoopProgram)
			Arm->SetProgram(ArmSpec.Program, ArmSpec.bLoopProgram);

		bool bPlacementChanged = (Arm->Q != ArmSpec.Q) || (Arm->R != ArmSpec.R) || (Arm->FacingDir != ArmSpec.Facing);
		if (bPlacementChanged)
			Arm->InitializeOnGrid(Driver, ArmSpec.Q, ArmSpec.R, ArmSpec.Facing % 6);
	}
}

void ATestLevelDirector::MarkActiveScenarioDirty()
{
	if (!Scenarios.IsValidIndex(ActiveScenarioIndex)) return;
	if (UPackage* Pkg = GetOutermost())
		Pkg->SetDirtyFlag(true);
}

void ATestLevelDirector::SetActiveScenarioIndex(int32 NewIndex)
{
	if (NewIndex == ActiveScenarioIndex) return;

	if (!Scenarios.IsValidIndex(NewIndex))
	{
		UE_LOG(LogTemp, Warning, TEXT("TestLevelDirector: SetActiveScenarioIndex(%d) invalid."), NewIndex);
		return;
	}

	ActiveScenarioIndex = NewIndex;
	BuildActiveScenario();          // Clears & applies new scenario

#if WITH_EDITOR
	MarkActiveScenarioDirty();
	if (GetWorld() && GetWorld()->IsPlayInEditor())
	{
		MirrorScenarioEditsToEditor();
	}
#endif
}

/* ---------------- Runtime Scenario Mutation ---------------- */

void ATestLevelDirector::UpsertTileSpec(const FHex& Hex, ETileKind Kind, uint8 Dir)
{
	if (!Scenarios.IsValidIndex(ActiveScenarioIndex)) return;
	FScenarioSpec& Sc = Scenarios[ActiveScenarioIndex];

	for (FPlacedTileSpec& T : Sc.Tiles)
	{
		if (T.Hex.Q == Hex.Q && T.Hex.R == Hex.R)
		{
			T.Kind = Kind;
			T.Dir = Dir;
			MarkActiveScenarioDirty();
#if WITH_EDITOR
			MirrorScenarioEditsToEditor();
#endif
			return;
		}
	}
	FPlacedTileSpec NewSpec;
	NewSpec.Hex = Hex;
	NewSpec.Kind = Kind;
	NewSpec.Dir = Dir;
	Sc.Tiles.Add(NewSpec);
	MarkActiveScenarioDirty();
#if WITH_EDITOR
	MirrorScenarioEditsToEditor();
#endif
}

bool ATestLevelDirector::RemoveTileSpec(const FHex& Hex)
{
	if (!Scenarios.IsValidIndex(ActiveScenarioIndex)) return false;
	FScenarioSpec& Sc = Scenarios[ActiveScenarioIndex];
	for (int32 i = 0; i < Sc.Tiles.Num(); ++i)
	{
		if (Sc.Tiles[i].Hex.Q == Hex.Q && Sc.Tiles[i].Hex.R == Hex.R)
		{
			Sc.Tiles.RemoveAt(i);
			MarkActiveScenarioDirty();
#if WITH_EDITOR
			MirrorScenarioEditsToEditor();
#endif
			return true;
		}
	}
	return false;
}

void ATestLevelDirector::UpsertArmSpec(int32 Q, int32 R, uint8 Facing, const TArray<FArmInstr>& Program, bool bLoop)
{
	if (!Scenarios.IsValidIndex(ActiveScenarioIndex)) return;
	FScenarioSpec& Sc = Scenarios[ActiveScenarioIndex];

	auto FindRuntimeArmAt = [&](int32 InQ, int32 InR) -> ABasicArm*
		{
			if (!GetWorld()) return nullptr;
			for (TActorIterator<ABasicArm> It(GetWorld()); It; ++It)
			{
				if (It->Q == InQ && It->R == InR)
				{
					return *It;
				}
			}
			return nullptr;
		};

	for (FArmSpawnSpec& A : Sc.Arms)
	{
		if (A.Q == Q && A.R == R)
		{
			A.Facing = Facing % 6;
			A.Program = Program;
			A.bLoopProgram = bLoop;
			if (!A.Spawned) A.Spawned = FindRuntimeArmAt(Q, R);
			MarkActiveScenarioDirty();
#if WITH_EDITOR
			MirrorScenarioEditsToEditor();
#endif
			return;
		}
	}

	FArmSpawnSpec NewA;
	NewA.Q = Q;
	NewA.R = R;
	NewA.Facing = Facing % 6;
	NewA.Program = Program;
	NewA.bLoopProgram = bLoop;
	NewA.Spawned = FindRuntimeArmAt(Q, R);   // capture runtime actor if already spawned
	Sc.Arms.Add(NewA);
	MarkActiveScenarioDirty();
#if WITH_EDITOR
	MirrorScenarioEditsToEditor();
#endif
}

bool ATestLevelDirector::RemoveArmSpec(int32 Q, int32 R)
{
	if (!Scenarios.IsValidIndex(ActiveScenarioIndex)) return false;
	FScenarioSpec& Sc = Scenarios[ActiveScenarioIndex];

	for (int32 i = 0; i < Sc.Arms.Num(); ++i)
	{
		if (Sc.Arms[i].Q == Q && Sc.Arms[i].R == R)
		{
			if (Sc.Arms[i].Spawned)
			{
				DestroyActorNow(Sc.Arms[i].Spawned);
				Sc.Arms[i].Spawned = nullptr;
			}
			else if (UWorld* W = GetWorld())
			{
				for (TActorIterator<ABasicArm> It(W); It; ++It)
				{
					if (It->Q == Q && It->R == R)
					{
						DestroyActorNow(*It);
						break;
					}
				}
			}

			Sc.Arms.RemoveAt(i);
			MarkActiveScenarioDirty();
#if WITH_EDITOR
			MirrorScenarioEditsToEditor();
#endif
			return true;
		}
	}

	if (UWorld* W = GetWorld())
	{
		for (TActorIterator<ABasicArm> It(W); It; ++It)
		{
			if (It->Q == Q && It->R == R)
			{
				DestroyActorNow(*It);
				MarkActiveScenarioDirty();
#if WITH_EDITOR
				MirrorScenarioEditsToEditor();
#endif
				return true;
			}
		}
	}
	return false;
}

/* ---------------- PIE Sync (Editor Only) ---------------- */
#if WITH_EDITOR
void ATestLevelDirector::HandlePreBeginPIE(bool /*bIsSimulating*/)
{
	if (!IsValid(this)) return;
	CommitPreviewEdits();
}

void ATestLevelDirector::HandleEndPIE(bool /*bIsSimulating*/)
{
	// Only run on the PIE world copy
	if (!GetWorld() || !GetWorld()->IsPlayInEditor()) return;

	// First harvest any runtime arm changes (program edits etc.)
	SyncAllRuntimeArmEdits();

	// Mirror one last time to editor world
	MirrorScenarioEditsToEditor();
}

void ATestLevelDirector::SyncAllRuntimeArmEdits()
{
	for (FScenarioSpec& Sc : Scenarios)
	{
		SyncPreviewEditsIntoScenarioFiltered(Sc);
	}
}

void ATestLevelDirector::MirrorScenarioEditsToEditor()
{
	if (!GIsEditor) return;
	if (!GetWorld() || !GetWorld()->IsPlayInEditor()) return;

	UEditorEngine* EE = Cast<UEditorEngine>(GEngine);
	if (!EE) return;

	UWorld* EditorWorld = EE->GetEditorWorldContext().World();
	if (!EditorWorld || EditorWorld == GetWorld()) return;

	for (TActorIterator<ATestLevelDirector> It(EditorWorld); It; ++It)
	{
		ATestLevelDirector* EditorDir = *It;
		// Copy arrays (deep copy structs)
		EditorDir->Scenarios = Scenarios;
		EditorDir->ActiveScenarioIndex = ActiveScenarioIndex;
		EditorDir->MarkActiveScenarioDirty();
		break;
	}
}

void ATestLevelDirector::PostEditChangeProperty(FPropertyChangedEvent& Event)
{
	Super::PostEditChangeProperty(Event);

	if (Event.MemberProperty)
	{
		const FName Name = Event.MemberProperty->GetFName();

		if (Name == GET_MEMBER_NAME_CHECKED(ATestLevelDirector, ActiveScenarioIndex))
		{
			// Rebuild immediately when index changes (works in editor & PIE)
			BuildActiveScenario();
			MarkActiveScenarioDirty();
		}
		else if (Name == GET_MEMBER_NAME_CHECKED(ATestLevelDirector, Scenarios))
		{
			RefreshSpawnedArmsFromScenario(ActiveScenarioIndex);
		}
	}
}
#endif

void ATestLevelDirector::ReapplyActiveScenario()
{
	if (Scenarios.IsValidIndex(ActiveScenarioIndex))
		ApplyScenario(Scenarios[ActiveScenarioIndex]);
}

void ATestLevelDirector::CommitRuntimeFacingEdits()
{
	// One�shot capture of current facing directions regardless of bCaptureRuntimeFacingEdits flag
	for (FScenarioSpec& Scenario : Scenarios)
	{
		for (FArmSpawnSpec& ArmSpec : Scenario.Arms)
		{
			if (ArmSpec.Spawned)
			{
				ArmSpec.Facing = ArmSpec.Spawned->FacingDir;
			}
		}
	}
	MarkActiveScenarioDirty();
#if WITH_EDITOR
	if (GetWorld() && GetWorld()->IsPlayInEditor())
	{
		MirrorScenarioEditsToEditor();
	}
#endif
}

void ATestLevelDirector::SyncPreviewEditsIntoScenarioFiltered(FScenarioSpec& Scenario)
{
	// First, update existing arms from spawned actors
	for (FArmSpawnSpec& ArmSpec : Scenario.Arms)
	{
		if (!ArmSpec.Spawned) continue;

		if (bCaptureRuntimeProgramEdits)
		{
			ArmSpec.Program = ArmSpec.Spawned->Program;
			ArmSpec.bLoopProgram = ArmSpec.Spawned->bLoopProgram;
		}
		if (bCaptureRuntimeFacingEdits)
		{
			ArmSpec.Facing = ArmSpec.Spawned->FacingDir;
		}
	}

	// NEW: Capture ALL current simulation state (including manually placed arms/tiles)
	if (Driver && Driver->GetSim())
	{
		USimWorldHex* Sim = Driver->GetSim();
		UWorld* W = GetWorld();
		if (!W) return;

		// Clear and rebuild tile specs from current simulation
		Scenario.Tiles.Empty();
		for (const auto& KV : Sim->TileMap)
		{
			const FHex& Hex = KV.Key;
			const FTile& Tile = KV.Value;
			
			FPlacedTileSpec TileSpec;
			TileSpec.Hex = Hex;
			TileSpec.Kind = Tile.Kind;
			TileSpec.Dir = Tile.Arg;
			Scenario.Tiles.Add(TileSpec);
			UE_LOG(LogTemp, Log, TEXT("TestLevelDirector: Captured tile at (%d,%d) type %d"), Hex.Q, Hex.R, (int32)Tile.Kind);
		}

		// Build a map of existing arm specs to preserve their original facing (if not capturing runtime edits)
		TMap<FHex, FArmSpawnSpec> ExistingArmSpecs;
		for (const FArmSpawnSpec& ExistingSpec : Scenario.Arms)
		{
			ExistingArmSpecs.Add(FHex(ExistingSpec.Q, ExistingSpec.R), ExistingSpec);
		}
		
		// Completely rebuild arm specs from current world state
		// This ensures removed arms are properly deleted from the scenario
		Scenario.Arms.Empty();
		
		for (TActorIterator<ABasicArm> It(W); It; ++It)
		{
			ABasicArm* Arm = *It;
			if (!Arm) continue;

			// Create new arm spec for every arm currently in the world
			FArmSpawnSpec NewArmSpec;
			NewArmSpec.Q = Arm->Q;
			NewArmSpec.R = Arm->R;
			
			// Preserve original facing unless we're explicitly capturing runtime facing edits
			FHex ArmHex(Arm->Q, Arm->R);
			if (!bCaptureRuntimeFacingEdits && ExistingArmSpecs.Contains(ArmHex))
			{
				// Use original facing from scenario to avoid rotation drift
				NewArmSpec.Facing = ExistingArmSpecs[ArmHex].Facing;
				UE_LOG(LogTemp, Log, TEXT("TestLevelDirector: Preserved original facing %d for arm at (%d,%d)"), 
					NewArmSpec.Facing, Arm->Q, Arm->R);
			}
			else if (!bCaptureRuntimeFacingEdits)
			{
				// For new arms (including blueprint-placed), use their HomeFacing from simulation (starting rotation)
				if (Sim && Arm->SimArmIndex >= 0 && Arm->SimArmIndex < Sim->Arms.Num())
				{
					const FArmState& ArmState = Sim->Arms[Arm->SimArmIndex];
					NewArmSpec.Facing = (uint8)ArmState.HomeFacing;
					UE_LOG(LogTemp, Log, TEXT("TestLevelDirector: Used HomeFacing %d from simulation for new arm at (%d,%d)"), 
						NewArmSpec.Facing, Arm->Q, Arm->R);
				}
				else
				{
					// Fallback to current facing if simulation not available
					NewArmSpec.Facing = Arm->FacingDir;
					UE_LOG(LogTemp, Warning, TEXT("TestLevelDirector: Fallback to current facing %d for arm at (%d,%d) (no sim state)"), 
						NewArmSpec.Facing, Arm->Q, Arm->R);
				}
			}
			else
			{
				// Use current facing when explicitly capturing runtime edits
				NewArmSpec.Facing = Arm->FacingDir;
				UE_LOG(LogTemp, Log, TEXT("TestLevelDirector: Captured current facing %d for arm at (%d,%d) (runtime capture enabled)"), 
					NewArmSpec.Facing, Arm->Q, Arm->R);
			}
			
			NewArmSpec.Program = Arm->Program;
			NewArmSpec.bLoopProgram = Arm->bLoopProgram;
			NewArmSpec.Spawned = Arm; // Keep reference
			Scenario.Arms.Add(NewArmSpec);
			
			// Enhanced logging to debug program capture
			UE_LOG(LogTemp, Warning, TEXT("TestLevelDirector: Captured arm at (%d,%d) SimIndex=%d with %d instructions"), 
				Arm->Q, Arm->R, Arm->SimArmIndex, Arm->Program.Num());
			for (int32 i = 0; i < FMath::Min(3, Arm->Program.Num()); i++)
			{
				const FArmInstr& Instr = Arm->Program[i];
				UE_LOG(LogTemp, Log, TEXT("  [%d]: Cmd=%d Arg=%d"), i, (int32)Instr.Cmd, Instr.Arg);
			}
		}

		UE_LOG(LogTemp, Warning, TEXT("TestLevelDirector: Captured %d tiles and %d arms to scenario"), 
			Scenario.Tiles.Num(), Scenario.Arms.Num());
	}
}
