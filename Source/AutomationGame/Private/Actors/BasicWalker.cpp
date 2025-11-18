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

					// Get the component transform for space conversion (no scale)
					const FTransform ComponentToWorld = BodyComp->GetComponentToWorld();
					const FRotator ComponentRotation = BodyComp->GetComponentRotation();
					const FTransform ComponentTransformNoScale(ComponentRotation, ComponentToWorld.GetLocation(), FVector(1.f));
					const FTransform WorldToComponentNoScale = ComponentTransformNoScale.Inverse();

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
										// Convert world-space goal to component-local space (no scale)
										// The goal should position the foot BONE at the tile center
										*GoalValue = WorldToComponentNoScale.TransformPosition(FootPositions[i]);
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

		// Calculate body position: lerp X/Y from foot positions, use StandHeight for Z
		FVector BodyEndWorld;
		if (PlantedFootPositions.Num() > 0)
		{
			// Average X and Y from planted feet
			FVector SumXY = FVector::ZeroVector;
			for (const FVector& FootPos : PlantedFootPositions)
			{
				SumXY += FVector(FootPos.X, FootPos.Y, 0.f);
			}
			FVector AvgXY = SumXY / PlantedFootPositions.Num();
			// Use StandHeight for Z (not BodyHeightOffset which includes transition lerping)
			BodyEndWorld = FVector(AvgXY.X, AvgXY.Y, SimState.StandHeight);
		}
		else
		{
			// No feet planted, use hex position as base (horizontal) with StandHeight for Z
			FVector HexWorld = HexToWorld(CurrentHex);
			BodyEndWorld = FVector(HexWorld.X, HexWorld.Y, SimState.StandHeight);
		}

		// Update IK Rig goals via animation blueprint with end-of-tick values (no interpolation - handled in event graph)
		if (UAnimInstance* AnimInstance = BodyComp->GetAnimInstance())
		{
			// Get the component transform for space conversion (no scale)
			const FTransform ComponentToWorld = BodyComp->GetComponentToWorld();
			const FRotator ComponentRotation = BodyComp->GetComponentRotation();
			const FTransform ComponentTransformNoScale(ComponentRotation, ComponentToWorld.GetLocation(), FVector(1.f));
			const FTransform WorldToComponentNoScale = ComponentTransformNoScale.Inverse();

			// Set foot goal positions - IK Rig goals position the foot BONE at the assigned tile center
			const FName FootGoalNames[] = { TEXT("DFootGoal"), TEXT("LFootGoal"), TEXT("RFootGoal"), TEXT("UFootGoal") };
			const FFootState* FootStates[] = { &SimState.DFoot, &SimState.LFoot, &SimState.RFoot, &SimState.UFoot };

			for (int32 i = 0; i < 4; ++i)
			{
				if (FProperty* GoalProperty = AnimInstance->GetClass()->FindPropertyByName(FootGoalNames[i]))
				{
					if (FStructProperty* StructProp = CastField<FStructProperty>(GoalProperty))
					{
						if (StructProp->Struct == TBaseStructure<FVector>::Get())
						{
							FVector* GoalValue = StructProp->ContainerPtrToValuePtr<FVector>(AnimInstance);
							if (GoalValue && BodyComp)
							{
								// Get assigned tile world position (CurrentTile already has AssignedTileOffset applied)
								const FHex AssignedTile = FootStates[i]->CurrentTile;
								const FVector FootGoalWorld = HexToWorld(AssignedTile);
								
								// Convert to component-local space (without scale)
								*GoalValue = WorldToComponentNoScale.TransformPosition(FootGoalWorld);
								
								// Debug: Log first foot occasionally
								static int32 LogCounter = 0;
								if (i == 0 && (++LogCounter % 60 == 0))
								{
									UE_LOG(LogTemp, Warning, TEXT("Walker IK: Foot %s Tile(%d,%d) World(%.1f,%.1f,%.1f) Local(%.1f,%.1f,%.1f)"),
										*FootGoalNames[i].ToString(), AssignedTile.Q, AssignedTile.R,
										FootGoalWorld.X, FootGoalWorld.Y, FootGoalWorld.Z,
										GoalValue->X, GoalValue->Y, GoalValue->Z);
								}
							}
						}
					}
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
			// Use body location from simulation state (BaseHex) instead of actor location
			const FVector BodyLocation = HexToWorld(CurrentHex);

			// Get quadrant colors for each leg (applies LegQuadrantOffset)
			// Leg order: D (0), L (1), R (2), U (3)
			FLinearColor DColor = GM->GetLegQuadrantColor(TEXT("D"), 0);
			FLinearColor LColor = GM->GetLegQuadrantColor(TEXT("L"), 1);
			FLinearColor RColor = GM->GetLegQuadrantColor(TEXT("R"), 2);
			FLinearColor UColor = GM->GetLegQuadrantColor(TEXT("U"), 3);

			// Draw body location sphere
			DrawDebugSphere(GetWorld(), BodyLocation, 20.f, 12, FColor::White, false, -1.f, 0, 2.f);

			// Draw text showing body hex position
			if (Driver)
			{
				FString BodyText = FString::Printf(TEXT("Body:(%d,%d)"), CurrentHex.Q, CurrentHex.R);
				DrawDebugString(GetWorld(), BodyLocation + FVector(0, 0, 30), BodyText, nullptr, FColor::White, 0.f, true, 1.2f);
			}

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

			// Draw quadrant outlines showing the outermost edges of each quadrant
			if (Driver)
			{
				const int32 ReachDist = SimState.ReachDistance;
				const FHex& BodyHex = CurrentHex;

				// Helper function to get quadrant from hex (same logic as GetFixedQuadrantFromHex)
				auto GetQuadrant = [](const FHex& BodyHex, const FHex& TestHex) -> FString
					{
						const int32 dq = TestHex.Q - BodyHex.Q;
						const int32 dr = TestHex.R - BodyHex.R;

						if (dq > 0 && dr == 0)  return TEXT("E");
						if (dq < 0 && dr == 0)  return TEXT("W");
						if (dq == 0 && dr > 0)  return TEXT("S");
						if (dq == 0 && dr < 0)  return TEXT("N");

						if (FMath::Abs(dq) > FMath::Abs(dr))
							return (dq > 0) ? TEXT("E") : TEXT("W");
						if (FMath::Abs(dr) > FMath::Abs(dq))
							return (dr > 0) ? TEXT("S") : TEXT("N");

						if (dq > 0 && dr > 0)   return TEXT("S");
						if (dq > 0 && dr < 0)   return TEXT("E");
						if (dq < 0 && dr > 0)   return TEXT("W");
						return TEXT("N");
					};

				// Find all tiles in each quadrant (within reach distance)
				// Map: D->S, L->W, R->E, U->N
				TSet<FHex> STiles, WTiles, ETiles, NTiles;

				for (int32 q = -ReachDist; q <= ReachDist; ++q)
				{
					for (int32 r = -ReachDist; r <= ReachDist; ++r)
					{
						FHex TestHex(BodyHex.Q + q, BodyHex.R + r);
						const int32 dq = TestHex.Q - BodyHex.Q;
						const int32 dr = TestHex.R - BodyHex.R;
						const int32 HexDist = FMath::Max(FMath::Max(FMath::Abs(dq), FMath::Abs(dr)), FMath::Abs(dq + dr));

						if (HexDist <= ReachDist && HexDist > 0)
						{
							FString Quad = GetQuadrant(BodyHex, TestHex);
							if (Quad == TEXT("S")) STiles.Add(TestHex);
							else if (Quad == TEXT("W")) WTiles.Add(TestHex);
							else if (Quad == TEXT("E")) ETiles.Add(TestHex);
							else if (Quad == TEXT("N")) NTiles.Add(TestHex);
						}
					}
				}

				// Helper to get hex vertex positions (for pointy-top hexes)
				// Vertices are at angles: 0°, 60°, 120°, 180°, 240°, 300° from +X axis
				auto GetHexVertices = [this](const FHex& Hex) -> TArray<FVector>
					{
						const FVector Center = HexToWorld(Hex);
						const float TileSize = Driver ? Driver->GetTileSize() : 100.f;
						const float GridYaw = Driver ? Driver->GetGridYaw() : 0.f;
						const EHexOrientation Ori = Driver ? Driver->GetGridOrientation() : EHexOrientation::PointyTop;

						// For pointy-top hexes, radius (center to vertex) equals tile size
						// Vertices are at 0°, 60°, 120°, 180°, 240°, 300° from +X
						const float Radius = TileSize;

						TArray<FVector> Vertices;
						for (int32 i = 0; i < 6; ++i)
						{
							// Start at 0° (pointing right/East) for pointy-top
							const float AngleDeg = 60.f * i;
							const float AngleRad = FMath::DegreesToRadians(AngleDeg);

							// Calculate vertex in local hex space
							FVector2D LocalVertex(
								Radius * FMath::Cos(AngleRad),
								Radius * FMath::Sin(AngleRad)
							);

							// Apply grid yaw rotation
							FVector2D RotatedVertex = HexCoord::Rotate2D(LocalVertex, GridYaw);

							// Convert to world space
							const FVector Vertex = Center + FVector(RotatedVertex.X, RotatedVertex.Y, 0.f);
							Vertices.Add(Vertex);
						}
						return Vertices;
					};

				// Helper to draw quadrant outline by finding perimeter edges and connecting them into a continuous path
				// An edge is on the perimeter if the neighbor in that direction is NOT in the same quadrant
				auto DrawQuadrantOutline = [this, &GetQuadrant, &GetHexVertices, &BodyHex](
					const TSet<FHex>& QuadrantTiles,
					const FLinearColor& Color,
					float ZOffset)
					{
						if (QuadrantTiles.Num() == 0) return;

						// Collect all perimeter edges (edges that are on the boundary)
						// Use a map to track edges by their vertex positions (to avoid duplicates and enable path building)
						struct FEdge
						{
							FVector Start;
							FVector End;
							bool bUsed = false;
						};

						TArray<FEdge> PerimeterEdges;

						// For each tile in the quadrant, check each of its 6 edges
						for (const FHex& Tile : QuadrantTiles)
						{
							const TArray<FVector> Vertices = GetHexVertices(Tile);

							// Check each of the 6 edges (each edge connects two vertices)
							for (int32 EdgeIdx = 0; EdgeIdx < 6; ++EdgeIdx)
							{
								const int32 NextIdx = (EdgeIdx + 1) % 6;
								const FVector Start = Vertices[EdgeIdx] + FVector(0, 0, ZOffset);
								const FVector End = Vertices[NextIdx] + FVector(0, 0, ZOffset);

								// Get the neighbor in the direction of this edge
								// Edge directions: 0=E, 1=NE, 2=NW, 3=W, 4=SW, 5=SE
								const EHexDir NeighborDir = static_cast<EHexDir>(EdgeIdx);
								const FHex Neighbor = HexUtil::Neighbor(Tile, NeighborDir);

								// This edge is on the perimeter if the neighbor is NOT in the same quadrant
								FString NeighborQuad = GetQuadrant(BodyHex, Neighbor);
								FString TileQuad = GetQuadrant(BodyHex, Tile);

								if (!NeighborQuad.Equals(TileQuad, ESearchCase::CaseSensitive) || !QuadrantTiles.Contains(Neighbor))
								{
									// Add this perimeter edge
									FEdge Edge;
									Edge.Start = Start;
									Edge.End = End;
									PerimeterEdges.Add(Edge);
								}
							}
						}

						// Build continuous paths by connecting edges that share vertices
						// Use a tolerance for vertex matching (floating point comparison)
						const float VertexTolerance = 1.0f;

						while (PerimeterEdges.Num() > 0)
						{
							// Find an unused edge to start a new path
							int32 StartEdgeIdx = -1;
							for (int32 i = 0; i < PerimeterEdges.Num(); ++i)
							{
								if (!PerimeterEdges[i].bUsed)
								{
									StartEdgeIdx = i;
									break;
								}
							}

							if (StartEdgeIdx == -1) break; // All edges used

							// Build a continuous path starting from this edge
							TArray<FVector> Path;
							FEdge& CurrentEdge = PerimeterEdges[StartEdgeIdx];
							CurrentEdge.bUsed = true;

							// Start the path with the first edge
							Path.Add(CurrentEdge.Start);
							Path.Add(CurrentEdge.End);
							FVector CurrentEnd = CurrentEdge.End;

							// Try to extend the path by finding edges that connect
							bool bExtended = true;
							while (bExtended)
							{
								bExtended = false;

								// Look for an edge that connects to CurrentEnd
								for (int32 i = 0; i < PerimeterEdges.Num(); ++i)
								{
									if (PerimeterEdges[i].bUsed) continue;

									FEdge& Candidate = PerimeterEdges[i];

									// Check if candidate edge connects to current path end
									if (FVector::Dist(CurrentEnd, Candidate.Start) < VertexTolerance)
									{
										// Connect forward
										Path.Add(Candidate.End);
										CurrentEnd = Candidate.End;
										Candidate.bUsed = true;
										bExtended = true;
										break;
									}
									else if (FVector::Dist(CurrentEnd, Candidate.End) < VertexTolerance)
									{
										// Connect backward (reverse the edge)
										Path.Add(Candidate.Start);
										CurrentEnd = Candidate.Start;
										Candidate.bUsed = true;
										bExtended = true;
										break;
									}
								}
							}

							// Draw the continuous path
							if (Path.Num() >= 2)
							{
								for (int32 i = 0; i < Path.Num() - 1; ++i)
								{
									DrawDebugLine(GetWorld(), Path[i], Path[i + 1], Color.ToFColor(true), false, -1.f, 0, 2.f);
								}
							}

							// Remove used edges
							PerimeterEdges.RemoveAll([](const FEdge& E) { return E.bUsed; });
						}
					};

				// Draw quadrant outlines (D->S, L->W, R->E, U->N)
				DrawQuadrantOutline(STiles, DColor, 5.f);
				DrawQuadrantOutline(WTiles, LColor, 5.f);
				DrawQuadrantOutline(ETiles, RColor, 5.f);
				DrawQuadrantOutline(NTiles, UColor, 5.f);
			}
		}

		// Set actor location to body end position
		//SetActorLocation(BodyEndWorld);
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
	if (Driver)
	{
		DFootAssignedTileWorldLocation = Driver->MapHexToWorld(DFootHex);
		LFootAssignedTileWorldLocation = Driver->MapHexToWorld(LFootHex);
		RFootAssignedTileWorldLocation = Driver->MapHexToWorld(RFootHex);
		UFootAssignedTileWorldLocation = Driver->MapHexToWorld(UFootHex);
	}
	else
	{
		// Fallback to zero if no driver
		DFootAssignedTileWorldLocation = FVector::ZeroVector;
		LFootAssignedTileWorldLocation = FVector::ZeroVector;
		RFootAssignedTileWorldLocation = FVector::ZeroVector;
		UFootAssignedTileWorldLocation = FVector::ZeroVector;
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

