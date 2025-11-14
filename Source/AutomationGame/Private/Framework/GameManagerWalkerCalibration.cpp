#include "Framework/GameManager.h"
#include "Actors/BasicWalker.h"
#include "Actors/OrbSimDriver.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "UObject/UnrealType.h"
#include "UObject/Class.h"
#include "Animation/AnimInstance.h"
#include "Sim/HexTypes.h"

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

	CancelWalkerCalibration();

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

	// Work directly with the skeletal mesh component (IK Rig will handle posing)
	WalkerBodyBaseWorldLocation = BodyComp->GetBoneLocation(WalkerBodyBoneName, EBoneSpaces::WorldSpace);
	WalkerBodyActorOffset = Walker->GetActorLocation() - WalkerBodyBaseWorldLocation;
	WalkerCalibrationBaseLocation = Walker->GetActorLocation();

	GatherWalkerCalibrationData(BodyComp);

	if (WalkerLegCalibrations.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("GameManager: No valid walker legs found during calibration."));
		Walker->Destroy();
		WalkerSittingPose.Empty();
		return;
	}

	// If StartingFootDistance is set, update foot positions to snap to grid tiles
	if (StartingFootDistance > 0.f && OrbSimDriver)
	{
		// Get body position in world space (horizontal plane only)
		const FVector BodyWorld2D = FVector(WalkerBodyBaseWorldLocation.X, WalkerBodyBaseWorldLocation.Y, 0.f);
		
		for (FWalkerLegCalibrationData& LegData : WalkerLegCalibrations)
		{
			// Calculate direction from body to hip in horizontal plane
			const FVector HipWorld2D = FVector(LegData.HipWorldLocation.X, LegData.HipWorldLocation.Y, 0.f);
			FVector BodyToHipDir = (HipWorld2D - BodyWorld2D).GetSafeNormal();
			
			// If direction is invalid (body and hip are at same position), use a default direction
			if (BodyToHipDir.IsNearlyZero())
			{
				// Use the component-space forward direction projected to world horizontal plane
				const FTransform ComponentTransform = BodyComp->GetComponentTransform();
				const FVector ForwardComponent = FVector(1.f, 0.f, 0.f);
				const FVector ForwardWorld = ComponentTransform.TransformVector(ForwardComponent);
				BodyToHipDir = FVector(ForwardWorld.X, ForwardWorld.Y, 0.f).GetSafeNormal();
				if (BodyToHipDir.IsNearlyZero())
				{
					BodyToHipDir = FVector(1.f, 0.f, 0.f);
				}
			}
			
			// Calculate target foot position at StartingFootDistance from body
			const FVector TargetFootWorld2D = BodyWorld2D + BodyToHipDir * StartingFootDistance;
			
			// Snap to nearest grid tile
			FHex NearestHex;
			if (OrbSimDriver->WorldToHex(FVector(TargetFootWorld2D.X, TargetFootWorld2D.Y, WalkerBodyBaseWorldLocation.Z), NearestHex))
			{
				// Get the world position of the snapped hex tile
				const FVector SnappedWorld = OrbSimDriver->MapHexToWorld(NearestHex);
				
				// Update foot position (keep original Z coordinate from calibration)
				LegData.FootAxleWorldLocation = FVector(SnappedWorld.X, SnappedWorld.Y, LegData.FootAxleWorldLocation.Z);
				
				UE_LOG(LogTemp, Log, TEXT("GameManager: Foot %s snapped to hex (%d, %d) at distance %.1f"),
					*LegData.HipAxleBone.ToString(), NearestHex.Q, NearestHex.R, StartingFootDistance);
			}
			else
			{
				// If snapping fails, use the calculated position
				LegData.FootAxleWorldLocation = FVector(TargetFootWorld2D.X, TargetFootWorld2D.Y, LegData.FootAxleWorldLocation.Z);
				UE_LOG(LogTemp, Warning, TEXT("GameManager: Failed to snap foot %s to grid, using calculated position"),
					*LegData.HipAxleBone.ToString());
			}
		}
	}

	CalibratingWalker = Walker;

	bWalkerCalibrationActive = true;
	bWalkerStanding = false;
	DesiredStandHeight = 0.f;
	CurrentStandHeight = 0.f;
	WalkerStandAlpha = 0.f;

	// Initialize IK Rig goal positions to current foot positions (may have been updated by StartingFootDistance)
	SetIKRigGoalPositions(BodyComp, 0.f);

	UE_LOG(LogTemp, Log, TEXT("GameManager: Walker calibration started (%d legs)."), WalkerLegCalibrations.Num());

	CreateWalkerDebugMeshes(Walker, BodyComp);
	UpdateWalkerDebugMeshes(BodyComp);
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

	UE_LOG(LogTemp, Log, TEXT("SetStandHeight: Requested=%.1f, Allowed=%.1f, Actual=%.1f"),
		RequestedHeight, AllowedHeight, ActualHeight);

	WalkerStandAlpha = FMath::Clamp(ActualHeight / MaxStandHeight, 0.f, 1.f);

	// Set IK Rig goal positions - the IK Rig will handle the leg posing and body positioning
	// Do NOT move the actor - let the IK Rig move the body bone to match BodyGoal
	SetIKRigGoalPositions(BodyComp, ActualHeight);

	UpdateWalkerDebugMeshes(BodyComp);

	CurrentStandHeight = ActualHeight;
	bWalkerStanding = !FMath::IsNearlyZero(CurrentStandHeight, 1.0f);

	UE_LOG(LogTemp, Log, TEXT("GameManager: Calibrated walker stand height set to %.2f"), CurrentStandHeight);
}

void AGameManager::ToggleStance()
{
	if (!bWalkerCalibrationActive || !CalibratingWalker.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("GameManager: ToggleStance called without an active calibration."));
		return;
	}

	const float MaxStandHeight = 1000.f;
	DesiredStandHeight = bWalkerStanding ? 0.f : MaxStandHeight;
	SetStandHeight();
}

void AGameManager::CancelWalkerCalibration()
{
	if (CalibratingWalker.IsValid())
	{
		CalibratingWalker->Destroy();
		CalibratingWalker.Reset();
	}

	WalkerLegCalibrations.Empty();
	WalkerSittingPose.Empty();
	bWalkerCalibrationActive = false;
	DesiredStandHeight = 0.f;
	CurrentStandHeight = 0.f;
	bWalkerStanding = false;
	WalkerBodyBaseWorldLocation = FVector::ZeroVector;
	WalkerBodyActorOffset = FVector::ZeroVector;
	WalkerStandAlpha = 0.f;
	ClearWalkerDebugMeshes();
}

void AGameManager::GatherWalkerCalibrationData(USkeletalMeshComponent* BodyComp)
{
	WalkerLegCalibrations.Empty();
	WalkerSittingPose.Empty();
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

		WalkerSittingPose.FindOrAdd(Names.HipRotator) = Data.HipRotatorBaseRotation;
		WalkerSittingPose.FindOrAdd(Names.HipAxle) = Data.HipAxleBaseRotation;
		WalkerSittingPose.FindOrAdd(Names.KneeAxle) = Data.KneeAxleBaseRotation;
		WalkerSittingPose.FindOrAdd(Names.FootAxle) = Data.FootAxleBaseRotation;
		WalkerSittingPose.FindOrAdd(Names.FootBone) = Data.FootBoneBaseRotation;
	}
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
					UE_LOG(LogTemp, Log, TEXT("SetIKRigGoalPositions: BodyGoal set to World(%.2f, %.2f, %.2f) Component(%.2f, %.2f, %.2f) on Animation Blueprint"), 
						BodyGoalWorld.X, BodyGoalWorld.Y, BodyGoalWorld.Z,
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

	// Force the animation system to update with the new values
	// Setting values via reflection doesn't automatically trigger updates, so we need to force it
	
	// Mark the animation blueprint as needing an update
	// This tells the animation system to re-evaluate the blueprint
	AnimInstance->BlueprintThreadSafeUpdateAnimation(0.f);
	
	// Force the skeletal mesh component to update its animation
	// This will cause the animation blueprint to run and the IK Rig to solve
	BodyComp->TickAnimation(0.f, false);
	BodyComp->RefreshBoneTransforms();
	BodyComp->UpdateComponentToWorld();
	
	// Also try to force an immediate evaluation of the animation graph
	// This ensures the IK Rig solver runs with the new goal values
	if (BodyComp->GetAnimInstance())
	{
		BodyComp->GetAnimInstance()->NativeUpdateAnimation(0.f);
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

