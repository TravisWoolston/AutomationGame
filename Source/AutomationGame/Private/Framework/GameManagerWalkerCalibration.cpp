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

	bWalkerCalibrationActive = true;
	bWalkerStanding = false;
	CurrentStandHeight = 0.f;
	WalkerStandAlpha = 0.f;

	// Apply StartingFootDistance offset to foot positions
	ApplyStartingFootDistance(BodyComp);

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

	CurrentStandHeight = ActualHeight;
	bWalkerStanding = !FMath::IsNearlyZero(CurrentStandHeight, 1.0f);
	
	UE_LOG(LogTemp, Log, TEXT("GameManager: Stand height updated - Desired=%.1f, Actual=%.1f, Standing=%d"),
		DesiredStandHeight, CurrentStandHeight, bWalkerStanding ? 1 : 0);
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
		CalibratingWalker->Destroy();
		CalibratingWalker.Reset();
	}

	// Clear runtime data only
	WalkerLegCalibrations.Empty();
	bWalkerCalibrationActive = false;
	CurrentStandHeight = 0.f;
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
	
	UE_LOG(LogTemp, Log, TEXT("ApplyStartingFootDistance: Applying offset %.1f to %d legs"), StartingFootDistance, WalkerLegCalibrations.Num());

	// Apply offset from original positions
	for (int32 Index = 0; Index < WalkerLegCalibrations.Num(); ++Index)
	{
		FWalkerLegCalibrationData& LegData = WalkerLegCalibrations[Index];
		const FVector& OriginalPos = WalkerOriginalFootPositions[Index];
		
		// Calculate direction from body to original foot position in horizontal plane
		const FVector BodyWorld2D = FVector(WalkerBodyBaseWorldLocation.X, WalkerBodyBaseWorldLocation.Y, 0.f);
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
		
		// Calculate new position by adding offset along the direction from body to original foot
		const FVector Offset2D = BodyToFootDir * StartingFootDistance;
		const FVector NewFootWorld2D = OriginalFoot2D + Offset2D;
		
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
		// Recalculate the offset position (same logic as ApplyStartingFootDistance)
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
		
		const FVector Offset2D = BodyToFootDir * StartingFootDistance;
		const FVector NewFootWorld2D = OriginalFoot2D + Offset2D;
		
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
	SetIKRigGoalPositions(BodyComp, CurrentStandHeight);
	
	// Update debug meshes
	UpdateWalkerDebugMeshes(BodyComp);
	
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
	// These goal variables should be UPROPERTY(BlueprintReadWrite) FVector properties on the Animation Blueprint
	// so they can be used directly in the IK Rig:
	// - UFootGoal, DFootGoal, LFootGoal, RFootGoal, BodyGoal
	
	// Get the component transform for space conversion
	const FTransform ComponentTransform = BodyComp->GetComponentTransform();
	
	// Get the body goal position (current body location + height offset) in world space
	const FVector BodyGoalWorld = WalkerBodyBaseWorldLocation + FVector(0.f, 0.f, HeightOffset);
	
	// Convert body goal to component space (IK Rig might need component space)
	const FVector BodyGoalComponent = ComponentTransform.InverseTransformPosition(BodyGoalWorld);
	
	// Set foot goal positions to their original world locations (feet stay planted)
	// Map each leg to its corresponding goal:
	// D -> DFootGoal, L -> LFootGoal, R -> RFootGoal, U -> UFootGoal
	
	for (int32 Index = 0; Index < WalkerLegCalibrations.Num(); ++Index)
	{
		const FWalkerLegCalibrationData& LegData = WalkerLegCalibrations[Index];
		const FVector FootGoalWorld = LegData.FootAxleWorldLocation;
		
		// Convert foot goal to component space (IK Rig might need component space)
		const FVector FootGoalComponent = ComponentTransform.InverseTransformPosition(FootGoalWorld);
		
		// Determine which goal this leg corresponds to based on bone names
		FString GoalName;
		const FString HipName = LegData.HipAxleBone.ToString();
		if (HipName.Contains(TEXT("DHip")))
		{
			GoalName = TEXT("DFootGoal");
		}
		else if (HipName.Contains(TEXT("LHip")))
		{
			GoalName = TEXT("LFootGoal");
		}
		else if (HipName.Contains(TEXT("RHip")))
		{
			GoalName = TEXT("RFootGoal");
		}
		else if (HipName.Contains(TEXT("UHip")))
		{
			GoalName = TEXT("UFootGoal");
		}
		
		// Set the goal position on the animation blueprint instance via reflection
		// These should be UPROPERTY(BlueprintReadWrite, Category = "IK Rig") FVector properties on the Animation Blueprint
		// Try both world space and component space - IK Rig typically uses world space, but check your setup
		if (FProperty* GoalProperty = AnimInstance->GetClass()->FindPropertyByName(*GoalName))
		{
			if (FStructProperty* StructProp = CastField<FStructProperty>(GoalProperty))
			{
				if (StructProp->Struct == TBaseStructure<FVector>::Get())
				{
					FVector* GoalValue = StructProp->ContainerPtrToValuePtr<FVector>(AnimInstance);
					if (GoalValue)
					{
						// Use world space (IK Rig goals are typically in world space)
						// If your IK Rig setup uses component space, change this to FootGoalComponent
						*GoalValue = FootGoalWorld;
					}
				}
			}
		}
	}
	
	// Set BodyGoal position on the animation blueprint instance
	// IK Rig goals are typically in world space, but some setups use component space
	if (FProperty* BodyGoalProperty = AnimInstance->GetClass()->FindPropertyByName(TEXT("BodyGoal")))
	{
		if (FStructProperty* StructProp = CastField<FStructProperty>(BodyGoalProperty))
		{
			if (StructProp->Struct == TBaseStructure<FVector>::Get())
			{
				FVector* GoalValue = StructProp->ContainerPtrToValuePtr<FVector>(AnimInstance);
				if (GoalValue)
				{
					// Use world space (IK Rig goals are typically in world space)
					// If your IK Rig setup uses component space, change this to BodyGoalComponent
					*GoalValue = BodyGoalWorld;
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

	// Force the animation system to update with the new values
	// Setting values via reflection doesn't automatically trigger updates, so we need to force it
	// In the editor, we need to explicitly evaluate the animation blueprint
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
	
	if (!bShowLegTileQuadrants || !bWalkerCalibrationActive || !CalibratingWalker.IsValid() || !OrbSimDriver)
	{
		return;
	}

	ABasicWalker* Walker = CalibratingWalker.Get();
	if (!Walker)
	{
		return;
	}

	// Get body hex position
	FHex BodyHex;
	if (!OrbSimDriver->WorldToHex(WalkerBodyBaseWorldLocation, BodyHex))
	{
		return;
	}

	// Base color mapping for each leg quadrant (consistent colors, no variation)
	static const TMap<FString, FLinearColor> LegBaseColors = {
		{ TEXT("D"), FLinearColor(1.0f, 0.0f, 0.0f) },  // Red
		{ TEXT("L"), FLinearColor(0.0f, 1.0f, 0.0f) },  // Green
		{ TEXT("R"), FLinearColor(0.0f, 0.0f, 1.0f) },  // Blue
		{ TEXT("U"), FLinearColor(1.0f, 1.0f, 0.0f) }   // Yellow
	};

	// Get tile size for marker scaling
	const float TileSize = OrbSimDriver->GetTileSize();
	const float MarkerScale = 0.25f; // Scale relative to tile size

	UStaticMesh* SphereMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	if (!SphereMesh)
	{
		UE_LOG(LogTemp, Warning, TEXT("GameManager: Failed to load sphere mesh for quadrant visualization."));
		return;
	}

	// Helper function to calculate angle between two world positions (in radians, XY plane)
	auto CalculateWorldAngle = [](const FVector& From, const FVector& To) -> float
	{
		const FVector2D Dir(To.X - From.X, To.Y - From.Y);
		return FMath::Atan2(Dir.Y, Dir.X);
	};

	// Helper function to normalize angle difference to [0, PI]
	auto NormalizeAngleDiff = [](float Angle1, float Angle2) -> float
	{
		float Diff = FMath::Abs(Angle1 - Angle2);
		if (Diff > PI)
		{
			Diff = 2.f * PI - Diff;
		}
		return Diff;
	};

	// Get body world position
	const FVector BodyWorldPos = OrbSimDriver->MapHexToWorld(BodyHex);

	// For each leg, create a cone-shaped quadrant from body center
	for (int32 LegIndex = 0; LegIndex < WalkerLegCalibrations.Num(); ++LegIndex)
	{
		const FWalkerLegCalibrationData& LegData = WalkerLegCalibrations[LegIndex];
		
		// Determine leg quadrant from bone name
		FString LegQuadrant;
		const FString HipName = LegData.HipAxleBone.ToString();
		if (HipName.StartsWith(TEXT("D")))
		{
			LegQuadrant = TEXT("D");
		}
		else if (HipName.StartsWith(TEXT("L")))
		{
			LegQuadrant = TEXT("L");
		}
		else if (HipName.StartsWith(TEXT("R")))
		{
			LegQuadrant = TEXT("R");
		}
		else if (HipName.StartsWith(TEXT("U")))
		{
			LegQuadrant = TEXT("U");
		}
		else
		{
			continue;
		}

		const FLinearColor* QuadrantBaseColor = LegBaseColors.Find(LegQuadrant);
		if (!QuadrantBaseColor)
		{
			continue;
		}

		// Get foot world position (this defines the cone direction)
		const FVector FootWorldPos = LegData.FootAxleWorldLocation;
		
		// Calculate direction from body to foot in world space (this defines the cone center)
		const float LegDirectionAngle = CalculateWorldAngle(BodyWorldPos, FootWorldPos);
		
		// Cone angle (90 degrees = PI/2 radians half-angle = 180 degrees total cone)
		// This creates a wide cone that covers the leg's reach area
		const float ConeHalfAngle = PI / 2.f; // 90 degrees half-angle
		
		// Find all hexes within ReachDistance from body that are in the cone
		TArray<FHex> ReachableHexes;
		const int32 SearchRadius = ReachDistance + 1; // Search slightly beyond reach distance
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
				
				// Get test hex world position
				const FVector TestWorldPos = OrbSimDriver->MapHexToWorld(TestHex);
				
				// Calculate angle from body to test hex in world space
				const float TestAngle = CalculateWorldAngle(BodyWorldPos, TestWorldPos);
				
				// Calculate angle difference (handle wrap-around)
				const float AngleDiff = NormalizeAngleDiff(TestAngle, LegDirectionAngle);
				
				// Check if within cone
				if (AngleDiff <= ConeHalfAngle)
				{
					ReachableHexes.Add(TestHex);
				}
			}
		}

		// Create visual markers for each reachable tile with consistent leg color
		// Use a single material instance per leg for better performance
		UMaterialInstanceDynamic* LegMaterial = nullptr;
		if (UMaterialInterface* BaseMaterial = GEngine->DebugMeshMaterial)
		{
			LegMaterial = UMaterialInstanceDynamic::Create(BaseMaterial, this);
			if (LegMaterial)
			{
				// Use consistent color for this leg (no variation)
				LegMaterial->SetVectorParameterValue(TEXT("Color"), *QuadrantBaseColor);
				LegMaterial->SetVectorParameterValue(TEXT("BaseColor"), *QuadrantBaseColor);
				LegMaterial->SetVectorParameterValue(TEXT("EmissiveColor"), *QuadrantBaseColor);
				LegMaterial->SetVectorParameterValue(TEXT("DiffuseColor"), *QuadrantBaseColor);
			}
		}

		for (const FHex& ReachableHex : ReachableHexes)
		{
			const FVector TileWorld = OrbSimDriver->MapHexToWorld(ReachableHex);
			
			UStaticMeshComponent* MarkerComp = NewObject<UStaticMeshComponent>(Walker);
			if (MarkerComp)
			{
				MarkerComp->SetStaticMesh(SphereMesh);
				MarkerComp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
				MarkerComp->SetMobility(EComponentMobility::Movable);
				MarkerComp->SetCastShadow(false);
				MarkerComp->SetIsVisualizationComponent(true);
				
				// Use the leg's consistent material
				if (LegMaterial)
				{
					MarkerComp->SetMaterial(0, LegMaterial);
				}
				
				MarkerComp->RegisterComponent();
				MarkerComp->AttachToComponent(Walker->GetRootComponent(), FAttachmentTransformRules::KeepWorldTransform);
				MarkerComp->SetWorldScale3D(FVector(MarkerScale));
				MarkerComp->SetWorldLocation(TileWorld + FVector(0.f, 0.f, 5.f)); // Slightly above ground
				MarkerComp->SetVisibility(true, true);
			}
			WalkerQuadrantTileMarkers.Add(MarkerComp);
		}
	}

	UE_LOG(LogTemp, Log, TEXT("GameManager: Updated leg tile quadrant visualization with %d markers"), WalkerQuadrantTileMarkers.Num());
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
		ActiveFoot = SavedWalkerCalibration.ActiveFoot;
		// Don't load WalkerOriginalFootPositions or WalkerSittingPose from saved config
		// These will be gathered fresh from the mesh in GatherWalkerCalibrationData()
		// Saved positions are only for reference, not for active calibration
		
		UE_LOG(LogTemp, Log, TEXT("GameManager: Loaded walker calibration settings from config - StandHeight=%.1f, StartingFootDistance=%.1f"), 
			DesiredStandHeight, StartingFootDistance);
	}
	else
	{
		UE_LOG(LogTemp, Log, TEXT("GameManager: No saved walker calibration found, using defaults"));
	}
}

void AGameManager::SaveWalkerCalibrationToConfig()
{
	// Save current settings to config
	SavedWalkerCalibration.StandHeight = DesiredStandHeight;
	SavedWalkerCalibration.StartingFootDistance = StartingFootDistance;
	SavedWalkerCalibration.StepHeight = StepHeight;
	SavedWalkerCalibration.StepRaiseTicks = StepRaiseTicks;
	SavedWalkerCalibration.StepMoveTicks = StepMoveTicks;
	SavedWalkerCalibration.StepLowerTicks = StepLowerTicks;
	SavedWalkerCalibration.ReachDistance = ReachDistance;
	SavedWalkerCalibration.ActiveFoot = ActiveFoot;
	SavedWalkerCalibration.OriginalFootPositions = WalkerOriginalFootPositions;
	SavedWalkerCalibration.SittingPose = WalkerSittingPose;
	
	// Save to config file
	SaveConfig();
	
	UE_LOG(LogTemp, Log, TEXT("GameManager: Saved walker calibration to config file"));
}

