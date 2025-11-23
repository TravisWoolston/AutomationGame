#include "Framework/GameManager.h"
#include "Actors/BasicWalker.h"
#include "Actors/OrbSimDriver.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "UObject/UnrealType.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"
#include "Animation/AnimInstance.h"
#include "Sim/HexTypes.h"
#include "Sim/HexCoord.h"
#include "Sim/SimWorldHex.h"
#include "DrawDebugHelpers.h"
#include "HAL/PlatformProcess.h"


// Local helper structures and constants for walker calibration
namespace {
	struct FWalkerLegNames
	{
		FName HipRotator;
		FName HipAxle;
		FName KneeAxle;
		FName FootAxle;
		FName FootBone;
	};

	static const FWalkerLegNames WalkerLegBones[] =
	{
		{ TEXT("DHipRotatorAxle38"), TEXT("DHipAxle41"), TEXT("DKneeAxle44"), TEXT("DFootAxle47"), TEXT("DFoot49") },
		{ TEXT("LHipRotatorAxle14"), TEXT("LHipAxle17"), TEXT("LKneeAxle20"), TEXT("LFootAxle23"), TEXT("LFoot25") },
		{ TEXT("RHipRotatorAxle2"), TEXT("RHipAxle5"), TEXT("RKneeAxle8"), TEXT("RFootAxle11"), TEXT("RFoot13") },
		{ TEXT("UHipRotatorAxle26"), TEXT("UHipAxle29"), TEXT("UKneeAxle32"), TEXT("UFootAxle35"), TEXT("UFoot37") },
	};

	static const FName WalkerBodyBoneName = TEXT("Body1");
}

void AGameManager::CalibrateWalker()
{
	if (bWalkerCalibrationActive)
	{
		UE_LOG(LogTemp, Warning, TEXT("GameManager: Walker calibration already active."));
		return;
	}

	if (!BasicWalkerClass)
	{
		UE_LOG(LogTemp, Warning, TEXT("GameManager: Cannot calibrate walker because BasicWalkerClass is not set."));
		return;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		UE_LOG(LogTemp, Warning, TEXT("GameManager: Cannot calibrate walker without a valid world."));
		return;
	}

	// Clear calibration data before loading (to prevent duplicates)
	WalkerLegCalibrations.Empty();
	WalkerSittingPose.Empty();
	WalkerOriginalFootPositions.Empty();

	// Load saved calibration settings if they exist
	LoadWalkerCalibration();

	// Clear only the walker actor and visual debug elements, not the settings
	if (CalibratingWalker.IsValid())
	{
		CalibratingWalker->Destroy();
		CalibratingWalker.Reset();
	}
	ClearWalkerDebugMeshes();
	ClearLegTileQuadrantVisualization();

	const FVector SpawnLocation = GetActorLocation() + FVector(0.f, 0.f, 50.f);
	const FTransform SpawnTransform(FRotator::ZeroRotator, SpawnLocation);

	ABasicWalker* Walker = World->SpawnActorDeferred<ABasicWalker>(BasicWalkerClass, SpawnTransform, this, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!Walker)
	{
		UE_LOG(LogTemp, Warning, TEXT("GameManager: Failed to spawn calibration walker."));
		return;
	}

	Walker->SetActorEnableCollision(false);
	Walker->FinishSpawning(SpawnTransform);
	Walker->Stop();
	Walker->SetProgram(TArray<FWalkerInstr>(), true);

	USkeletalMeshComponent* BodyComp = Walker->GetBodyComponent();
	if (!BodyComp)
	{
		UE_LOG(LogTemp, Warning, TEXT("GameManager: Calibration walker has no skeletal mesh component."));
		Walker->Destroy();
		return;
	}

	// Wait a moment for the walker to fully initialize
	FPlatformProcess::Sleep(0.05f);

	// Ensure animation instance is initialized before gathering data
	if (!BodyComp->GetAnimInstance() && BodyComp->GetAnimClass())
	{
		BodyComp->InitAnim(true);
		FPlatformProcess::Sleep(0.02f);
	}

	// Force an initial update
	BodyComp->RefreshBoneTransforms();
	BodyComp->UpdateComponentToWorld();

	// Capture bone positions from current pose (animation should be initialized by now)
	// If animation hasn't stretched the bones yet, we'll get reference pose anyway
	WalkerBodyBaseWorldLocation = BodyComp->GetBoneLocation(WalkerBodyBoneName, EBoneSpaces::WorldSpace);
	WalkerBodyActorOffset = Walker->GetActorLocation() - WalkerBodyBaseWorldLocation;
	WalkerCalibrationBaseLocation = Walker->GetActorLocation();

	// Gather calibration data (this will get current bone positions)
	GatherWalkerCalibrationData(BodyComp);

	// Now ensure animation is fully updated with initial IK goals
	BodyComp->RefreshBoneTransforms();
	BodyComp->UpdateComponentToWorld();

	if (WalkerLegCalibrations.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("GameManager: No valid walker legs found during calibration."));
		Walker->Destroy();
		WalkerSittingPose.Empty();
		return;
	}

	CalibratingWalker = Walker;
	Walker->Calibrating = true;

	// Initialize walker on grid so it has a Driver and can access grid functions
	// Use a default position (0,0) for calibration - the walker doesn't need to be in simulation
	if (OrbSimDriver)
	{
		Walker->InitializeOnGrid(OrbSimDriver, 0, 0);
		UE_LOG(LogTemp, Log, TEXT("CalibrateWalker: Initialized walker on grid with Driver"));
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("CalibrateWalker: No OrbSimDriver available - assigned tile locations may not work"));
	}

	bWalkerCalibrationActive = true;
	bWalkerStanding = false;
	StandHeight = 0.f;
	WalkerStandAlpha = 0.f;

	// Apply StartingFootDistance offset to foot positions
	ApplyStartingFootDistance(BodyComp);

	// Check if walker is in simulation and log assigned tiles
	if (Walker->SimWalkerIndex >= 0 && OrbSimDriver)
	{
		if (USimWorldHex* Sim = OrbSimDriver->GetSim())
		{
			if (Walker->SimWalkerIndex < Sim->Walkers.Num())
			{
				const FWalkerState& SimState = Sim->Walkers[Walker->SimWalkerIndex];
				UE_LOG(LogTemp, Warning, TEXT("CalibrateWalker: Walker is in simulation (index=%d). Assigned tiles:"), Walker->SimWalkerIndex);
				UE_LOG(LogTemp, Warning, TEXT("  DFoot: (%d,%d), LFoot: (%d,%d), RFoot: (%d,%d), UFoot: (%d,%d)"),
					SimState.DFoot.CurrentTile.Q, SimState.DFoot.CurrentTile.R,
					SimState.LFoot.CurrentTile.Q, SimState.LFoot.CurrentTile.R,
					SimState.RFoot.CurrentTile.Q, SimState.RFoot.CurrentTile.R,
					SimState.UFoot.CurrentTile.Q, SimState.UFoot.CurrentTile.R);
				
				// Log world positions of assigned tiles
				const FVector DWorld = OrbSimDriver->MapHexToWorld(SimState.DFoot.CurrentTile);
				const FVector LWorld = OrbSimDriver->MapHexToWorld(SimState.LFoot.CurrentTile);
				const FVector RWorld = OrbSimDriver->MapHexToWorld(SimState.RFoot.CurrentTile);
				const FVector UWorld = OrbSimDriver->MapHexToWorld(SimState.UFoot.CurrentTile);
				UE_LOG(LogTemp, Warning, TEXT("  Assigned tile world positions: D(%.1f,%.1f,%.1f) L(%.1f,%.1f,%.1f) R(%.1f,%.1f,%.1f) U(%.1f,%.1f,%.1f)"),
					DWorld.X, DWorld.Y, DWorld.Z, LWorld.X, LWorld.Y, LWorld.Z,
					RWorld.X, RWorld.Y, RWorld.Z, UWorld.X, UWorld.Y, UWorld.Z);
			}
			else
			{
				UE_LOG(LogTemp, Warning, TEXT("CalibrateWalker: Walker SimWalkerIndex=%d but Sim only has %d walkers"), Walker->SimWalkerIndex, Sim->Walkers.Num());
			}
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("CalibrateWalker: Walker SimWalkerIndex=%d but Sim is null"), Walker->SimWalkerIndex);
		}
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("CalibrateWalker: Walker is NOT in simulation (SimWalkerIndex=%d, OrbSimDriver=%d)"), 
			Walker->SimWalkerIndex, OrbSimDriver ? 1 : 0);
	}

	// Log current FootAxleWorldLocation from calibration data
	UE_LOG(LogTemp, Warning, TEXT("CalibrateWalker: FootAxleWorldLocation from calibration data:"));
	for (int32 i = 0; i < WalkerLegCalibrations.Num(); ++i)
	{
		const FWalkerLegCalibrationData& LegData = WalkerLegCalibrations[i];
		UE_LOG(LogTemp, Warning, TEXT("  Leg %d (%s): FootAxleWorldLocation=(%.1f,%.1f,%.1f)"),
			i, *LegData.HipAxleBone.ToString(), LegData.FootAxleWorldLocation.X, LegData.FootAxleWorldLocation.Y, LegData.FootAxleWorldLocation.Z);
	}

	// Initialize IK Rig goal positions to current foot positions (updated by StartingFootDistance)
	// Use saved DesiredStandHeight if it was previously calibrated
	SetIKRigGoalPositions(BodyComp, DesiredStandHeight);

	if (!FMath::IsNearlyZero(DesiredStandHeight))
	{
		SetStandHeight();
	}
	else
	{
		// Even if stand height is 0, update the debug meshes to show StartingFootDistance effect
		UpdateWalkerDebugMeshes(BodyComp);
	}

	UE_LOG(LogTemp, Log, TEXT("GameManager: Walker calibration started (%d legs). Loaded settings: StandHeight=%.1f, StartingFootDistance=%.1f, StepHeight=%.1f"),
		WalkerLegCalibrations.Num(), DesiredStandHeight, StartingFootDistance, StepHeight);

	CreateWalkerDebugMeshes(Walker, BodyComp);
	UpdateWalkerDebugMeshes(BodyComp);

	// Update quadrant visualization if enabled
	if (bShowLegTileQuadrants)
	{
		UpdateLegTileQuadrantVisualization();
	}

	// Force update of assigned tile world locations now that calibration is set up
	// This ensures the walker's public properties are populated even if BeginPlay hasn't run
	if (Walker && Walker->Driver)
	{
		Walker->UpdateAssignedTileWorldLocations();
		UE_LOG(LogTemp, Log, TEXT("CalibrateWalker: Forced update of assigned tile world locations"));
	}
}

void AGameManager::SetStandHeight()
{
	if (!bWalkerCalibrationActive || !CalibratingWalker.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("GameManager: SetStandHeight called without an active calibration."));
		return;
	}

	ABasicWalker* Walker = CalibratingWalker.Get();
	if (!Walker)
	{
		UE_LOG(LogTemp, Warning, TEXT("GameManager: Calibration walker missing."));
		return;
	}

	USkeletalMeshComponent* BodyComp = Walker->GetBodyComponent();
	if (!BodyComp)
	{
		UE_LOG(LogTemp, Warning, TEXT("GameManager: Calibration walker has no skeletal mesh component."));
		return;
	}

	const float MaxStandHeight = 1000.f;
	float RequestedHeight = FMath::Clamp(DesiredStandHeight, -200.f, MaxStandHeight);

	// Calculate the maximum height the body can rise to, constrained by all legs.
	float AllowedHeight = RequestedHeight;
	for (const FWalkerLegCalibrationData& LegData : WalkerLegCalibrations)
	{
		const float MaxLen = LegData.UpperLength + LegData.LowerLength - KINDA_SMALL_NUMBER;

		// Vector from the foot's ground position to the hip's base (sitting) position
		const FVector FootToHipBase = LegData.HipWorldLocation - LegData.FootAxleWorldLocation;
		const float HorizontalDistSq = FVector::DistSquared2D(LegData.HipWorldLocation, LegData.FootAxleWorldLocation);

		// Maximum vertical distance the hip can be from the foot
		const float MaxVertical = FMath::Sqrt(FMath::Max(0.f, MaxLen * MaxLen - HorizontalDistSq));

		// The vertical distance in the base sitting pose
		const float BaseVertical = FootToHipBase.Z;

		// The maximum height we can add from the sitting position
		const float MaxAdditionalHeight = MaxVertical - BaseVertical;

		AllowedHeight = FMath::Min(AllowedHeight, MaxAdditionalHeight);
	}

	// Clamp to a valid, achievable range
	float ActualHeight = FMath::Clamp(AllowedHeight, -200.f, MaxStandHeight);

	WalkerStandAlpha = FMath::Clamp(ActualHeight / MaxStandHeight, 0.f, 1.f);

	// Set IK Rig goal positions - the IK Rig will handle the leg posing and body positioning
	// Do NOT move the actor - let the IK Rig move the body bone to match BodyGoal
	SetIKRigGoalPositions(BodyComp, ActualHeight);

	UpdateWalkerDebugMeshes(BodyComp);

	// Update assigned tile world locations in the walker
	if (Walker && Walker->Driver)
	{
		Walker->UpdateAssignedTileWorldLocations();
		UE_LOG(LogTemp, VeryVerbose, TEXT("SetStandHeight: Updated assigned tile world locations"));
	}

	// Update tile/assigned-tile visualization when foot positions change
	if (bShowLegTileQuadrants)
	{
		UpdateLegTileQuadrantVisualization();
	}

	StandHeight = ActualHeight;
	bWalkerStanding = !FMath::IsNearlyZero(StandHeight, 1.0f);

	UE_LOG(LogTemp, Log, TEXT("GameManager: Stand height updated - Desired=%.1f, Actual=%.1f, Standing=%d"),
		DesiredStandHeight, StandHeight, bWalkerStanding ? 1 : 0);
}

void AGameManager::SetStandHeightButton()
{
	if (!bWalkerCalibrationActive || !CalibratingWalker.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("GameManager: SetStandHeightButton called without an active calibration."));
		return;
	}

	// Manually trigger SetStandHeight with current DesiredStandHeight
	SetStandHeight();
	UE_LOG(LogTemp, Log, TEXT("GameManager: SetStandHeight applied manually - Height=%.1f"), DesiredStandHeight);
}

void AGameManager::ApplyStandHeight()
{
	if (!bWalkerCalibrationActive || !CalibratingWalker.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("GameManager: ApplyStandHeight called without an active calibration."));
		return;
	}

	// Manually trigger SetStandHeight with current DesiredStandHeight
	SetStandHeight();
}

void AGameManager::ResetStandHeight()
{
	if (!bWalkerCalibrationActive || !CalibratingWalker.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("GameManager: ResetStandHeight called without an active calibration."));
		return;
	}

	// Save current height before resetting
	if (!FMath::IsNearlyZero(DesiredStandHeight, 1.0f))
	{
		SavedStandHeight = DesiredStandHeight;
	}

	// Reset to sitting position
	DesiredStandHeight = 0.f;
	// SetStandHeight() will be called automatically via PostEditChangeProperty
}

void AGameManager::SaveWalkerCalibration()
{
	if (!bWalkerCalibrationActive)
	{
		UE_LOG(LogTemp, Warning, TEXT("GameManager: SaveWalkerCalibration called without an active calibration."));
		return;
	}

	SaveWalkerCalibrationToConfig();

	UE_LOG(LogTemp, Log, TEXT("GameManager: Walker Calibration Saved to Config:"));
	UE_LOG(LogTemp, Log, TEXT("  - StandHeight: %.2f"), DesiredStandHeight);
	UE_LOG(LogTemp, Log, TEXT("  - StartingFootDistance: %.2f"), StartingFootDistance);
	UE_LOG(LogTemp, Log, TEXT("  - StepHeight: %.2f"), StepHeight);
	UE_LOG(LogTemp, Log, TEXT("  - StepRaiseTicks: %d"), StepRaiseTicks);
	UE_LOG(LogTemp, Log, TEXT("  - StepMoveTicks: %d"), StepMoveTicks);
	UE_LOG(LogTemp, Log, TEXT("  - StepLowerTicks: %d"), StepLowerTicks);
	UE_LOG(LogTemp, Log, TEXT("  - ReachDistance: %d"), ReachDistance);
	UE_LOG(LogTemp, Log, TEXT("  - MinimumReachDistance: %.2f"), MinimumReachDistance);
	UE_LOG(LogTemp, Log, TEXT("  - ActiveFoot: %s"), *StaticEnum<EWalkerFoot>()->GetNameStringByValue((int32)ActiveFoot));
	UE_LOG(LogTemp, Log, TEXT("  - Legs: %d"), WalkerLegCalibrations.Num());

	for (int32 i = 0; i < WalkerLegCalibrations.Num() && i < WalkerOriginalFootPositions.Num(); ++i)
	{
		const FWalkerLegCalibrationData& LegData = WalkerLegCalibrations[i];
		const FVector& FootPos = WalkerOriginalFootPositions[i];
		UE_LOG(LogTemp, Log, TEXT("    Leg %d (%s): Foot at (%.1f, %.1f, %.1f), Upper=%.1f, Lower=%.1f"),
			i, *LegData.HipAxleBone.ToString(), FootPos.X, FootPos.Y, FootPos.Z,
			LegData.UpperLength, LegData.LowerLength);
	}
}

void AGameManager::CancelWalkerCalibration()
{
	// Clean up walker actor and visual elements only
	// DO NOT clear settings - they persist between sessions
	if (CalibratingWalker.IsValid())
	{
		CalibratingWalker->Calibrating = false;
		CalibratingWalker->Destroy();
		CalibratingWalker.Reset();
	}

	// Clear runtime data only
	WalkerLegCalibrations.Empty();
	bWalkerCalibrationActive = false;
	StandHeight = 0.f;
	bWalkerStanding = false;
	WalkerBodyBaseWorldLocation = FVector::ZeroVector;
	WalkerBodyActorOffset = FVector::ZeroVector;
	WalkerStandAlpha = 0.f;
	ClearWalkerDebugMeshes();
	ClearLegTileQuadrantVisualization();

	// Settings like DesiredStandHeight, StartingFootDistance, StepHeight, ReachDistance, etc.
	// are NOT cleared - they persist for the next calibration session
	UE_LOG(LogTemp, Log, TEXT("GameManager: Walker calibration cancelled. Settings preserved for next session."));
}

void AGameManager::GatherWalkerCalibrationData(USkeletalMeshComponent* BodyComp)
{
	WalkerLegCalibrations.Empty();
	WalkerSittingPose.Empty();
	WalkerOriginalFootPositions.Empty(); // Clear before gathering new data
	if (!BodyComp)
	{
		return;
	}

	const int32 BodyIndex = BodyComp->GetBoneIndex(WalkerBodyBoneName);
	if (BodyIndex != INDEX_NONE)
	{
		const FTransform BodyXform = BodyComp->GetBoneTransform(BodyIndex);
		WalkerSittingPose.Add(WalkerBodyBoneName, BodyXform.GetRotation());
	}

	for (const FWalkerLegNames& Names : WalkerLegBones)
	{
		const int32 HipRotIdx = BodyComp->GetBoneIndex(Names.HipRotator);
		const int32 HipIdx = BodyComp->GetBoneIndex(Names.HipAxle);
		const int32 KneeIdx = BodyComp->GetBoneIndex(Names.KneeAxle);
		const int32 FootAxleIdx = BodyComp->GetBoneIndex(Names.FootAxle);
		const int32 FootIdx = BodyComp->GetBoneIndex(Names.FootBone);

		if (HipRotIdx == INDEX_NONE || HipIdx == INDEX_NONE || KneeIdx == INDEX_NONE || FootAxleIdx == INDEX_NONE || FootIdx == INDEX_NONE)
		{
			UE_LOG(LogTemp, Warning, TEXT("GameManager: Missing bones for walker leg (%s, %s, %s, %s, %s)"),
				*Names.HipRotator.ToString(), *Names.HipAxle.ToString(), *Names.KneeAxle.ToString(), *Names.FootAxle.ToString(), *Names.FootBone.ToString());
			continue;
		}

		FWalkerLegCalibrationData Data;
		Data.HipRotatorBone = Names.HipRotator;
		Data.HipAxleBone = Names.HipAxle;
		Data.KneeAxleBone = Names.KneeAxle;
		Data.FootAxleBone = Names.FootAxle;
		Data.FootBone = Names.FootBone;

		const FTransform HipRotXform = BodyComp->GetBoneTransform(HipRotIdx);
		const FTransform HipXform = BodyComp->GetBoneTransform(HipIdx);
		const FTransform KneeXform = BodyComp->GetBoneTransform(KneeIdx);
		const FTransform FootAxleXform = BodyComp->GetBoneTransform(FootAxleIdx);
		const FTransform FootXform = BodyComp->GetBoneTransform(FootIdx);

		Data.HipRotatorBaseRotation = HipRotXform.GetRotation();
		Data.HipAxleBaseRotation = HipXform.GetRotation();
		Data.KneeAxleBaseRotation = KneeXform.GetRotation();
		Data.FootAxleBaseRotation = FootAxleXform.GetRotation();
		Data.FootBoneBaseRotation = FootXform.GetRotation();

		Data.HipComponentLocation = HipXform.GetLocation();
		Data.KneeComponentLocation = KneeXform.GetLocation();
		Data.FootAxleComponentLocation = FootAxleXform.GetLocation();
		Data.FootComponentLocation = FootXform.GetLocation();
		Data.HipWorldLocation = BodyComp->GetBoneLocation(Names.HipAxle, EBoneSpaces::WorldSpace);
		Data.HipRotAxisComponent = HipRotXform.GetUnitAxis(EAxis::Z).GetSafeNormal();

		const FString HipName = Names.HipAxle.ToString();
		if (HipName.Contains(TEXT("LHip")) || HipName.Contains(TEXT("RHip")))
		{
			Data.HipAxleAxisComponent = HipXform.GetUnitAxis(EAxis::Y).GetSafeNormal();
		}
		else
		{
			Data.HipAxleAxisComponent = HipXform.GetUnitAxis(EAxis::X).GetSafeNormal();
		}

		const FString KneeName = Names.KneeAxle.ToString();
		if (KneeName.Contains(TEXT("LKnee")) || KneeName.Contains(TEXT("RKnee")))
		{
			Data.KneeAxleAxisComponent = KneeXform.GetUnitAxis(EAxis::Y).GetSafeNormal();
		}
		else
		{
			Data.KneeAxleAxisComponent = KneeXform.GetUnitAxis(EAxis::X).GetSafeNormal();
		}

		const FString FootName = Names.FootAxle.ToString();
		if (FootName.Contains(TEXT("LFoot")) || FootName.Contains(TEXT("RFoot")))
		{
			Data.FootAxleAxisComponent = FootAxleXform.GetUnitAxis(EAxis::Y).GetSafeNormal();
		}
		else
		{
			Data.FootAxleAxisComponent = FootAxleXform.GetUnitAxis(EAxis::X).GetSafeNormal();
		}

		if (Data.HipRotAxisComponent.IsNearlyZero()) Data.HipRotAxisComponent = FVector::UpVector;
		if (Data.HipAxleAxisComponent.IsNearlyZero()) Data.HipAxleAxisComponent = (HipName.Contains(TEXT("LHip")) || HipName.Contains(TEXT("RHip"))) ? FVector::YAxisVector : FVector::XAxisVector;
		if (Data.KneeAxleAxisComponent.IsNearlyZero()) Data.KneeAxleAxisComponent = (KneeName.Contains(TEXT("LKnee")) || KneeName.Contains(TEXT("RKnee"))) ? FVector::YAxisVector : FVector::XAxisVector;
		if (Data.FootAxleAxisComponent.IsNearlyZero()) Data.FootAxleAxisComponent = (FootName.Contains(TEXT("LFoot")) || FootName.Contains(TEXT("RFoot"))) ? FVector::YAxisVector : FVector::XAxisVector;

		const FVector ThighDir = (Data.KneeComponentLocation - Data.HipComponentLocation).GetSafeNormal();
		const FVector CalfDir = (Data.FootComponentLocation - Data.KneeComponentLocation).GetSafeNormal();
		FVector PlaneNormal = FVector::CrossProduct(ThighDir, CalfDir).GetSafeNormal();
		if (PlaneNormal.IsNearlyZero())
		{
			PlaneNormal = FVector::UpVector;
		}
		Data.PlaneNormalComponent = PlaneNormal;
		Data.ThighBaseDirComponent = ThighDir;
		Data.CalfBaseDirComponent = CalfDir;

		FVector HipToFootInPlane = (Data.FootComponentLocation - Data.HipComponentLocation) - PlaneNormal * FVector::DotProduct(Data.FootComponentLocation - Data.HipComponentLocation, PlaneNormal);
		if (!HipToFootInPlane.Normalize())
		{
			HipToFootInPlane = ThighDir;
		}
		FVector BasePerp = FVector::CrossProduct(PlaneNormal, HipToFootInPlane).GetSafeNormal();
		if (BasePerp.IsNearlyZero())
		{
			BasePerp = FVector::CrossProduct(PlaneNormal, ThighDir).GetSafeNormal();
		}

		Data.BaseKneeAlong = FVector::DotProduct(Data.KneeComponentLocation - Data.HipComponentLocation, HipToFootInPlane);
		Data.BaseKneeHeight = FVector::DotProduct(Data.KneeComponentLocation - Data.HipComponentLocation, BasePerp);

		FVector HipToFoot = Data.FootComponentLocation - Data.HipComponentLocation;

		Data.HipForwardDirComponent = HipToFoot - FVector::DotProduct(HipToFoot, PlaneNormal) * PlaneNormal;
		if (!Data.HipForwardDirComponent.Normalize())
		{
			Data.HipForwardDirComponent = ThighDir;
		}
		Data.BasePerpDirComponent = BasePerp;

		Data.UpperLength = (Data.KneeComponentLocation - Data.HipComponentLocation).Size();
		Data.LowerLength = (Data.FootComponentLocation - Data.KneeComponentLocation).Size();

		const FVector Point = Data.HipComponentLocation + ThighDir * (Data.UpperLength > KINDA_SMALL_NUMBER ? Data.UpperLength : 1.f);
		const FVector KneeOffset = Data.KneeComponentLocation - Point;
		const float Sign = FVector::DotProduct(KneeOffset, PlaneNormal) >= 0.f ? 1.f : -1.f;
		Data.PlaneNormalComponent *= Sign;

		// Calculate and store the pole vector in the hip's local space
		Data.PoleVectorLocal = HipXform.InverseTransformVector(Data.PlaneNormalComponent);

		Data.FootAxleWorldLocation = BodyComp->GetBoneLocation(Names.FootAxle, EBoneSpaces::WorldSpace);
		Data.FootWorldLocation = BodyComp->GetBoneLocation(Names.FootBone, EBoneSpaces::WorldSpace);

		Data.FootAxleBaseRotation = FootAxleXform.GetRotation();
		Data.FootBoneBaseRotation = FootXform.GetRotation();
		Data.FootRelativeTransform = FootXform.GetRelativeTransform(FootAxleXform);

		WalkerLegCalibrations.Add(Data);

		// Store original foot position for offset calculation
		WalkerOriginalFootPositions.Add(Data.FootAxleWorldLocation);

		WalkerSittingPose.FindOrAdd(Names.HipRotator) = Data.HipRotatorBaseRotation;
		WalkerSittingPose.FindOrAdd(Names.HipAxle) = Data.HipAxleBaseRotation;
		WalkerSittingPose.FindOrAdd(Names.KneeAxle) = Data.KneeAxleBaseRotation;
		WalkerSittingPose.FindOrAdd(Names.FootAxle) = Data.FootAxleBaseRotation;
		WalkerSittingPose.FindOrAdd(Names.FootBone) = Data.FootBoneBaseRotation;
	}
}

void AGameManager::ApplyStartingFootDistance(USkeletalMeshComponent* BodyComp)
{
	if (!bWalkerCalibrationActive || !OrbSimDriver || WalkerLegCalibrations.Num() == 0 || WalkerOriginalFootPositions.Num() != WalkerLegCalibrations.Num())
	{
		UE_LOG(LogTemp, Warning, TEXT("ApplyStartingFootDistance: Conditions not met - Active=%d, Driver=%d, Legs=%d, Origins=%d"),
			bWalkerCalibrationActive ? 1 : 0, OrbSimDriver ? 1 : 0, WalkerLegCalibrations.Num(), WalkerOriginalFootPositions.Num());
		return;
	}

	// If StartingFootDistance is 0, restore original positions
	if (FMath::IsNearlyZero(StartingFootDistance))
	{
		for (int32 Index = 0; Index < WalkerLegCalibrations.Num(); ++Index)
		{
			WalkerLegCalibrations[Index].FootAxleWorldLocation = WalkerOriginalFootPositions[Index];
		}
		UE_LOG(LogTemp, Log, TEXT("ApplyStartingFootDistance: Restored original foot positions (StartingFootDistance=0)"));
		return;
	}

	UE_LOG(LogTemp, Log, TEXT("ApplyStartingFootDistance: Applying horizontal offset of %.1f tiles to %d legs"), StartingFootDistance, WalkerLegCalibrations.Num());

	// Apply offset from the body-aligned tile in the horizontal plane, expressed in tile distance.
	// StartingFootDistance is interpreted as "number of tiles away from the body tile" along the
	// direction from body to the original foot position.
	const float TileSize = OrbSimDriver->GetTileSize();

	// Body position in XY plane (used as origin for tile-based offset)
	const FVector BodyWorld2D = FVector(WalkerBodyBaseWorldLocation.X, WalkerBodyBaseWorldLocation.Y, 0.f);

	// Apply offset from original positions
	for (int32 Index = 0; Index < WalkerLegCalibrations.Num(); ++Index)
	{
		FWalkerLegCalibrationData& LegData = WalkerLegCalibrations[Index];
		const FVector& OriginalPos = WalkerOriginalFootPositions[Index];

		// Calculate direction from body to original foot position in horizontal plane
		const FVector OriginalFoot2D = FVector(OriginalPos.X, OriginalPos.Y, 0.f);
		FVector BodyToFootDir = (OriginalFoot2D - BodyWorld2D).GetSafeNormal();

		// If direction is invalid, use direction from body to hip
		if (BodyToFootDir.IsNearlyZero())
		{
			const FVector HipWorld2D = FVector(LegData.HipWorldLocation.X, LegData.HipWorldLocation.Y, 0.f);
			BodyToFootDir = (HipWorld2D - BodyWorld2D).GetSafeNormal();
			if (BodyToFootDir.IsNearlyZero())
			{
				// Use component forward direction as fallback
				const FTransform ComponentTransform = BodyComp->GetComponentTransform();
				const FVector ForwardComponent = FVector(1.f, 0.f, 0.f);
				const FVector ForwardWorld = ComponentTransform.TransformVector(ForwardComponent);
				BodyToFootDir = FVector(ForwardWorld.X, ForwardWorld.Y, 0.f).GetSafeNormal();
				if (BodyToFootDir.IsNearlyZero())
				{
					BodyToFootDir = FVector(1.f, 0.f, 0.f);
				}
			}
		}

		// Calculate new position by adding offset along the direction from body to original foot.
		// Offset magnitude is expressed in tiles and converted to world units via TileSize.
		const FVector Offset2D = BodyToFootDir * (StartingFootDistance * TileSize);
		const FVector NewFootWorld2D = BodyWorld2D + Offset2D;

		// Snap to nearest grid tile
		FHex NearestHex;
		if (OrbSimDriver->WorldToHex(FVector(NewFootWorld2D.X, NewFootWorld2D.Y, OriginalPos.Z), NearestHex))
		{
			const FVector SnappedWorld = OrbSimDriver->MapHexToWorld(NearestHex);

			// Check if within leg reach
			const float MaxLegLength = LegData.UpperLength + LegData.LowerLength;
			const float VerticalDist = FMath::Abs(LegData.HipWorldLocation.Z - OriginalPos.Z);
			const float MaxHorizontalReach = FMath::Sqrt(FMath::Max(0.f, MaxLegLength * MaxLegLength - VerticalDist * VerticalDist));
			const float NewDistance = FVector::Dist2D(FVector(LegData.HipWorldLocation.X, LegData.HipWorldLocation.Y, 0.f), FVector(SnappedWorld.X, SnappedWorld.Y, 0.f));

			if (NewDistance <= MaxHorizontalReach * 0.98f)
			{
				LegData.FootAxleWorldLocation = FVector(SnappedWorld.X, SnappedWorld.Y, OriginalPos.Z);
				UE_LOG(LogTemp, Verbose, TEXT("ApplyStartingFootDistance: Leg %d snapped to hex (%d,%d)"), Index, NearestHex.Q, NearestHex.R);
			}
			else
			{
				// Too far, clamp to reachable distance
				const FVector Hip2D = FVector(LegData.HipWorldLocation.X, LegData.HipWorldLocation.Y, 0.f);
				const FVector HipToFootDir = (NewFootWorld2D - Hip2D).GetSafeNormal();
				const FVector ReachablePos = Hip2D + HipToFootDir * (MaxHorizontalReach * 0.95f);
				LegData.FootAxleWorldLocation = FVector(ReachablePos.X, ReachablePos.Y, OriginalPos.Z);
				UE_LOG(LogTemp, Warning, TEXT("ApplyStartingFootDistance: Leg %d clamped to reachable distance (%.1f > %.1f)"),
					Index, NewDistance, MaxHorizontalReach * 0.98f);
			}
		}
		else
		{
			// Snap failed, use offset position
			LegData.FootAxleWorldLocation = FVector(NewFootWorld2D.X, NewFootWorld2D.Y, OriginalPos.Z);
			UE_LOG(LogTemp, Warning, TEXT("ApplyStartingFootDistance: Leg %d snap to hex failed, using offset position"), Index);
		}
	}

	UE_LOG(LogTemp, Log, TEXT("ApplyStartingFootDistance: Updated %d foot positions"), WalkerLegCalibrations.Num());

	// Update assigned tile world locations in the walker so they reflect the new foot positions
	if (CalibratingWalker.IsValid() && CalibratingWalker->Driver)
	{
		CalibratingWalker->UpdateAssignedTileWorldLocations();
		UE_LOG(LogTemp, VeryVerbose, TEXT("ApplyStartingFootDistance: Updated assigned tile world locations"));
	}

	// Keep quadrant/assigned-tile visualization in sync whenever starting foot distance
	// changes the base foot positions.
	if (bShowLegTileQuadrants && CalibratingWalker.IsValid())
	{
		UpdateLegTileQuadrantVisualization();
	}
}

// Helper function to convert EWalkerFoot enum to foot bone name
static FName GetFootBoneNameFromEnum(EWalkerFoot Foot)
{
	switch (Foot)
	{
	case EWalkerFoot::DFoot:
		return TEXT("DFoot49");
	case EWalkerFoot::LFoot:
		return TEXT("LFoot25");
	case EWalkerFoot::RFoot:
		return TEXT("RFoot13");
	case EWalkerFoot::UFoot:
		return TEXT("UFoot37");
	default:
		return TEXT("DFoot49");
	}
}

void AGameManager::SetStepHeight()
{
	// Only update if preview is enabled
	if (!bActiveFootPreview)
	{
		return;
	}

	if (!bWalkerCalibrationActive || !CalibratingWalker.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("GameManager: SetStepHeight called without an active calibration."));
		return;
	}

	ABasicWalker* Walker = CalibratingWalker.Get();
	if (!Walker)
	{
		UE_LOG(LogTemp, Warning, TEXT("GameManager: Calibration walker missing."));
		return;
	}

	USkeletalMeshComponent* BodyComp = Walker->GetBodyComponent();
	if (!BodyComp)
	{
		UE_LOG(LogTemp, Warning, TEXT("GameManager: Calibration walker has no skeletal mesh component."));
		return;
	}

	// Get foot bone name from enum
	FName ActiveFootBoneName = GetFootBoneNameFromEnum(ActiveFoot);

	// Find the leg data for the active foot
	int32 ActiveLegIndex = INDEX_NONE;
	for (int32 Index = 0; Index < WalkerLegCalibrations.Num(); ++Index)
	{
		if (WalkerLegCalibrations[Index].FootBone == ActiveFootBoneName)
		{
			ActiveLegIndex = Index;
			break;
		}
	}

	if (ActiveLegIndex == INDEX_NONE)
	{
		UE_LOG(LogTemp, Warning, TEXT("GameManager: ActiveFoot '%s' not found in leg calibrations."), *ActiveFootBoneName.ToString());
		return;
	}

	FWalkerLegCalibrationData& LegData = WalkerLegCalibrations[ActiveLegIndex];

	// Get the base foot position (original position + StartingFootDistance offset, but without step height)
	// First, restore to original position
	FVector BaseFootPos = WalkerOriginalFootPositions[ActiveLegIndex];

	// Apply StartingFootDistance offset if it was set
	if (StartingFootDistance != 0.f && OrbSimDriver)
	{
		// Recalculate the offset position (same logic as ApplyStartingFootDistance, using tile distance)
		const FVector BodyWorld2D = FVector(WalkerBodyBaseWorldLocation.X, WalkerBodyBaseWorldLocation.Y, 0.f);
		const FVector OriginalFoot2D = FVector(BaseFootPos.X, BaseFootPos.Y, 0.f);
		FVector BodyToFootDir = (OriginalFoot2D - BodyWorld2D).GetSafeNormal();

		if (BodyToFootDir.IsNearlyZero())
		{
			const FVector HipWorld2D = FVector(LegData.HipWorldLocation.X, LegData.HipWorldLocation.Y, 0.f);
			BodyToFootDir = (HipWorld2D - BodyWorld2D).GetSafeNormal();
			if (BodyToFootDir.IsNearlyZero())
			{
				const FTransform ComponentTransform = BodyComp->GetComponentTransform();
				const FVector ForwardComponent = FVector(1.f, 0.f, 0.f);
				const FVector ForwardWorld = ComponentTransform.TransformVector(ForwardComponent);
				BodyToFootDir = FVector(ForwardWorld.X, ForwardWorld.Y, 0.f).GetSafeNormal();
				if (BodyToFootDir.IsNearlyZero())
				{
					BodyToFootDir = FVector(1.f, 0.f, 0.f);
				}
			}
		}

		const float TileSize = OrbSimDriver->GetTileSize();
		const FVector Offset2D = BodyToFootDir * (StartingFootDistance * TileSize);
		const FVector NewFootWorld2D = BodyWorld2D + Offset2D;

		// Snap to grid
		FHex NearestHex;
		if (OrbSimDriver->WorldToHex(FVector(NewFootWorld2D.X, NewFootWorld2D.Y, BaseFootPos.Z), NearestHex))
		{
			const FVector SnappedWorld = OrbSimDriver->MapHexToWorld(NearestHex);
			BaseFootPos = FVector(SnappedWorld.X, SnappedWorld.Y, BaseFootPos.Z);
		}
		else
		{
			BaseFootPos = FVector(NewFootWorld2D.X, NewFootWorld2D.Y, BaseFootPos.Z);
		}
	}

	// Raise the active foot by StepHeight from the base position
	LegData.FootAxleWorldLocation = FVector(BaseFootPos.X, BaseFootPos.Y, BaseFootPos.Z + StepHeight);

	// Update IK Rig goal positions
	SetIKRigGoalPositions(BodyComp, StandHeight);

	// Update debug meshes
	UpdateWalkerDebugMeshes(BodyComp);

	// Update assigned tile world locations in the walker
	if (Walker && Walker->Driver)
	{
		Walker->UpdateAssignedTileWorldLocations();
		UE_LOG(LogTemp, VeryVerbose, TEXT("SetStepHeight: Updated assigned tile world locations"));
	}

	// Update tile/assigned-tile visualization so highlighted tiles follow the previewed foot
	if (bShowLegTileQuadrants)
	{
		UpdateLegTileQuadrantVisualization();
	}

	UE_LOG(LogTemp, Log, TEXT("GameManager: Set step height %.1f for foot %s"), StepHeight, *ActiveFootBoneName.ToString());
}

void AGameManager::SetIKRigGoalPositions(USkeletalMeshComponent* BodyComp, float HeightOffset)
{
	if (!BodyComp || !CalibratingWalker.IsValid())
	{
		return;
	}

	ABasicWalker* Walker = CalibratingWalker.Get();
	if (!Walker)
	{
		return;
	}

	// Get the animation blueprint instance from the skeletal mesh component
	UAnimInstance* AnimInstance = BodyComp->GetAnimInstance();
	if (!AnimInstance)
	{
		UE_LOG(LogTemp, Warning, TEXT("SetIKRigGoalPositions: Animation blueprint instance not found"));
		return;
	}

	// Map leg calibration data to IK Rig goal positions
	// IK Rig goals should position the foot BONE at the target world location (tile center)
	// The goal is where we want the foot bone to end up after IK is applied

	// Get the component transform for space conversion
	// IK Rig typically works in component-local space
	const FTransform ComponentToWorld = BodyComp->GetComponentToWorld();
	const FRotator ComponentRotation = BodyComp->GetComponentRotation();
	
	// Validate component transform before using it
	if (ComponentToWorld.ContainsNaN() || ComponentRotation.ContainsNaN())
	{
		UE_LOG(LogTemp, Error, TEXT("SetIKRigGoalPositions: Component transform contains NaN, skipping IK goal update"));
		return;
	}
	
	const FTransform ComponentTransformNoScale(ComponentRotation, ComponentToWorld.GetLocation(), FVector(1.f));
	const FTransform WorldToComponentNoScale = ComponentTransformNoScale.Inverse();
	
	// Validate inverse transform
	if (WorldToComponentNoScale.ContainsNaN())
	{
		UE_LOG(LogTemp, Error, TEXT("SetIKRigGoalPositions: WorldToComponent transform contains NaN, skipping IK goal update"));
		return;
	}

	// Get the body goal position (current body location + height offset) in world space
	const FVector BodyGoalWorld = WalkerBodyBaseWorldLocation + FVector(0.f, 0.f, HeightOffset);

	// Validate body goal world position
	if (BodyGoalWorld.ContainsNaN())
	{
		UE_LOG(LogTemp, Error, TEXT("SetIKRigGoalPositions: BodyGoalWorld contains NaN, skipping body goal update"));
		return;
	}
	
	// Convert body goal to component space
	const FVector BodyGoalComponent = WorldToComponentNoScale.TransformPosition(BodyGoalWorld);
	
	// Validate body goal component space result
	if (BodyGoalComponent.ContainsNaN())
	{
		UE_LOG(LogTemp, Error, TEXT("SetIKRigGoalPositions: BodyGoalComponent contains NaN after transform, skipping body goal update"));
		return;
	}

	// Set BodyGoal position on the animation blueprint instance (always set, even when ManualFootGoal is true)
	if (FProperty* BodyGoalProperty = AnimInstance->GetClass()->FindPropertyByName(TEXT("BodyGoal")))
	{
		if (FStructProperty* StructProp = CastField<FStructProperty>(BodyGoalProperty))
		{
			if (StructProp->Struct == TBaseStructure<FVector>::Get())
			{
				FVector* GoalValue = StructProp->ContainerPtrToValuePtr<FVector>(AnimInstance);
				if (GoalValue)
				{
					// Use component-local space (no scale)
					*GoalValue = BodyGoalComponent;
					UE_LOG(LogTemp, VeryVerbose, TEXT("SetIKRigGoalPositions: Set BodyGoal to component space (%.1f,%.1f,%.1f)"), 
						BodyGoalComponent.X, BodyGoalComponent.Y, BodyGoalComponent.Z);
				}
			}
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("SetIKRigGoalPositions: BodyGoal property not found or not a FVector on Animation Blueprint"));
		}
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("SetIKRigGoalPositions: BodyGoal property not found on Animation Blueprint %s"),
			*AnimInstance->GetClass()->GetName());
	}

	// Skip automatic foot goal assignment if ManualFootGoal is enabled
	if (bManualFootGoal)
	{
		UE_LOG(LogTemp, VeryVerbose, TEXT("SetIKRigGoalPositions: bManualFootGoal is true, skipping automatic foot goal assignment (BodyGoal was set)"));
		
		// Preserve foot goal values before animation update (they may be reset by event graph)
		const FName FootGoalNames[] = { TEXT("DFootGoal"), TEXT("LFootGoal"), TEXT("RFootGoal"), TEXT("UFootGoal") };
		FVector PreservedFootGoals[4];
		bool bHasPreservedGoals[4] = { false, false, false, false };
		
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
		
		// Force the animation system to update with the new body goal value
		if (AnimInstance)
		{
			AnimInstance->BlueprintThreadSafeUpdateAnimation(0.f);
			BodyComp->TickAnimation(0.f, false);
			BodyComp->RefreshBoneTransforms();
		}
		
		// Restore preserved foot goal values (in case event graph reset them)
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
		
		return;
	}

	// Set foot goal positions - these should position the foot BONE at the tile center
	// Map each leg to its corresponding goal:
	// D -> DFootGoal, L -> LFootGoal, R -> RFootGoal, U -> UFootGoal

	// Check if walker is in simulation and get assigned tiles
	FHex AssignedTiles[4] = { FHex(), FHex(), FHex(), FHex() };
	bool bHasSimTiles = false;
	if (Walker->SimWalkerIndex >= 0 && OrbSimDriver)
	{
		if (USimWorldHex* Sim = OrbSimDriver->GetSim())
		{
			if (Walker->SimWalkerIndex < Sim->Walkers.Num())
			{
				const FWalkerState& SimState = Sim->Walkers[Walker->SimWalkerIndex];
				AssignedTiles[0] = SimState.DFoot.CurrentTile;  // D
				AssignedTiles[1] = SimState.LFoot.CurrentTile;  // L
				AssignedTiles[2] = SimState.RFoot.CurrentTile;  // R
				AssignedTiles[3] = SimState.UFoot.CurrentTile;  // U
				bHasSimTiles = true;
				UE_LOG(LogTemp, Warning, TEXT("SetIKRigGoalPositions: Using assigned tiles from simulation"));
			}
		}
	}

	for (int32 Index = 0; Index < WalkerLegCalibrations.Num(); ++Index)
	{
		const FWalkerLegCalibrationData& LegData = WalkerLegCalibrations[Index];

		// Determine which goal this leg corresponds to based on bone names
		FString GoalName;
		int32 FootIndex = -1; // 0=D, 1=L, 2=R, 3=U
		const FString HipName = LegData.HipAxleBone.ToString();
		if (HipName.Contains(TEXT("DHip")))
		{
			GoalName = TEXT("DFootGoal");
			FootIndex = 0;
		}
		else if (HipName.Contains(TEXT("LHip")))
		{
			GoalName = TEXT("LFootGoal");
			FootIndex = 1;
		}
		else if (HipName.Contains(TEXT("RHip")))
		{
			GoalName = TEXT("RFootGoal");
			FootIndex = 2;
		}
		else if (HipName.Contains(TEXT("UHip")))
		{
			GoalName = TEXT("UFootGoal");
			FootIndex = 3;
		}

		// Determine target world position - prefer assigned tile from simulation if available
		FVector FootGoalWorld = LegData.FootAxleWorldLocation;
		FHex TargetHex;
		bool bUsingAssignedTile = false;

		if (bHasSimTiles && FootIndex >= 0 && FootIndex < 4)
		{
			// Use assigned tile from simulation
			TargetHex = AssignedTiles[FootIndex];
			FootGoalWorld = OrbSimDriver->MapHexToWorld(TargetHex);
			bUsingAssignedTile = true;
			UE_LOG(LogTemp, Warning, TEXT("SetIKRigGoalPositions: Leg %d (%s) using assigned tile (%d,%d) -> world (%.1f,%.1f,%.1f)"),
				Index, *HipName, TargetHex.Q, TargetHex.R, FootGoalWorld.X, FootGoalWorld.Y, FootGoalWorld.Z);
		}
		else
		{
			// Use FootAxleWorldLocation from calibration data
			if (OrbSimDriver && OrbSimDriver->WorldToHex(FootGoalWorld, TargetHex))
			{
				UE_LOG(LogTemp, Warning, TEXT("SetIKRigGoalPositions: Leg %d (%s) using FootAxleWorldLocation (%.1f,%.1f,%.1f) -> hex (%d,%d)"),
					Index, *HipName, FootGoalWorld.X, FootGoalWorld.Y, FootGoalWorld.Z, TargetHex.Q, TargetHex.R);
			}
			else
			{
				UE_LOG(LogTemp, Warning, TEXT("SetIKRigGoalPositions: Leg %d (%s) using FootAxleWorldLocation (%.1f,%.1f,%.1f) [could not convert to hex]"),
					Index, *HipName, FootGoalWorld.X, FootGoalWorld.Y, FootGoalWorld.Z);
			}
		}

		// Log comparison if we have both
		if (bHasSimTiles && FootIndex >= 0 && FootIndex < 4)
		{
			const FVector CalibrationWorld = LegData.FootAxleWorldLocation;
			const float Distance = FVector::Dist(CalibrationWorld, FootGoalWorld);
			UE_LOG(LogTemp, Warning, TEXT("SetIKRigGoalPositions: Leg %d comparison - Calibration: (%.1f,%.1f,%.1f) vs Assigned: (%.1f,%.1f,%.1f) [Distance=%.1f]"),
				Index, CalibrationWorld.X, CalibrationWorld.Y, CalibrationWorld.Z,
				FootGoalWorld.X, FootGoalWorld.Y, FootGoalWorld.Z, Distance);
		}

		// Validate foot goal world position before transformation
		if (FootGoalWorld.ContainsNaN())
		{
			UE_LOG(LogTemp, Error, TEXT("SetIKRigGoalPositions: Leg %d (%s) FootGoalWorld contains NaN, skipping"), Index, *HipName);
			continue;
		}
		
		// Convert foot goal to component-local space (without scale to avoid distortion)
		const FVector FootGoalComponent = WorldToComponentNoScale.TransformPosition(FootGoalWorld);
		
		// Validate component space result
		if (FootGoalComponent.ContainsNaN())
		{
			UE_LOG(LogTemp, Error, TEXT("SetIKRigGoalPositions: Leg %d (%s) FootGoalComponent contains NaN after transform, skipping"), Index, *HipName);
			continue;
		}

		// Log distance between assigned tile and actual foot bone location
		const FVector BoneWorldLocation = BodyComp->GetBoneLocation(LegData.FootBone, EBoneSpaces::WorldSpace);
		if (!BoneWorldLocation.ContainsNaN())
		{
			const float AssignedDistance = FVector::Dist(FootGoalWorld, BoneWorldLocation);
			UE_LOG(LogTemp, Warning, TEXT("WalkerCalib: %s tile vs bone distance %.1f (Tile: %.1f, %.1f, %.1f | Bone: %.1f, %.1f, %.1f)"),
				*LegData.FootBone.ToString(),
				AssignedDistance,
				FootGoalWorld.X, FootGoalWorld.Y, FootGoalWorld.Z,
				BoneWorldLocation.X, BoneWorldLocation.Y, BoneWorldLocation.Z);
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("WalkerCalib: %s bone location contains NaN, skipping distance calculation"),
				*LegData.FootBone.ToString());
		}

		// Set the goal position on the animation blueprint instance via reflection
			// The goal should be in component-local space so IK Rig can apply it correctly
		if (FProperty* GoalProperty = AnimInstance->GetClass()->FindPropertyByName(*GoalName))
		{
			if (FStructProperty* StructProp = CastField<FStructProperty>(GoalProperty))
			{
				if (StructProp->Struct == TBaseStructure<FVector>::Get())
				{
					FVector* GoalValue = StructProp->ContainerPtrToValuePtr<FVector>(AnimInstance);
					if (GoalValue)
					{
						// Use component-local space (no scale) so IK Rig positions the foot bone correctly
						*GoalValue = FootGoalComponent;
						UE_LOG(LogTemp, Warning, TEXT("SetIKRigGoalPositions: Set %s to component space (%.1f,%.1f,%.1f)"),
							*GoalName, FootGoalComponent.X, FootGoalComponent.Y, FootGoalComponent.Z);
					}
					else
					{
						UE_LOG(LogTemp, Warning, TEXT("SetIKRigGoalPositions: Failed to get value pointer for %s"), *GoalName);
					}
				}
				else
				{
					UE_LOG(LogTemp, Warning, TEXT("SetIKRigGoalPositions: %s is not a FVector property"), *GoalName);
				}
			}
			else
			{
				UE_LOG(LogTemp, Warning, TEXT("SetIKRigGoalPositions: %s is not a struct property"), *GoalName);
			}
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("SetIKRigGoalPositions: Property %s not found on Animation Blueprint %s"),
				*GoalName, *AnimInstance->GetClass()->GetName());
		}
	}

	// Force the animation system to update with the new values
	if (AnimInstance)
	{
		// Force animation blueprint to update with new goal values
		AnimInstance->BlueprintThreadSafeUpdateAnimation(0.f);

		// Force skeletal mesh to refresh from animation
		BodyComp->TickAnimation(0.f, false);
		BodyComp->RefreshBoneTransforms();
		BodyComp->UpdateComponentToWorld();

		// Mark for render update in editor
		if (GIsEditor && !GIsPlayInEditorWorld)
		{
			BodyComp->MarkRenderStateDirty();
			BodyComp->MarkRenderTransformDirty();
		}
	}
	else
	{
		// Fallback if no animation instance
		BodyComp->RefreshBoneTransforms();
		BodyComp->UpdateComponentToWorld();
	}
}

void AGameManager::CreateWalkerDebugMeshes(ABasicWalker* Walker, USkeletalMeshComponent* BodyComp)
{
	ClearWalkerDebugMeshes();
	if (!Walker || !BodyComp || WalkerLegCalibrations.Num() == 0)
	{
		return;
	}

	UStaticMesh* SphereMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	if (!SphereMesh)
	{
		UE_LOG(LogTemp, Warning, TEXT("GameManager: Failed to load debug sphere mesh."));
		return;
	}

	WalkerFootDebugMeshes.SetNum(WalkerLegCalibrations.Num());
	WalkerKneeDebugMeshes.SetNum(WalkerLegCalibrations.Num());

	UMaterialInterface* FootMaterial = WalkerFootDebugMaterial;
	UMaterialInterface* KneeMaterial = WalkerKneeDebugMaterial;

	if (!FootMaterial && GEngine)
	{
		UMaterialInterface* BaseMaterial = GEngine->DebugMeshMaterial;
		if (BaseMaterial)
		{
			if (UMaterialInstanceDynamic* FootMID = UMaterialInstanceDynamic::Create(BaseMaterial, this))
			{
				FootMID->SetVectorParameterValue(TEXT("DiffuseColor"), FLinearColor::Green);
				FootMaterial = FootMID;
			}
			if (!FootMaterial)
			{
				FootMaterial = BaseMaterial;
			}
		}
	}
	if (!KneeMaterial && GEngine)
	{
		UMaterialInterface* BaseMaterial = GEngine->DebugMeshMaterial;
		if (BaseMaterial)
		{
			UMaterialInstanceDynamic* KneeMID = UMaterialInstanceDynamic::Create(BaseMaterial, this);
			if (KneeMID)
			{
				KneeMID->SetVectorParameterValue(TEXT("DiffuseColor"), FLinearColor(1.f, 0.56f, 0.f));
				KneeMaterial = KneeMID;
			}
			if (!KneeMaterial)
			{
				KneeMaterial = BaseMaterial;
			}
		}
	}

	const float FootScale = 40.f / 50.f;
	const float KneeScale = 28.f / 50.f;

	for (int32 Index = 0; Index < WalkerLegCalibrations.Num(); ++Index)
	{
		const FWalkerLegCalibrationData& LegData = WalkerLegCalibrations[Index];

		UStaticMeshComponent* FootComp = NewObject<UStaticMeshComponent>(Walker);
		if (FootComp)
		{
			FootComp->SetStaticMesh(SphereMesh);
			FootComp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			FootComp->SetMobility(EComponentMobility::Movable);
			FootComp->SetCastShadow(false);
			FootComp->SetIsVisualizationComponent(true);
			if (FootMaterial)
			{
				FootComp->SetMaterial(0, FootMaterial);
			}
			FootComp->RegisterComponent();
			FootComp->AttachToComponent(Walker->GetRootComponent(), FAttachmentTransformRules::KeepWorldTransform);
			FootComp->SetWorldScale3D(FVector(FootScale));
			FootComp->SetWorldLocation(BodyComp->GetBoneLocation(LegData.FootBone, EBoneSpaces::WorldSpace));
			FootComp->SetVisibility(true, true);
		}
		WalkerFootDebugMeshes[Index] = FootComp;

		UStaticMeshComponent* KneeComp = NewObject<UStaticMeshComponent>(Walker);
		if (KneeComp)
		{
			KneeComp->SetStaticMesh(SphereMesh);
			KneeComp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			KneeComp->SetMobility(EComponentMobility::Movable);
			KneeComp->SetCastShadow(false);
			KneeComp->SetIsVisualizationComponent(true);
			if (KneeMaterial)
			{
				KneeComp->SetMaterial(0, KneeMaterial);
			}
			KneeComp->RegisterComponent();
			KneeComp->AttachToComponent(Walker->GetRootComponent(), FAttachmentTransformRules::KeepWorldTransform);
			KneeComp->SetWorldScale3D(FVector(KneeScale));
			KneeComp->SetWorldLocation(BodyComp->GetBoneLocation(LegData.KneeAxleBone, EBoneSpaces::WorldSpace));
			KneeComp->SetVisibility(true, true);
		}
		WalkerKneeDebugMeshes[Index] = KneeComp;
	}
}

void AGameManager::UpdateWalkerDebugMeshes(const USkeletalMeshComponent* BodyComp)
{
	if (!bWalkerCalibrationActive || !BodyComp || !CalibratingWalker.IsValid())
	{
		return;
	}

	const int32 DesiredCount = WalkerLegCalibrations.Num();
	if (WalkerFootDebugMeshes.Num() != DesiredCount || WalkerKneeDebugMeshes.Num() != DesiredCount)
	{
		CreateWalkerDebugMeshes(CalibratingWalker.Get(), const_cast<USkeletalMeshComponent*>(BodyComp));
		return;
	}

	for (int32 Index = 0; Index < DesiredCount; ++Index)
	{
		const FWalkerLegCalibrationData& LegData = WalkerLegCalibrations[Index];

		if (UStaticMeshComponent* FootComp = WalkerFootDebugMeshes[Index].Get())
		{
			FootComp->SetWorldLocation(BodyComp->GetBoneLocation(LegData.FootBone, EBoneSpaces::WorldSpace));
		}
		else
		{
			CreateWalkerDebugMeshes(CalibratingWalker.Get(), const_cast<USkeletalMeshComponent*>(BodyComp));
			return;
		}

		if (UStaticMeshComponent* KneeComp = WalkerKneeDebugMeshes[Index].Get())
		{
			KneeComp->SetWorldLocation(BodyComp->GetBoneLocation(LegData.KneeAxleBone, EBoneSpaces::WorldSpace));
		}
		else
		{
			CreateWalkerDebugMeshes(CalibratingWalker.Get(), const_cast<USkeletalMeshComponent*>(BodyComp));
			return;
		}
	}
}

void AGameManager::ClearWalkerDebugMeshes()
{
	for (TWeakObjectPtr<UStaticMeshComponent>& MeshPtr : WalkerFootDebugMeshes)
	{
		if (UStaticMeshComponent* Comp = MeshPtr.Get())
		{
			Comp->DestroyComponent();
		}
	}
	WalkerFootDebugMeshes.Empty();

	for (TWeakObjectPtr<UStaticMeshComponent>& MeshPtr : WalkerKneeDebugMeshes)
	{
		if (UStaticMeshComponent* Comp = MeshPtr.Get())
		{
			Comp->DestroyComponent();
		}
	}
	WalkerKneeDebugMeshes.Empty();
}

void AGameManager::UpdateLegTileQuadrantVisualization()
{
	ClearLegTileQuadrantVisualization();

	// Knee/leg-color markers should always be visible when calibration is active,
	// tile markers are gated by bShowLegTileQuadrants further down.
	if (!bWalkerCalibrationActive || !CalibratingWalker.IsValid() || !OrbSimDriver)
	{
		return;
	}

	ABasicWalker* Walker = CalibratingWalker.Get();
	USkeletalMeshComponent* BodyComp = Walker ? Walker->GetBodyComponent() : nullptr;
	if (!Walker || !BodyComp)
	{
		return;
	}

	// Get body hex position
	FHex BodyHex;
	if (!OrbSimDriver->WorldToHex(WalkerBodyBaseWorldLocation, BodyHex))
	{
		return;
	}

	// Helper function to apply quadrant offset
	// Quadrant order: N (0), E (1), S (2), W (3) - clockwise around the body
	// Offset rotates the quadrant assignment
	auto ApplyQuadrantOffset = [](const FString& OriginalQuadrant, int32 Offset) -> FString
		{
			static const TArray<FString> QuadrantOrder = { TEXT("N"), TEXT("E"), TEXT("S"), TEXT("W") };
			int32 OriginalIndex = -1;
			for (int32 i = 0; i < QuadrantOrder.Num(); ++i)
			{
				if (QuadrantOrder[i] == OriginalQuadrant)
				{
					OriginalIndex = i;
					break;
				}
			}
			if (OriginalIndex < 0)
			{
				return OriginalQuadrant; // Invalid quadrant, return as-is
			}
			// Apply offset and wrap around
			int32 OffsetValue = FMath::Clamp(Offset, 0, 3);
			int32 NewIndex = (OriginalIndex + OffsetValue) % 4;
			return QuadrantOrder[NewIndex];
		};

	// Base color mapping for each leg quadrant (consistent colors, no variation)
	// Colors are assigned based on actual tile quadrant colors, not bone names
	// NOTE: Colors preserve previous semantics where:
	// - South (formerly D)  = Yellow
	// - West  (formerly L)  = Blue
	// - East  (formerly R)  = Red
	// - North (formerly U)  = Green
	static const TMap<FString, FLinearColor> LegBaseColors = {
		{ TEXT("N"), FLinearColor(0.0f, 1.0f, 0.0f) },  // Green
		{ TEXT("E"), FLinearColor(1.0f, 0.0f, 0.0f) },  // Red
		{ TEXT("W"), FLinearColor(0.0f, 0.0f, 1.0f) },  // Blue
		{ TEXT("S"), FLinearColor(1.0f, 1.0f, 0.0f) }   // Yellow
	};

	// Get tile size for marker scaling
	const float TileSize = OrbSimDriver->GetTileSize();
	const float MarkerScale = 0.4f;      // Larger tile markers
	const float KneeMarkerScale = 0.6f;  // Larger knee markers

	UStaticMesh* SphereMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	if (!SphereMesh)
	{
		UE_LOG(LogTemp, Warning, TEXT("GameManager: Failed to load sphere mesh for quadrant visualization."));
		return;
	}

	// Helper function to determine fixed quadrant from hex coordinates
	// Fixed quadrants are based on hex coordinates relative to body, NOT leg positions or offset
	// Compass mapping on axial coordinates:
	//   East  (E) = Q+1
	//   West  (W) = Q-1
	//   South (S) = R+1
	//   North (N) = R-1
	auto GetFixedQuadrantFromHex = [](const FHex& BodyHex, const FHex& TestHex) -> FString
	{
		// Use hex coordinate differences to determine fixed quadrant
		const int32 dq = TestHex.Q - BodyHex.Q;
		const int32 dr = TestHex.R - BodyHex.R;
		
		// For axis-aligned hexes:
		if (dq > 0 && dr == 0)
		{
			return TEXT("E"); // Positive Q (Q+1)
		}
		else if (dq == 0 && dr > 0)
		{
			return TEXT("S"); // Positive R (R+1)
		}
		else if (dq < 0 && dr == 0)
		{
			return TEXT("W"); // Negative Q (Q-1)
		}
		else if (dq == 0 && dr < 0)
		{
			return TEXT("N"); // Negative R (R-1)
		}
		// For diagonal hexes, use the dominant direction
		else if (FMath::Abs(dq) > FMath::Abs(dr))
		{
			// Q direction is dominant
			return (dq > 0) ? TEXT("E") : TEXT("W");
		}
		else if (FMath::Abs(dr) > FMath::Abs(dq))
		{
			// R direction is dominant
			return (dr > 0) ? TEXT("S") : TEXT("N");
		}
		else
		{
			// Equal magnitude - use diagonal assignment based on signs
			// Prioritize based on compass-style mapping
			if (dq > 0 && dr > 0)
			{
				// Southeast diagonal - closer to South / East; prefer South
				return TEXT("S");
			}
			else if (dq > 0 && dr < 0)
			{
				// Northeast diagonal - closer to North / East; prefer East
				return TEXT("E");
			}
			else if (dq < 0 && dr > 0)
			{
				// Southwest diagonal - closer to South / West; prefer West
				return TEXT("W");
			}
			else // dq < 0 && dr < 0
			{
				// Northwest diagonal - closer to North / West; prefer North
				return TEXT("N");
			}
		}
	};

	// First pass: determine leg indices for each physical leg from bone names
	// We keep the physical bone naming (D,L,R,U) but map them into compass quadrants
	int32 DLegIndex = INDEX_NONE; // Physical "Down" leg
	int32 LLegIndex = INDEX_NONE; // Physical "Left" leg
	int32 RLegIndex = INDEX_NONE; // Physical "Right" leg
	int32 ULegIndex = INDEX_NONE; // Physical "Up" leg
	for (int32 LegIndex = 0; LegIndex < WalkerLegCalibrations.Num(); ++LegIndex)
	{
		const FWalkerLegCalibrationData& LegData = WalkerLegCalibrations[LegIndex];
		const FString HipName = LegData.HipAxleBone.ToString();
		if (HipName.StartsWith(TEXT("D")))
		{
			DLegIndex = LegIndex;
		}
		else if (HipName.StartsWith(TEXT("L")))
		{
			LLegIndex = LegIndex;
		}
		else if (HipName.StartsWith(TEXT("R")))
		{
			RLegIndex = LegIndex;
		}
		else if (HipName.StartsWith(TEXT("U")))
		{
			ULegIndex = LegIndex;
		}
	}

	// Helper to convert an EWalkerFoot selection to a leg index
	auto GetLegIndexForFoot = [&](EWalkerFoot Foot) -> int32
	{
		switch (Foot)
		{
		case EWalkerFoot::DFoot: return DLegIndex;
		case EWalkerFoot::LFoot: return LLegIndex;
		case EWalkerFoot::RFoot: return RLegIndex;
		case EWalkerFoot::UFoot: return ULegIndex;
		default: return INDEX_NONE;
		}
	};

	// Map: LegIndex -> AssignedQuadrant and color, using per-quadrant leg selection
	TMap<int32, FString> LegToQuadrantMap;
	TMap<int32, FLinearColor> LegToColorMap;
	TMap<int32, FName> LegToKneeBoneMap;

	auto AssignQuadrantToLeg = [&](const FString& Quadrant, EWalkerFoot QuadrantLeg)
	{
		const int32 LegIndex = GetLegIndexForFoot(QuadrantLeg);
		if (LegIndex == INDEX_NONE || !WalkerLegCalibrations.IsValidIndex(LegIndex))
		{
			return;
		}

		const FLinearColor* QuadrantBaseColor = LegBaseColors.Find(Quadrant);
		if (!QuadrantBaseColor)
		{
			return;
		}

		LegToQuadrantMap.Add(LegIndex, Quadrant);
		LegToColorMap.Add(LegIndex, *QuadrantBaseColor);
		LegToKneeBoneMap.Add(LegIndex, WalkerLegCalibrations[LegIndex].KneeAxleBone);
	};

	// Apply per-quadrant leg assignment (compass order N,E,S,W around the body)
	AssignQuadrantToLeg(TEXT("N"), NQuadrantLeg);
	AssignQuadrantToLeg(TEXT("E"), EQuadrantLeg);
	AssignQuadrantToLeg(TEXT("W"), WQuadrantLeg);
	AssignQuadrantToLeg(TEXT("S"), SQuadrantLeg);

	// Fallback: if no legs were assigned via N/E/W/S mapping (e.g., due to invalid enum values),
	// fall back to physical leg mapping D/L/R/U -> S/W/E/N so that visualization always has data.
	if (LegToQuadrantMap.Num() == 0)
	{
		auto AddDefaultLegMapping = [&](int32 LegIndex, const FString& Quadrant)
		{
			if (LegIndex == INDEX_NONE || !WalkerLegCalibrations.IsValidIndex(LegIndex))
			{
				return;
			}

			const FLinearColor* QuadrantBaseColor = LegBaseColors.Find(Quadrant);
			if (!QuadrantBaseColor)
			{
				return;
			}

			LegToQuadrantMap.Add(LegIndex, Quadrant);
			LegToColorMap.Add(LegIndex, *QuadrantBaseColor);
			LegToKneeBoneMap.Add(LegIndex, WalkerLegCalibrations[LegIndex].KneeAxleBone);
		};

		// Map physical legs to compass quadrants (matches original D/L/R/U semantics):
		// D -> S, L -> W, R -> E, U -> N
		AddDefaultLegMapping(DLegIndex, TEXT("S"));
		AddDefaultLegMapping(LLegIndex, TEXT("W"));
		AddDefaultLegMapping(RLegIndex, TEXT("E"));
		AddDefaultLegMapping(ULegIndex, TEXT("N"));
	}

	// Second pass: Assign tiles to fixed quadrants based on hex coordinates (NO offset applied)
	// Map: Hex -> FixedQuadrant (N, E, W, S based on hex position)
	TMap<FHex, FString> HexToQuadrantMap;
	
	const int32 SearchRadius = ReachDistance;
	for (int32 q = -SearchRadius; q <= SearchRadius; ++q)
	{
		for (int32 r = -SearchRadius; r <= SearchRadius; ++r)
		{
			FHex TestHex(BodyHex.Q + q, BodyHex.R + r);
			
			// Skip body hex itself
			if (TestHex == BodyHex)
			{
				continue;
			}
			
			// Check hex distance from body
			const int32 dq = TestHex.Q - BodyHex.Q;
			const int32 dr = TestHex.R - BodyHex.R;
			const int32 HexDist = FMath::Max(FMath::Max(FMath::Abs(dq), FMath::Abs(dr)), FMath::Abs(dq + dr));
			
			if (HexDist > ReachDistance)
			{
				continue;
			}

			if (MinimumReachDistance > 0.f && (float)HexDist < MinimumReachDistance)
			{
				continue;
			}
			
			// Assign hex to fixed quadrant based on hex coordinates (spatial quadrant around body)
			FString FixedQuadrant = GetFixedQuadrantFromHex(BodyHex, TestHex);

			// Apply global tile quadrant rotation around the center (does NOT change which leg is assigned
			// to each logical D/L/R/U quadrant, only which spatial slice gets which logical label)
			if (TileQuadrantOffset != 0)
			{
				FixedQuadrant = ApplyQuadrantOffset(FixedQuadrant, TileQuadrantOffset);
			}
			HexToQuadrantMap.Add(TestHex, FixedQuadrant);
		}
	}
	
	// Third pass: Map fixed quadrants to legs (using offset)
	// Then create Hex -> LegIndex mapping
	TMap<FHex, int32> HexToLegMap;
	
	// Create reverse mapping: FixedQuadrant -> LegIndex (which leg is assigned to this fixed quadrant)
	TMap<FString, int32> QuadrantToLegMap;
	for (const auto& LegQuadPair : LegToQuadrantMap)
	{
		QuadrantToLegMap.Add(LegQuadPair.Value, LegQuadPair.Key);
	}
	
	// Assign each hex to the leg that's assigned to its fixed quadrant
	for (const auto& HexQuadPair : HexToQuadrantMap)
	{
		const FHex& Hex = HexQuadPair.Key;
		const FString& FixedQuadrant = HexQuadPair.Value;
		const int32 HexDist = FMath::Max(
			FMath::Max(FMath::Abs(Hex.Q - BodyHex.Q), FMath::Abs(Hex.R - BodyHex.R)),
			FMath::Abs((Hex.Q - BodyHex.Q) + (Hex.R - BodyHex.R)));

		if (MinimumReachDistance > 0.f && (float)HexDist < MinimumReachDistance)
		{
			continue;
		}
		
		// Find which leg is assigned to this fixed quadrant (after offset)
		const int32* LegIndexPtr = QuadrantToLegMap.Find(FixedQuadrant);
		if (LegIndexPtr)
		{
			HexToLegMap.Add(Hex, *LegIndexPtr);
		}
	}

	// Fourth pass: Create material instances for each leg (one per leg, shared across all tiles)
	TMap<int32, UMaterialInstanceDynamic*> LegMaterials;
	
	if (UMaterialInterface* BaseMaterial = GEngine->DebugMeshMaterial)
	{
		for (const auto& LegColorPair : LegToColorMap)
		{
			const int32 LegIndex = LegColorPair.Key;
			const FLinearColor& LegColor = LegColorPair.Value;
			
			UMaterialInstanceDynamic* LegMaterial = UMaterialInstanceDynamic::Create(BaseMaterial, this);
			if (LegMaterial)
			{
				// Use consistent color for this leg (no variation)
				LegMaterial->SetVectorParameterValue(TEXT("Color"), LegColor);
				LegMaterial->SetVectorParameterValue(TEXT("BaseColor"), LegColor);
				LegMaterial->SetVectorParameterValue(TEXT("EmissiveColor"), LegColor);
				LegMaterial->SetVectorParameterValue(TEXT("DiffuseColor"), LegColor);
				
				LegMaterials.Add(LegIndex, LegMaterial);
			}
		}
	}

	// Fourth pass: Create tile markers (each hex gets exactly one marker with its assigned leg's color)
	// Instead of orbs at tile centers, we draw flat tile outlines using the grid's TileMesh (if assigned).
	if (bShowLegTileQuadrants)
	{
		// Choose a mesh for tile outlines: prefer the GameManager's TileMesh, otherwise fall back to sphere.
		UStaticMesh* TileOutlineMesh = TileMesh ? TileMesh : SphereMesh;

		for (const auto& HexLegPair : HexToLegMap)
		{
			const FHex& ReachableHex = HexLegPair.Key;
			const int32 LegIndex = HexLegPair.Value;
			
			UMaterialInstanceDynamic** LegMaterialPtr = LegMaterials.Find(LegIndex);
			if (!LegMaterialPtr || !*LegMaterialPtr)
			{
				continue;
			}

			const FVector TileWorld = OrbSimDriver->MapHexToWorld(ReachableHex);
			
			UStaticMeshComponent* MarkerComp = NewObject<UStaticMeshComponent>(Walker);
			if (MarkerComp)
			{
				MarkerComp->SetStaticMesh(TileOutlineMesh);
				MarkerComp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
				MarkerComp->SetMobility(EComponentMobility::Movable);
				MarkerComp->SetCastShadow(false);
				MarkerComp->SetIsVisualizationComponent(true);
				
				// Use the assigned leg's material
				MarkerComp->SetMaterial(0, *LegMaterialPtr);
				
				MarkerComp->RegisterComponent();
				MarkerComp->AttachToComponent(Walker->GetRootComponent(), FAttachmentTransformRules::KeepWorldTransform);

				// Draw a thin outline over the tile by slightly scaling down and lifting above the base tiles.
				const float BaseZOffset = 2.f;
				const float BaseScale = MarkerScale;

				MarkerComp->SetWorldScale3D(FVector(BaseScale));
				MarkerComp->SetWorldLocation(TileWorld + FVector(0.f, 0.f, BaseZOffset)); // Slightly above ground
				MarkerComp->SetVisibility(true, true);
			}
			WalkerQuadrantTileMarkers.Add(MarkerComp);
		}

		// Fifth pass: explicitly highlight the currently assigned tile for each foot with a white marker
		// rendered above the colored quadrant tiles so it is always visible.
		// We compute the current tile purely from each leg's FootAxleWorldLocation, independent of the
		// quadrant assignment used for reach visualization.
		if (UMaterialInterface* BaseMaterial = GEngine->DebugMeshMaterial)
		{
			UMaterialInstanceDynamic* FootHighlightMaterial = UMaterialInstanceDynamic::Create(BaseMaterial, this);
			if (FootHighlightMaterial)
			{
				const FLinearColor WhiteColor = FLinearColor::White;
				FootHighlightMaterial->SetVectorParameterValue(TEXT("Color"), WhiteColor);
				FootHighlightMaterial->SetVectorParameterValue(TEXT("BaseColor"), WhiteColor);
				FootHighlightMaterial->SetVectorParameterValue(TEXT("EmissiveColor"), WhiteColor);
				FootHighlightMaterial->SetVectorParameterValue(TEXT("DiffuseColor"), WhiteColor);

				// Use the same outline mesh but scaled/lifted a bit more so it visually sits on top.
				UStaticMesh* FootTileOutlineMesh = TileMesh ? TileMesh : SphereMesh;

				// Try to get assigned tiles from simulation if calibration walker is in sim
				FHex AssignedTiles[4] = { FHex(), FHex(), FHex(), FHex() };
				bool bHasSimTiles = false;
				if (CalibratingWalker.IsValid() && CalibratingWalker->SimWalkerIndex >= 0 && OrbSimDriver)
				{
					if (USimWorldHex* Sim = OrbSimDriver->GetSim())
					{
						if (CalibratingWalker->SimWalkerIndex < Sim->Walkers.Num())
						{
							const FWalkerState& SimState = Sim->Walkers[CalibratingWalker->SimWalkerIndex];
							AssignedTiles[0] = SimState.DFoot.CurrentTile;  // D
							AssignedTiles[1] = SimState.LFoot.CurrentTile;  // L
							AssignedTiles[2] = SimState.RFoot.CurrentTile;  // R
							AssignedTiles[3] = SimState.UFoot.CurrentTile;  // U
							bHasSimTiles = true;
						}
					}
				}

				for (int32 LegIndex = 0; LegIndex < WalkerLegCalibrations.Num(); ++LegIndex)
				{
					const FWalkerLegCalibrationData& LegData = WalkerLegCalibrations[LegIndex];
					
					FHex FootHex;
					if (bHasSimTiles)
					{
						// Use assigned tile from simulation
						// Map leg index to foot: D=0, L=1, R=2, U=3
						const FString HipName = LegData.HipAxleBone.ToString();
						if (HipName.Contains(TEXT("DHip")))
						{
							FootHex = AssignedTiles[0];
						}
						else if (HipName.Contains(TEXT("LHip")))
						{
							FootHex = AssignedTiles[1];
						}
						else if (HipName.Contains(TEXT("RHip")))
						{
							FootHex = AssignedTiles[2];
						}
						else if (HipName.Contains(TEXT("UHip")))
						{
							FootHex = AssignedTiles[3];
						}
						else
						{
							// Fallback to foot bone location if we can't determine leg
							const FVector& FootWorld = LegData.FootAxleWorldLocation;
							if (!OrbSimDriver->WorldToHex(FVector(FootWorld.X, FootWorld.Y, WalkerBodyBaseWorldLocation.Z), FootHex))
							{
								continue;
							}
						}
					}
					else
					{
						// Fallback: use foot bone location if not in simulation
						const FVector& FootWorld = LegData.FootAxleWorldLocation;
						if (!OrbSimDriver->WorldToHex(FVector(FootWorld.X, FootWorld.Y, WalkerBodyBaseWorldLocation.Z), FootHex))
						{
							continue;
						}
					}

					const FVector FootTileWorld = OrbSimDriver->MapHexToWorld(FootHex);

					UStaticMeshComponent* FootMarkerComp = NewObject<UStaticMeshComponent>(Walker);
					if (FootMarkerComp)
					{
						FootMarkerComp->SetStaticMesh(FootTileOutlineMesh);
						FootMarkerComp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
						FootMarkerComp->SetMobility(EComponentMobility::Movable);
						FootMarkerComp->SetCastShadow(false);
						FootMarkerComp->SetIsVisualizationComponent(true);

						FootMarkerComp->SetMaterial(0, FootHighlightMaterial);

						FootMarkerComp->RegisterComponent();
						FootMarkerComp->AttachToComponent(Walker->GetRootComponent(), FAttachmentTransformRules::KeepWorldTransform);

						const float HighlightScaleMult = 1.35f;
						const float HighlightZOffset = 6.f;

						FootMarkerComp->SetWorldScale3D(FVector(MarkerScale * HighlightScaleMult));
						FootMarkerComp->SetWorldLocation(FootTileWorld + FVector(0.f, 0.f, HighlightZOffset));
						FootMarkerComp->SetVisibility(true, true);

						WalkerQuadrantTileMarkers.Add(FootMarkerComp);
					}
				}
			}
		}
	}

	// Sixth pass: Create knee markers with leg quadrant colors (always visible while calibration is active)
	for (const auto& LegKneePair : LegToKneeBoneMap)
	{
		const int32 LegIndex = LegKneePair.Key;
		const FName& KneeBone = LegKneePair.Value;
		
		// Get knee world position and raise marker a bit above the joint for better visibility
		const FVector KneeWorldPos = BodyComp->GetBoneLocation(KneeBone, EBoneSpaces::WorldSpace) + FVector(0.f, 0.f, 200.f);
		
		UMaterialInstanceDynamic** LegMaterialPtr = LegMaterials.Find(LegIndex);
		if (!LegMaterialPtr || !*LegMaterialPtr)
		{
			continue;
		}
		
		UStaticMeshComponent* KneeMarkerComp = NewObject<UStaticMeshComponent>(Walker);
		if (KneeMarkerComp)
		{
			KneeMarkerComp->SetStaticMesh(SphereMesh);
			KneeMarkerComp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			KneeMarkerComp->SetMobility(EComponentMobility::Movable);
			KneeMarkerComp->SetCastShadow(false);
			KneeMarkerComp->SetIsVisualizationComponent(true);
			
			// Use the leg's quadrant color (after offset applied)
			KneeMarkerComp->SetMaterial(0, *LegMaterialPtr);
			
			KneeMarkerComp->RegisterComponent();
			KneeMarkerComp->AttachToComponent(Walker->GetRootComponent(), FAttachmentTransformRules::KeepWorldTransform);
			KneeMarkerComp->SetWorldScale3D(FVector(KneeMarkerScale));
			KneeMarkerComp->SetWorldLocation(KneeWorldPos);
			KneeMarkerComp->SetVisibility(true, true);
		}
		WalkerQuadrantTileMarkers.Add(KneeMarkerComp);
	}

	UE_LOG(LogTemp, Log, TEXT("GameManager: Updated leg tile quadrant visualization with %d tile markers and %d knee markers"), 
		HexToLegMap.Num(), LegToKneeBoneMap.Num());
}

void AGameManager::ClearLegTileQuadrantVisualization()
{
	for (UStaticMeshComponent* MarkerComp : WalkerQuadrantTileMarkers)
	{
		if (MarkerComp)
		{
			MarkerComp->DestroyComponent();
		}
	}
	WalkerQuadrantTileMarkers.Empty();
}

void AGameManager::LoadWalkerCalibration()
{
	// Load from saved config
	if (SavedWalkerCalibration.StandHeight != 0.f || SavedWalkerCalibration.StartingFootDistance != 0.f)
	{
		DesiredStandHeight = SavedWalkerCalibration.StandHeight;
		StartingFootDistance = SavedWalkerCalibration.StartingFootDistance;
		StepHeight = SavedWalkerCalibration.StepHeight;
		StepRaiseTicks = SavedWalkerCalibration.StepRaiseTicks;
		StepMoveTicks = SavedWalkerCalibration.StepMoveTicks;
		StepLowerTicks = SavedWalkerCalibration.StepLowerTicks;
		ReachDistance = SavedWalkerCalibration.ReachDistance;
		MinimumReachDistance = SavedWalkerCalibration.MinimumReachDistance;
		if (ReachDistance > 0 && MinimumReachDistance > ReachDistance)
		{
			UE_LOG(LogTemp, Warning, TEXT("GameManager: Loaded MinimumReachDistance=%.2f greater than ReachDistance=%d, clamping to ReachDistance when applied"),
				MinimumReachDistance, ReachDistance);
		}
		ActiveFoot = SavedWalkerCalibration.ActiveFoot;
		NQuadrantLeg = SavedWalkerCalibration.NQuadrantLeg;
		EQuadrantLeg = SavedWalkerCalibration.EQuadrantLeg;
		WQuadrantLeg = SavedWalkerCalibration.WQuadrantLeg;
		SQuadrantLeg = SavedWalkerCalibration.SQuadrantLeg;
		TileQuadrantOffset = SavedWalkerCalibration.TileQuadrantOffset;
		// Don't load WalkerOriginalFootPositions or WalkerSittingPose from saved config
		// These will be gathered fresh from the mesh in GatherWalkerCalibrationData()
		// Saved positions are only for reference, not for active calibration
		
		UE_LOG(LogTemp, Log, TEXT("GameManager: Loaded walker calibration settings from config - StandHeight=%.1f, StartingFootDistance=%.1f, TileQuadrantOffset=%d"), 
			DesiredStandHeight, StartingFootDistance, TileQuadrantOffset);
	}
	else
	{
		UE_LOG(LogTemp, Log, TEXT("GameManager: No saved walker calibration found, using defaults"));
	}
}

void AGameManager::SaveWalkerCalibrationToConfig()
{
	// Save current settings to config
	// Save StandHeight (actual applied height) instead of DesiredStandHeight
	SavedWalkerCalibration.StandHeight = StandHeight;
	SavedWalkerCalibration.StartingFootDistance = StartingFootDistance;
	SavedWalkerCalibration.StepHeight = StepHeight;
	SavedWalkerCalibration.StepRaiseTicks = StepRaiseTicks;
	SavedWalkerCalibration.StepMoveTicks = StepMoveTicks;
	SavedWalkerCalibration.StepLowerTicks = StepLowerTicks;
	SavedWalkerCalibration.ReachDistance = ReachDistance;
	SavedWalkerCalibration.MinimumReachDistance = MinimumReachDistance;
	SavedWalkerCalibration.ActiveFoot = ActiveFoot;
	SavedWalkerCalibration.OriginalFootPositions = WalkerOriginalFootPositions;
	SavedWalkerCalibration.SittingPose = WalkerSittingPose;
	// Legacy field; keep zeroed for backwards compatibility
	SavedWalkerCalibration.LegQuadrantOffset = 0;
	SavedWalkerCalibration.NQuadrantLeg = NQuadrantLeg;
	SavedWalkerCalibration.EQuadrantLeg = EQuadrantLeg;
	SavedWalkerCalibration.WQuadrantLeg = WQuadrantLeg;
	SavedWalkerCalibration.SQuadrantLeg = SQuadrantLeg;
	SavedWalkerCalibration.TileQuadrantOffset = TileQuadrantOffset;
	
	// Save to config file
	SaveConfig();
	
	UE_LOG(LogTemp, Log, TEXT("GameManager: Saved walker calibration to config file"));
}

void AGameManager::FootToOuterMostTile()
{
	if (!bWalkerCalibrationActive || !CalibratingWalker.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("GameManager: FootToOuterMostTile called without active calibration"));
		return;
	}

	USkeletalMeshComponent* BodyComp = CalibratingWalker->GetBodyComponent();
	if (!BodyComp || !OrbSimDriver)
	{
		UE_LOG(LogTemp, Warning, TEXT("GameManager: No BodyComp or OrbSimDriver for FootToOuterMostTile"));
		return;
	}

	UAnimInstance* AnimInstance = BodyComp->GetAnimInstance();
	if (!AnimInstance)
	{
		UE_LOG(LogTemp, Warning, TEXT("GameManager: No AnimInstance for FootToOuterMostTile"));
		return;
	}

	// Toggle state
	bFeetAtOuterMost = !bFeetAtOuterMost;

	// Declare FootGoalNames at function scope to avoid duplicate declaration
	const FName FootGoalNames[] = { TEXT("DFootGoal"), TEXT("LFootGoal"), TEXT("RFootGoal"), TEXT("UFootGoal") };

	if (bFeetAtOuterMost)
	{
		// Save current foot positions before moving to outer most
		WalkerFootPositionsBeforeOuterMost.Empty();
		
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
							WalkerFootPositionsBeforeOuterMost.Add(*GoalValue);
						}
					}
				}
			}
		}

		// Get body hex position
		FHex BodyHex = FHex(CalibratingWalker->Q, CalibratingWalker->R);
		const FVector BodyWorldPos = OrbSimDriver->MapHexToWorld(BodyHex);
		
		// Map foot indices to compass quadrants using the per-quadrant leg mapping
		// This determines which leg (D/L/R/U) controls each spatial quadrant (N/E/S/W)
		auto GetLegForQuadrant = [this](const FString& Quadrant) -> int32
		{
			// Map quadrant name to leg index (0=D, 1=L, 2=R, 3=U)
			EWalkerFoot LegFoot;
			if (Quadrant == TEXT("N")) LegFoot = NQuadrantLeg;
			else if (Quadrant == TEXT("E")) LegFoot = EQuadrantLeg;
			else if (Quadrant == TEXT("W")) LegFoot = WQuadrantLeg;
			else if (Quadrant == TEXT("S")) LegFoot = SQuadrantLeg;
			else return -1;
			
			return static_cast<int32>(LegFoot);
		};

		// Find outermost tile in each quadrant
		struct FQuadrantResult
		{
			FHex OuterMostHex;
			int32 MaxDist = -1;
		};
		
		TMap<FString, FQuadrantResult> QuadrantResults;
		QuadrantResults.Add(TEXT("N"), FQuadrantResult());
		QuadrantResults.Add(TEXT("E"), FQuadrantResult());
		QuadrantResults.Add(TEXT("S"), FQuadrantResult());
		QuadrantResults.Add(TEXT("W"), FQuadrantResult());
		
		// Search all tiles within reach distance
		for (int32 q = -ReachDistance; q <= ReachDistance; ++q)
		{
			for (int32 r = -ReachDistance; r <= ReachDistance; ++r)
			{
				FHex TestHex(BodyHex.Q + q, BodyHex.R + r);
				const int32 dq = TestHex.Q - BodyHex.Q;
				const int32 dr = TestHex.R - BodyHex.R;
				const int32 HexDist = FMath::Max(FMath::Max(FMath::Abs(dq), FMath::Abs(dr)), FMath::Abs(dq + dr));
				
				if (HexDist <= ReachDistance && HexDist > 0)
				{
					// Determine which quadrant this tile belongs to
					FString Quadrant;
					if (dq > 0 && dr == 0) Quadrant = TEXT("E");
					else if (dq < 0 && dr == 0) Quadrant = TEXT("W");
					else if (dq == 0 && dr > 0) Quadrant = TEXT("S");
					else if (dq == 0 && dr < 0) Quadrant = TEXT("N");
					else if (FMath::Abs(dq) > FMath::Abs(dr))
						Quadrant = (dq > 0) ? TEXT("E") : TEXT("W");
					else if (FMath::Abs(dr) > FMath::Abs(dq))
						Quadrant = (dr > 0) ? TEXT("S") : TEXT("N");
					else
					{
						if (dq > 0 && dr > 0) Quadrant = TEXT("S");
						else if (dq > 0 && dr < 0) Quadrant = TEXT("E");
						else if (dq < 0 && dr > 0) Quadrant = TEXT("W");
						else Quadrant = TEXT("N");
					}
					
					// Update outermost tile for this quadrant if this tile is farther
					FQuadrantResult& Result = QuadrantResults[Quadrant];
					if (HexDist > Result.MaxDist)
					{
						Result.OuterMostHex = TestHex;
						Result.MaxDist = HexDist;
					}
				}
			}
		}
		
		// Apply outermost positions to feet AND update assigned tiles in simulation
		const FTransform ComponentToWorld = BodyComp->GetComponentToWorld();
		const FRotator ComponentRotation = BodyComp->GetComponentRotation();
		const FTransform ComponentTransformNoScale(ComponentRotation, ComponentToWorld.GetLocation(), FVector(1.f));
		const FTransform WorldToComponentNoScale = ComponentTransformNoScale.Inverse();
		
		// Update assigned tiles in simulation
		if (CalibratingWalker->SimWalkerIndex >= 0 && OrbSimDriver)
		{
			USimWorldHex* Sim = OrbSimDriver->GetSim();
			if (Sim && CalibratingWalker->SimWalkerIndex < Sim->Walkers.Num())
			{
				FWalkerState& SimWalkerState = Sim->Walkers[CalibratingWalker->SimWalkerIndex];
				
				// Update each foot's assigned tile to the outermost tile in its quadrant
				for (const auto& Pair : QuadrantResults)
				{
					const FString& Quadrant = Pair.Key;
					const FQuadrantResult& Result = Pair.Value;
					
					// Get which leg controls this quadrant
					int32 LegIndex = GetLegForQuadrant(Quadrant);
					if (LegIndex < 0 || LegIndex > 3) continue;
					
					// Update assigned tile in simulation
					FFootState* Feet[] = { &SimWalkerState.DFoot, &SimWalkerState.LFoot, &SimWalkerState.RFoot, &SimWalkerState.UFoot };
					FFootState* Foot = Feet[LegIndex];
					Foot->CurrentTile = Result.OuterMostHex;
					Foot->TargetTile = Result.OuterMostHex;
					
					// Also update IK goal
					const FVector OuterMostWorld = OrbSimDriver->MapHexToWorld(Result.OuterMostHex);
					
					if (FProperty* GoalProperty = AnimInstance->GetClass()->FindPropertyByName(FootGoalNames[LegIndex]))
					{
						if (FStructProperty* StructProp = CastField<FStructProperty>(GoalProperty))
						{
							if (StructProp->Struct == TBaseStructure<FVector>::Get())
							{
								FVector* GoalValue = StructProp->ContainerPtrToValuePtr<FVector>(AnimInstance);
								if (GoalValue)
								{
									*GoalValue = WorldToComponentNoScale.TransformPosition(OuterMostWorld);
								}
							}
						}
					}
					
					UE_LOG(LogTemp, Log, TEXT("GameManager: Set foot %d (quadrant %s) assigned tile to outermost (%d,%d) at distance %d"),
						LegIndex, *Quadrant, Result.OuterMostHex.Q, Result.OuterMostHex.R, Result.MaxDist);
				}
			}
		}
		
		// Force animation update
		BodyComp->TickAnimation(0.f, false);
		BodyComp->RefreshBoneTransforms();
		BodyComp->UpdateComponentToWorld();
		
		// Update debug visualization
		UpdateWalkerDebugMeshes(BodyComp);
		
		UE_LOG(LogTemp, Log, TEXT("GameManager: Moved feet to outermost tiles in each quadrant (reach distance = %d)"), ReachDistance);
	}
	else
	{
		// Restore previous foot positions
		if (WalkerFootPositionsBeforeOuterMost.Num() == 4)
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
							if (GoalValue)
							{
								*GoalValue = WalkerFootPositionsBeforeOuterMost[i];
							}
						}
					}
				}
			}
			
			// Also restore assigned tiles in simulation
			if (CalibratingWalker->SimWalkerIndex >= 0 && OrbSimDriver)
			{
				USimWorldHex* Sim = OrbSimDriver->GetSim();
			}
			
			// Force animation update
			BodyComp->TickAnimation(0.f, false);
			BodyComp->RefreshBoneTransforms();
			BodyComp->UpdateComponentToWorld();
			
			// Update debug visualization
			UpdateWalkerDebugMeshes(BodyComp);
			
			UE_LOG(LogTemp, Log, TEXT("GameManager: Restored previous foot positions"));
		}
	}
}

FLinearColor AGameManager::GetLegQuadrantColor(const FString& OriginalQuadrant, int32 LegIndex) const
{
	// Base color mapping for each leg quadrant (consistent colors, no variation)
	static const TMap<FString, FLinearColor> LegBaseColors = {
		{ TEXT("N"), FLinearColor(0.0f, 1.0f, 0.0f) },  // Green (North)
		{ TEXT("E"), FLinearColor(1.0f, 0.0f, 0.0f) },  // Red   (East)
		{ TEXT("W"), FLinearColor(0.0f, 0.0f, 1.0f) },  // Blue  (West)
		{ TEXT("S"), FLinearColor(1.0f, 1.0f, 0.0f) }   // Yellow(South)
	};
	
	const FLinearColor* QuadrantBaseColor = LegBaseColors.Find(OriginalQuadrant);
	if (QuadrantBaseColor)
	{
		return *QuadrantBaseColor;
	}
	
	// Default to white if quadrant not found
	return FLinearColor::White;
}

FVector AGameManager::GetFootAxleWorldLocationForBone(const FName& HipBoneName) const
{
	if (!bWalkerCalibrationActive)
	{
		return FVector::ZeroVector;
	}

	// Find the calibration data for this bone
	for (const FWalkerLegCalibrationData& LegData : WalkerLegCalibrations)
	{
		if (LegData.HipAxleBone == HipBoneName)
		{
			return LegData.FootAxleWorldLocation;
		}
	}

	return FVector::ZeroVector;
}

