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
#include "CollisionQueryParams.h"
#include "Engine/Engine.h"
#include "UObject/EnumProperty.h"
#include "Rendering/OrbRenderer.h"
#include "Data/OrbMaterialManager.h"

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

		// Update public world location properties
		UpdateAssignedTileWorldLocations();

		// Initialize body position from hex location using actual grid settings
		// This ensures the walker is positioned correctly even before the first tick
		if (Driver)
		{
			const FVector HexWorldPos = Driver->MapHexToWorld(CurrentHex);
			// Set actor location to hex position (body position will be calculated from feet in Tick)
			SetActorLocation(HexWorldPos);

			// Initialize IK goals to current foot positions (using actual grid settings)
			// Skip if ManualFootGoal is enabled (goals will be set via Event Graph)
			bool bShouldSetFootGoals = true;
			if (UWorld* World = GetWorld())
			{
				for (TActorIterator<AGameManager> It(World); It; ++It)
				{
					AGameManager* GM = *It;
					if (GM && GM->bManualFootGoal)
					{
						bShouldSetFootGoals = false;
						break;
					}
				}
			}
			
			if (bShouldSetFootGoals && BodyComp && BodyComp->GetAnimInstance())
			{
				// Calculate foot positions using actual grid settings
				// Map based on bone names to match calibration logic (DHip→DFoot, LHip→LFoot, etc.)
				const FName BoneNames[] = { TEXT("DHipAxle41"), TEXT("LHipAxle17"), TEXT("RHipAxle5"), TEXT("UHipAxle29") };
				const FName FootGoalNames[] = { TEXT("DFootGoal"), TEXT("LFootGoal"), TEXT("RFootGoal"), TEXT("UFootGoal") };
				const FHex FootHexes[] = { DFootHex, LFootHex, RFootHex, UFootHex };

				// Set initial IK goals via animation blueprint
				UAnimInstance* AnimInstance = BodyComp->GetAnimInstance();
				if (AnimInstance)
				{
					// Get the component transform for space conversion (no scale)
					const FTransform ComponentToWorld = BodyComp->GetComponentToWorld();
					const FRotator ComponentRotation = BodyComp->GetComponentRotation();
					const FTransform ComponentTransformNoScale(ComponentRotation, ComponentToWorld.GetLocation(), FVector(1.f));
					const FTransform WorldToComponentNoScale = ComponentTransformNoScale.Inverse();

					// Map each bone to its corresponding simulation foot and goal
					for (int32 BoneIdx = 0; BoneIdx < 4; ++BoneIdx)
					{
						const FName& BoneName = BoneNames[BoneIdx];
						const FName& GoalName = FootGoalNames[BoneIdx];
						
						// Determine which simulation foot this bone corresponds to based on bone name
						int32 SimFootIndex = -1;
						const FString BoneNameStr = BoneName.ToString();
						if (BoneNameStr.Contains(TEXT("DHip")))
						{
							SimFootIndex = 0; // DFoot
						}
						else if (BoneNameStr.Contains(TEXT("LHip")))
						{
							SimFootIndex = 1; // LFoot
						}
						else if (BoneNameStr.Contains(TEXT("RHip")))
						{
							SimFootIndex = 2; // RFoot
						}
						else if (BoneNameStr.Contains(TEXT("UHip")))
						{
							SimFootIndex = 3; // UFoot
						}
						
						if (SimFootIndex >= 0 && SimFootIndex < 4)
						{
							if (FProperty* GoalProperty = AnimInstance->GetClass()->FindPropertyByName(GoalName))
							{
								if (FStructProperty* StructProp = CastField<FStructProperty>(GoalProperty))
								{
									if (StructProp->Struct == TBaseStructure<FVector>::Get())
									{
										FVector* GoalValue = StructProp->ContainerPtrToValuePtr<FVector>(AnimInstance);
										if (GoalValue)
										{
											// Get the world position for the correct simulation foot
											const FVector FootWorld = Driver->MapHexToWorld(FootHexes[SimFootIndex]);
											
											// Convert world-space goal to component-local space (no scale)
											// The goal should position the foot BONE at the tile center
											*GoalValue = WorldToComponentNoScale.TransformPosition(FootWorld);
										}
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
									// Convert world-space goal to component-local space (no scale)
									*GoalValue = WorldToComponentNoScale.TransformPosition(HexWorldPos);
								}
							}
						}
					}

					// Force animation update
					AnimInstance->BlueprintThreadSafeUpdateAnimation(0.033f);
					BodyComp->TickAnimation(0.033f, false);
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
FVector ABasicWalker::GetFootGoalFromBoneToTile(const FName& FootBoneName, const FVector& AssignedTileWorldLocation, bool bTraceDownward) const
{
    if (!BodyComp || !GetWorld())
    {
        return FVector::ZeroVector;
    }

    // Get current foot bone location in world space
    FVector BoneWorldLocation = BodyComp->GetBoneLocation(FootBoneName, EBoneSpaces::WorldSpace);
    
    if (BoneWorldLocation.IsNearlyZero())
    {
        UE_LOG(LogTemp, Warning, TEXT("GetFootGoalFromBoneToTile: Bone %s location is zero"), *FootBoneName.ToString());
        return FVector::ZeroVector;
    }

    FVector HitLocation;
    bool bHit = false;

    if (bTraceDownward)
    {
        // Trace downward from the assigned tile location to find the grid surface
        FVector TraceStart = AssignedTileWorldLocation + FVector(0.0f, 0.0f, 1000.0f); // Start above the tile
        bHit = LineTraceToGrid(TraceStart, HitLocation, 200.0f);
        
        if (!bHit)
        {
            // Fallback: use the tile location directly
            HitLocation = AssignedTileWorldLocation;
        }
    }
    else
    {
        // Trace from bone location toward the assigned tile location
        FVector Direction = (AssignedTileWorldLocation - BoneWorldLocation).GetSafeNormal();
        
        // If direction is mostly horizontal, trace downward instead
        if (FMath::Abs(Direction.Z) < 0.1f)
        {
            Direction = FVector(0.0f, 0.0f, -1.0f); // Trace straight down
        }
        
        float TraceDistance = FVector::Dist(BoneWorldLocation, AssignedTileWorldLocation) + 100.0f; // Add buffer
        bHit = LineTraceToGridDirection(BoneWorldLocation, Direction, HitLocation, TraceDistance);
        
        if (!bHit)
        {
            // Fallback: use the tile location directly
            HitLocation = AssignedTileWorldLocation;
        }
    }

    // Convert hit location to component space
    // Get component transform
    const FTransform ComponentToWorld = BodyComp->GetComponentToWorld();
    const FRotator ComponentRotation = BodyComp->GetComponentRotation();
    const FTransform ComponentTransformNoScale(ComponentRotation, ComponentToWorld.GetLocation(), FVector(1.f));
    const FTransform WorldToComponentNoScale = ComponentTransformNoScale.Inverse();

    // Convert world position to component space
    FVector GoalComponentSpace = WorldToComponentNoScale.TransformPosition(HitLocation);

    return GoalComponentSpace;
}

FVector ABasicWalker::GetFootGoalFromBoneToTileString(const FString& FootBoneName, const FVector& AssignedTileWorldLocation, bool bTraceDownward) const
{
    return GetFootGoalFromBoneToTile(FName(*FootBoneName), AssignedTileWorldLocation, bTraceDownward);
}
FVector ABasicWalker::WorldToComponentSpace(const FVector& WorldLocation) const
{
    if (!BodyComp)
    {
        return FVector::ZeroVector;
    }
    
    // Get component transform (world space)
    const FTransform ComponentToWorld = BodyComp->GetComponentToWorld();
    const FRotator ComponentRotation = BodyComp->GetComponentRotation();
    
    // Create transform without scale (IK systems typically don't use scale)
    const FTransform ComponentTransformNoScale(ComponentRotation, ComponentToWorld.GetLocation(), FVector(1.f));
    
    // Get inverse transform to convert world -> component
    const FTransform WorldToComponentNoScale = ComponentTransformNoScale.Inverse();
    
    // Convert the world position to component space
    return WorldToComponentNoScale.TransformPosition(WorldLocation);
}
void ABasicWalker::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	// During calibration, ensure tick runs and updates properties even if not in simulation
	if (Calibrating)
	{
		// Update assigned tile world locations continuously during calibration
		// This ensures the public properties are always up-to-date for the event graph
		UpdateAssignedTileWorldLocations();
		
		// Force animation update during calibration so event graph can run continuously
		// This allows the event graph to update foot goals in real-time
		if (BodyComp && BodyComp->IsRegistered())
		{
			// Ensure animation instance exists
			if (!BodyComp->GetAnimInstance() && BodyComp->GetAnimClass())
			{
				BodyComp->InitAnim(true);
			}
			
			UAnimInstance* AnimInstance = BodyComp->GetAnimInstance();
			if (AnimInstance)
			{
				// Validate component transform before forcing animation update
				const FTransform ComponentToWorld = BodyComp->GetComponentToWorld();
				if (!ComponentToWorld.ContainsNaN())
				{
					// Check if manual foot goals are enabled - if so, preserve foot goal values
					bool bShouldPreserveFootGoals = false;
					AGameManager* GM = nullptr;
					if (UWorld* World = GetWorld())
					{
						for (TActorIterator<AGameManager> It(World); It; ++It)
						{
							GM = *It;
							if (GM && GM->bManualFootGoal)
							{
								bShouldPreserveFootGoals = true;
								break;
							}
						}
					}
					
					// Preserve foot goal values if manual foot goals are enabled
					const FName FootGoalNames[] = { TEXT("DFootGoal"), TEXT("LFootGoal"), TEXT("RFootGoal"), TEXT("UFootGoal") };
					FVector PreservedFootGoals[4];
					bool bHasPreservedGoals[4] = { false, false, false, false };
					
					if (bShouldPreserveFootGoals)
					{
						for (int32 i = 0; i < 4; ++i)
						{
							if (FProperty* GoalProperty = AnimInstance->GetClass()->FindPropertyByName(FootGoalNames[i]))
							{
								if (FStructProperty* StructProp = CastField<FStructProperty>(GoalProperty))
								{
									if (StructProp->Struct == TBaseStructure<FVector>::Get())
									{
										FVector* GoalValue = StructProp->ContainerPtrToValuePtr<FVector>(AnimInstance);
										if (GoalValue && !GoalValue->IsNearlyZero())
										{
											PreservedFootGoals[i] = *GoalValue;
											bHasPreservedGoals[i] = true;
										}
									}
								}
							}
						}
					}
					
					AnimInstance->BlueprintThreadSafeUpdateAnimation(DeltaSeconds);
					BodyComp->TickAnimation(DeltaSeconds, false);
					BodyComp->RefreshBoneTransforms();
					
					// Restore preserved foot goal values if manual foot goals are enabled
					if (bShouldPreserveFootGoals)
					{
						for (int32 i = 0; i < 4; ++i)
						{
							if (bHasPreservedGoals[i])
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
												*GoalValue = PreservedFootGoals[i];
											}
										}
									}
								}
							}
						}
					}
				}
				else
				{
					UE_LOG(LogTemp, VeryVerbose, TEXT("BasicWalker::Tick: Component transform contains NaN during calibration, skipping animation update"));
				}
			}
			else
			{
				UE_LOG(LogTemp, VeryVerbose, TEXT("BasicWalker::Tick: No animation instance during calibration, attempting to initialize"));
			}
		}
	}

	// Sync state from simulation
	if (Sim && SimWalkerIndex >= 0 && SimWalkerIndex < Sim->Walkers.Num())
	{
		FWalkerState& SimState = Sim->Walkers[SimWalkerIndex];
		CurrentHex = SimState.BaseHex;
		TargetHex = SimState.TargetHex;
		bMoving = SimState.bMoving;
		MovementProgress = SimState.MovementProgress;
		PC = SimState.PC;
		WaitLeft = SimState.WaitCounter;

		// Update body target from BodyTargetWorldPosition if set
		if (Driver && !BodyTargetWorldPosition.IsNearlyZero())
		{
			FHex TargetHexFromWorld;
			if (Driver->WorldToHex(BodyTargetWorldPosition, TargetHexFromWorld))
			{
				// Check if the target has changed
				if (SimState.CurrentTargetHex != TargetHexFromWorld)
				{
					// Add a request to move to the new target (or update current target if processing)
					if (SimState.bProcessingRequest)
					{
						// Update current target if already processing a request
						SimState.CurrentTargetHex = TargetHexFromWorld;
					}
					else
					{
						// Add new request with high priority
						FWalkerRequest NewRequest;
						NewRequest.TargetHex = TargetHexFromWorld;
						NewRequest.Priority = 1000; // High priority to ensure it's processed
						SimState.Requests.Add(NewRequest);
					}
				}
			}
		}

		// Sync foot states
		DFootHex = SimState.DFoot.CurrentTile;
		LFootHex = SimState.LFoot.CurrentTile;
		RFootHex = SimState.RFoot.CurrentTile;
		UFootHex = SimState.UFoot.CurrentTile;

		// Update public world location properties
		UpdateAssignedTileWorldLocations();

		DFootHeightOffset = SimState.DFoot.HeightOffset;
		LFootHeightOffset = SimState.LFoot.HeightOffset;
		RFootHeightOffset = SimState.RFoot.HeightOffset;
		UFootHeightOffset = SimState.UFoot.HeightOffset;

		// Sync foot step states and progress
		DFootStepState = SimState.DFoot.StepState;
		LFootStepState = SimState.LFoot.StepState;
		RFootStepState = SimState.RFoot.StepState;
		UFootStepState = SimState.UFoot.StepState;

		// Update animation status booleans for Event Graph
		bDFootRaising = (DFootStepState == EFootStepState::Raising);
		bDFootMoving = (DFootStepState == EFootStepState::Moving);
		bDFootLowering = (DFootStepState == EFootStepState::Lowering);

		bLFootRaising = (LFootStepState == EFootStepState::Raising);
		bLFootMoving = (LFootStepState == EFootStepState::Moving);
		bLFootLowering = (LFootStepState == EFootStepState::Lowering);

		bRFootRaising = (RFootStepState == EFootStepState::Raising);
		bRFootMoving = (RFootStepState == EFootStepState::Moving);
		bRFootLowering = (RFootStepState == EFootStepState::Lowering);

		bUFootRaising = (UFootStepState == EFootStepState::Raising);
		bUFootMoving = (UFootStepState == EFootStepState::Moving);
		bUFootLowering = (UFootStepState == EFootStepState::Lowering);

		// Sync held item states
		bool bPrevDHolding = bDFootHoldingItem;
		bool bPrevLHolding = bLFootHoldingItem;
		bool bPrevRHolding = bRFootHoldingItem;
		bool bPrevUHolding = bUFootHoldingItem;
		
		bDFootHoldingItem = SimState.DFoot.bHoldingItem;
		bLFootHoldingItem = SimState.LFoot.bHoldingItem;
		bRFootHoldingItem = SimState.RFoot.bHoldingItem;
		bUFootHoldingItem = SimState.UFoot.bHoldingItem;

		DFootHeldItem = SimState.DFoot.HeldItem;
		LFootHeldItem = SimState.LFoot.HeldItem;
		RFootHeldItem = SimState.RFoot.HeldItem;
		UFootHeldItem = SimState.UFoot.HeldItem;

		// Log state changes
		if (bDFootHoldingItem != bPrevDHolding)
			UE_LOG(LogTemp, Log, TEXT("Walker '%s': DFoot holding changed to %d"), *GetName(), bDFootHoldingItem ? 1 : 0);
		if (bLFootHoldingItem != bPrevLHolding)
			UE_LOG(LogTemp, Log, TEXT("Walker '%s': LFoot holding changed to %d"), *GetName(), bLFootHoldingItem ? 1 : 0);
		if (bRFootHoldingItem != bPrevRHolding)
			UE_LOG(LogTemp, Log, TEXT("Walker '%s': RFoot holding changed to %d"), *GetName(), bRFootHoldingItem ? 1 : 0);
		if (bUFootHoldingItem != bPrevUHolding)
			UE_LOG(LogTemp, Log, TEXT("Walker '%s': UFoot holding changed to %d"), *GetName(), bUFootHoldingItem ? 1 : 0);

		// Update held item visuals
		UpdateHeldItemVisuals(DFootHeldOrbComp, bDFootHoldingItem, DFootHeldItem, FName(TEXT("DFoot_Socket")));
		UpdateHeldItemVisuals(LFootHeldOrbComp, bLFootHoldingItem, LFootHeldItem, FName(TEXT("LFoot_Socket")));
		UpdateHeldItemVisuals(RFootHeldOrbComp, bRFootHoldingItem, RFootHeldItem, FName(TEXT("RFoot_Socket")));
		UpdateHeldItemVisuals(UFootHeldOrbComp, bUFootHoldingItem, UFootHeldItem, FName(TEXT("UFoot_Socket")));

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

		// Calculate body position: lerp X/Y from foot positions, add StandHeight to ground Z
		FVector BodyEndWorld;
		if (PlantedFootPositions.Num() > 0)
		{
			// Average X, Y, and Z from planted feet (Z represents ground level)
			FVector Sum = FVector::ZeroVector;
			for (const FVector& FootPos : PlantedFootPositions)
			{
				Sum += FootPos;
			}
			FVector Avg = Sum / PlantedFootPositions.Num();
			// Add StandHeight to the average ground Z (not BodyHeightOffset which includes transition lerping)
			BodyEndWorld = FVector(Avg.X, Avg.Y, Avg.Z + SimState.StandHeight);
		}
		else
		{
			// No feet planted, use hex position as base and add StandHeight to ground Z
			FVector HexWorld = HexToWorld(CurrentHex);
			BodyEndWorld = FVector(HexWorld.X, HexWorld.Y, HexWorld.Z + SimState.StandHeight);
		}

		// Update IK Rig goals via animation blueprint with end-of-tick values (no interpolation - handled in event graph)
		// Skip automatic assignment if ManualFootGoal is enabled (goals will be set via Event Graph)
		bool bShouldSetFootGoals = true;
		if (UWorld* World = GetWorld())
		{
			for (TActorIterator<AGameManager> It(World); It; ++It)
			{
				AGameManager* GM = *It;
				if (GM && GM->bManualFootGoal)
				{
					bShouldSetFootGoals = false;
					break;
				}
			}
		}
		
		if (UAnimInstance* AnimInstance = BodyComp->GetAnimInstance())
		{
			// Get the component transform for space conversion (no scale)
			const FTransform ComponentToWorld = BodyComp->GetComponentToWorld();
			const FRotator ComponentRotation = BodyComp->GetComponentRotation();
			const FTransform ComponentTransformNoScale(ComponentRotation, ComponentToWorld.GetLocation(), FVector(1.f));
			const FTransform WorldToComponentNoScale = ComponentTransformNoScale.Inverse();

			// Set foot goal positions - IK Rig goals position the foot BONE at the assigned tile center
			// Only set if ManualFootGoal is disabled
			// Map based on bone names to match calibration logic (DHip→DFoot, LHip→LFoot, etc.)
			if (bShouldSetFootGoals)
			{
				// Bone names that correspond to each foot goal
				const FName BoneNames[] = { TEXT("DHipAxle41"), TEXT("LHipAxle17"), TEXT("RHipAxle5"), TEXT("UHipAxle29") };
				const FName FootGoalNames[] = { TEXT("DFootGoal"), TEXT("LFootGoal"), TEXT("RFootGoal"), TEXT("UFootGoal") };
				const FFootState* FootStates[] = { &SimState.DFoot, &SimState.LFoot, &SimState.RFoot, &SimState.UFoot };

				// Map each bone to its corresponding simulation foot and goal
				for (int32 BoneIdx = 0; BoneIdx < 4; ++BoneIdx)
				{
					const FName& BoneName = BoneNames[BoneIdx];
					const FName& GoalName = FootGoalNames[BoneIdx];
					
					// Determine which simulation foot this bone corresponds to based on bone name
					int32 SimFootIndex = -1;
					const FString BoneNameStr = BoneName.ToString();
					if (BoneNameStr.Contains(TEXT("DHip")))
					{
						SimFootIndex = 0; // DFoot
					}
					else if (BoneNameStr.Contains(TEXT("LHip")))
					{
						SimFootIndex = 1; // LFoot
					}
					else if (BoneNameStr.Contains(TEXT("RHip")))
					{
						SimFootIndex = 2; // RFoot
					}
					else if (BoneNameStr.Contains(TEXT("UHip")))
					{
						SimFootIndex = 3; // UFoot
					}
					
					if (SimFootIndex >= 0 && SimFootIndex < 4)
					{
						if (FProperty* GoalProperty = AnimInstance->GetClass()->FindPropertyByName(GoalName))
						{
							if (FStructProperty* StructProp = CastField<FStructProperty>(GoalProperty))
							{
								if (StructProp->Struct == TBaseStructure<FVector>::Get())
								{
									FVector* GoalValue = StructProp->ContainerPtrToValuePtr<FVector>(AnimInstance);
									if (GoalValue && BodyComp)
									{
									// Get assigned tile world position from the correct simulation foot
									const FHex AssignedTile = FootStates[SimFootIndex]->CurrentTile;
										const FVector FootGoalWorld = HexToWorld(AssignedTile);
										
										// Convert to component-local space (without scale)
										*GoalValue = WorldToComponentNoScale.TransformPosition(FootGoalWorld);
										
										// Debug: Log occasionally
										static int32 LogCounter = 0;
										if (BoneIdx == 0 && (++LogCounter % 60 == 0))
										{
											UE_LOG(LogTemp, VeryVerbose, TEXT("Walker IK: Bone %s → Goal %s → SimFoot[%d] Tile(%d,%d) World(%.1f,%.1f,%.1f)"),
												*BoneName.ToString(), *GoalName.ToString(), SimFootIndex,
												AssignedTile.Q, AssignedTile.R,
												FootGoalWorld.X, FootGoalWorld.Y, FootGoalWorld.Z);
										}
									}
								}
							}
						}
					}
					else
					{
						UE_LOG(LogTemp, Warning, TEXT("Walker IK: Could not determine SimFootIndex for bone %s"), *BoneName.ToString());
					}
				}
			}
			else
			{
				// Log when ManualFootGoal is preventing script from setting foot goals
				static int32 ManualGoalLogCounter = 0;
				if ((++ManualGoalLogCounter % 300 == 0)) // Log every 5 seconds at 60fps
				{
					UE_LOG(LogTemp, VeryVerbose, TEXT("Walker IK: ManualFootGoal is enabled - script is NOT setting foot goals (they should be set by Animation Blueprint event graph)"));
				}
			}

			// Set body goal position (end-of-tick target), expressed in component-local space
			if (FProperty* BodyGoalProperty = AnimInstance->GetClass()->FindPropertyByName(TEXT("BodyGoal")))
			{
				if (FStructProperty* StructProp = CastField<FStructProperty>(BodyGoalProperty))
				{
					if (StructProp->Struct == TBaseStructure<FVector>::Get())
					{
						FVector* GoalValue = StructProp->ContainerPtrToValuePtr<FVector>(AnimInstance);
						if (GoalValue)
						{
							// Convert world-space goal to component-local space (no scale)
							*GoalValue = WorldToComponentNoScale.TransformPosition(BodyEndWorld);
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
			// Use the same tile marking approach as CalibrateWalker
			GM->UpdateWalkerQuadrantVisualization(this);
		}
		else
		{
			// Clear visualization when debug is disabled
			if (GM)
			{
				GM->ClearWalkerQuadrantVisualization(this);
			}
		}

		// Set actor location to body end position
		//SetActorLocation(BodyEndWorld);
	}

	// Update Body Rotation Goal from foot positions
	if (bDynamicBodyRotation)
	{
		FVector BodyLoc = GetActorLocation();
		if (BodyComp)
		{
			// Try to use the actual body bone location for more accurate angle calculations relative to the body center
			if (BodyComp->GetBoneIndex(TEXT("Body1")) != INDEX_NONE)
			{
				BodyLoc = BodyComp->GetBoneLocation(TEXT("Body1"), EBoneSpaces::WorldSpace);
			}
			else if (BodyComp->GetBoneIndex(TEXT("pelvis")) != INDEX_NONE)
			{
				BodyLoc = BodyComp->GetBoneLocation(TEXT("pelvis"), EBoneSpaces::WorldSpace);
			}
		}

		float TotalWeightedSin = 0.f;
		float TotalWeightedCos = 0.f;
		float TotalWeight = 0.f;

		struct FFootRotInfo {
			FVector Pos;
			float IdealAngle; // Degrees: U=0, R=90, D=180, L=-90 (Front, Right, Back, Left)
		};

		// Ideal angles relative to body forward (0)
		const FFootRotInfo Feet[] = {
			{ UFootAssignedTileWorldLocation, 0.f },   // U/North/Front
			{ RFootAssignedTileWorldLocation, 90.f },  // R/East/Right
			{ DFootAssignedTileWorldLocation, 180.f }, // D/South/Back
			{ LFootAssignedTileWorldLocation, -90.f }  // L/West/Left
		};

		const float CurrentBodyYaw = GetActorRotation().Yaw;

		for (const FFootRotInfo& Foot : Feet)
		{
			if (Foot.Pos.IsNearlyZero()) continue;

			FVector ToFoot = Foot.Pos - BodyLoc;
			if (ToFoot.SizeSquared2D() < KINDA_SMALL_NUMBER) continue;

			float FootWorldYaw = ToFoot.Rotation().Yaw;

			// Calculate where the body "should" be to put this foot at IdealAngle
			// FootWorldYaw - BodyYaw = IdealAngle  =>  BodyYaw = FootWorldYaw - IdealAngle
			float DesiredBodyYaw = FRotator::NormalizeAxis(FootWorldYaw - Foot.IdealAngle);

			// Weighting:
			// "nearer a foot is to its neighboring boundary, the more influence it should have"
			// Deviation = how far the foot IS currently from IdealAngle relative to body
			// RelativeAngle = FootWorldYaw - CurrentBodyYaw
			// Deviation = RelativeAngle - IdealAngle
			float RelativeAngle = FRotator::NormalizeAxis(FootWorldYaw - CurrentBodyYaw);
			float Deviation = FRotator::NormalizeAxis(RelativeAngle - Foot.IdealAngle);

			// Quadrant is +/- 45 degrees from IdealAngle.
			// Boundary is at 45 degrees deviation.
			// Weight should increase as Abs(Deviation) -> 45.
			// Let's use a curve that ramps up near 45.
			float Weight = FMath::Abs(Deviation);

			// Add to circular average
			float Rad = FMath::DegreesToRadians(DesiredBodyYaw);
			TotalWeightedSin += FMath::Sin(Rad) * Weight;
			TotalWeightedCos += FMath::Cos(Rad) * Weight;
			TotalWeight += Weight;
		}

		if (TotalWeight > KINDA_SMALL_NUMBER)
		{
			AvgYaw = FMath::RadiansToDegrees(FMath::Atan2(TotalWeightedSin, TotalWeightedCos));
			BodyGoalRotation = FRotator(90.f, 0.f, AvgYaw);

			// Update QuadrantRotationOffset to compensate
			QuadrantRotationOffset = -AvgYaw;
		}
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

void ABasicWalker::UpdateAssignedTileWorldLocations()
{
	if (!Driver || !BodyComp)
	{
		// Fallback to zero if no driver or body component
		UE_LOG(LogTemp, VeryVerbose, TEXT("UpdateAssignedTileWorldLocations: No Driver (%d) or BodyComp (%d)"), Driver ? 1 : 0, BodyComp ? 1 : 0);
		DFootAssignedTileWorldLocation = FVector::ZeroVector;
		LFootAssignedTileWorldLocation = FVector::ZeroVector;
		RFootAssignedTileWorldLocation = FVector::ZeroVector;
		UFootAssignedTileWorldLocation = FVector::ZeroVector;
		return;
	}

	// Map simulation feet to bone-based properties by checking which bone corresponds to which simulation foot
	// The simulation uses DFoot/LFoot/RFoot/UFoot, but we need to map them to the actual bone names (DHip/LHip/RHip/UHip)
	// We'll determine the mapping by checking bone names in the skeletal mesh
	
	// Helper to find which simulation foot index corresponds to a bone name
	auto GetSimFootIndexForBone = [](const FString& BoneName) -> int32
	{
		if (BoneName.Contains(TEXT("DHip")))
		{
			return 0; // DFoot
		}
		else if (BoneName.Contains(TEXT("LHip")))
		{
			return 1; // LFoot
		}
		else if (BoneName.Contains(TEXT("RHip")))
		{
			return 2; // RFoot
		}
		else if (BoneName.Contains(TEXT("UHip")))
		{
			return 3; // UFoot
		}
		return -1;
	};

	// Get simulation foot states if available
	FHex SimFootHexes[4] = { DFootHex, LFootHex, RFootHex, UFootHex };
	bool bHasSimTiles = false;
	if (Sim && SimWalkerIndex >= 0 && SimWalkerIndex < Sim->Walkers.Num())
	{
		const FWalkerState& SimState = Sim->Walkers[SimWalkerIndex];
		// Use TargetTile instead of CurrentTile so the assigned tile updates immediately when raising begins
		SimFootHexes[0] = SimState.DFoot.TargetTile;
		SimFootHexes[1] = SimState.LFoot.TargetTile;
		SimFootHexes[2] = SimState.RFoot.TargetTile;
		SimFootHexes[3] = SimState.UFoot.TargetTile;
		bHasSimTiles = true;
	}
	
	// During calibration, calculate assigned tiles from body position if not in simulation
	if (!bHasSimTiles && Calibrating)
	{
		// Try to get assigned tiles from GameManager calibration data
		AGameManager* GM = nullptr;
		if (UWorld* World = GetWorld())
		{
			for (TActorIterator<AGameManager> It(World); It; ++It)
			{
				GM = *It;
				break;
			}
		}
		if (GM && GM->bWalkerCalibrationActive && GM->CalibratingWalker.Get() == this)
		{
			// First try: Use calibration data's FootAxleWorldLocation if available
			// This is more reliable than calculating from body position
			const FName BoneNames[] = { TEXT("DHipAxle41"), TEXT("LHipAxle17"), TEXT("RHipAxle5"), TEXT("UHipAxle29") };
			bool bFoundAllTiles = true;
			
			for (int32 BoneIdx = 0; BoneIdx < 4; ++BoneIdx)
			{
				const FName& BoneName = BoneNames[BoneIdx];
				const FString BoneNameStr = BoneName.ToString();
				
				// Get foot axle world location from calibration data
				const FVector FootAxleWorld = GM->GetFootAxleWorldLocationForBone(BoneName);
				
				if (!FootAxleWorld.IsNearlyZero())
				{
					FHex FootHex;
					
					if (Driver->WorldToHex(FootAxleWorld, FootHex))
					{
						// Determine which simulation foot this corresponds to
						int32 SimFootIndex = -1;
						if (BoneNameStr.Contains(TEXT("DHip")))
						{
							SimFootIndex = 0; // DFoot
						}
						else if (BoneNameStr.Contains(TEXT("LHip")))
						{
							SimFootIndex = 1; // LFoot
						}
						else if (BoneNameStr.Contains(TEXT("RHip")))
						{
							SimFootIndex = 2; // RFoot
						}
						else if (BoneNameStr.Contains(TEXT("UHip")))
						{
							SimFootIndex = 3; // UFoot
						}
						
						if (SimFootIndex >= 0 && SimFootIndex < 4)
						{
							SimFootHexes[SimFootIndex] = FootHex;
						}
					}
					else
					{
						bFoundAllTiles = false;
					}
				}
				else
				{
					bFoundAllTiles = false;
				}
			}
			
			if (bFoundAllTiles)
			{
				bHasSimTiles = true;
				UE_LOG(LogTemp, Log, TEXT("UpdateAssignedTileWorldLocations: Using calibration data - D(%d,%d) L(%d,%d) R(%d,%d) U(%d,%d)"), 
					SimFootHexes[0].Q, SimFootHexes[0].R, SimFootHexes[1].Q, SimFootHexes[1].R, 
					SimFootHexes[2].Q, SimFootHexes[2].R, SimFootHexes[3].Q, SimFootHexes[3].R);
			}
			else
			{
				UE_LOG(LogTemp, Warning, TEXT("UpdateAssignedTileWorldLocations: Could not find all tiles from calibration data"));
			}
			
			// Fallback: Calculate from body position if calibration data didn't work
			if (!bHasSimTiles)
			{
				// Get body bone location
				const FName BodyBoneName = TEXT("Body1");
				const FVector BodyWorldLocation = BodyComp->GetBoneLocation(BodyBoneName, EBoneSpaces::WorldSpace);
				FHex BodyHex;
				
				if (!BodyWorldLocation.IsNearlyZero() && Driver->WorldToHex(BodyWorldLocation, BodyHex))
				{
					// Calculate assigned tiles from body position using D/L/R/U pattern
					// This matches the logic in SetIKRigGoalPositions during calibration
					// FHex doesn't have a + operator, so manually add Q and R components
					const FHex DHex(BodyHex.Q + 0, BodyHex.R + 1);   // D
					const FHex LHex(BodyHex.Q + -1, BodyHex.R + 1); // L
					const FHex RHex(BodyHex.Q + 1, BodyHex.R + -1); // R
					const FHex UHex(BodyHex.Q + 0, BodyHex.R + -1); // U
					
					SimFootHexes[0] = DHex;
					SimFootHexes[1] = LHex;
					SimFootHexes[2] = RHex;
					SimFootHexes[3] = UHex;
					bHasSimTiles = true;
					UE_LOG(LogTemp, VeryVerbose, TEXT("UpdateAssignedTileWorldLocations: Calculated from body position (%d,%d)"), BodyHex.Q, BodyHex.R);
				}
				else
				{
					UE_LOG(LogTemp, Warning, TEXT("UpdateAssignedTileWorldLocations: Failed to get body bone location or convert to hex. BodyLocation=(%.1f,%.1f,%.1f)"), 
						BodyWorldLocation.X, BodyWorldLocation.Y, BodyWorldLocation.Z);
				}
			}
		}
		else
		{
			UE_LOG(LogTemp, VeryVerbose, TEXT("UpdateAssignedTileWorldLocations: Calibrating but GM check failed - Active=%d, WalkerMatch=%d"), 
				GM ? (GM->bWalkerCalibrationActive ? 1 : 0) : -1, 
				GM && GM->CalibratingWalker.IsValid() ? (GM->CalibratingWalker.Get() == this ? 1 : 0) : -1);
		}
	}

	// Map each bone to its corresponding simulation foot
	// Bone names: DHipAxle41, LHipAxle17, RHipAxle5, UHipAxle29
	const FName BoneNames[] = { TEXT("DHipAxle41"), TEXT("LHipAxle17"), TEXT("RHipAxle5"), TEXT("UHipAxle29") };
	FVector* AssignedLocations[] = { &DFootAssignedTileWorldLocation, &LFootAssignedTileWorldLocation, &RFootAssignedTileWorldLocation, &UFootAssignedTileWorldLocation };

	// Initialize all to zero first
	for (int32 i = 0; i < 4; ++i)
	{
		*AssignedLocations[i] = FVector::ZeroVector;
	}

	// Map each bone to its simulation foot
	for (int32 BoneIdx = 0; BoneIdx < 4; ++BoneIdx)
	{
		const FName& BoneName = BoneNames[BoneIdx];
		const int32 BoneIndex = BodyComp->GetBoneIndex(BoneName);
		
		if (BoneIndex != INDEX_NONE)
		{
			const FString BoneNameStr = BoneName.ToString();
			const int32 SimFootIndex = GetSimFootIndexForBone(BoneNameStr);
			
			if (SimFootIndex >= 0 && SimFootIndex < 4 && bHasSimTiles)
			{
				// Map this bone's assigned tile location
				const FVector WorldLoc = Driver->MapHexToWorld(SimFootHexes[SimFootIndex]);
				*AssignedLocations[BoneIdx] = WorldLoc;
				UE_LOG(LogTemp, VeryVerbose, TEXT("UpdateAssignedTileWorldLocations: Mapped %s (SimFootIndex=%d) to hex (%d,%d) -> world (%.1f,%.1f,%.1f)"), 
					*BoneNameStr, SimFootIndex, SimFootHexes[SimFootIndex].Q, SimFootHexes[SimFootIndex].R, 
					WorldLoc.X, WorldLoc.Y, WorldLoc.Z);
			}
			else
			{
				UE_LOG(LogTemp, VeryVerbose, TEXT("UpdateAssignedTileWorldLocations: Skipped %s - SimFootIndex=%d, bHasSimTiles=%d"), 
					*BoneNameStr, SimFootIndex, bHasSimTiles ? 1 : 0);
			}
		}
		else
		{
			UE_LOG(LogTemp, VeryVerbose, TEXT("UpdateAssignedTileWorldLocations: Bone %s not found (Index=%d)"), 
				*BoneName.ToString(), BoneIndex);
		}
	}
	
	UE_LOG(LogTemp, VeryVerbose, TEXT("UpdateAssignedTileWorldLocations: Final locations - D(%.1f,%.1f,%.1f) L(%.1f,%.1f,%.1f) R(%.1f,%.1f,%.1f) U(%.1f,%.1f,%.1f)"), 
		DFootAssignedTileWorldLocation.X, DFootAssignedTileWorldLocation.Y, DFootAssignedTileWorldLocation.Z,
		LFootAssignedTileWorldLocation.X, LFootAssignedTileWorldLocation.Y, LFootAssignedTileWorldLocation.Z,
		RFootAssignedTileWorldLocation.X, RFootAssignedTileWorldLocation.Y, RFootAssignedTileWorldLocation.Z,
		UFootAssignedTileWorldLocation.X, UFootAssignedTileWorldLocation.Y, UFootAssignedTileWorldLocation.Z);
	
	// Broadcast event so Event Graph can respond to assigned tile updates
	OnAssignedTilesUpdated.Broadcast();
}

FVector ABasicWalker::GetBoneLocationByName(const FName& BoneName, bool bWorldSpace) const
{
	if (!BodyComp)
	{
		return FVector::ZeroVector;
	}

	if (bWorldSpace)
	{
		return BodyComp->GetBoneLocation(BoneName, EBoneSpaces::WorldSpace);
	}
	else
	{
		return BodyComp->GetBoneLocation(BoneName, EBoneSpaces::ComponentSpace);
	}
}

FVector ABasicWalker::GetBoneLocationByNameString(const FString& BoneName, bool bWorldSpace) const
{
	return GetBoneLocationByName(FName(*BoneName), bWorldSpace);
}

FVector ABasicWalker::GetBoneLocationComponentSpace(const FName& BoneName) const
{
	if (!BodyComp)
	{
		return FVector::ZeroVector;
	}

	return BodyComp->GetBoneLocation(BoneName, EBoneSpaces::ComponentSpace);
}

UAnimInstance* ABasicWalker::GetAnimInstance() const
{
	if (BodyComp)
	{
		return BodyComp->GetAnimInstance();
	}
	return nullptr;
}

bool ABasicWalker::LineTraceToGrid(const FVector& StartLocation, FVector& OutHitLocation, float TraceDistance) const
{
	// Trace downward (negative Z) to find the grid surface
	return LineTraceToGridDirection(StartLocation, FVector(0.0f, 0.0f, -1.0f), OutHitLocation, TraceDistance);
}

bool ABasicWalker::LineTraceToGridDirection(const FVector& StartLocation, const FVector& Direction, FVector& OutHitLocation, float TraceDistance) const
{
	if (!GetWorld())
	{
		return false;
	}

	// Normalize direction
	FVector TraceDirection = Direction.GetSafeNormal();
	if (TraceDirection.IsNearlyZero())
	{
		return false;
	}

	// Calculate end location
	FVector EndLocation = StartLocation + (TraceDirection * TraceDistance);

	// Set up trace parameters
	FCollisionQueryParams QueryParams;
	QueryParams.bTraceComplex = true; // Use complex collision for ISM
	QueryParams.bReturnPhysicalMaterial = false;
	QueryParams.bFindInitialOverlaps = false;
	
	// Ignore this walker and its components
	QueryParams.AddIgnoredActor(this);
	if (BodyComp)
	{
		QueryParams.AddIgnoredComponent(BodyComp);
	}

	// Try multiple trace channels - ISM components might be on different channels
	FHitResult HitResult;
	bool bHit = false;
	
	// Try WorldStatic first (most common for static meshes)
	bHit = GetWorld()->LineTraceSingleByChannel(
		HitResult,
		StartLocation,
		EndLocation,
		ECC_WorldStatic,
		QueryParams
	);
	
	// If that didn't work, try Visibility channel (often used for ISM)
	if (!bHit)
	{
		bHit = GetWorld()->LineTraceSingleByChannel(
			HitResult,
			StartLocation,
			EndLocation,
			ECC_Visibility,
			QueryParams
		);
	}
	
	// If still no hit, try WorldDynamic
	if (!bHit)
	{
		bHit = GetWorld()->LineTraceSingleByChannel(
			HitResult,
			StartLocation,
			EndLocation,
			ECC_WorldDynamic,
			QueryParams
		);
	}
	
	// Last resort: try object type trace (hits anything with collision)
	if (!bHit)
	{
		FCollisionObjectQueryParams ObjectParams;
		ObjectParams.AddObjectTypesToQuery(ECC_WorldStatic);
		ObjectParams.AddObjectTypesToQuery(ECC_WorldDynamic);
		ObjectParams.AddObjectTypesToQuery(ECC_Visibility);
		
		bHit = GetWorld()->LineTraceSingleByObjectType(
			HitResult,
			StartLocation,
			EndLocation,
			ObjectParams,
			QueryParams
		);
	}

	if (bHit && HitResult.bBlockingHit)
	{
		OutHitLocation = HitResult.ImpactPoint;
		return true;
	}

	// No hit - set output to end location
	OutHitLocation = EndLocation;
	return false;
}

bool ABasicWalker::LineTraceToGridDebug(const FVector& StartLocation, FVector& OutHitLocation, float TraceDistance, bool bDrawDebug) const
{
	// Always log that the function was called
	UE_LOG(LogTemp, Warning, TEXT("=== LineTraceToGridDebug CALLED ==="));
	UE_LOG(LogTemp, Warning, TEXT("StartLocation=(%.1f,%.1f,%.1f) TraceDistance=%.1f bDrawDebug=%d"), 
		StartLocation.X, StartLocation.Y, StartLocation.Z, TraceDistance, bDrawDebug ? 1 : 0);
	
	if (!GetWorld())
	{
		UE_LOG(LogTemp, Error, TEXT("LineTraceToGridDebug: No world!"));
		return false;
	}

	FVector EndLocation = StartLocation + FVector(0.0f, 0.0f, -TraceDistance);
	
	// Draw the trace line immediately (before doing the trace) so we can see it
	if (bDrawDebug)
	{
		// Draw a bright yellow line that persists for 10 seconds
		DrawDebugLine(GetWorld(), StartLocation, EndLocation, FColor::Yellow, false, 10.0f, 0, 5.0f);
		// Draw spheres at start and end
		DrawDebugSphere(GetWorld(), StartLocation, 15.0f, 12, FColor::Cyan, false, 10.0f, 0, 3.0f);
		DrawDebugSphere(GetWorld(), EndLocation, 15.0f, 12, FColor::Magenta, false, 10.0f, 0, 3.0f);
	}
	
	FCollisionQueryParams QueryParams;
	QueryParams.bTraceComplex = true;
	QueryParams.bReturnPhysicalMaterial = false;
	QueryParams.AddIgnoredActor(this);
	if (BodyComp)
	{
		QueryParams.AddIgnoredComponent(BodyComp);
	}

	FHitResult HitResult;
	bool bHit = false;
	FString HitChannel = TEXT("None");
	FString HitActorName = TEXT("None");
	FString HitComponentName = TEXT("None");
	
	// Try all channels and log which one works
	struct FChannelInfo
	{
		ECollisionChannel Channel;
		FString Name;
	};
	
	TArray<FChannelInfo> ChannelsToTry = {
		{ ECC_WorldStatic, TEXT("WorldStatic") },
		{ ECC_Visibility, TEXT("Visibility") },
		{ ECC_WorldDynamic, TEXT("WorldDynamic") },
		{ ECC_Pawn, TEXT("Pawn") },
		{ ECC_PhysicsBody, TEXT("PhysicsBody") }
	};
	
	UE_LOG(LogTemp, Warning, TEXT("Trying %d collision channels..."), ChannelsToTry.Num());
	
	for (const FChannelInfo& ChannelInfo : ChannelsToTry)
	{
		bHit = GetWorld()->LineTraceSingleByChannel(HitResult, StartLocation, EndLocation, ChannelInfo.Channel, QueryParams);
		if (bHit)
		{
			HitChannel = ChannelInfo.Name;
			if (HitResult.GetActor())
			{
				HitActorName = HitResult.GetActor()->GetName();
			}
			if (HitResult.GetComponent())
			{
				HitComponentName = HitResult.GetComponent()->GetName();
			}
			UE_LOG(LogTemp, Warning, TEXT("HIT on channel %s!"), *HitChannel);
			break;
		}
	}
	
	// If no hit on channels, try object type trace
	if (!bHit)
	{
		UE_LOG(LogTemp, Warning, TEXT("No hit on channels, trying object type trace..."));
		FCollisionObjectQueryParams ObjectParams;
		ObjectParams.AddObjectTypesToQuery(ECC_WorldStatic);
		ObjectParams.AddObjectTypesToQuery(ECC_WorldDynamic);
		ObjectParams.AddObjectTypesToQuery(ECC_Visibility);
		
		bHit = GetWorld()->LineTraceSingleByObjectType(HitResult, StartLocation, EndLocation, ObjectParams, QueryParams);
		if (bHit)
		{
			HitChannel = TEXT("ObjectType");
			if (HitResult.GetActor())
			{
				HitActorName = HitResult.GetActor()->GetName();
			}
			if (HitResult.GetComponent())
			{
				HitComponentName = HitResult.GetComponent()->GetName();
			}
			UE_LOG(LogTemp, Warning, TEXT("HIT on object type trace!"));
		}
	}
	
	if (bDrawDebug)
	{
		if (bHit)
		{
			// Draw green sphere at hit point
			DrawDebugSphere(GetWorld(), HitResult.ImpactPoint, 20.0f, 12, FColor::Green, false, 10.0f, 0, 4.0f);
			// Draw a line from start to hit point in green
			DrawDebugLine(GetWorld(), StartLocation, HitResult.ImpactPoint, FColor::Green, false, 10.0f, 0, 5.0f);
			// Draw string above hit point
			DrawDebugString(GetWorld(), HitResult.ImpactPoint + FVector(0, 0, 30), 
				FString::Printf(TEXT("HIT!\nActor: %s\nComponent: %s\nChannel: %s"), 
					*HitActorName, *HitComponentName, *HitChannel), 
				nullptr, FColor::White, 10.0f, true);
		}
		else
		{
			// Draw red sphere at end location (no hit)
			DrawDebugSphere(GetWorld(), EndLocation, 20.0f, 12, FColor::Red, false, 10.0f, 0, 4.0f);
			DrawDebugString(GetWorld(), EndLocation + FVector(0, 0, 30), 
				TEXT("NO HIT"), nullptr, FColor::Red, 10.0f, true);
		}
	}
	
	UE_LOG(LogTemp, Warning, TEXT("=== LineTraceToGridDebug RESULT ==="));
	UE_LOG(LogTemp, Warning, TEXT("Hit: %d | Channel: %s | Actor: %s | Component: %s | ImpactPoint: (%.1f,%.1f,%.1f)"),
		bHit ? 1 : 0, *HitChannel, *HitActorName, *HitComponentName,
		bHit ? HitResult.ImpactPoint.X : 0.0f,
		bHit ? HitResult.ImpactPoint.Y : 0.0f,
		bHit ? HitResult.ImpactPoint.Z : 0.0f);
	
	if (bHit)
	{
		OutHitLocation = HitResult.ImpactPoint;
		return true;
	}
	
	OutHitLocation = EndLocation;
	return false;
}

void ABasicWalker::UpdateHeldItemVisuals(UStaticMeshComponent*& HeldComp, bool bIsHolding, const FItem& HeldItem, const FName& FootSocketName)
{
	if (!bIsHolding)
	{
		// Clear visuals if not holding
		ClearHeldItemVisuals(HeldComp);
		return;
	}
	
	// Handle both single orbs and clusters
	if (!HeldItem.bIsCluster && HeldItem.Single.Type == EOrbType::None)
	{
		// Invalid single orb
		ClearHeldItemVisuals(HeldComp);
		return;
	}
	
	if (HeldItem.bIsCluster && HeldItem.Orbs.Num() == 0)
	{
		// Empty cluster
		ClearHeldItemVisuals(HeldComp);
		return;
	}
	
	UE_LOG(LogTemp, Log, TEXT("Walker '%s': Updating held item visuals for socket '%s' - %s with %d orbs"),
		*GetName(), *FootSocketName.ToString(), 
		HeldItem.bIsCluster ? TEXT("Cluster") : TEXT("Single"),
		HeldItem.bIsCluster ? HeldItem.Orbs.Num() : 1);

	// Get orb mesh and materials from OrbMaterialManager
	UOrbMaterialManager* MaterialManager = UOrbMaterialManager::FindInWorld(this);
	if (!MaterialManager)
	{
		UE_LOG(LogTemp, Warning, TEXT("Walker '%s': Cannot find OrbMaterialManager"), *GetName());
		ClearHeldItemVisuals(HeldComp);
		return;
	}
	
	UStaticMesh* OrbMesh = MaterialManager->GetOrbMesh();
	if (!OrbMesh)
	{
		UE_LOG(LogTemp, Warning, TEXT("Walker '%s': OrbMesh is null"), *GetName());
		ClearHeldItemVisuals(HeldComp);
		return;
	}
	
	// Get scale from OrbRenderer
	float OrbScale = 0.30f; // Default
	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			if (UOrbRenderer* Renderer = (*It)->FindComponentByClass<UOrbRenderer>())
			{
				OrbScale = Renderer->OrbScale;
				break;
			}
		}
	}

	// Ensure BodyComp exists
	if (!BodyComp)
	{
		UE_LOG(LogTemp, Error, TEXT("Walker '%s': Cannot attach held item - BodyComp is null!"), *GetName());
		return;
	}

	// Create component if it doesn't exist
	if (!HeldComp)
	{
		HeldComp = NewObject<UStaticMeshComponent>(this, FName(TEXT("HeldOrbComp")));
		
		// Check if socket exists
		if (BodyComp->DoesSocketExist(FootSocketName))
		{
			HeldComp->SetupAttachment(BodyComp, FootSocketName);
			UE_LOG(LogTemp, Log, TEXT("Walker '%s': Created HeldComp attached to socket '%s'"), *GetName(), *FootSocketName.ToString());
		}
		else
		{
			// Socket doesn't exist, attach to root of skeletal mesh
			HeldComp->SetupAttachment(BodyComp);
			UE_LOG(LogTemp, Warning, TEXT("Walker '%s': Socket '%s' not found, attaching to BodyComp root"), 
				*GetName(), *FootSocketName.ToString());
		}
		
		HeldComp->RegisterComponent();
		HeldComp->SetMobility(EComponentMobility::Movable);
		HeldComp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		HeldComp->SetCastShadow(false); // Optional - prevent shadow conflicts
		
		UE_LOG(LogTemp, Log, TEXT("Walker '%s': HeldComp created and registered"), *GetName());
	}

	// Clear existing child meshes
	TArray<USceneComponent*> ChildComps;
	HeldComp->GetChildrenComponents(false, ChildComps);
	for (USceneComponent* Child : ChildComps)
	{
		if (Child)
		{
			Child->DestroyComponent();
		}
	}

	// Set visibility
	HeldComp->SetVisibility(true, true);
	HeldComp->SetStaticMesh(nullptr); // Container doesn't need a mesh

	// Handle single orbs
	if (!HeldItem.bIsCluster)
	{
		UStaticMeshComponent* SubMesh = NewObject<UStaticMeshComponent>(this);
		SubMesh->SetStaticMesh(OrbMesh);
		SubMesh->SetMobility(EComponentMobility::Movable);
		SubMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		SubMesh->RegisterComponent();
		SubMesh->AttachToComponent(HeldComp, FAttachmentTransformRules::KeepRelativeTransform);
		SubMesh->SetRelativeLocation(FVector::ZeroVector);
		SubMesh->SetRelativeScale3D(FVector(OrbScale));
		
		// Get material for this orb type
		UMaterialInterface* OrbMat = MaterialManager->GetMaterialForOrbType(HeldItem.Single.Type);
		if (OrbMat)
		{
			SubMesh->SetMaterial(0, OrbMat);
		}
		
		UE_LOG(LogTemp, Log, TEXT("Walker '%s': Spawned single orb visual (Type: %d)"), *GetName(), (int32)HeldItem.Single.Type);
		return;
	}

	// Spawn orb meshes for each orb in cluster
	UE_LOG(LogTemp, Log, TEXT("Walker '%s': Spawning %d orb meshes for held cluster"), *GetName(), HeldItem.Orbs.Num());
	
	for (int32 i = 0; i < HeldItem.Orbs.Num(); ++i)
	{
		const FOrbData& OrbData = HeldItem.Orbs[i];

		UStaticMeshComponent* SubMesh = NewObject<UStaticMeshComponent>(this);
		SubMesh->SetStaticMesh(OrbMesh); // Use actual orb mesh, not bonded mesh
		SubMesh->SetMobility(EComponentMobility::Movable);
		SubMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		SubMesh->RegisterComponent();
		SubMesh->AttachToComponent(HeldComp, FAttachmentTransformRules::KeepRelativeTransform);

		// Position based on dQ, dR (hex coordinates within cluster)
		// Convert hex offset to world offset (match grid scale)
		const float HexSize = 100.f; // Match grid hex size
		const float X = HexSize * 1.7320508f * (float)OrbData.dQ + HexSize * 0.8660254f * (float)OrbData.dR;
		const float Y = HexSize * 1.5f * (float)OrbData.dR;
		SubMesh->SetRelativeLocation(FVector(X, Y, 0.f));

		// Set orb scale
		SubMesh->SetRelativeScale3D(FVector(OrbScale));
		
		// Get material for this orb type
		EOrbType OrbType = (EOrbType)OrbData.Type;
		UMaterialInterface* OrbMat = MaterialManager->GetMaterialForOrbType(OrbType);
		if (OrbMat)
		{
			SubMesh->SetMaterial(0, OrbMat);
		}
		
		UE_LOG(LogTemp, Verbose, TEXT("  Orb %d: Type=%d, dQ=%d, dR=%d, Pos=(%.1f, %.1f, 0), Scale=%.2f"), 
			i, OrbData.Type, OrbData.dQ, OrbData.dR, X, Y, OrbScale);
	}
}

void ABasicWalker::ClearHeldItemVisuals(UStaticMeshComponent*& HeldComp)
{
	if (HeldComp)
	{
		HeldComp->SetVisibility(false, true);
		
		// Clear child meshes
		TArray<USceneComponent*> ChildComps;
		HeldComp->GetChildrenComponents(false, ChildComps);
		for (USceneComponent* Child : ChildComps)
		{
			if (Child)
			{
				Child->DestroyComponent();
			}
		}
	}
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

