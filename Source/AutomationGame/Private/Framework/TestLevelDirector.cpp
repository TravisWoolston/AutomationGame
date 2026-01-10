#include "Framework/TestLevelDirector.h"
#include "Framework/GameTickManager.h"
#include "Framework/GameManager.h"
#include "Actors/OrbSimDriver.h"
#include "Actors/HexFactoryActor.h"
#include "Actors/RequestBeacon.h"
#include "Actors/TriggerTile.h"
#include "Sim/SimWorldHex.h"
#include "Geometry/GoldbergGrid.h"
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
#if WITH_EDITOR
		SyncAllRuntimeArmEdits();  // Capture any program/facing changes from visual arms
#endif
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
			
			// Also destroy any residual walkers
			TArray<ABasicWalker*> WalkersToDestroy;
			for (TActorIterator<ABasicWalker> It(W); It; ++It)
			{
				WalkersToDestroy.Add(*It);
			}
			for (ABasicWalker* Walker : WalkersToDestroy)
			{
				if (Walker)
				{
					UE_LOG(LogTemp, Warning, TEXT("TestLevelDirector: Destroying walker at (%d,%d) SimIndex=%d during scenario reset"),
						Walker->Q, Walker->R, Walker->SimWalkerIndex);
					DestroyActorNow(Walker);
				}
			}
			
			// Also destroy any residual beacons
			TArray<ARequestBeacon*> BeaconsToDestroy;
			for (TActorIterator<ARequestBeacon> It(W); It; ++It)
			{
				BeaconsToDestroy.Add(*It);
			}
			for (ARequestBeacon* Beacon : BeaconsToDestroy)
			{
				if (Beacon)
				{
					UE_LOG(LogTemp, Warning, TEXT("TestLevelDirector: Destroying beacon at (%d,%d) during scenario reset"),
						Beacon->Q, Beacon->R);
					DestroyActorNow(Beacon);
				}
			}
			
			// Also destroy any residual trigger tiles
			TArray<ATriggerTile*> TriggerTilesToDestroy;
			for (TActorIterator<ATriggerTile> It(W); It; ++It)
			{
				TriggerTilesToDestroy.Add(*It);
			}
			for (ATriggerTile* TriggerTile : TriggerTilesToDestroy)
			{
				if (TriggerTile)
				{
					UE_LOG(LogTemp, Warning, TEXT("TestLevelDirector: Destroying TriggerTile at (%d,%d) during scenario reset"),
						TriggerTile->Q, TriggerTile->R);
					DestroyActorNow(TriggerTile);
				}
			}
		}
	}

	if (Driver && Driver->GetSim())
	{
		USimWorldHex* Sim = Driver->GetSim();
		const FScenarioSpec& Spec = Scenarios[ActiveScenarioIndex];

		// Reinitialize simulation (clears tiles, items, arms, walkers, bonds, etc.)
		Sim->Initialize(Spec.GridRadius);
		Sim->TileSizeUU = Spec.TileSizeUU;
		// Walkers array is cleared by Initialize

		// Ensure sim gets the correct grid parameters from OrbSimDriver
		const FVector AnchorLoc = Driver->GetGridAnchor() ? Driver->GetGridAnchor()->GetActorLocation() : FVector::ZeroVector;
		Sim->SetWorldTransform(AnchorLoc, Driver->GetGridYaw(), Driver->GetGridOrientation());
	}
}

void ATestLevelDirector::ClearArms()
{
	if (!GetWorld()) return;
	for (FScenarioSpec& Sc : Scenarios)
	{
		for (FArmSpawnSpec& S : Sc.Arms)
			if (S.Spawned)
			{
				DestroyActorNow(S.Spawned);
				S.Spawned = nullptr;
			}
		for (FWalkerSpawnSpec& W : Sc.Walkers)
			if (W.Spawned)
			{
				DestroyActorNow(W.Spawned);
				W.Spawned = nullptr;
			}
		for (FRequestBeaconSpec& B : Sc.Beacons)
			if (B.Spawned)
			{
				DestroyActorNow(B.Spawned);
				B.Spawned = nullptr;
			}
		for (FTriggerTileSpec& T : Sc.TriggerTiles)
			if (T.Spawned)
			{
				DestroyActorNow(T.Spawned);
				T.Spawned = nullptr;
			}
	}
}

void ATestLevelDirector::ApplyScenario(const FScenarioSpec& S)
{
	// Note: We already synced current state to scenario in ResetSimForActiveScenario()
	// Don't sync here as it would overwrite the scenario we're trying to load
	ClearArms();

	UWorld* W = GetWorld();
	if (!W || !Driver) return;

	// Find GameManager for Goldberg mode check
	AGameManager* GM = nullptr;
	for (TActorIterator<AGameManager> It(W); It; ++It)
	{
		GM = *It;
		break;
	}
	const bool bGoldbergMode = GM && GM->IsGoldbergMode();
	UGoldbergGrid* GoldbergGrid = bGoldbergMode ? GM->GetGoldbergGrid() : nullptr;

	if (USimWorldHex* Sim = Driver->GetSim())
	{
		// Build a map of hex -> orb type for ClusterOutput tiles from the saved specs
		TMap<FHex, uint8> ClusterOutputOrbTypes;
		for (const FPlacedTileSpec& P : S.Tiles)
		{
			if (P.Kind == ETileKind::ClusterOutput)
			{
				ClusterOutputOrbTypes.Add(P.Hex, P.OrbType);
			}
		}
		
		for (const FPlacedTileSpec& P : S.Tiles)
		{
			// In Goldberg mode, validate that the hex has a corresponding cell
			if (bGoldbergMode && GoldbergGrid)
			{
				int32 CellIndex = GoldbergGrid->GetCellIndexForHex(P.Hex);
				if (CellIndex == INDEX_NONE)
				{
					// Invalid hex (out of range or pentagon) - skip tile
					UE_LOG(LogTemp, Warning, TEXT("ApplyScenario: Skipping tile at (%d,%d) - no valid Goldberg cell"), 
						P.Hex.Q, P.Hex.R);
					continue;
				}
			}
			
			FTile T; T.Kind = P.Kind; T.Arg = P.Dir;
			Sim->PlaceTile(P.Hex, T);
		}
		
		// Rebuild cluster output relationships after loading all tiles, using saved orb types
		Sim->RebuildClusterOutputRelationships(ClusterOutputOrbTypes);
	}

	for (FArmSpawnSpec& A : const_cast<TArray<FArmSpawnSpec>&>(S.Arms))
	{
		if (!ArmClass) continue;
		
		// Skip if this arm is already spawned (shouldn't happen, but prevents duplicates)
		if (A.Spawned && IsValid(A.Spawned))
		{
			UE_LOG(LogTemp, Warning, TEXT("ApplyScenario: Arm at (%d,%d) already spawned, skipping"), A.Q, A.R);
			continue;
		}

		// Spawn deferred so we can finish configuration before BeginPlay-style logic
		FTransform Tm;
		Tm.SetLocation(Driver->GetGridAnchor() ? Driver->GetGridAnchor()->GetActorLocation() : FVector::ZeroVector);

		A.Spawned = W->SpawnActorDeferred<ABasicArm>(ArmClass, Tm, nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (!A.Spawned) continue;

		// Basic spatial init - use StartingRotation for initial facing
		A.Spawned->InitializeOnGrid(Driver, A.Q, A.R, A.Level, A.Facing % 6);
		A.Spawned->FacingDir = A.Facing % 6;

		// Set starting rotation and reach for Reset command
		A.Spawned->StartingRotation = A.StartingRotation;
		A.Spawned->StartingReach = A.StartingReach;

		int32 ArmIndex = INDEX_NONE;
		if (USimWorldHex* Sim = Driver->GetSim())
		{
			// Build initial sim-side arm state (tape comes from scenario program)
			FArmState ArmState;
			ArmState.BaseHex = FHex(A.Q, A.R, A.Level);
			ArmState.Facing = (EHexDir)(A.Facing % 6);  // Use saved Facing for initial facing
			ArmState.HomeFacing = (EHexDir)(A.StartingRotation % 6);
			ArmState.HomeArmLen = A.StartingReach;
			ArmState.HomeTrackIdx = 0;
			ArmState.ArmLen = A.StartingReach;
			ArmState.TrackIdx = 0;
			ArmState.PC = 0;
			ArmState.bHolding = false;
			ArmState.Tape = ConvertProgramToOps_TLD(A.Program);

			ArmIndex = Sim->AddArm(ArmState);
			A.Spawned->SimArmIndex = ArmIndex;

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

	// Spawn walkers from scenario (GM already found above)

	for (FWalkerSpawnSpec& WalkerSpec : const_cast<TArray<FWalkerSpawnSpec>&>(S.Walkers))
	{
		if (!GM || !GM->BasicWalkerClass) continue;
		
		// Skip if this walker is already spawned
		if (WalkerSpec.Spawned && IsValid(WalkerSpec.Spawned))
		{
			UE_LOG(LogTemp, Warning, TEXT("ApplyScenario: Walker at (%d,%d) already spawned, skipping"), WalkerSpec.Q, WalkerSpec.R);
			continue;
		}

		// Spawn walker using GameManager
		FHex WalkerHex(WalkerSpec.Q, WalkerSpec.R);
		WalkerSpec.Spawned = GM->SpawnBasicWalkerAt(WalkerHex, WalkerSpec.Program, WalkerSpec.bLoopProgram);
		
		if (WalkerSpec.Spawned && Driver && Driver->GetSim())
		{
			USimWorldHex* Sim = Driver->GetSim();
			// Update the walker's home hex to match StartingLocation
			if (WalkerSpec.Spawned->SimWalkerIndex >= 0 && WalkerSpec.Spawned->SimWalkerIndex < Sim->Walkers.Num())
			{
				FWalkerState& WalkerState = Sim->Walkers[WalkerSpec.Spawned->SimWalkerIndex];
				WalkerState.HomeHex = WalkerSpec.StartingLocation;
				WalkerState.HomeHex.Level = WalkerSpec.Level; // Ensure level is set
				WalkerSpec.Spawned->HomeHex = WalkerSpec.StartingLocation;
				WalkerSpec.Spawned->HomeHex.Level = WalkerSpec.Level;
			}
		}
	}

	// Spawn beacons from scenario
	if (!BeaconClass)
	{
		// Try to get from GameManager
		if (GM && GM->RequestBeaconClass)
		{
			BeaconClass = GM->RequestBeaconClass;
		}
	}

	for (FRequestBeaconSpec& BeaconSpec : const_cast<TArray<FRequestBeaconSpec>&>(S.Beacons))
	{
		if (!BeaconClass) continue;
		
		// Skip if already spawned
		if (BeaconSpec.Spawned && IsValid(BeaconSpec.Spawned))
		{
			UE_LOG(LogTemp, Warning, TEXT("ApplyScenario: Beacon at (%d,%d) already spawned, skipping"), BeaconSpec.Q, BeaconSpec.R);
			continue;
		}

		FHex BeaconHex(BeaconSpec.Q, BeaconSpec.R, BeaconSpec.Level);
		FVector Location = Driver->MapHexToWorld(BeaconHex);
		FRotator Rotation(0.f, BeaconSpec.Rotation, 0.f);
		
		BeaconSpec.Spawned = W->SpawnActor<ARequestBeacon>(BeaconClass, Location, Rotation);
		if (BeaconSpec.Spawned)
		{
			BeaconSpec.Spawned->Q = BeaconSpec.Q;
			BeaconSpec.Spawned->R = BeaconSpec.R;
			BeaconSpec.Spawned->Level = BeaconSpec.Level;
			BeaconSpec.Spawned->BeaconType = BeaconSpec.BeaconType;
			BeaconSpec.Spawned->BeaconRole = BeaconSpec.BeaconRole;
			
			// DropOff beacons start as Ready, Pickup beacons start as NotReady (CheckReadiness will update)
			EBeaconState InitialState = (BeaconSpec.BeaconRole == EBeaconRole::DropOff) ? EBeaconState::Ready : EBeaconState::NotReady;
			BeaconSpec.Spawned->SetState(InitialState);
			BeaconSpec.Spawned->SyncWorldLocation();
			
			UE_LOG(LogTemp, Log, TEXT("TestLevelDirector: Spawned beacon at (%d,%d) Role=%d State=%d"),
				BeaconSpec.Q, BeaconSpec.R, (int32)BeaconSpec.BeaconRole, (int32)InitialState);
		}
	}

	// Re-link beacon pairs after all beacons are spawned
	for (FRequestBeaconSpec& BeaconSpec : const_cast<TArray<FRequestBeaconSpec>&>(S.Beacons))
	{
		if (!BeaconSpec.Spawned) continue;
		if (BeaconSpec.PairedBeaconRole == EBeaconRole::None) continue; // No pair

		// Find paired beacon
		for (FRequestBeaconSpec& OtherSpec : const_cast<TArray<FRequestBeaconSpec>&>(S.Beacons))
		{
			if (!OtherSpec.Spawned) continue;
			if (OtherSpec.Q == BeaconSpec.PairedBeaconQ && 
				OtherSpec.R == BeaconSpec.PairedBeaconR && 
				OtherSpec.BeaconRole == BeaconSpec.PairedBeaconRole)
			{
				// Set bidirectional pairing
				BeaconSpec.Spawned->PairedBeacon = OtherSpec.Spawned;
				OtherSpec.Spawned->PairedBeacon = BeaconSpec.Spawned;
				
				UE_LOG(LogTemp, Log, TEXT("TestLevelDirector: Linked beacon at (%d,%d) Role=%d to paired beacon at (%d,%d) Role=%d"),
					BeaconSpec.Q, BeaconSpec.R, (int32)BeaconSpec.BeaconRole,
					OtherSpec.Q, OtherSpec.R, (int32)OtherSpec.BeaconRole);
				break;
			}
		}
	}

	// Spawn trigger tiles from scenario
	UClass* TriggerTileClassToUse = TriggerTileClass ? TriggerTileClass.Get() : nullptr;
	if (!TriggerTileClassToUse && GM && GM->TriggerTileClass)
	{
		TriggerTileClassToUse = GM->TriggerTileClass.Get();
	}

	for (FTriggerTileSpec& TriggerSpec : const_cast<TArray<FTriggerTileSpec>&>(S.TriggerTiles))
	{
		if (!TriggerTileClassToUse) continue;
		
		// Skip if already spawned
		if (TriggerSpec.Spawned && IsValid(TriggerSpec.Spawned))
		{
			UE_LOG(LogTemp, Warning, TEXT("ApplyScenario: TriggerTile at (%d,%d) already spawned, skipping"), TriggerSpec.Q, TriggerSpec.R);
			continue;
		}

		FHex TriggerHex(TriggerSpec.Q, TriggerSpec.R, TriggerSpec.Level);
		FVector Location = Driver->MapHexToWorld(TriggerHex);
		FRotator Rotation(0.f, TriggerSpec.Rotation, 0.f);
		
		TriggerSpec.Spawned = W->SpawnActor<ATriggerTile>(TriggerTileClassToUse, Location, Rotation);
		if (TriggerSpec.Spawned)
		{
			TriggerSpec.Spawned->Q = TriggerSpec.Q;
			TriggerSpec.Spawned->R = TriggerSpec.R;
			TriggerSpec.Spawned->Level = TriggerSpec.Level;
			TriggerSpec.Spawned->TriggerOrbType = TriggerSpec.TriggerOrbType;
			TriggerSpec.Spawned->Variant = TriggerSpec.Variant;
			if (TriggerSpec.OwnerType == ETriggerTileOwnerType::Belt)
				TriggerSpec.Spawned->SetOwnerBelt(TriggerSpec.OwnerQ, TriggerSpec.OwnerR);
			else
				TriggerSpec.Spawned->SetOwnerArm(TriggerSpec.OwnerQ, TriggerSpec.OwnerR);
			TriggerSpec.Spawned->SyncWorldLocation();
			TriggerSpec.Spawned->UpdateVisuals();
			
			UE_LOG(LogTemp, Log, TEXT("TestLevelDirector: Spawned TriggerTile at (%d,%d) Type=%s Owner=(%d,%d)"),
				TriggerSpec.Q, TriggerSpec.R, *UEnum::GetValueAsString(TriggerSpec.TriggerOrbType),
				TriggerSpec.OwnerQ, TriggerSpec.OwnerR);
		}
	}

	if (TickManager && S.bAutoPlay)
	{
		TickManager->Play();
	}
}

void ATestLevelDirector::SpawnScenarioPreview()
{
	// DEPRECATED: This function caused duplicate arm spawning issues
	// Arms are now only spawned through ApplyScenario during PIE
	// Keeping this stub in case it's called from blueprints
	UE_LOG(LogTemp, Warning, TEXT("SpawnScenarioPreview called but is deprecated - use BuildActiveScenario instead"));
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
			Arm->InitializeOnGrid(Driver, ArmSpec.Q, ArmSpec.R, ArmSpec.Level, ArmSpec.Facing % 6);
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
		if (T.Hex.Q == Hex.Q && T.Hex.R == Hex.R && T.Hex.Level == Hex.Level)
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

void ATestLevelDirector::UpsertClusterOutputTileSpec(const FHex& Hex, uint8 TemplateId, uint8 OrbType)
{
	if (!Scenarios.IsValidIndex(ActiveScenarioIndex)) return;
	FScenarioSpec& Sc = Scenarios[ActiveScenarioIndex];

	for (FPlacedTileSpec& T : Sc.Tiles)
	{
		if (T.Hex.Q == Hex.Q && T.Hex.R == Hex.R && T.Hex.Level == Hex.Level)
		{
			T.Kind = ETileKind::ClusterOutput;
			T.Dir = TemplateId;
			T.OrbType = OrbType;
			MarkActiveScenarioDirty();
#if WITH_EDITOR
			MirrorScenarioEditsToEditor();
#endif
			return;
		}
	}
	FPlacedTileSpec NewSpec;
	NewSpec.Hex = Hex;
	NewSpec.Kind = ETileKind::ClusterOutput;
	NewSpec.Dir = TemplateId;
	NewSpec.OrbType = OrbType;
	Sc.Tiles.Add(NewSpec);
	MarkActiveScenarioDirty();
#if WITH_EDITOR
	MirrorScenarioEditsToEditor();
#endif
}

void ATestLevelDirector::SyncClusterOutputTiles()
{
	if (!Driver) return;
	USimWorldHex* Sim = Driver->GetSim();
	if (!Sim) return;
	
	// Sync ALL ClusterOutput tiles, not just merged ones
	// This ensures proper saving of orb types for all cluster configurations
	const TMap<FHex, FTile>& TileMap = Sim->TileMap;
	const TMap<uint8, USimWorldHex::FClusterTemplate>& Templates = Sim->GetClusterTemplates();
	
	// Group ClusterOutput tiles by template ID
	TMap<uint8, TArray<FHex>> TilesByTemplate;
	for (const auto& TileKV : TileMap)
	{
		if (TileKV.Value.Kind == ETileKind::ClusterOutput)
		{
			uint8 TemplateId = TileKV.Value.Arg;
			TilesByTemplate.FindOrAdd(TemplateId).Add(TileKV.Key);
		}
	}
	
	// For each template group, find the BaseHex and sync orb types
	for (const auto& TemplateGroup : TilesByTemplate)
	{
		uint8 TemplateId = TemplateGroup.Key;
		const TArray<FHex>& Tiles = TemplateGroup.Value;
		
		if (Tiles.Num() == 0) continue;
		
		const USimWorldHex::FClusterTemplate* Template = Templates.Find(TemplateId);
		if (!Template) continue;
		
		// Find the center-most tile as BaseHex (same logic as RebuildClusterOutputRelationships)
		int32 SumQ = 0;
		int32 SumR = 0;
		for (const FHex& H : Tiles)
		{
			SumQ += H.Q;
			SumR += H.R;
		}
		
		int32 CenterQ = FMath::RoundToInt((float)SumQ / Tiles.Num());
		int32 CenterR = FMath::RoundToInt((float)SumR / Tiles.Num());
		
		FHex BaseHex = Tiles[0];
		int32 MinDist = INT_MAX;
		for (const FHex& H : Tiles)
		{
			int32 dQ = H.Q - CenterQ;
			int32 dR = H.R - CenterR;
			int32 dist = FMath::Abs(dQ) + FMath::Abs(dR) + FMath::Abs(dQ + dR);
			if (dist < MinDist)
			{
				MinDist = dist;
				BaseHex = H;
			}
		}
		
		// Now update each tile with its orb type from the template
		for (const FHex& TileHex : Tiles)
		{
			// Calculate this tile's relative position from BaseHex
			int32 RelQ = TileHex.Q - BaseHex.Q;
			int32 RelR = TileHex.R - BaseHex.R;
			
			// Find matching orb in template
			uint8 OrbType = 0;
			for (const FOrbData& TemplateOrb : Template->Orbs)
			{
				if (TemplateOrb.dQ == RelQ && TemplateOrb.dR == RelR)
				{
					OrbType = TemplateOrb.Type;
					break;
				}
			}
			
			// Update the scenario
			UpsertClusterOutputTileSpec(TileHex, TemplateId, OrbType);
			
			UE_LOG(LogTemp, Log, TEXT("SyncClusterOutputTiles: Updated tile at (%d,%d) - Template %d, Type %d"), 
				TileHex.Q, TileHex.R, TemplateId, OrbType);
		}
	}
}

bool ATestLevelDirector::RemoveTileSpec(const FHex& Hex)
{
	if (!Scenarios.IsValidIndex(ActiveScenarioIndex)) return false;
	FScenarioSpec& Sc = Scenarios[ActiveScenarioIndex];
	for (int32 i = 0; i < Sc.Tiles.Num(); ++i)
	{
		if (Sc.Tiles[i].Hex.Q == Hex.Q && Sc.Tiles[i].Hex.R == Hex.R && Sc.Tiles[i].Hex.Level == Hex.Level)
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

void ATestLevelDirector::UpsertArmSpec(int32 Q, int32 R, uint8 Facing, const TArray<FArmInstr>& Program, bool bLoop, int32 Level)
{
	if (!Scenarios.IsValidIndex(ActiveScenarioIndex)) return;
	FScenarioSpec& Sc = Scenarios[ActiveScenarioIndex];
	
	auto FindRuntimeArmAt = [&](int32 InQ, int32 InR, int32 InLevel) -> ABasicArm*
		{
			if (!GetWorld()) return nullptr;
			for (TActorIterator<ABasicArm> It(GetWorld()); It; ++It)
			{
				if (It->Q == InQ && It->R == InR && It->Level == InLevel)
				{
					return *It;
				}
			}
			return nullptr;
		};
	
	for (FArmSpawnSpec& A : Sc.Arms)
	{
		if (A.Q == Q && A.R == R && A.Level == Level)
		{
			A.Facing = Facing % 6;
			A.Program = Program;
			A.bLoopProgram = bLoop;
			if (!A.Spawned) A.Spawned = FindRuntimeArmAt(Q, R, Level);
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
	NewA.Level = Level;
	NewA.Facing = Facing % 6;
	NewA.StartingRotation = Facing % 6;  // Initialize to placement direction
	NewA.StartingReach = 1;              // Default reach
	NewA.Program = Program;
	NewA.bLoopProgram = bLoop;
	NewA.Spawned = FindRuntimeArmAt(Q, R, Level);   // capture runtime actor if already spawned
	Sc.Arms.Add(NewA);
	MarkActiveScenarioDirty();
#if WITH_EDITOR
	MirrorScenarioEditsToEditor();
#endif
}

bool ATestLevelDirector::RemoveArmSpec(int32 Q, int32 R, int32 Level)
{
	if (!Scenarios.IsValidIndex(ActiveScenarioIndex)) return false;
	FScenarioSpec& Sc = Scenarios[ActiveScenarioIndex];
	
	for (int32 i = 0; i < Sc.Arms.Num(); ++i)
	{
		if (Sc.Arms[i].Q == Q && Sc.Arms[i].R == R && Sc.Arms[i].Level == Level)
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
					if (It->Q == Q && It->R == R && It->Level == Level)
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
			if (It->Q == Q && It->R == R && It->Level == Level)
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

void ATestLevelDirector::UpsertWalkerSpec(int32 Q, int32 R, const FHex& StartingLocation, const TArray<FWalkerInstr>& Program, bool bLoop, int32 Level)
{
	if (!Scenarios.IsValidIndex(ActiveScenarioIndex)) return;
	FScenarioSpec& Sc = Scenarios[ActiveScenarioIndex];
	
	auto FindRuntimeWalkerAt = [&](int32 InQ, int32 InR, int32 InLevel) -> ABasicWalker*
		{
			if (!GetWorld()) return nullptr;
			for (TActorIterator<ABasicWalker> It(GetWorld()); It; ++It)
			{
				if (It->Q == InQ && It->R == InR && It->Level == InLevel)
				{
					return *It;
				}
			}
			return nullptr;
		};
	
	for (FWalkerSpawnSpec& W : Sc.Walkers)
	{
		if (W.Q == Q && W.R == R && W.Level == Level)
		{
			W.StartingLocation = StartingLocation;
			W.Program = Program;
			W.bLoopProgram = bLoop;
			if (!W.Spawned) W.Spawned = FindRuntimeWalkerAt(Q, R, Level);
			MarkActiveScenarioDirty();
#if WITH_EDITOR
			MirrorScenarioEditsToEditor();
#endif
			return;
		}
	}
	
	FWalkerSpawnSpec NewW;
	NewW.Q = Q;
	NewW.R = R;
	NewW.Level = Level;
	NewW.StartingLocation = StartingLocation;
	NewW.Program = Program;
	NewW.bLoopProgram = bLoop;
	NewW.Spawned = FindRuntimeWalkerAt(Q, R, Level);   // capture runtime actor if already spawned
	Sc.Walkers.Add(NewW);
	MarkActiveScenarioDirty();
#if WITH_EDITOR
	MirrorScenarioEditsToEditor();
#endif
}

bool ATestLevelDirector::RemoveWalkerSpec(int32 Q, int32 R, int32 Level)
{
	if (!Scenarios.IsValidIndex(ActiveScenarioIndex)) return false;
	FScenarioSpec& Sc = Scenarios[ActiveScenarioIndex];
	
	for (int32 i = 0; i < Sc.Walkers.Num(); ++i)
	{
		if (Sc.Walkers[i].Q == Q && Sc.Walkers[i].R == R && Sc.Walkers[i].Level == Level)
		{
			if (Sc.Walkers[i].Spawned)
			{
				DestroyActorNow(Sc.Walkers[i].Spawned);
				Sc.Walkers[i].Spawned = nullptr;
			}
			else if (UWorld* W = GetWorld())
			{
				for (TActorIterator<ABasicWalker> It(W); It; ++It)
				{
					if (It->Q == Q && It->R == R && It->Level == Level)
					{
						DestroyActorNow(*It);
						break;
					}
				}
			}
			
			Sc.Walkers.RemoveAt(i);
			MarkActiveScenarioDirty();
#if WITH_EDITOR
			MirrorScenarioEditsToEditor();
#endif
			return true;
		}
	}
	
	if (UWorld* W = GetWorld())
	{
		for (TActorIterator<ABasicWalker> It(W); It; ++It)
		{
			if (It->Q == Q && It->R == R && It->Level == Level)
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

void ATestLevelDirector::UpsertBeaconSpec(int32 Q, int32 R, EBeaconType BeaconType, EBeaconRole BeaconRole, float Rotation, int32 Level)
{
	if (!Scenarios.IsValidIndex(ActiveScenarioIndex)) return;
	FScenarioSpec& Sc = Scenarios[ActiveScenarioIndex];
	
	auto FindRuntimeBeaconAt = [&](int32 InQ, int32 InR, EBeaconRole InRole, int32 InLevel) -> ARequestBeacon*
		{
			if (!GetWorld()) return nullptr;
			for (TActorIterator<ARequestBeacon> It(GetWorld()); It; ++It)
			{
				if (It->Q == InQ && It->R == InR && It->BeaconRole == InRole && It->Level == InLevel)
				{
					return *It;
				}
			}
			return nullptr;
		};
	
	for (FRequestBeaconSpec& B : Sc.Beacons)
	{
		if (B.Q == Q && B.R == R && B.BeaconRole == BeaconRole && B.Level == Level)
		{
			B.BeaconType = BeaconType;
			B.Rotation = Rotation;
			if (!B.Spawned) B.Spawned = FindRuntimeBeaconAt(Q, R, BeaconRole, Level);
			
			// Update paired beacon info from runtime
			if (B.Spawned && B.Spawned->PairedBeacon && IsValid(B.Spawned->PairedBeacon))
			{
				B.PairedBeaconQ = B.Spawned->PairedBeacon->Q;
				B.PairedBeaconR = B.Spawned->PairedBeacon->R;
				B.PairedBeaconRole = B.Spawned->PairedBeacon->BeaconRole;
			}
			
			MarkActiveScenarioDirty();
#if WITH_EDITOR
			MirrorScenarioEditsToEditor();
#endif
			return;
		}
	}
	
	FRequestBeaconSpec NewB;
	NewB.Q = Q;
	NewB.R = R;
	NewB.Level = Level;
	NewB.BeaconType = BeaconType;
	NewB.BeaconRole = BeaconRole;
	NewB.Rotation = Rotation;
	NewB.Spawned = FindRuntimeBeaconAt(Q, R, BeaconRole, Level);
	
	// Save paired beacon info from runtime
	if (NewB.Spawned && NewB.Spawned->PairedBeacon && IsValid(NewB.Spawned->PairedBeacon))
	{
		NewB.PairedBeaconQ = NewB.Spawned->PairedBeacon->Q;
		NewB.PairedBeaconR = NewB.Spawned->PairedBeacon->R;
		NewB.PairedBeaconRole = NewB.Spawned->PairedBeacon->BeaconRole;
	}
	
	Sc.Beacons.Add(NewB);
	MarkActiveScenarioDirty();
#if WITH_EDITOR
	MirrorScenarioEditsToEditor();
#endif
}

bool ATestLevelDirector::RemoveBeaconSpec(int32 Q, int32 R, int32 Level)
{
	if (!Scenarios.IsValidIndex(ActiveScenarioIndex)) return false;
	FScenarioSpec& Sc = Scenarios[ActiveScenarioIndex];
	
	bool bRemovedAny = false;
	
	// Remove all beacons at this hex (both Pickup and DropOff if at same location)
	for (int32 i = Sc.Beacons.Num() - 1; i >= 0; --i)
	{
		if (Sc.Beacons[i].Q == Q && Sc.Beacons[i].R == R && Sc.Beacons[i].Level == Level)
		{
			if (Sc.Beacons[i].Spawned)
			{
				DestroyActorNow(Sc.Beacons[i].Spawned);
				Sc.Beacons[i].Spawned = nullptr;
			}
			else if (UWorld* W = GetWorld())
			{
				for (TActorIterator<ARequestBeacon> It(W); It; ++It)
				{
					if (It->Q == Q && It->R == R && It->BeaconRole == Sc.Beacons[i].BeaconRole && It->Level == Level)
					{
						DestroyActorNow(*It);
						break;
					}
				}
			}
			
			Sc.Beacons.RemoveAt(i);
			bRemovedAny = true;
		}
	}
	
	if (bRemovedAny)
	{
		MarkActiveScenarioDirty();
#if WITH_EDITOR
		MirrorScenarioEditsToEditor();
#endif
		return true;
	}
	
	// Fallback: remove any runtime beacons not in specs
	if (UWorld* W = GetWorld())
	{
		for (TActorIterator<ARequestBeacon> It(W); It; ++It)
		{
			if (It->Q == Q && It->R == R && It->Level == Level)
			{
				DestroyActorNow(*It);
				bRemovedAny = true;
			}
		}
	}
	
	if (bRemovedAny)
	{
		MarkActiveScenarioDirty();
#if WITH_EDITOR
		MirrorScenarioEditsToEditor();
#endif
	}
	
	return bRemovedAny;
}

void ATestLevelDirector::UpsertTriggerTileSpec(int32 Q, int32 R, EOrbType TriggerOrbType, ETriggerVariant Variant, int32 OwnerQ, int32 OwnerR, ETriggerTileOwnerType OwnerType, float Rotation, int32 Level)
{
	if (!Scenarios.IsValidIndex(ActiveScenarioIndex)) return;
	FScenarioSpec& Sc = Scenarios[ActiveScenarioIndex];
	
	auto FindRuntimeTriggerTileAt = [&](int32 InQ, int32 InR, int32 InLevel) -> ATriggerTile*
		{
			if (!GetWorld()) return nullptr;
			for (TActorIterator<ATriggerTile> It(GetWorld()); It; ++It)
			{
				if (It->Q == InQ && It->R == InR && It->Level == InLevel)
				{
					return *It;
				}
			}
			return nullptr;
		};
	
	for (FTriggerTileSpec& T : Sc.TriggerTiles)
	{
		if (T.Q == Q && T.R == R && T.Level == Level)
		{
			T.TriggerOrbType = TriggerOrbType;
			T.Variant = Variant;
			T.OwnerQ = OwnerQ;
			T.OwnerR = OwnerR;
			T.OwnerType = OwnerType;
			T.Rotation = Rotation;
			if (!T.Spawned) T.Spawned = FindRuntimeTriggerTileAt(Q, R, Level);
			MarkActiveScenarioDirty();
#if WITH_EDITOR
			MirrorScenarioEditsToEditor();
#endif
			return;
		}
	}
	
	FTriggerTileSpec NewT;
	NewT.Q = Q;
	NewT.R = R;
	NewT.Level = Level;
	NewT.TriggerOrbType = TriggerOrbType;
	NewT.Variant = Variant;
	NewT.OwnerQ = OwnerQ;
	NewT.OwnerR = OwnerR;
	NewT.OwnerType = OwnerType;
	NewT.Rotation = Rotation;
	NewT.Spawned = FindRuntimeTriggerTileAt(Q, R, Level);
	
	Sc.TriggerTiles.Add(NewT);
	MarkActiveScenarioDirty();
#if WITH_EDITOR
	MirrorScenarioEditsToEditor();
#endif
}

bool ATestLevelDirector::RemoveTriggerTileSpec(int32 Q, int32 R, int32 Level)
{
	if (!Scenarios.IsValidIndex(ActiveScenarioIndex)) return false;
	FScenarioSpec& Sc = Scenarios[ActiveScenarioIndex];
	
	bool bRemovedAny = false;
	
	// Remove all trigger tiles at this hex
	for (int32 i = Sc.TriggerTiles.Num() - 1; i >= 0; --i)
	{
		if (Sc.TriggerTiles[i].Q == Q && Sc.TriggerTiles[i].R == R && Sc.TriggerTiles[i].Level == Level)
		{
			if (Sc.TriggerTiles[i].Spawned)
			{
				DestroyActorNow(Sc.TriggerTiles[i].Spawned);
				Sc.TriggerTiles[i].Spawned = nullptr;
			}
			else if (UWorld* W = GetWorld())
			{
				for (TActorIterator<ATriggerTile> It(W); It; ++It)
				{
					if (It->Q == Q && It->R == R && It->Level == Level)
					{
						DestroyActorNow(*It);
						break;
					}
				}
			}
			
			Sc.TriggerTiles.RemoveAt(i);
			bRemovedAny = true;
		}
	}
	
	if (bRemovedAny)
	{
		MarkActiveScenarioDirty();
#if WITH_EDITOR
		MirrorScenarioEditsToEditor();
#endif
		return true;
	}
	
	// Fallback: remove any runtime trigger tiles not in specs
	if (UWorld* W = GetWorld())
	{
		for (TActorIterator<ATriggerTile> It(W); It; ++It)
		{
			if (It->Q == Q && It->R == R && It->Level == Level)
			{
				DestroyActorNow(*It);
				bRemovedAny = true;
			}
		}
	}
	
	if (bRemovedAny)
	{
		MarkActiveScenarioDirty();
#if WITH_EDITOR
		MirrorScenarioEditsToEditor();
#endif
	}
	
	return bRemovedAny;
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
#if WITH_EDITOR
	SyncAllRuntimeArmEdits();
#endif

	// Mirror one last time to editor world
	MirrorScenarioEditsToEditor();
}

#if WITH_EDITOR
void ATestLevelDirector::SyncAllRuntimeArmEdits()
{
	// Only sync the active scenario - other scenarios should not be overwritten
	if (Scenarios.IsValidIndex(ActiveScenarioIndex))
	{
		SyncPreviewEditsIntoScenarioFiltered(Scenarios[ActiveScenarioIndex]);
	}
}
#endif

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
			// Only rebuild scenario when in PIE, not in editor
			// Editor spawning causes duplicate arms when switching to PIE
			if (GetWorld() && GetWorld()->IsPlayInEditor())
			{
				BuildActiveScenario();
			}
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
			// UE_LOG(LogTemp, Log, TEXT("TestLevelDirector: Captured tile at (%d,%d) type %d"), Hex.Q, Hex.R, (int32)Tile.Kind);
		}
		
		// Update ClusterOutput tiles with their orb types from MergedClusterOutputs
		const TMap<FHex, FMergedTileInfo>& MergedOutputs = Sim->GetMergedClusterOutputs();
		for (const auto& MergedKV : MergedOutputs)
		{
			const FMergedTileInfo& Info = MergedKV.Value;
			
			// Find and update the tile specs for both hexes
			for (FPlacedTileSpec& TileSpec : Scenario.Tiles)
			{
				if (TileSpec.Hex == Info.Hex1)
				{
					TileSpec.OrbType = Info.OrbType1;
				}
				else if (TileSpec.Hex == Info.Hex2)
				{
					TileSpec.OrbType = Info.OrbType2;
				}
			}
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
			NewArmSpec.StartingRotation = Arm->StartingRotation;
			NewArmSpec.StartingReach = Arm->StartingReach;

			// Preserve original facing unless we're explicitly capturing runtime facing edits
			FHex ArmHex(Arm->Q, Arm->R);
			if (!bCaptureRuntimeFacingEdits && ExistingArmSpecs.Contains(ArmHex))
			{
				// Use original facing from scenario to avoid rotation drift
				NewArmSpec.Facing = ExistingArmSpecs[ArmHex].Facing;

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
			NewArmSpec.StartingRotation = Arm->StartingRotation;
			NewArmSpec.StartingReach = Arm->StartingReach;
			NewArmSpec.Spawned = Arm; // Keep reference
			Scenario.Arms.Add(NewArmSpec);
		}

		// Preserve existing walker specs (for program data) unless capturing edits
		TMap<FHex, FWalkerSpawnSpec> ExistingWalkerSpecs;
		for (const FWalkerSpawnSpec& ExistingWalker : Scenario.Walkers)
		{
			ExistingWalkerSpecs.Add(FHex(ExistingWalker.Q, ExistingWalker.R), ExistingWalker);
		}

		Scenario.Walkers.Empty();

		for (TActorIterator<ABasicWalker> It(W); It; ++It)
		{
			ABasicWalker* Walker = *It;
			if (!Walker) continue;

			FWalkerSpawnSpec NewWalkerSpec;
			NewWalkerSpec.Q = Walker->Q;
			NewWalkerSpec.R = Walker->R;
			NewWalkerSpec.StartingLocation = Walker->HomeHex;
			NewWalkerSpec.Spawned = Walker;

			FHex WalkerHex(Walker->Q, Walker->R);
			if (!bCaptureRuntimeProgramEdits && ExistingWalkerSpecs.Contains(WalkerHex))
			{
				NewWalkerSpec.Program = ExistingWalkerSpecs[WalkerHex].Program;
				NewWalkerSpec.bLoopProgram = ExistingWalkerSpecs[WalkerHex].bLoopProgram;
			}
			else
			{
				NewWalkerSpec.Program = Walker->Program;
				NewWalkerSpec.bLoopProgram = Walker->bLoopProgram;
			}

			Scenario.Walkers.Add(NewWalkerSpec);
		}

		// Capture request beacons
		Scenario.Beacons.Empty();
		for (TActorIterator<ARequestBeacon> It(W); It; ++It)
		{
			ARequestBeacon* Beacon = *It;
			if (!Beacon) continue;

			FRequestBeaconSpec NewBeaconSpec;
			NewBeaconSpec.Q = Beacon->Q;
			NewBeaconSpec.R = Beacon->R;
			NewBeaconSpec.BeaconType = Beacon->BeaconType;
			NewBeaconSpec.BeaconRole = Beacon->BeaconRole;
			NewBeaconSpec.Rotation = Beacon->GetActorRotation().Yaw;
			NewBeaconSpec.Spawned = Beacon;
			
			// Save paired beacon info
			if (Beacon->PairedBeacon && IsValid(Beacon->PairedBeacon))
			{
				NewBeaconSpec.PairedBeaconQ = Beacon->PairedBeacon->Q;
				NewBeaconSpec.PairedBeaconR = Beacon->PairedBeacon->R;
				NewBeaconSpec.PairedBeaconRole = Beacon->PairedBeacon->BeaconRole;
			}
			else
			{
				NewBeaconSpec.PairedBeaconRole = EBeaconRole::None;
			}
			
			Scenario.Beacons.Add(NewBeaconSpec);
		}

		// Capture trigger tiles
		Scenario.TriggerTiles.Empty();
		for (TActorIterator<ATriggerTile> It(W); It; ++It)
		{
			ATriggerTile* TriggerTile = *It;
			if (!TriggerTile) continue;

			FTriggerTileSpec NewTriggerSpec;
			NewTriggerSpec.Q = TriggerTile->Q;
			NewTriggerSpec.R = TriggerTile->R;
			NewTriggerSpec.TriggerOrbType = TriggerTile->TriggerOrbType;
			NewTriggerSpec.Variant = TriggerTile->Variant;
			NewTriggerSpec.OwnerQ = TriggerTile->OwnerQ;
			NewTriggerSpec.OwnerR = TriggerTile->OwnerR;
			NewTriggerSpec.OwnerType = TriggerTile->OwnerType;
			NewTriggerSpec.Rotation = TriggerTile->GetActorRotation().Yaw;
			NewTriggerSpec.Spawned = TriggerTile;
			
			Scenario.TriggerTiles.Add(NewTriggerSpec);
		}
	}
}
