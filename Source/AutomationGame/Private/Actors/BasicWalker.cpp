#include "Actors/BasicWalker.h"
#include "Actors/OrbSimDriver.h"
#include "Framework/GameTickManager.h"
#include "Framework/GameManager.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "Sim/SimWorldHex.h"
#include "EngineUtils.h"
#include "Animation/AnimInstance.h"
#include "UObject/UnrealType.h"
#include "DrawDebugHelpers.h"

ABasicWalker::ABasicWalker()
{
	PrimaryActorTick.bCanEverTick = true;

	SimWalkerIndex = -1;

	USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("WalkerRoot"));
	SetRootComponent(Root);

	BodyComp = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("WalkerBody"));
	BodyComp->SetupAttachment(Root);
	BodyComp->SetMobility(EComponentMobility::Movable);
}

void ABasicWalker::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	EnsureComps();
	RefreshMeshes();
	RefreshPose(1.f);
}

void ABasicWalker::BeginPlay()
{
	Super::BeginPlay();

	EnsureComps();
	RefreshMeshes();

	HomeHex = FHex(Q, R);
	CurrentHex = HomeHex;
	TargetHex = HomeHex;

	// auto-find driver if not set
	if (!Driver)
	{
		if (UWorld* W = GetWorld())
		{
			Driver = AOrbSimDriver::Find(W);
		}
	}
	Sim = (Driver ? Driver->GetSim() : nullptr);
	if (!Sim)
	{
		UE_LOG(LogTemp, Warning, TEXT("BasicWalker '%s': No Sim found; movement will be visual only."), *GetName());
	}

	// Initialize foot positions from sim if walker was already added
	if (Sim && SimWalkerIndex >= 0 && SimWalkerIndex < Sim->Walkers.Num())
	{
		const FWalkerState& SimState = Sim->Walkers[SimWalkerIndex];
		DFootHex = SimState.DFoot.CurrentTile;
		LFootHex = SimState.LFoot.CurrentTile;
		RFootHex = SimState.RFoot.CurrentTile;
		UFootHex = SimState.UFoot.CurrentTile;
		
		UE_LOG(LogTemp, Log, TEXT("BasicWalker '%s': Initialized foot positions from sim - D(%d,%d) L(%d,%d) R(%d,%d) U(%d,%d)"),
			*GetName(), DFootHex.Q, DFootHex.R, LFootHex.Q, LFootHex.R, RFootHex.Q, RFootHex.R, UFootHex.Q, UFootHex.R);
		
		// Initialize body position from hex location using actual grid settings
		// This ensures the walker is positioned correctly even before the first tick
		if (Driver)
		{
			const FVector HexWorldPos = Driver->MapHexToWorld(CurrentHex);
			// Set actor location to hex position (body position will be calculated from feet in Tick)
			SetActorLocation(HexWorldPos);
			
			// Initialize IK goals to current foot positions (using actual grid settings)
			if (BodyComp && BodyComp->GetAnimInstance())
			{
				// Calculate foot positions using actual grid settings
				const FVector DFootWorld = Driver->MapHexToWorld(DFootHex);
				const FVector LFootWorld = Driver->MapHexToWorld(LFootHex);
				const FVector RFootWorld = Driver->MapHexToWorld(RFootHex);
				const FVector UFootWorld = Driver->MapHexToWorld(UFootHex);
				
				// Set initial IK goals via animation blueprint
				UAnimInstance* AnimInstance = BodyComp->GetAnimInstance();
				if (AnimInstance)
				{
					const FName FootGoalNames[] = { TEXT("DFootGoal"), TEXT("LFootGoal"), TEXT("RFootGoal"), TEXT("UFootGoal") };
					const FVector FootPositions[] = { DFootWorld, LFootWorld, RFootWorld, UFootWorld };
					
					for (int32 i = 0; i < 4; ++i)
					{
						if (FProperty* GoalProperty = AnimInstance->GetClass()->FindPropertyByName(FootGoalNames[i]))
						{
							if (FStructProperty* StructProp = CastField<FStructProperty>(GoalProperty))
							{
								if (StructProp->Struct == TBaseStructure<FVector>::Get())
								{
									FVector* GoalValue = StructProp->ContainerPtrToValuePtr<FVector>(AnimInstance);
									if (GoalValue)
									{
										*GoalValue = FootPositions[i];
									}
								}
							}
						}
					}
					
					// Set body goal to hex position
					if (FProperty* BodyGoalProperty = AnimInstance->GetClass()->FindPropertyByName(TEXT("BodyGoal")))
					{
						if (FStructProperty* StructProp = CastField<FStructProperty>(BodyGoalProperty))
						{
							if (StructProp->Struct == TBaseStructure<FVector>::Get())
							{
								FVector* GoalValue = StructProp->ContainerPtrToValuePtr<FVector>(AnimInstance);
								if (GoalValue)
								{
									*GoalValue = HexWorldPos;
								}
							}
						}
					}
					
					// Force animation update
					AnimInstance->BlueprintThreadSafeUpdateAnimation(0.f);
					BodyComp->TickAnimation(0.f, false);
					BodyComp->RefreshBoneTransforms();
				}
			}
			
			UE_LOG(LogTemp, Log, TEXT("BasicWalker '%s': Initialized position from hex (%d,%d) to world (%.1f,%.1f,%.1f) using Driver=%s"),
				*GetName(), CurrentHex.Q, CurrentHex.R, HexWorldPos.X, HexWorldPos.Y, HexWorldPos.Z,
				Driver ? *Driver->GetName() : TEXT("null"));
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("BasicWalker '%s': No Driver set - cannot initialize position from grid settings"), *GetName());
		}
	}

	// hook shared fixed tick
	if (AGameTickManager* M = AGameTickManager::Get(this))
	{
		M->OnFixedTick.RemoveDynamic(this, &ABasicWalker::HandleFixedTickSeconds);
		M->OnFixedTick.AddDynamic(this, &ABasicWalker::HandleFixedTickSeconds);
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("BasicWalker '%s': No GameTickManager; will not step."), *GetName());
	}

	PushProgramToSim();
}

void ABasicWalker::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	// Sync state from simulation
	if (Sim && SimWalkerIndex >= 0 && SimWalkerIndex < Sim->Walkers.Num())
	{
		const FWalkerState& SimState = Sim->Walkers[SimWalkerIndex];
		CurrentHex = SimState.BaseHex;
		TargetHex = SimState.TargetHex;
		bMoving = SimState.bMoving;
		MovementProgress = SimState.MovementProgress;
		PC = SimState.PC;
		WaitLeft = SimState.WaitCounter;
		
		// Sync foot states
		DFootHex = SimState.DFoot.CurrentTile;
		LFootHex = SimState.LFoot.CurrentTile;
		RFootHex = SimState.RFoot.CurrentTile;
		UFootHex = SimState.UFoot.CurrentTile;
		
		DFootHeightOffset = SimState.DFoot.HeightOffset;
		LFootHeightOffset = SimState.LFoot.HeightOffset;
		RFootHeightOffset = SimState.RFoot.HeightOffset;
		UFootHeightOffset = SimState.UFoot.HeightOffset;
		
		// Sync foot step states and progress
		DFootStepState = SimState.DFoot.StepState;
		LFootStepState = SimState.LFoot.StepState;
		RFootStepState = SimState.RFoot.StepState;
		UFootStepState = SimState.UFoot.StepState;
		
		// Helper function to calculate end-of-tick foot position based on step state and progress
		auto CalculateEndOfTickFootPosition = [&](const FFootState& FootState, const FHex& CurrentHex, const FHex& TargetHex) -> FVector
		{
			FVector EndWorld;
			float EndHeight = 0.f;
			
			switch (FootState.StepState)
			{
			case EFootStepState::Raising:
			{
				// Raising: stays at CurrentTile, height increases
				EndWorld = HexToWorld(CurrentHex);
				const int32 NextProgress = FootState.StepProgress + 1;
				EndHeight = (float)NextProgress / (float)SimState.StepRaiseTicks * SimState.StepHeight;
				break;
			}
			case EFootStepState::Moving:
			{
				// Moving: lerps from CurrentTile to TargetTile, stays at StepHeight
				const int32 NextProgress = FootState.StepProgress + 1;
				const float MoveAlpha = (float)NextProgress / (float)SimState.StepMoveTicks;
				const FVector CurrentWorld = HexToWorld(CurrentHex);
				const FVector TargetWorld = HexToWorld(TargetHex);
				EndWorld = FMath::Lerp(CurrentWorld, TargetWorld, FMath::Clamp(MoveAlpha, 0.f, 1.f));
				EndHeight = SimState.StepHeight;
				break;
			}
			case EFootStepState::Lowering:
			{
				// Lowering: stays at TargetTile (which is now CurrentTile), height decreases
				EndWorld = HexToWorld(CurrentHex); // CurrentTile equals TargetTile after moving
				const int32 NextProgress = FootState.StepProgress + 1;
				EndHeight = SimState.StepHeight * (1.f - (float)NextProgress / (float)SimState.StepLowerTicks);
				break;
			}
			case EFootStepState::Planted:
			default:
			{
				// Planted: at CurrentTile, no height
				EndWorld = HexToWorld(CurrentHex);
				EndHeight = 0.f;
				break;
			}
			}
			
			EndWorld.Z += EndHeight;
			return EndWorld;
		};
		
		// Calculate end-of-tick positions for each foot (where they'll be at the END of this tick)
		FVector DFootEndWorld = CalculateEndOfTickFootPosition(SimState.DFoot, DFootHex, SimState.DFoot.TargetTile);
		FVector LFootEndWorld = CalculateEndOfTickFootPosition(SimState.LFoot, LFootHex, SimState.LFoot.TargetTile);
		FVector RFootEndWorld = CalculateEndOfTickFootPosition(SimState.RFoot, RFootHex, SimState.RFoot.TargetTile);
		FVector UFootEndWorld = CalculateEndOfTickFootPosition(SimState.UFoot, UFootHex, SimState.UFoot.TargetTile);
		
		// Calculate body target position (center of PLANTED feet only, horizontal only)
		// Only include feet that will be planted at end of tick
		TArray<FVector> PlantedFootPositions;
		if (DFootStepState == EFootStepState::Planted || (DFootStepState == EFootStepState::Lowering && SimState.DFoot.StepProgress + 1 >= SimState.StepLowerTicks))
		{
			PlantedFootPositions.Add(DFootEndWorld);
		}
		if (LFootStepState == EFootStepState::Planted || (LFootStepState == EFootStepState::Lowering && SimState.LFoot.StepProgress + 1 >= SimState.StepLowerTicks))
		{
			PlantedFootPositions.Add(LFootEndWorld);
		}
		if (RFootStepState == EFootStepState::Planted || (RFootStepState == EFootStepState::Lowering && SimState.RFoot.StepProgress + 1 >= SimState.StepLowerTicks))
		{
			PlantedFootPositions.Add(RFootEndWorld);
		}
		if (UFootStepState == EFootStepState::Planted || (UFootStepState == EFootStepState::Lowering && SimState.UFoot.StepProgress + 1 >= SimState.StepLowerTicks))
		{
			PlantedFootPositions.Add(UFootEndWorld);
		}
		
		// Calculate body height offset based on stance and transition progress
		float BodyHeightOffset = 0.f;
		switch (SimState.Stance)
		{
		case EWalkerStance::Sitting:
			BodyHeightOffset = 0.f;
			break;
		case EWalkerStance::StandingUp:
		{
			// Lerp from 0 to StandHeight over 3 ticks
			// Calculate end-of-tick position: StanceTransitionProgress will be incremented at start of next fixed tick
			// So at end of current tick, it will be StanceTransitionProgress + 1
			const float StandAlpha = (float)(SimState.StanceTransitionProgress + 1) / 3.0f;
			BodyHeightOffset = SimState.StandHeight * FMath::Clamp(StandAlpha, 0.f, 1.f);
			break;
		}
		case EWalkerStance::Standing:
			BodyHeightOffset = SimState.StandHeight;
			break;
		case EWalkerStance::SittingDown:
		{
			// Lerp from StandHeight to 0 over 3 ticks
			// Calculate end-of-tick position: StanceTransitionProgress will be incremented at start of next fixed tick
			// So at end of current tick, it will be StanceTransitionProgress + 1
			const float SitAlpha = (float)(SimState.StanceTransitionProgress + 1) / 3.0f;
			BodyHeightOffset = SimState.StandHeight * (1.f - FMath::Clamp(SitAlpha, 0.f, 1.f));
			break;
		}
		}
		
		// Calculate average of planted feet, or use current location if no feet are planted
		FVector BodyEndWorld;
		if (PlantedFootPositions.Num() > 0)
		{
			FVector Sum = FVector::ZeroVector;
			for (const FVector& FootPos : PlantedFootPositions)
			{
				Sum += FootPos;
			}
			BodyEndWorld = Sum / PlantedFootPositions.Num();
			// Apply stand height offset to body position
			BodyEndWorld.Z += BodyHeightOffset;
		}
		else
		{
			// No feet planted, use hex position as base (horizontal) with height offset
			// Use Driver's MapHexToWorld to ensure we use actual grid settings
			FVector HexWorld = HexToWorld(CurrentHex);
			BodyEndWorld = FVector(HexWorld.X, HexWorld.Y, HexWorld.Z);
			BodyEndWorld.Z += BodyHeightOffset;
		}
		
		// Update IK Rig goals via animation blueprint with end-of-tick values (no interpolation - handled in event graph)
		if (UAnimInstance* AnimInstance = BodyComp->GetAnimInstance())
		{
			// Set foot goal positions (end-of-tick targets)
			const FName FootGoalNames[] = { TEXT("DFootGoal"), TEXT("LFootGoal"), TEXT("RFootGoal"), TEXT("UFootGoal") };
			const FVector FootEndPositions[] = { DFootEndWorld, LFootEndWorld, RFootEndWorld, UFootEndWorld };
			
			for (int32 i = 0; i < 4; ++i)
			{
				if (FProperty* GoalProperty = AnimInstance->GetClass()->FindPropertyByName(FootGoalNames[i]))
				{
					if (FStructProperty* StructProp = CastField<FStructProperty>(GoalProperty))
					{
						if (StructProp->Struct == TBaseStructure<FVector>::Get())
						{
							FVector* GoalValue = StructProp->ContainerPtrToValuePtr<FVector>(AnimInstance);
							if (GoalValue)
							{
								*GoalValue = FootEndPositions[i];
							}
						}
					}
				}
			}
			
			// Set body goal position (end-of-tick target)
			if (FProperty* BodyGoalProperty = AnimInstance->GetClass()->FindPropertyByName(TEXT("BodyGoal")))
			{
				if (FStructProperty* StructProp = CastField<FStructProperty>(BodyGoalProperty))
				{
					if (StructProp->Struct == TBaseStructure<FVector>::Get())
					{
						FVector* GoalValue = StructProp->ContainerPtrToValuePtr<FVector>(AnimInstance);
						if (GoalValue)
						{
							*GoalValue = BodyEndWorld;
						}
					}
				}
			}
			
			// Force animation update
			AnimInstance->BlueprintThreadSafeUpdateAnimation(0.f);
			BodyComp->TickAnimation(0.f, false);
			BodyComp->RefreshBoneTransforms();
		}
		
		// Draw debug lines from body to foot goals if enabled
		AGameManager* GM = nullptr;
		for (TActorIterator<AGameManager> It(GetWorld()); It; ++It)
		{
			GM = *It;
			break;
		}
		
		if (GM && GM->bDebugWalkers)
		{
			const FVector BodyLocation = GetActorLocation();
			
			// Get quadrant colors for each leg (applies LegQuadrantOffset)
			// Leg order: D (0), L (1), R (2), U (3)
			FLinearColor DColor = GM->GetLegQuadrantColor(TEXT("D"), 0);
			FLinearColor LColor = GM->GetLegQuadrantColor(TEXT("L"), 1);
			FLinearColor RColor = GM->GetLegQuadrantColor(TEXT("R"), 2);
			FLinearColor UColor = GM->GetLegQuadrantColor(TEXT("U"), 3);
			
			// Draw lines from body to foot goals with quadrant colors
			DrawDebugLine(GetWorld(), BodyLocation, DFootEndWorld, DColor.ToFColor(true), false, -1.f, 0, 3.f);
			DrawDebugLine(GetWorld(), BodyLocation, LFootEndWorld, LColor.ToFColor(true), false, -1.f, 0, 3.f);
			DrawDebugLine(GetWorld(), BodyLocation, RFootEndWorld, RColor.ToFColor(true), false, -1.f, 0, 3.f);
			DrawDebugLine(GetWorld(), BodyLocation, UFootEndWorld, UColor.ToFColor(true), false, -1.f, 0, 3.f);
			
			// Draw spheres at foot positions with quadrant colors
			DrawDebugSphere(GetWorld(), DFootEndWorld, 15.f, 8, DColor.ToFColor(true), false, -1.f, 0, 2.f);
			DrawDebugSphere(GetWorld(), LFootEndWorld, 15.f, 8, LColor.ToFColor(true), false, -1.f, 0, 2.f);
			DrawDebugSphere(GetWorld(), RFootEndWorld, 15.f, 8, RColor.ToFColor(true), false, -1.f, 0, 2.f);
			DrawDebugSphere(GetWorld(), UFootEndWorld, 15.f, 8, UColor.ToFColor(true), false, -1.f, 0, 2.f);
			
			// Draw text showing foot hex positions and quadrant assignments
			if (Driver)
			{
				FHex DFootHexLocal, LFootHexLocal, RFootHexLocal, UFootHexLocal;
				if (Driver->WorldToHex(DFootEndWorld, DFootHexLocal))
				{
					FString DText = FString::Printf(TEXT("D:(%d,%d)"), DFootHexLocal.Q, DFootHexLocal.R);
					DrawDebugString(GetWorld(), DFootEndWorld + FVector(0, 0, 20), DText, nullptr, DColor.ToFColor(true), 0.f, true, 1.2f);
				}
				if (Driver->WorldToHex(LFootEndWorld, LFootHexLocal))
				{
					FString LText = FString::Printf(TEXT("L:(%d,%d)"), LFootHexLocal.Q, LFootHexLocal.R);
					DrawDebugString(GetWorld(), LFootEndWorld + FVector(0, 0, 20), LText, nullptr, LColor.ToFColor(true), 0.f, true, 1.2f);
				}
				if (Driver->WorldToHex(RFootEndWorld, RFootHexLocal))
				{
					FString RText = FString::Printf(TEXT("R:(%d,%d)"), RFootHexLocal.Q, RFootHexLocal.R);
					DrawDebugString(GetWorld(), RFootEndWorld + FVector(0, 0, 20), RText, nullptr, RColor.ToFColor(true), 0.f, true, 1.2f);
				}
				if (Driver->WorldToHex(UFootEndWorld, UFootHexLocal))
				{
					FString UText = FString::Printf(TEXT("U:(%d,%d)"), UFootHexLocal.Q, UFootHexLocal.R);
					DrawDebugString(GetWorld(), UFootEndWorld + FVector(0, 0, 20), UText, nullptr, UColor.ToFColor(true), 0.f, true, 1.2f);
				}
			}
		}
		
		// Set actor location to body end position
		SetActorLocation(BodyEndWorld);
	}

	RefreshPose(1.0f);
}

void ABasicWalker::EnsureComps()
{
	if (!BodyComp)
	{
		BodyComp = NewObject<USkeletalMeshComponent>(this, TEXT("WalkerBody"));
		BodyComp->SetupAttachment(GetRootComponent());
		BodyComp->RegisterComponent();
	}

	if (BodyComp)
	{
		// Set the skeletal mesh
		if (WalkerSkeletalMesh)
		{
			BodyComp->SetSkeletalMesh(WalkerSkeletalMesh);
		}

		// Set the Animation Blueprint class
		if (WalkerAnimationBlueprintClass)
		{
			BodyComp->SetAnimInstanceClass(WalkerAnimationBlueprintClass);
		}

		// Get the Animation Blueprint instance
		WalkerAnimInstance = BodyComp->GetAnimInstance();
	}
}

void ABasicWalker::RefreshMeshes()
{
	if (BodyComp)
	{
		BodyComp->SetSkinnedAssetAndUpdate(WalkerSkeletalMesh);
		BodyComp->SetRelativeScale3D(BaseScale);
	}
}

FVector ABasicWalker::HexToWorld(const FHex& H) const
{
	if (!Driver)
	{
		UE_LOG(LogTemp, Warning, TEXT("BasicWalker '%s': HexToWorld called without Driver - using actor location"), *GetName());
		return GetActorLocation();
	}
	// Always use the Driver's MapHexToWorld which uses actual grid settings (tile size, orientation, yaw, anchor)
	return Driver->MapHexToWorld(H);
}

void ABasicWalker::RefreshPose(float InAlpha)
{
	// Don't set body location here - it's handled by the IK Rig and actor location
	// The actor location is set in Tick() based on body goal position
	// This function is kept for compatibility but doesn't modify positions
}

void ABasicWalker::HandleFixedTickSeconds(float StepSeconds)
{
	if (!bRunning || !Sim) return;

	// Step program is handled by simulation
	// This is just for visual sync
}

void ABasicWalker::SetProgram(const TArray<FWalkerInstr>& InProgram, bool bLoop)
{
	Program = InProgram;
	bLoopProgram = bLoop;
	PC = 0;
	PushProgramToSim();
}

void ABasicWalker::Start()
{
	bRunning = true;
}

void ABasicWalker::Stop()
{
	bRunning = false;
}

void ABasicWalker::ResetProgram()
{
	PC = 0;
	ExpandResetAtPC();
	PushProgramToSim();
}

void ABasicWalker::SyncFromSim(const FWalkerState& SimState)
{
	SimWalkerIndex = Sim->Walkers.IndexOfByKey(SimState);
	CurrentHex = SimState.BaseHex;
	TargetHex = SimState.TargetHex;
	bMoving = SimState.bMoving;
	MovementProgress = SimState.MovementProgress;
	PC = SimState.PC;
	WaitLeft = SimState.WaitCounter;
}

void ABasicWalker::SyncFromCurrentSim()
{
	if (Sim && SimWalkerIndex >= 0 && SimWalkerIndex < Sim->Walkers.Num())
	{
		SyncFromSim(Sim->Walkers[SimWalkerIndex]);
	}
}

void ABasicWalker::PushProgramToSim()
{
	if (!Sim) return;

	TArray<FWalkerInstr> ExpandedProgram = Program;
	
	// Expand Reset commands
	for (int32 i = 0; i < ExpandedProgram.Num(); ++i)
	{
		if (ExpandedProgram[i].Cmd == EWalkerCmd::Reset)
		{
			FWalkerInstr MoveToHome;
			MoveToHome.Cmd = EWalkerCmd::MoveTo;
			MoveToHome.Arg1 = HomeHex.Q;
			MoveToHome.Arg2 = HomeHex.R;
			ExpandedProgram[i] = MoveToHome;
		}
	}

	if (SimWalkerIndex >= 0 && SimWalkerIndex < Sim->Walkers.Num())
	{
		FWalkerState& State = Sim->Walkers[SimWalkerIndex];
		State.Program = ExpandedProgram;
		State.PC = PC;
	}
}

TArray<FWalkerInstr> ABasicWalker::BuildProgramFromInstructions(const TArray<FWalkerInstr>& InProgram) const
{
	return InProgram; // No conversion needed for now
}

void ABasicWalker::RebuildProgramFromInstructions()
{
	PushProgramToSim();
}

void ABasicWalker::StepProgram()
{
	// Program stepping is handled by simulation
}

void ABasicWalker::ExpandResetAtPC()
{
	// Reset expansion is handled in PushProgramToSim
}

void ABasicWalker::InitializeOnGrid(AOrbSimDriver* InDriver, int32 InQ, int32 InR)
{
	Driver = InDriver;
	Q = InQ;
	R = InR;
	HomeHex = FHex(Q, R);
	CurrentHex = HomeHex;
	TargetHex = HomeHex;
}

#if WITH_EDITOR
void ABasicWalker::PostEditChangeProperty(FPropertyChangedEvent& Event)
{
	Super::PostEditChangeProperty(Event);
	if (Event.Property)
	{
		const FName PropName = Event.Property->GetFName();
		if (PropName == GET_MEMBER_NAME_CHECKED(ABasicWalker, Program) ||
			PropName == GET_MEMBER_NAME_CHECKED(ABasicWalker, bLoopProgram))
		{
			PushProgramToSim();
		}
	}
}
#endif

