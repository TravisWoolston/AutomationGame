#include "Actors/BasicWalker.h"
#include "Actors/WalkerAnimInstance.h"
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
											const FVector NewGoal = WorldToComponentNoScale.TransformPosition(FootWorld);
											
											// Validate goal before setting - NaN would crash the IK solver
											if (!NewGoal.ContainsNaN())
											{
												*GoalValue = NewGoal;
											}
											else
											{
												UE_LOG(LogTemp, Warning, TEXT("Walker %s: Skipping NaN foot goal for %s (FootWorld=%s)"), 
													*GetName(), *GoalName.ToString(), *FootWorld.ToString());
											}
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
									const FVector NewGoal = WorldToComponentNoScale.TransformPosition(HexWorldPos);
									
									// Validate goal before setting - NaN would crash the IK solver
									if (!NewGoal.ContainsNaN())
									{
										*GoalValue = NewGoal;
									}
									else
									{
										UE_LOG(LogTemp, Warning, TEXT("Walker %s: Skipping NaN body goal (HexWorldPos=%s)"), 
											*GetName(), *HexWorldPos.ToString());
									}
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

		// Smoothly interpolate height offsets toward sim values
		// This prevents visual snapping when sim states change faster than visual frames
		// Calculate HeightInterpSpeed based on GTM tick rate to stay in sync with Progress updates
		float HeightInterpSpeed = 15.0f; // Default fallback
		if (AGameTickManager* GTM = AGameTickManager::Get(this))
		{
			// Match the position interpolation speed (2.0 * TicksPerSecond) for consistent motion
			// This ensures height and horizontal position reach their targets at similar rates
			HeightInterpSpeed = 2.0f * FMath::Max(0.01f, GTM->TicksPerSecond);
		}
		DFootSmoothedHeightOffset = FMath::FInterpTo(DFootSmoothedHeightOffset, DFootHeightOffset, DeltaSeconds, HeightInterpSpeed);
		LFootSmoothedHeightOffset = FMath::FInterpTo(LFootSmoothedHeightOffset, LFootHeightOffset, DeltaSeconds, HeightInterpSpeed);
		RFootSmoothedHeightOffset = FMath::FInterpTo(RFootSmoothedHeightOffset, RFootHeightOffset, DeltaSeconds, HeightInterpSpeed);
		UFootSmoothedHeightOffset = FMath::FInterpTo(UFootSmoothedHeightOffset, UFootHeightOffset, DeltaSeconds, HeightInterpSpeed);
		
		// Copy to public properties for AnimBP access
		DFootSmoothedHeight = DFootSmoothedHeightOffset;
		LFootSmoothedHeight = LFootSmoothedHeightOffset;
		RFootSmoothedHeight = RFootSmoothedHeightOffset;
		UFootSmoothedHeight = UFootSmoothedHeightOffset;

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

		// Log state changes and handle drop visual transitions
		if (bDFootHoldingItem != bPrevDHolding)
		{
			UE_LOG(LogTemp, Log, TEXT("Walker '%s': DFoot holding changed to %d"), *GetName(), bDFootHoldingItem ? 1 : 0);
			if (!bDFootHoldingItem && bPrevDHolding)
			{
				// Transitioning to NOT holding - start pending drop visual
				bDFootPendingDropVisual = true;
				bDFootHeldVisualReady = false;
				DFootDropTargetHex = Sim->Walkers[SimWalkerIndex].DFoot.CurrentTile;
				DFootCachedDropItem = DFootHeldItem; // Cache the item for continued rendering
			}
		}
		if (bLFootHoldingItem != bPrevLHolding)
		{
			UE_LOG(LogTemp, Log, TEXT("Walker '%s': LFoot holding changed to %d"), *GetName(), bLFootHoldingItem ? 1 : 0);
			if (!bLFootHoldingItem && bPrevLHolding)
			{
				bLFootPendingDropVisual = true;
				bLFootHeldVisualReady = false;
				LFootDropTargetHex = Sim->Walkers[SimWalkerIndex].LFoot.CurrentTile;
				LFootCachedDropItem = LFootHeldItem;
			}
		}
		if (bRFootHoldingItem != bPrevRHolding)
		{
			UE_LOG(LogTemp, Log, TEXT("Walker '%s': RFoot holding changed to %d"), *GetName(), bRFootHoldingItem ? 1 : 0);
			if (!bRFootHoldingItem && bPrevRHolding)
			{
				bRFootPendingDropVisual = true;
				bRFootHeldVisualReady = false;
				RFootDropTargetHex = Sim->Walkers[SimWalkerIndex].RFoot.CurrentTile;
				RFootCachedDropItem = RFootHeldItem;
			}
		}
		if (bUFootHoldingItem != bPrevUHolding)
		{
			UE_LOG(LogTemp, Log, TEXT("Walker '%s': UFoot holding changed to %d"), *GetName(), bUFootHoldingItem ? 1 : 0);
			if (!bUFootHoldingItem && bPrevUHolding)
			{
				bUFootPendingDropVisual = true;
				bUFootHeldVisualReady = false;
				UFootDropTargetHex = Sim->Walkers[SimWalkerIndex].UFoot.CurrentTile;
				UFootCachedDropItem = UFootHeldItem;
			}
		}
		
		// NOTE: UpdateHeldItemVisuals is now called AFTER smoothed positions are calculated,
		// to delay showing held items until feet have visually arrived at pickup/dropoff tiles.

		// --- Foot Visual Update Events ---
		// Calculate progress and call UpdateFoot*Visual for feet that are animating (progress < 1)
		// Helper lambda to calculate progress and visual state
		auto CalcFootVisualProgress = [&](const FFootState& FootState, bool bIsHolding) -> TPair<float, EFootVisualState>
		{
			if (bIsHolding)
			{
				return TPair<float, EFootVisualState>(0.5f, EFootVisualState::Holding);
			}
			
			float TotalTicks = (float)(SimState.StepRaiseTicks + SimState.StepMoveTicks + SimState.StepLowerTicks);
			if (TotalTicks <= 0.f || FootState.StepState == EFootStepState::Planted)
			{
				return TPair<float, EFootVisualState>(1.0f, EFootVisualState::Planted);
			}

			float CurrentTick = 0.f;
			EFootVisualState VisState = EFootVisualState::Planted;
			
			switch (FootState.StepState)
			{
			case EFootStepState::Raising:
				CurrentTick = (float)FootState.StepProgress;
				VisState = EFootVisualState::Raising;
				break;
			case EFootStepState::Moving:
				CurrentTick = (float)SimState.StepRaiseTicks + (float)FootState.StepProgress;
				VisState = EFootVisualState::Moving;
				break;
			case EFootStepState::Lowering:
				CurrentTick = (float)SimState.StepRaiseTicks + (float)SimState.StepMoveTicks + (float)FootState.StepProgress;
				VisState = EFootVisualState::Lowering;
				break;
			}
			
			return TPair<float, EFootVisualState>(CurrentTick / TotalTicks, VisState);
		};
		
		// Helper to convert EFootVisualState to EFootAnimState for AnimInstance
		auto ToAnimState = [](EFootVisualState S) -> EFootAnimState {
			switch (S) {
				case EFootVisualState::Planted:  return EFootAnimState::Planted;
				case EFootVisualState::Raising:  return EFootAnimState::Raising;
				case EFootVisualState::Moving:   return EFootAnimState::Moving;
				case EFootVisualState::Lowering: return EFootAnimState::Lowering;
				case EFootVisualState::Holding:  return EFootAnimState::Holding;
				default: return EFootAnimState::Planted;
			}
		};
		
		// Get WalkerAnimInstance (only if we have a body component with animation)
		UWalkerAnimInstance* WalkerAnim = nullptr;
		if (BodyComp)
		{
			WalkerAnim = Cast<UWalkerAnimInstance>(BodyComp->GetAnimInstance());
		}
		
		// Helper to get correct previous/current positions based on phase
		// - Raising: foot rises in place (Previous = Current = CurrentTile)
		// - Moving: foot moves horizontally (Previous = start position, Current = TargetTile)
		// - Lowering: foot lowers in place (Previous = Current = CurrentTile)
		// - Planted: foot is stationary (Previous = Current = CurrentTile)
		auto GetFootPositions = [&](const FFootState& FootState, const FHex& FootHex, 
			EFootStepState PrevState, EFootStepState CurrState, FVector& VisualStartPos) 
			-> TPair<FVector, FVector>
		{
			FVector CurrentPos = HexToWorld(FootState.TargetTile);
			FVector PreviousPos;
			
			// Capture start position at the very beginning of the transition from Planted
			if (CurrState != EFootStepState::Planted && PrevState == EFootStepState::Planted)
			{
				VisualStartPos = HexToWorld(FootHex);
				UE_LOG(LogTemp, Log, TEXT("Captured VisualStartPos for horizontal flight: (%.0f,%.0f,%.0f) from FootHex (%d,%d)"),
					VisualStartPos.X, VisualStartPos.Y, VisualStartPos.Z, FootHex.Q, FootHex.R);
			}
			
			if (CurrState != EFootStepState::Planted)
			{
				// In-flight: interpolate from the captured start to the destination target
				PreviousPos = VisualStartPos;
				CurrentPos = HexToWorld(FootState.TargetTile);
			}
			else
			{
				// Stationary/Planted
				PreviousPos = HexToWorld(FootHex);
				CurrentPos = HexToWorld(FootHex);
			}
			
			return TPair<FVector, FVector>(PreviousPos, CurrentPos);
		};
		
		// Calculate and update for each foot
		// Only call AnimInstance events when sim has actually updated (StepProgress changed)
		{
			auto [DProgress, DVisState] = CalcFootVisualProgress(SimState.DFoot, bDFootHoldingItem);
			auto [PreviousPos, CurrentPos] = GetFootPositions(SimState.DFoot, DFootHex, 
				DFootPrevStepState, SimState.DFoot.StepState, DFootVisualStartPos);
			
			// Update public properties for direct access (always, for interpolation in AnimBP)
			DFootProgress = DProgress;
			DFootVisualState = DVisState;
			DFootTargetWorldPosition = CurrentPos;
			DFootPreviousWorldPosition = PreviousPos;
			
			// Only call events when sim has actually updated (state or progress changed)
			bool bSimUpdated = (SimState.DFoot.StepState != DFootPrevStepState) || 
				(SimState.DFoot.StepProgress != DFootPrevStepProgress);
			
			if (bSimUpdated && DProgress < 1.0f)
			{
				if (WalkerAnim)
				{
					WalkerAnim->UpdateFootDVisual(DProgress, ToAnimState(DVisState), CurrentPos, PreviousPos);
				}
				UpdateFootDVisual(DProgress, DVisState, CurrentPos, PreviousPos);
			}
			
			// Update previous state for next tick
			DFootPrevStepState = SimState.DFoot.StepState;
			DFootPrevStepProgress = SimState.DFoot.StepProgress;
		}
		{
			auto [LProgress, LVisState] = CalcFootVisualProgress(SimState.LFoot, bLFootHoldingItem);
			auto [PreviousPos, CurrentPos] = GetFootPositions(SimState.LFoot, LFootHex,
				LFootPrevStepState, SimState.LFoot.StepState, LFootVisualStartPos);
			
			LFootProgress = LProgress;
			LFootVisualState = LVisState;
			LFootTargetWorldPosition = CurrentPos;
			LFootPreviousWorldPosition = PreviousPos;
			
			bool bSimUpdated = (SimState.LFoot.StepState != LFootPrevStepState) || 
				(SimState.LFoot.StepProgress != LFootPrevStepProgress);
			
			if (bSimUpdated && LProgress < 1.0f)
			{
				if (WalkerAnim)
				{
					WalkerAnim->UpdateFootLVisual(LProgress, ToAnimState(LVisState), CurrentPos, PreviousPos);
				}
				UpdateFootLVisual(LProgress, LVisState, CurrentPos, PreviousPos);
			}
			
			LFootPrevStepState = SimState.LFoot.StepState;
			LFootPrevStepProgress = SimState.LFoot.StepProgress;
		}
		{
			auto [RProgress, RVisState] = CalcFootVisualProgress(SimState.RFoot, bRFootHoldingItem);
			auto [PreviousPos, CurrentPos] = GetFootPositions(SimState.RFoot, RFootHex,
				RFootPrevStepState, SimState.RFoot.StepState, RFootVisualStartPos);
			
			RFootProgress = RProgress;
			RFootVisualState = RVisState;
			RFootTargetWorldPosition = CurrentPos;
			RFootPreviousWorldPosition = PreviousPos;
			
			bool bSimUpdated = (SimState.RFoot.StepState != RFootPrevStepState) || 
				(SimState.RFoot.StepProgress != RFootPrevStepProgress);
			
			if (bSimUpdated && RProgress < 1.0f)
			{
				if (WalkerAnim)
				{
					WalkerAnim->UpdateFootRVisual(RProgress, ToAnimState(RVisState), CurrentPos, PreviousPos);
				}
				UpdateFootRVisual(RProgress, RVisState, CurrentPos, PreviousPos);
			}
			
			RFootPrevStepState = SimState.RFoot.StepState;
			RFootPrevStepProgress = SimState.RFoot.StepProgress;
		}
		{
			auto [UProgress, UVisState] = CalcFootVisualProgress(SimState.UFoot, bUFootHoldingItem);
			auto [PreviousPos, CurrentPos] = GetFootPositions(SimState.UFoot, UFootHex,
				UFootPrevStepState, SimState.UFoot.StepState, UFootVisualStartPos);
			
			UFootProgress = UProgress;
			UFootVisualState = UVisState;
			UFootTargetWorldPosition = CurrentPos;
			UFootPreviousWorldPosition = PreviousPos;
			
			bool bSimUpdated = (SimState.UFoot.StepState != UFootPrevStepState) || 
				(SimState.UFoot.StepProgress != UFootPrevStepProgress);
			
			if (bSimUpdated && UProgress < 1.0f)
			{
				if (WalkerAnim)
				{
					WalkerAnim->UpdateFootUVisual(UProgress, ToAnimState(UVisState), CurrentPos, PreviousPos);
				}
				UpdateFootUVisual(UProgress, UVisState, CurrentPos, PreviousPos);
			}
			
			UFootPrevStepState = SimState.UFoot.StepState;
			UFootPrevStepProgress = SimState.UFoot.StepProgress;
		}

		// Calculate smoothed foot world positions
		// Skip if ManualFootGoal is enabled (positions are determined by Event Graph)
		// SmoothedHeight values are still calculated regardless
		{
			// Check if ManualFootGoal is enabled
			bool bManualFootGoal = false;
			if (UWorld* World = GetWorld())
			{
				for (TActorIterator<AGameManager> It(World); It; ++It)
				{
					AGameManager* GM = *It;
					if (GM && GM->bManualFootGoal)
					{
						bManualFootGoal = true;
						break;
					}
				}
			}
			
			if (!bManualFootGoal)
			{
				// Use same GTM-synced interp speed as height
				float PosInterpSpeed = 12.0f; // Default fallback
				if (AGameTickManager* GTM = AGameTickManager::Get(this))
				{
					PosInterpSpeed = 2.0f * FMath::Max(0.01f, GTM->TicksPerSecond);
				}
				
				// Calculate target positions based on current progress
				auto CalcProgressedPos = [](const FVector& Previous, const FVector& Target, float Progress, float SmoothedHeight) -> FVector
				{
					FVector Pos = FMath::Lerp(Previous, Target, FMath::Clamp(Progress, 0.f, 1.f));
					Pos.Z += SmoothedHeight; // Add smoothed height offset
					return Pos;
				};
				
				FVector DTarget = CalcProgressedPos(DFootPreviousWorldPosition, DFootTargetWorldPosition, DFootProgress, DFootSmoothedHeightOffset);
				FVector LTarget = CalcProgressedPos(LFootPreviousWorldPosition, LFootTargetWorldPosition, LFootProgress, LFootSmoothedHeightOffset);
				FVector RTarget = CalcProgressedPos(RFootPreviousWorldPosition, RFootTargetWorldPosition, RFootProgress, RFootSmoothedHeightOffset);
				FVector UTarget = CalcProgressedPos(UFootPreviousWorldPosition, UFootTargetWorldPosition, UFootProgress, UFootSmoothedHeightOffset);
				
				if (!bSmoothedPositionsInitialized)
				{
					// First frame: snap to target positions
					DFootSmoothedWorldPosition = DTarget;
					LFootSmoothedWorldPosition = LTarget;
					RFootSmoothedWorldPosition = RTarget;
					UFootSmoothedWorldPosition = UTarget;
					bSmoothedPositionsInitialized = true;
				}
				else
				{
					// Smoothly interpolate toward target positions
					DFootSmoothedWorldPosition = FMath::VInterpTo(DFootSmoothedWorldPosition, DTarget, DeltaSeconds, PosInterpSpeed);
					LFootSmoothedWorldPosition = FMath::VInterpTo(LFootSmoothedWorldPosition, LTarget, DeltaSeconds, PosInterpSpeed);
					RFootSmoothedWorldPosition = FMath::VInterpTo(RFootSmoothedWorldPosition, RTarget, DeltaSeconds, PosInterpSpeed);
					UFootSmoothedWorldPosition = FMath::VInterpTo(UFootSmoothedWorldPosition, UTarget, DeltaSeconds, PosInterpSpeed);
				}
			}
		}

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
										const FVector NewGoal = WorldToComponentNoScale.TransformPosition(FootGoalWorld);
										
										// Validate goal before setting - NaN would crash the IK solver
										if (!NewGoal.ContainsNaN())
										{
											*GoalValue = NewGoal;
										}
										else
										{
											UE_LOG(LogTemp, Warning, TEXT("Walker %s: Skipping NaN foot goal for %s Tile(%d,%d) FootGoalWorld=%s"), 
												*GetName(), *GoalName.ToString(), AssignedTile.Q, AssignedTile.R, *FootGoalWorld.ToString());
										}
										
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
							const FVector NewGoal = WorldToComponentNoScale.TransformPosition(BodyEndWorld);
							
							// Validate goal before setting - NaN would crash the IK solver
							if (!NewGoal.ContainsNaN())
							{
								*GoalValue = NewGoal;
							}
							else
							{
								UE_LOG(LogTemp, Warning, TEXT("Walker %s: Skipping NaN body goal in Tick (BodyEndWorld=%s)"), 
									*GetName(), *BodyEndWorld.ToString());
							}
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
	// When bDynamicBodyRotation is true, automatically update each tick
	// Otherwise, call UpdateBodyRotationFromFeet() manually from Event Graph for optimization
	if (bDynamicBodyRotation)
	{
		UpdateBodyRotationFromFeet();
	}

	// --- Deferred Held Item Visuals ---
	// Only show held item visual once foot has visually arrived at target tile
	// This prevents items from appearing before the foot animation completes
	if (Sim && SimWalkerIndex >= 0 && SimWalkerIndex < Sim->Walkers.Num() && Driver && BodyComp)
	{
		const FWalkerState& SimState = Sim->Walkers[SimWalkerIndex];
		
		// Use actual tile size for threshold - foot should be within ~25% of tile radius
		const float TileSize = Driver->GetTileSize();
		const float ArrivalThresholdSq = (TileSize * 0.25f) * (TileSize * 0.25f);
		
		// Get actual foot bone world positions (works correctly with ManualFootGoals)
		const FVector DFootBonePos = BodyComp->GetBoneLocation(DFootBoneName, EBoneSpaces::WorldSpace);
		const FVector LFootBonePos = BodyComp->GetBoneLocation(LFootBoneName, EBoneSpaces::WorldSpace);
		const FVector RFootBonePos = BodyComp->GetBoneLocation(RFootBoneName, EBoneSpaces::WorldSpace);
		const FVector UFootBonePos = BodyComp->GetBoneLocation(UFootBoneName, EBoneSpaces::WorldSpace);
		
		// Lambda to check foot arrival and handle both pickup and drop visual timing
		// Returns: pair of (bShowItem, ItemToShow)
		auto CheckFootVisualState = [&](bool bSimHolding, bool& bVisualReady, bool& bPendingDrop, 
			const FVector& BonePos, const FHex& CurrentTile, const FHex& DropTargetHex,
			const FItem& HeldItem, const FItem& CachedDropItem) -> TPair<bool, FItem>
		{
			// Case 1: Pending drop visual - sim dropped but foot hasn't arrived visually
			if (bPendingDrop)
			{
				const FVector DropWorld = Driver->MapHexToWorld(DropTargetHex);
				const float DistSq = FVector::DistSquared2D(BonePos, DropWorld);
				
				if (DistSq < ArrivalThresholdSq)
				{
					// Foot has visually arrived at drop location - stop showing
					bPendingDrop = false;
					return TPair<bool, FItem>(false, FItem());
				}
				
				// Still in transit to drop location - keep showing cached item
				return TPair<bool, FItem>(true, CachedDropItem);
			}
			
			// Case 2: Not holding
			if (!bSimHolding)
			{
				bVisualReady = false;
				return TPair<bool, FItem>(false, FItem());
			}
			
			// Case 3: Already visually ready (holding and arrived) - keep showing
			if (bVisualReady)
			{
				return TPair<bool, FItem>(true, HeldItem);
			}
			
			// Case 4: Holding but waiting for visual arrival at pickup
			// NOTE: For holding feet that are constantly recentering, CurrentTile changes every tick.
			// Use a more lenient check - if we've been holding for a bit, just show it.
			// The sim sets bHoldingItem=true when pickup completes, so we can trust that.
			// Set bVisualReady=true immediately to avoid the check failing due to moving CurrentTile.
			bVisualReady = true;
			return TPair<bool, FItem>(true, HeldItem);
		};
		
		auto DResult = CheckFootVisualState(bDFootHoldingItem, bDFootHeldVisualReady, bDFootPendingDropVisual,
			DFootBonePos, SimState.DFoot.CurrentTile, DFootDropTargetHex, DFootHeldItem, DFootCachedDropItem);
		auto LResult = CheckFootVisualState(bLFootHoldingItem, bLFootHeldVisualReady, bLFootPendingDropVisual,
			LFootBonePos, SimState.LFoot.CurrentTile, LFootDropTargetHex, LFootHeldItem, LFootCachedDropItem);
		auto RResult = CheckFootVisualState(bRFootHoldingItem, bRFootHeldVisualReady, bRFootPendingDropVisual,
			RFootBonePos, SimState.RFoot.CurrentTile, RFootDropTargetHex, RFootHeldItem, RFootCachedDropItem);
		auto UResult = CheckFootVisualState(bUFootHoldingItem, bUFootHeldVisualReady, bUFootPendingDropVisual,
			UFootBonePos, SimState.UFoot.CurrentTile, UFootDropTargetHex, UFootHeldItem, UFootCachedDropItem);
		
		// Update held item visuals based on visual arrival state
		UpdateHeldItemVisuals(DFootHeldOrbComp, DResult.Key, DResult.Value, FName(TEXT("DFoot_Socket")), CachedDFootHeldItemId);
		UpdateHeldItemVisuals(LFootHeldOrbComp, LResult.Key, LResult.Value, FName(TEXT("LFoot_Socket")), CachedLFootHeldItemId);
		UpdateHeldItemVisuals(RFootHeldOrbComp, RResult.Key, RResult.Value, FName(TEXT("RFoot_Socket")), CachedRFootHeldItemId);
		UpdateHeldItemVisuals(UFootHeldOrbComp, UResult.Key, UResult.Value, FName(TEXT("UFoot_Socket")), CachedUFootHeldItemId);
	}
	else
	{
		// Fallback when no simulation or body comp - use immediate holding state
		UpdateHeldItemVisuals(DFootHeldOrbComp, bDFootHoldingItem, DFootHeldItem, FName(TEXT("DFoot_Socket")), CachedDFootHeldItemId);
		UpdateHeldItemVisuals(LFootHeldOrbComp, bLFootHoldingItem, LFootHeldItem, FName(TEXT("LFoot_Socket")), CachedLFootHeldItemId);
		UpdateHeldItemVisuals(RFootHeldOrbComp, bRFootHoldingItem, RFootHeldItem, FName(TEXT("RFoot_Socket")), CachedRFootHeldItemId);
		UpdateHeldItemVisuals(UFootHeldOrbComp, bUFootHoldingItem, UFootHeldItem, FName(TEXT("UFoot_Socket")), CachedUFootHeldItemId);
	}

	// Update Held Item Comp Rotation (if exists and is absolute)
	// We need to check all 4 feet since HeldComp is not a single member
	UStaticMeshComponent* HandComps[] = { DFootHeldOrbComp, LFootHeldOrbComp, RFootHeldOrbComp, UFootHeldOrbComp };
	
	FRotator TargetRot = GetActorRotation();
	// Offset logic consistent with previous fix (0 offset)
	float ClusterRotationDeg = 0.f; 
	TargetRot.Yaw += ClusterRotationDeg;
	
	for (UStaticMeshComponent* Comp : HandComps)
	{
		if (Comp && Comp->IsUsingAbsoluteRotation())
		{
			Comp->SetWorldRotation(TargetRot);
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
				// Get body bone location (using class member BodyBoneName)
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

void ABasicWalker::UpdateBodyRotationFromFeet()
{
	if (!BodyComp)
	{
		return;
	}
	
	// Get body center location using configurable bone names
	FVector BodyLoc = GetActorLocation();
	if (BodyComp->GetBoneIndex(BodyBoneName) != INDEX_NONE)
	{
		BodyLoc = BodyComp->GetBoneLocation(BodyBoneName, EBoneSpaces::WorldSpace);
	}
	else if (BodyComp->GetBoneIndex(BodyBoneNameFallback) != INDEX_NONE)
	{
		BodyLoc = BodyComp->GetBoneLocation(BodyBoneNameFallback, EBoneSpaces::WorldSpace);
	}

	float TotalWeightedSin = 0.f;
	float TotalWeightedCos = 0.f;
	float TotalWeight = 0.f;

	struct FFootRotInfo {
		FVector Pos;
		float IdealAngle; // Degrees: U=0, R=90, D=180, L=-90 (Front, Right, Back, Left)
		bool bIsValid;
	};

	// Get actual foot bone world positions from the skeletal mesh using configurable bone names
	FVector ActualFootPositions[4] = { FVector::ZeroVector, FVector::ZeroVector, FVector::ZeroVector, FVector::ZeroVector };
	ActualFootPositions[0] = BodyComp->GetBoneLocation(DFootBoneName, EBoneSpaces::WorldSpace);  // D/South/Back
	ActualFootPositions[1] = BodyComp->GetBoneLocation(LFootBoneName, EBoneSpaces::WorldSpace);  // L/West/Left
	ActualFootPositions[2] = BodyComp->GetBoneLocation(RFootBoneName, EBoneSpaces::WorldSpace);  // R/East/Right
	ActualFootPositions[3] = BodyComp->GetBoneLocation(UFootBoneName, EBoneSpaces::WorldSpace);  // U/North/Front

	// Ideal angles relative to body forward (0)
	const FFootRotInfo Feet[] = {
		{ ActualFootPositions[3], 0.f, !ActualFootPositions[3].IsNearlyZero() },   // U/North/Front
		{ ActualFootPositions[2], 90.f, !ActualFootPositions[2].IsNearlyZero() },  // R/East/Right
		{ ActualFootPositions[0], 180.f, !ActualFootPositions[0].IsNearlyZero() }, // D/South/Back
		{ ActualFootPositions[1], -90.f, !ActualFootPositions[1].IsNearlyZero() }  // L/West/Left
	};

	const float CurrentBodyYaw = GetActorRotation().Yaw;

	for (const FFootRotInfo& Foot : Feet)
	{
		if (!Foot.bIsValid) continue;

		FVector ToFoot = Foot.Pos - BodyLoc;
		if (ToFoot.SizeSquared2D() < KINDA_SMALL_NUMBER) continue;

		float FootWorldYaw = ToFoot.Rotation().Yaw;

		// Calculate where the body "should" be to put this foot at IdealAngle
		float DesiredBodyYaw = FRotator::NormalizeAxis(FootWorldYaw - Foot.IdealAngle);

		// Weighting: feet nearer to quadrant boundaries have more influence
		float RelativeAngle = FRotator::NormalizeAxis(FootWorldYaw - CurrentBodyYaw);
		float Deviation = FRotator::NormalizeAxis(RelativeAngle - Foot.IdealAngle);
		float Weight = FMath::Abs(Deviation);

		// Add to circular average
		float Rad = FMath::DegreesToRadians(DesiredBodyYaw);
		TotalWeightedSin += FMath::Sin(Rad) * Weight;
		TotalWeightedCos += FMath::Cos(Rad) * Weight;
		TotalWeight += Weight;
	}

	if (TotalWeight > KINDA_SMALL_NUMBER)
	{
		float NewAvgYaw = FMath::RadiansToDegrees(FMath::Atan2(TotalWeightedSin, TotalWeightedCos));
		
		// Smooth the body rotation to avoid twitching
		float DeltaYaw = FRotator::NormalizeAxis(NewAvgYaw - AvgYaw);
		
		if (FMath::Abs(DeltaYaw) > 0.01f)
		{
			const float SmoothFactor = 0.15f;
			AvgYaw = FRotator::NormalizeAxis(AvgYaw + DeltaYaw * SmoothFactor);
		}
		
		BodyGoalRotation = FRotator(90.f, 0.f, AvgYaw);
		QuadrantRotationOffset = AvgYaw;
	}
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

void ABasicWalker::UpdateHeldItemVisuals(UStaticMeshComponent*& HeldComp, bool bIsHolding, const FItem& HeldItem, const FName& FootSocketName, int32& CachedItemId)
{
	// Determine current item ID (0 if not holding or invalid)
	int32 CurrentItemId = 0;
	if (bIsHolding)
	{
		CurrentItemId = HeldItem.Id;
	}
	
	// PERFORMANCE: Only update visuals if the held item actually changed
	if (CurrentItemId == CachedItemId && HeldComp != nullptr)
	{
		// Same item (or still not holding) - no need to recreate visuals
		return;
	}
	
	// Update cache
	CachedItemId = CurrentItemId;
	
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
	
	// PERFORMANCE: Use cached OrbRenderer instead of scanning all actors
	float OrbScale = 1.6f; // Default
	if (UOrbRenderer* Renderer = GetCachedOrbRenderer())
	{
		OrbScale = Renderer->OrbScale;
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
		HeldComp = NewObject<UStaticMeshComponent>(this, FName(*FString::Printf(TEXT("HeldOrbComp_%s"), *FootSocketName.ToString())));
		
		// Check if socket exists
		if (BodyComp->DoesSocketExist(FootSocketName))
		{
			HeldComp->SetupAttachment(BodyComp, FootSocketName);
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
		HeldComp->SetCastShadow(false);
	}
	
	// Set visibility
	HeldComp->SetVisibility(true, true);
	
	// Clear existing child meshes (renamed to avoid shadowing AActor::Children)
	TArray<USceneComponent*> ChildComps;
	HeldComp->GetChildrenComponents(false, ChildComps);
	for (USceneComponent* Child : ChildComps)
	{
		if (Child) Child->DestroyComponent();
	}

	// Calculate Rotation - FIXED per user request: Always 30 degrees for walked clusters
	float ClusterRotationDeg = 30.f;
	
	// Use absolute rotation to maintain world orientation regardless of foot rotation
	HeldComp->SetUsingAbsoluteRotation(true);
	HeldComp->SetWorldRotation(FRotator(0.f, ClusterRotationDeg, 0.f));

	// If single orb
	if (!HeldItem.bIsCluster)
	{
		UStaticMeshComponent* SubMesh = NewObject<UStaticMeshComponent>(HeldComp);
		SubMesh->RegisterComponent();
		SubMesh->AttachToComponent(HeldComp, FAttachmentTransformRules::KeepRelativeTransform);
		SubMesh->SetRelativeLocation(FVector::ZeroVector);
		SubMesh->SetStaticMesh(OrbMesh);
		SubMesh->SetRelativeScale3D(FVector(OrbScale));
		
		UMaterialInterface* OrbMaterial = MaterialManager->GetMaterialForOrbType(HeldItem.Single.Type);
		if (OrbMaterial)
		{
			SubMesh->SetMaterial(0, OrbMaterial);
		}
	}
	else
	{
		// Cluster
		HeldComp->SetStaticMesh(nullptr); // Container mesh
		HeldComp->SetRelativeScale3D(FVector(1.f)); // Container scale
		
		const float TileSize = (Driver ? Driver->GetTileSize() : 100.f);
		const EHexOrientation GridOrientation = (Driver ? Driver->GetGridOrientation() : EHexOrientation::PointyTop);
		
		// Visualize sub-orbs
		for (const FOrbData& Sub : HeldItem.Orbs)
		{
			UStaticMeshComponent* SubMesh = NewObject<UStaticMeshComponent>(HeldComp);
			SubMesh->RegisterComponent();
			SubMesh->AttachToComponent(HeldComp, FAttachmentTransformRules::KeepRelativeTransform);
			
			// Calculate offset
			const FVector2D XY = HexCoord::AxialToXY(FHexAxial(Sub.dQ, Sub.dR), TileSize, GridOrientation);
			SubMesh->SetRelativeLocation(FVector(XY.X, XY.Y, 0.f));
			
			SubMesh->SetStaticMesh(OrbMesh);
			SubMesh->SetRelativeScale3D(FVector(OrbScale));
			
			UMaterialInterface* OrbMaterial = MaterialManager->GetMaterialForOrbType((EOrbType)Sub.Type);
			if (OrbMaterial)
			{
				SubMesh->SetMaterial(0, OrbMaterial);
			}
		}
		
		// Generate bonds
		GenerateBondedOrbMeshes(HeldComp, HeldItem);
	}
}

void ABasicWalker::ClearHeldItemVisuals(UStaticMeshComponent*& HeldComp)
{
	if (HeldComp)
	{
		// Destroy children
		TArray<USceneComponent*> ChildComps;
		HeldComp->GetChildrenComponents(false, ChildComps);
		for (USceneComponent* Child : ChildComps)
		{
			if (Child) Child->DestroyComponent();
		}
		
		HeldComp->SetStaticMesh(nullptr);
		HeldComp->SetVisibility(false, true);
	}
}

void ABasicWalker::GenerateBondedOrbMeshes(UStaticMeshComponent* HeldComp, const FItem& HeldItem)
{
	UStaticMesh* BondedOrbMesh = GetBondedOrbMesh();
	if (!HeldItem.bIsCluster || !BondedOrbMesh || !Sim || !HeldComp)
	{
		return;
	}

	// Get tile size from driver
	const float Tile = (Driver ? Driver->GetTileSize() : 100.f);
	const EHexOrientation GridOrientation = (Driver ? Driver->GetGridOrientation() : EHexOrientation::PointyTop);

	// Get the bonds from Sim
	const TArray<USimWorldHex::FBond>& Bonds = Sim->GetBonds();
	
	// Get scale specifically for bonds (use cached renderer)
	float BondThickness = 1.0f;
	if (UOrbRenderer* Renderer = const_cast<ABasicWalker*>(this)->GetCachedOrbRenderer())
	{
		BondThickness = Renderer->BondedOrbScale;
	}

	// Find bonds that involve sub-orbs in this held cluster
	for (const auto& Bond : Bonds)
	{
		if (!Bond.bActive) continue;

		// Check if both items are in this held cluster
		bool bItemAInCluster = false;
		bool bItemBInCluster = false;
		FVector PosA = FVector::ZeroVector;
		FVector PosB = FVector::ZeroVector;

		for (const FOrbData& Sub : HeldItem.Orbs)
		{
			if (Sub.ItemId == Bond.ItemA)
			{
				bItemAInCluster = true;
				FVector2D HexOffset = HexCoord::AxialToXY(FHexAxial(Sub.dQ, Sub.dR), Tile, GridOrientation);
				PosA = FVector(HexOffset.X, HexOffset.Y, 0.f);
			}
			if (Sub.ItemId == Bond.ItemB)
			{
				bItemBInCluster = true;
				FVector2D HexOffset = HexCoord::AxialToXY(FHexAxial(Sub.dQ, Sub.dR), Tile, GridOrientation);
				PosB = FVector(HexOffset.X, HexOffset.Y, 0.f);
			}
		}

		// If both items are in this cluster, create a bonded orb mesh
		if (bItemAInCluster && bItemBInCluster)
		{
			// Validate positions
			if (PosA.ContainsNaN() || PosB.ContainsNaN())
			{
				UE_LOG(LogTemp, Error, TEXT("Walker %s: Bond %d-%d has NaN positions! PosA=%s, PosB=%s. TileSize=%.2f"), 
					*GetName(), Bond.ItemA, Bond.ItemB, *PosA.ToString(), *PosB.ToString(), Tile);
				continue;
			}

			// Calculate midpoint and direction
			const FVector Midpoint = (PosA + PosB) * 0.5f;
			if (Midpoint.ContainsNaN())
			{
				UE_LOG(LogTemp, Error, TEXT("Walker %s: Bond %d-%d Midpoint is NaN! PosA=%s, PosB=%s"), 
					*GetName(), Bond.ItemA, Bond.ItemB, *PosA.ToString(), *PosB.ToString());
				continue;
			}

			// Direction
			FVector Direction = (PosB - PosA).GetSafeNormal();
			if (Direction.IsNearlyZero())
			{
				// This implies PosA == PosB, which shouldn't happen for distinct bonded items
				UE_LOG(LogTemp, Warning, TEXT("Walker %s: Bond %d-%d has zero length! PosA=%s, PosB=%s"), 
					*GetName(), Bond.ItemA, Bond.ItemB, *PosA.ToString(), *PosB.ToString());
				Direction = FVector::UpVector; // Fallback
			}

			UStaticMeshComponent* BondMesh = NewObject<UStaticMeshComponent>(HeldComp, *FString::Printf(TEXT("Bond_%d_%d"), Bond.ItemA, Bond.ItemB));
			BondMesh->RegisterComponent();
			BondMesh->AttachToComponent(HeldComp, FAttachmentTransformRules::KeepRelativeTransform);
			BondMesh->SetStaticMesh(BondedOrbMesh);

			UMaterialInterface* BondedOrbMaterial = GetBondedOrbMaterial();
			if (BondedOrbMaterial)
			{
				BondMesh->SetMaterial(0, BondedOrbMaterial);
			}
			
			// Align cylinder with bond direction
			FVector Up = Direction;
			FVector Right = FVector::CrossProduct(Up, FVector::UpVector).GetSafeNormal();
			if (Right.IsNearlyZero())
			{
				// Bond is vertical (unlikely in 2D hex grid, but possible if dQ/dR logic fails)
				Right = FVector::RightVector;
			}
			FVector Forward = FVector::CrossProduct(Right, Up).GetSafeNormal();
			if (Forward.IsNearlyZero())
			{
				// Fallback if forward vector is degenerate
				Forward = FVector::ForwardVector;
			}

			FMatrix RotationMatrix = FMatrix(Forward, Right, Up, FVector::ZeroVector);
			FRotator Rotation = FQuat(RotationMatrix).Rotator();
			
			if (Rotation.ContainsNaN())
			{
				UE_LOG(LogTemp, Error, TEXT("Walker %s: Bond %d-%d Rotation is NaN! Direction=%s"), 
					*GetName(), Bond.ItemA, Bond.ItemB, *Direction.ToString());
				Rotation = FRotator::ZeroRotator;
			}

			BondMesh->SetRelativeLocation(Midpoint);
			BondMesh->SetRelativeRotation(Rotation);
			BondMesh->SetRelativeScale3D(FVector(BondThickness, BondThickness, 1.f));
		}
	}
}

UOrbRenderer* ABasicWalker::GetCachedOrbRenderer()
{
	if (!CachedOrbRenderer)
	{
		if (UWorld* World = GetWorld())
		{
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				if (UOrbRenderer* Renderer = (*It)->FindComponentByClass<UOrbRenderer>())
				{
					CachedOrbRenderer = Renderer;
					break;
				}
			}
		}
	}
	return CachedOrbRenderer;
}

UStaticMesh* ABasicWalker::GetBondedOrbMesh() const
{
	if (UOrbRenderer* Renderer = const_cast<ABasicWalker*>(this)->GetCachedOrbRenderer())
	{
		return Renderer->BondedOrbMesh;
	}
	return nullptr;
}

UMaterialInterface* ABasicWalker::GetBondedOrbMaterial() const
{
	if (UOrbRenderer* Renderer = const_cast<ABasicWalker*>(this)->GetCachedOrbRenderer())
	{
		return Renderer->BondedOrbMaterial;
	}
	return nullptr;
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

