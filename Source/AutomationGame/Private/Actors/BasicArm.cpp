#include "Actors/BasicArm.h"
#include "Actors/OrbSimDriver.h"
#include "Framework/GameTickManager.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/World.h"
#include "AGLogChannels.h"
#include "Sim/SimWorldHex.h"
#include "DrawDebugHelpers.h"
#include "Rendering/OrbRenderer.h"
#include "EngineUtils.h"
#include "Data/OrbMaterialManager.h"

static inline float NormDeg(float D) { return FMath::UnwindDegrees(D); }
namespace { inline int32 Mod6(int32 V) { return (V % 6 + 6) % 6; } }

static const TCHAR* CmdName(EArmCmd C)
{
	switch (C)
	{
	case EArmCmd::NoOp:      return TEXT("NoOp");
	case EArmCmd::Grab:      return TEXT("Grab");
	case EArmCmd::Drop:      return TEXT("Drop");
	case EArmCmd::RotateCW:  return TEXT("RotateCW");
	case EArmCmd::RotateCCW: return TEXT("RotateCCW");
	case EArmCmd::Wait:      return TEXT("Wait");
	case EArmCmd::Reset:         return TEXT("Reset");
	case EArmCmd::RotateHeldCW:  return TEXT("RotateHeldCW");
	case EArmCmd::RotateHeldCCW: return TEXT("RotateHeldCCW");
	case EArmCmd::WaitForBond:   return TEXT("WaitForBond");
	default:                     return TEXT("<?>");
	}
}

ABasicArm::ABasicArm()
{
	PrimaryActorTick.bCanEverTick = true;   // we interpolate visuals in Tick

	SimArmIndex = -1;                // Initialize to invalid index to prevent accidental overwrites
	LastHeldItemId = 0;              // NEW

	USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("ArmRoot"));
	SetRootComponent(Root);

	BaseComp = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("ArmBase"));
	BaseComp->SetupAttachment(Root);
	BaseComp->SetMobility(EComponentMobility::Movable);

	HeldOrbComp = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("HeldOrbPreview"));
	HeldOrbComp->SetupAttachment(BaseComp); // Attach directly to BaseComp
	HeldOrbComp->SetMobility(EComponentMobility::Movable);
	HeldOrbComp->SetVisibility(false, true);

	GripPivotComp = CreateDefaultSubobject<USceneComponent>(TEXT("GripPivot"));
	GripPivotComp->SetupAttachment(BaseComp);
}

void ABasicArm::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	EnsureComps();
	RefreshMeshes();

	CurrYawDeg = DirToYaw(FacingDir);
	LastYawDeg = CurrYawDeg;                 // lock start == target
	RefreshPose(1.f);
}

void ABasicArm::BeginPlay()
{
	Super::BeginPlay();

	EnsureComps();
	RefreshMeshes();

	StartFacingDir = FacingDir;

	// Initialize ReachTiles from StartingReach if not already set
	if (StartingReach > 0)
	{
		ReachTiles = FMath::Clamp(StartingReach, 1, 6);
	}

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
		UE_LOG(LogAGArm, Warning, TEXT("BasicArm '%s': No Sim found; Grab/Drop will be visual only."), *GetName());
	}

	// hook shared fixed tick
	if (AGameTickManager* M = AGameTickManager::Get(this))
	{
		M->OnFixedTick.RemoveDynamic(this, &ABasicArm::HandleFixedTickSeconds);
		M->OnFixedTick.AddDynamic(this, &ABasicArm::HandleFixedTickSeconds);
		UE_LOG(LogAGArm, Log, TEXT("BasicArm '%s': Bound to GameTickManager '%s'"),
			*GetName(), *M->GetName()); // <— add this line
	}
	else
	{
		UE_LOG(LogAGArm, Warning, TEXT("BasicArm '%s': No GameTickManager; will not step."), *GetName());
	}

	UE_LOG(LogAGArm, Log, TEXT("BasicArm '%s' BeginPlay: Q=%d R=%d Facing=%d Driver=%s"),
		*GetName(), Q, R, (int32)FacingDir, Driver ? *Driver->GetName() : TEXT("null"));

	CurrYawDeg = DirToYaw(FacingDir);
	LastYawDeg = CurrYawDeg;
	RefreshPose(1.f);

	PushRuntimePropertiesToSim(); // NEW
}

void ABasicArm::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	// Sync all state from simulation immediately
	if (Sim && SimArmIndex >= 0 && SimArmIndex < Sim->Arms.Num())
	{
		const FArmState& SimState = Sim->Arms[SimArmIndex];

		// Update orientation and reach immediately
		FacingDir = static_cast<uint8>(SimState.Facing);
		CurrYawDeg = DirToYaw(FacingDir);

		if (ReachTiles != SimState.ArmLen)
		{
			ReachTiles = SimState.ArmLen;
		}

		// Sync wait state and program counter from simulation
		WaitLeft = SimState.WaitCounter;
		PC = SimState.PC;

		// Update holding state + item payload immediately when simulation state changes
		bool bWasHolding = bHolding;
		bHolding = SimState.bHolding;

		if (bHolding)
		{
			HeldOrb = SimState.Held;
		}
		else
		{
			HeldOrb = FItem();
		}

		// Update visual immediately when holding state or cluster composition changes
		if (bWasHolding != bHolding || (bHolding && HeldOrb.Orbs.Num() != LastHeldOrbCount))
		{
			UpdateHeldOrbVisual();
			LastHeldOrbCount = bHolding ? HeldOrb.Orbs.Num() : 0;
		}
	}

	// Advance StepAlpha for interpolation with improved robustness
	float PreviousStepAlpha = StepAlpha;
	if (StepSecondsCur > 0.f)
	{
		// Clamp DeltaSeconds to prevent extreme values during editor pause/focus
		float ClampedDeltaSeconds = FMath::Clamp(DeltaSeconds, 0.f, StepSecondsCur * 2.f);
		StepAlpha = FMath::Clamp(StepAlpha + ClampedDeltaSeconds / StepSecondsCur, 0.f, 1.f);
	}
	else
	{
		StepAlpha = 1.f;
	}

	// Check for rotation completion notifications with improved robustness
	// This handles editor pause/focus issues by using more reliable detection
	if (StepAlpha >= 0.95f && PreviousStepAlpha < 0.95f)
	{
		// Rotation is close enough to target - notify the simulation for bond formation
		if (Sim && SimArmIndex >= 0)
		{
			Sim->OnArmRotationComplete(SimArmIndex);
		}
	}

	// Robust rotation completion detection - use a more reliable threshold
	// This prevents issues during performance dips where StepAlpha might not reach exactly 1.0f
	if (StepAlpha >= 0.99f && PreviousStepAlpha < 0.99f)
	{
		// Rotation is effectively complete - notify the simulation for unbond processing
		if (Sim && SimArmIndex >= 0)
		{
			Sim->OnArmRotationFullyComplete(SimArmIndex);
		}
	}

	// Additional robustness: If we're at 100% but haven't notified yet, do it now
	// This handles cases where the editor was paused during the transition
	if (StepAlpha >= 1.0f && Sim && SimArmIndex >= 0)
	{
		// Check if we need to notify for unbond processing
		// This is idempotent, so it's safe to call multiple times
		Sim->OnArmRotationFullyComplete(SimArmIndex);
	}

	RefreshPose(StepAlpha);
}

void ABasicArm::HandleFixedTickSeconds(float StepSeconds)
{
	StepSecondsCur = StepSeconds;

	// FINALIZE current rendered yaw as starting point for new step
	const float RenderedYaw =
		NormDeg(LastYawDeg + FMath::FindDeltaAngleDegrees(LastYawDeg, CurrYawDeg) * StepAlpha);
	LastYawDeg = RenderedYaw;

	// Reset StepAlpha for new animation cycle
	StepAlpha = 0.f;
	RefreshPose(0.f);
}

void ABasicArm::EnsureComps()
{
	if (!RootComponent)
	{
		USceneComponent* Root = NewObject<USceneComponent>(this, TEXT("ArmRoot_Runtime"));
		SetRootComponent(Root);
		Root->RegisterComponent();
	}
	if (!BaseComp)
	{
		BaseComp = NewObject<UStaticMeshComponent>(this, TEXT("ArmBase"));
		BaseComp->SetupAttachment(GetRootComponent());
		BaseComp->RegisterComponent();
	}
	if (!HeldOrbComp)
	{
		HeldOrbComp = NewObject<UStaticMeshComponent>(this, TEXT("HeldOrbPreview"));
		HeldOrbComp->SetupAttachment(BaseComp); // Attach directly to BaseComp
		HeldOrbComp->RegisterComponent();
		HeldOrbComp->SetVisibility(false, true);
	}
	if (!GripPivotComp)
	{
		GripPivotComp = NewObject<USceneComponent>(this, TEXT("GripPivot"));
		GripPivotComp->SetupAttachment(BaseComp);
		GripPivotComp->RegisterComponent();
	}
	// Set the local rotation offset once during initialization
	GripPivotComp->SetRelativeRotation(GripPivotLocalRotation);
}

void ABasicArm::RefreshMeshes()
{
	if (BaseComp) { BaseComp->SetStaticMesh(BaseMesh); BaseComp->SetRelativeScale3D(BaseScale); }

	if (HeldOrbComp)
	{
		// Get mesh from centralized system
		UStaticMesh* CentralizedMesh = nullptr;
		if (UOrbMaterialManager* MaterialManager = UOrbMaterialManager::FindInWorld(this))
		{
			CentralizedMesh = MaterialManager->GetOrbMesh();
		}

		HeldOrbComp->SetStaticMesh(CentralizedMesh);
		HeldOrbComp->SetRelativeScale3D(HeldOrbScale);
		HeldOrbComp->SetVisibility(bHolding && CentralizedMesh != nullptr, true);
	}
}

FVector ABasicArm::HexToWorld(const FHex& H) const
{
	if (!Driver) return GetActorLocation();

	const AActor* Anchor = Driver->GetGridAnchor();
	const FVector Origin = Anchor ? Anchor->GetActorLocation() : FVector::ZeroVector;
	const float Tile = Driver->GetTileSize();
	const EHexOrientation Ori = Driver->GetGridOrientation();
	const FVector2D XY = HexCoord::AxialToXY(FHexAxial(H.Q, H.R), Tile, Ori);
	const FVector2D RotXY = HexCoord::Rotate2D(XY, Driver->GetGridYaw());
	return Origin + FVector(RotXY.X, RotXY.Y, 0.f);
}

float ABasicArm::DirToYaw(uint8 Dir) const
{
	const float GridYaw = Driver ? Driver->GetGridYaw() : 0.f;
	return NormDeg(GridYaw - 60.f * float(Dir % 6));
}

void ABasicArm::RefreshPose(float InAlpha)
{
	// Use linear interpolation for consistent movement speed
	float EasedAlpha = InAlpha;

	const FVector BaseWorld = HexToWorld(FHex(Q, R)) + FVector(0, 0, BaseZ);

	const float StartYaw = NormDeg(LastYawDeg);
	const float TargetYaw = NormDeg(CurrYawDeg);
	const float Delta = FMath::FindDeltaAngleDegrees(StartYaw, TargetYaw);
	const float Yaw = NormDeg(StartYaw + Delta * EasedAlpha);

	if (bDebugLogs)
	{
		UE_LOG(LogAGArm, Verbose, TEXT("RefreshPose: Q=%d R=%d LastYaw=%.2f CurrYaw=%.2f StepAlpha=%.2f EasedAlpha=%.2f Delta=%.2f Yaw=%.2f"),
			Q, R, StartYaw, TargetYaw, InAlpha, EasedAlpha, Delta, Yaw);
	}

	const FRotator BaseRot(0.f, Yaw + BaseYawOffsetDeg, 0.f);

	if (BaseComp)
	{
		BaseComp->SetWorldLocation(BaseWorld);
		BaseComp->SetWorldRotation(BaseRot);
	}

	// --- Use arm rotation for smooth movement, but target tile center as reference ---
	FVector HandWorld = BaseWorld; // fallback
	FVector InterpolatedHandWorld = BaseWorld;
	{
		// Calculate the arm's target hex (where it's reaching)
		const FHex TargetHex = ComputeTargetHex();
		const FVector TargetWorld = HexToWorld(TargetHex);

		// Use the arm's interpolated rotation to calculate grip direction
		const float GripYawRad = FMath::DegreesToRadians(Yaw);
		const FVector GripDirection = FVector(FMath::Cos(GripYawRad), FMath::Sin(GripYawRad), 0.f);

		// Use the arm's reach distance (based on ReachTiles) instead of actual distance
		const float Tile = (Driver ? Driver->GetTileSize() : 100.f);
		const float ReachDistance = Tile * ReachTiles;

		// Use the arm's rotation with its configured reach distance
		const FVector BaseGripPos = BaseWorld + GripDirection * ReachDistance + FVector(0, 0, GripZOffset);

		// Apply grip location offset relative to the hand's orientation
		const FVector HandForward = GripDirection;
		const FVector HandRight = FVector(-HandForward.Y, HandForward.X, 0.f);
		const FVector HandUp = FVector(0.f, 0.f, 1.f);
		const FVector RelativeOffset = HandForward * GripLocationOffset.X + HandRight * GripLocationOffset.Y + HandUp * GripLocationOffset.Z;
		const FVector FinalGripPos = BaseGripPos + RelativeOffset;

		// If holding a cluster, adjust grip position to account for which orb was grabbed
		if (bHolding && HeldOrb.bIsCluster && HeldOrb.Orbs.Num() > 0)
		{
			// Find the grabbed orb (the one at offset 0,0 - this is the one the arm is holding)
			const FOrbData* GrabbedOrb = nullptr;
			for (const FOrbData& Orb : HeldOrb.Orbs)
			{
				if (Orb.dQ == 0 && Orb.dR == 0)
				{
					GrabbedOrb = &Orb;
					break;
				}
			}

			if (GrabbedOrb)
			{
				// Calculate the offset of the grabbed orb from the cluster center
				const EHexOrientation GridOrientation = (Driver ? Driver->GetGridOrientation() : EHexOrientation::PointyTop);
				const FVector2D HexOffset = HexCoord::AxialToXY(FHexAxial(GrabbedOrb->dQ, GrabbedOrb->dR), Tile, GridOrientation);
				FVector2D OrbOffset = HexOffset * ClusterOffsetScale;

				// Apply rotation based on GrabFacing to make offset relative to arm direction
				if (HeldOrb.GrabFacing >= 0)
				{
					const float GrabYawDeg = 60.f * (HeldOrb.GrabFacing % 6);
					const float YawRad = FMath::DegreesToRadians(GrabYawDeg);
					const float CosA = FMath::Cos(YawRad);
					const float SinA = FMath::Sin(YawRad);
					const float RotX = OrbOffset.X * CosA - OrbOffset.Y * SinA;
					const float RotY = OrbOffset.X * SinA + OrbOffset.Y * CosA;
					OrbOffset = FVector2D(RotX, RotY);
				}

				// Adjust grip position so the grabbed orb is at the grip point
				InterpolatedHandWorld = FinalGripPos - FVector(OrbOffset.X, OrbOffset.Y, 0.f);
			}
			else
			{
				InterpolatedHandWorld = FinalGripPos;
			}
		}
		else
		{
			InterpolatedHandWorld = FinalGripPos;
		}

		HandWorld = InterpolatedHandWorld;
	}

	// Keep GripPivotComp at exact hand world position (unless using a socket)
	if (GripPivotComp && !bUseGripSocket)
	{
		// Use the interpolated hand world position for smooth movement
		GripPivotComp->SetWorldLocation(InterpolatedHandWorld);

		// Apply the full rotation to match the arm
		const FRotator WorldRotation = FRotator(0.f, Yaw + BaseYawOffsetDeg, 0.f) + GripPivotLocalRotation;
		GripPivotComp->SetWorldRotation(WorldRotation);
	}

	// Optional debug draw
	if (bDebugHandLocation && GetWorld())
	{
		const FVector Start = BaseComp ? BaseComp->GetComponentLocation() : GetActorLocation();
		DrawDebugLine(GetWorld(), Start, InterpolatedHandWorld, FColor::Green, false, 0.f, 0, 2.f);
		DrawDebugSphere(GetWorld(), InterpolatedHandWorld, 6.f, 12, FColor::Yellow, false, 0.f);
	}

	// Debug lines for arm reach and target
	if (bDebugLines && GetWorld())
	{
		const FHex TargetHex = ComputeTargetHex();
		const FVector TargetWorld = HexToWorld(TargetHex);

		// Draw line from arm base to target hex
		DrawDebugLine(GetWorld(), BaseWorld, TargetWorld, FColor::Red, false, 0.f, 0, 2.f);

		// Draw line from arm base to current hand position
		DrawDebugLine(GetWorld(), BaseWorld, InterpolatedHandWorld, FColor::Orange, false, 0.f, 0, 1.f);

		// Draw hex outline at target
		const float Tile = Driver ? Driver->GetTileSize() : 100.f;
		DrawDebugCircle(GetWorld(), TargetWorld, Tile * 0.5f, 6, FColor::Red, false, 0.f, 0, 2.f, FVector(1, 0, 0), FVector(0, 1, 0), false);
	}

	// Opus Magnum approach: cluster rotates naturally with the arm
	// No per-tick updates needed - the attachment system handles rotation

	// Debug: Draw spheres showing simulation positions of held cluster orbs
	if (bDebugSpheres && bHolding && HeldOrb.bIsCluster && GetWorld())
	{
		const float Tile = (Driver ? Driver->GetTileSize() : 100.f);
		const EHexOrientation GridOrientation = (Driver ? Driver->GetGridOrientation() : EHexOrientation::PointyTop);
		const float GridYaw = Driver ? Driver->GetGridYaw() : 0.f;
		const AActor* Anchor = Driver ? Driver->GetGridAnchor() : nullptr;
		const FVector AnchorLoc = Anchor ? Anchor->GetActorLocation() : FVector::ZeroVector;

		// Calculate the arm's current target hex (where it's reaching)
		const FHex TargetHex = ComputeTargetHex();
		const FVector TargetWorld = HexToWorld(TargetHex);

		// Draw debug spheres for each sub-orb in the cluster
		for (int32 i = 0; i < HeldOrb.Orbs.Num(); i++)
		{
			const FOrbData& Sub = HeldOrb.Orbs[i];

			// Calculate the simulation position of this sub-orb
			// First, get the base position from the target hex
			const FHex SubOrbHex = FHex(TargetHex.Q + Sub.dQ, TargetHex.R + Sub.dR);
			const FVector SubOrbWorld = HexToWorld(SubOrbHex);

			// Apply arm rotation to the sub-orb position relative to the target hex
			FVector RotatedSubOrbWorld = SubOrbWorld;
			if (Sub.dQ != 0 || Sub.dR != 0) // Don't rotate the grabbed orb (it's at the target)
			{
				// Calculate the offset from the target hex
				const FVector2D HexOffset = HexCoord::AxialToXY(FHexAxial(Sub.dQ, Sub.dR), Tile, GridOrientation);
				const FVector2D RotatedOffset = HexCoord::Rotate2D(HexOffset, GridYaw);

				// Apply the arm's current rotation to this offset
				const float ArmYawRad = FMath::DegreesToRadians(Yaw);
				const float CosA = FMath::Cos(ArmYawRad);
				const float SinA = FMath::Sin(ArmYawRad);
				const float RotX = RotatedOffset.X * CosA - RotatedOffset.Y * SinA;
				const float RotY = RotatedOffset.X * SinA + RotatedOffset.Y * CosA;

				// Position relative to the target hex center
				RotatedSubOrbWorld = TargetWorld + FVector(RotX, RotY, 0.f);
			}

			// Color code: grabbed orb (dQ=0, dR=0) in green, others in blue
			const FColor DebugColor = (Sub.dQ == 0 && Sub.dR == 0) ? FColor::Green : FColor::Blue;
			const float SphereSize = (Sub.dQ == 0 && Sub.dR == 0) ? 8.f : 6.f;

			DrawDebugSphere(GetWorld(), RotatedSubOrbWorld, SphereSize, 12, DebugColor, false, 0.f, 0, 2.f);

			// Draw a line from the arm base to each sub-orb for clarity
			DrawDebugLine(GetWorld(), BaseWorld, RotatedSubOrbWorld, DebugColor, false, 0.f, 0, 1.f);

			// Draw text showing the sub-orb's dQ, dR values
			DrawDebugString(GetWorld(), RotatedSubOrbWorld + FVector(0, 0, 15.f),
				FString::Printf(TEXT("dQ:%d dR:%d"), Sub.dQ, Sub.dR),
				nullptr, DebugColor, 0.f, true, 1.2f);
		}

		// Draw the target hex center in yellow
		// DrawDebugSphere(GetWorld(), TargetWorld, 10.f, 12, FColor::Yellow, false, 0.f, 0, 3.f);
		// DrawDebugString(GetWorld(), TargetWorld + FVector(0, 0, 20.f), 
		// 	FString::Printf(TEXT("Target Hex (%d,%d)"), TargetHex.Q, TargetHex.R), 
		// 	nullptr, FColor::Yellow, 0.f, true, 1.2f);
	}
}

void ABasicArm::SetProgram(const TArray<FArmInstr>& InProgram, bool bLoop /*= true*/)
{
	UE_LOG(LogAGArm, Warning, TEXT("SetProgram called on Arm '%s' (SimIndex=%d) with %d instructions"),
		*GetName(), SimArmIndex, InProgram.Num());

	Program = InProgram;
	bLoopProgram = bLoop;
	PC = 0;
	WaitLeft = 0;
	PushProgramToSim(); // NEW
}

TArray<FOp> ABasicArm::BuildOpsFromProgram(const TArray<FArmInstr>& InProgram) const
{
	TArray<FOp> Out;
	if (InProgram.Num() == 0)
	{
		Out.Add({ ECmd::Wait, 1 }); // harmless idle
		return Out;
	}
	for (const FArmInstr& I : InProgram)
	{
		switch (I.Cmd)
		{
		case EArmCmd::NoOp:      Out.Add({ ECmd::Wait, 1 }); break;
		case EArmCmd::Grab:      Out.Add({ ECmd::Grab, 0 }); break;
		case EArmCmd::Drop:      Out.Add({ ECmd::Drop, 0 }); break;
		case EArmCmd::RotateCW:  Out.Add({ ECmd::RotateR, (uint8)FMath::Max(1, I.Arg) }); break;
		case EArmCmd::RotateCCW: Out.Add({ ECmd::RotateL, (uint8)FMath::Max(1, I.Arg) }); break;
		case EArmCmd::Wait:      Out.Add({ ECmd::Wait, (uint8)FMath::Clamp(I.Arg, 0, 255) }); break;
		case EArmCmd::Reset:     Out.Add({ ECmd::Reset, 0 }); break;
		case EArmCmd::Extend:        Out.Add({ ECmd::Extend,  (uint8)FMath::Clamp(I.Arg == 0 ? 1 : I.Arg, 0, 255) }); break;   // NEW
		case EArmCmd::Retract:       Out.Add({ ECmd::Retract, (uint8)FMath::Clamp(I.Arg == 0 ? 1 : I.Arg, 0, 255) }); break;   // NEW
		case EArmCmd::RotateHeldCW:  Out.Add({ ECmd::RotateHeldR, (uint8)FMath::Max(1, I.Arg) }); break;   // NEW
		case EArmCmd::RotateHeldCCW: Out.Add({ ECmd::RotateHeldL, (uint8)FMath::Max(1, I.Arg) }); break;   // NEW
		case EArmCmd::WaitForBond:   Out.Add({ ECmd::WaitForBond, 0 }); break;   // NEW
		default:                     Out.Add({ ECmd::Wait, 1 }); break;
		}
	}
	return Out;
}

// NEW: push actor-side program / facing into sim
void ABasicArm::PushProgramToSim()
{
	if (!Sim || SimArmIndex < 0 || SimArmIndex >= Sim->Arms.Num())
	{
		UE_LOG(LogAGArm, Warning, TEXT("PushProgramToSim: Arm '%s' has invalid SimArmIndex=%d (Sim has %d arms)"),
			*GetName(), SimArmIndex, Sim ? Sim->Arms.Num() : -1);
		return;
	}

	FArmState& S = Sim->Arms[SimArmIndex];

	// Check if we're in the middle of executing temporary Reset operations
	// If the tape has more operations than what BuildOpsFromProgram would generate,
	// it means we have temporary operations that shouldn't be overwritten
	TArray<FOp> NewTape = BuildOpsFromProgram(Program);
	if (S.Tape.Num() > NewTape.Num() && S.PC > 0)
	{
		// We're likely executing temporary Reset operations - don't overwrite!
		UE_LOG(LogAGArm, Warning, TEXT("PushProgramToSim: Arm '%s' (SimIndex=%d) skipping update - temporary ops in progress (Tape=%d, Expected=%d)"),
			*GetName(), SimArmIndex, S.Tape.Num(), NewTape.Num());
		return;
	}

	// Log what we're about to replace
	UE_LOG(LogAGArm, Warning, TEXT("PushProgramToSim: Arm '%s' (SimIndex=%d) updating tape from %d ops to %d ops"),
		*GetName(), SimArmIndex, S.Tape.Num(), NewTape.Num());

	S.Tape = NewTape;
	S.PC = 0;
	S.Facing = static_cast<EHexDir>(FacingDir);

	// Debug: Log the converted program
	UE_LOG(LogAGArm, Log, TEXT("Arm '%s': PushProgramToSim called - Converting %d instructions to %d ops"),
		*GetName(), Program.Num(), S.Tape.Num());

	// Always log tape contents after Reset expansion for debugging
	UE_LOG(LogAGArm, Log, TEXT("Arm '%s': Program pushed (TapeLen=%d)"), *GetName(), S.Tape.Num());
	for (int32 i = 0; i < S.Tape.Num(); i++)
	{
		const FOp& Op = S.Tape[i];
		FString CmdName = TEXT("Unknown");
		switch (Op.Cmd)
		{
		case ECmd::Wait: CmdName = TEXT("Wait"); break;
		case ECmd::Grab: CmdName = TEXT("Grab"); break;
		case ECmd::Drop: CmdName = TEXT("Drop"); break;
		case ECmd::RotateR: CmdName = TEXT("RotateR"); break;
		case ECmd::RotateL: CmdName = TEXT("RotateL"); break;
		case ECmd::Extend: CmdName = TEXT("Extend"); break;
		case ECmd::Retract: CmdName = TEXT("Retract"); break;
		case ECmd::Reset: CmdName = TEXT("Reset"); break;
		case ECmd::RotateHeldL: CmdName = TEXT("RotateHeldL"); break;
		case ECmd::RotateHeldR: CmdName = TEXT("RotateHeldR"); break;
		case ECmd::WaitForBond: CmdName = TEXT("WaitForBond"); break;
		}
		UE_LOG(LogAGArm, Log, TEXT("  [%d]: %s %d"), i, *CmdName, Op.Arg);
	}

	UE_LOG(LogAGArm, Verbose, TEXT("Arm '%s': Program pushed (TapeLen=%d Holding(sim)=%d)"),
		*GetName(), S.Tape.Num(), S.bHolding ? 1 : 0);
}

void ABasicArm::PushRuntimePropertiesToSim()
{
	if (!Sim || SimArmIndex < 0 || SimArmIndex >= Sim->Arms.Num()) return;
	FArmState& S = Sim->Arms[SimArmIndex];
	S.Facing = static_cast<EHexDir>(FacingDir);
	S.ArmLen = FMath::Clamp(ReachTiles, 1, 3);
	S.HomeArmLen = FMath::Clamp(S.HomeArmLen, 1, 3); // keep bounds
	UE_LOG(LogAGArm, Verbose, TEXT("Arm '%s': Runtime props pushed (Facing=%d ArmLen=%d)"),
		*GetName(), (int32)S.Facing, S.ArmLen);
}

void ABasicArm::RebuildTapeFromProgram()
{
	PushProgramToSim();
}

#if WITH_EDITOR
void ABasicArm::PostEditChangeProperty(FPropertyChangedEvent& Event)
{
	Super::PostEditChangeProperty(Event);

	if (HasAnyFlags(RF_ClassDefaultObject)) return;
	if (!GIsPlayInEditorWorld) return; // only care while PIE is running

	const FName PropName = Event.Property ? Event.Property->GetFName() : NAME_None;
	const FName MemberName = Event.MemberProperty ? Event.MemberProperty->GetFName() : NAME_None;
	const bool bProgramMemberChanged =
		(MemberName == GET_MEMBER_NAME_CHECKED(ABasicArm, Program));

	const bool bProgramElementFieldChanged =
		(PropName == GET_MEMBER_NAME_CHECKED(FArmInstr, Cmd)) ||
		(PropName == GET_MEMBER_NAME_CHECKED(FArmInstr, Arg));

	const bool bLoopChanged =
		(MemberName == GET_MEMBER_NAME_CHECKED(ABasicArm, bLoopProgram));

	const bool bReachOrFacingChanged =
		(MemberName == GET_MEMBER_NAME_CHECKED(ABasicArm, ReachTiles)) ||
		(MemberName == GET_MEMBER_NAME_CHECKED(ABasicArm, FacingDir));

	const bool bStartingRotationChanged =
		(MemberName == GET_MEMBER_NAME_CHECKED(ABasicArm, StartingRotation));

	const bool bStartingReachChanged =
		(MemberName == GET_MEMBER_NAME_CHECKED(ABasicArm, StartingReach));

	// Any structural change to the array (add/remove/duplicate/clear) also fires MemberProperty == Program
	const bool bArrayStructuralChange =
		bProgramMemberChanged &&
		(Event.ChangeType == EPropertyChangeType::ArrayAdd ||
			Event.ChangeType == EPropertyChangeType::ArrayRemove ||
			Event.ChangeType == EPropertyChangeType::ArrayClear ||
			Event.ChangeType == EPropertyChangeType::Duplicate ||
			Event.ChangeType == EPropertyChangeType::ValueSet);

	if (bProgramMemberChanged || bProgramElementFieldChanged || bArrayStructuralChange)
	{
		PushProgramToSim();
		UE_LOG(LogAGArm, Verbose, TEXT("Arm '%s': Program edit -> tape rebuilt (Len=%d ChangeType=%d)"),
			*GetName(), Program.Num(), (int32)Event.ChangeType);
	}
	else if (bLoopChanged)
	{
		// If Sim supports a loop flag, set it here (pseudo: Sim->Arms[SimArmIndex].bLoop = bLoopProgram).
		if (Sim && SimArmIndex >= 0 && SimArmIndex < Sim->Arms.Num())
		{
			// If FArmState has something like bLoopTape, assign it here:
			// Sim->Arms[SimArmIndex].bLoopTape = bLoopProgram;
		}
		if (bDebugLogs)
		{
			UE_LOG(LogAGArm, Verbose, TEXT("Arm '%s': Loop flag changed=%d"), *GetName(), bLoopProgram ? 1 : 0);
		}
	}
	else if (bReachOrFacingChanged)
	{
		CurrYawDeg = DirToYaw(FacingDir);
		PushRuntimePropertiesToSim();
	}
	else if (bStartingRotationChanged || bStartingReachChanged)
	{
		// When starting values change, immediately update current values and restart program
		if (bStartingRotationChanged)
		{
			FacingDir = StartingRotation % 6;
			CurrYawDeg = DirToYaw(FacingDir);
		}

		if (bStartingReachChanged)
		{
			ReachTiles = FMath::Clamp(StartingReach, 1, 6);
		}

		// Update simulation state to match new starting values
		PushRuntimePropertiesToSim();

		// Reset program counter to restart the program
		if (Sim && SimArmIndex >= 0 && SimArmIndex < Sim->Arms.Num())
		{
			FArmState& S = Sim->Arms[SimArmIndex];
			S.PC = 0; // Reset to beginning of program
			S.WaitCounter = 0; // Clear any wait state
		}

		// Refresh visuals immediately
		RefreshPose(1.f);

		UE_LOG(LogAGArm, Log, TEXT("Arm '%s': Starting values changed - Rotation=%d Reach=%d, program restarted"),
			*GetName(), StartingRotation, StartingReach);
	}

	// For safety, if Program became empty we still ensure a minimal idle op
	if (Program.Num() == 0 && Sim && SimArmIndex >= 0 && SimArmIndex < Sim->Arms.Num())
	{
		if (Sim->Arms[SimArmIndex].Tape.Num() == 0)
		{
			PushProgramToSim();
		}
	}

	if (GIsPlayInEditorWorld && !bUseGripSocket && GripPivotComp)
	{
		//PropName = Event.Property ? Event.Property->GetFName() : NAME_None;
		if (PropName == GET_MEMBER_NAME_CHECKED(ABasicArm, GripPivotLocalRotation) ||
			PropName == GET_MEMBER_NAME_CHECKED(ABasicArm, GripDistanceOverride) ||
			PropName == GET_MEMBER_NAME_CHECKED(ABasicArm, GripSideOffset) ||
			PropName == GET_MEMBER_NAME_CHECKED(ABasicArm, GripZOffset))
		{
			GripPivotComp->SetRelativeLocation(ComputeGripLocalOffset());
			GripPivotComp->SetRelativeRotation(GripPivotLocalRotation);
		}
	}
}
#endif

void ABasicArm::Start()
{
	bRunning = true;
}

void ABasicArm::Stop()
{
	bRunning = false;
}

void ABasicArm::ResetProgram()
{
	PC = 0;
	WaitLeft = 0;
}

void ABasicArm::ExecuteCurrent()
{
	const int32 SavedWait = WaitLeft;
	WaitLeft = 0;
	//StepProgram();
	WaitLeft = SavedWait;
}

void ABasicArm::SyncDebugSettingsFromDriver()
{
	if (Driver)
	{
		bDebugLogs = Driver->bDebugLogs;
		bDebugSpheres = Driver->bDebugSpheres;
		bDebugLines = Driver->bDebugLines;
		UE_LOG(LogAGArm, Log, TEXT("BasicArm '%s': Synced debug settings from driver - Logs=%d Spheres=%d Lines=%d"),
			*GetName(), bDebugLogs ? 1 : 0, bDebugSpheres ? 1 : 0, bDebugLines ? 1 : 0);
	}
	else
	{
		UE_LOG(LogAGArm, Warning, TEXT("BasicArm '%s': No driver found for debug sync"), *GetName());
	}
}



void ABasicArm::ApplyRotateSteps(int32 StepsSigned)
{
	if (StepsSigned == 0) return;
	const uint8 Old = FacingDir;
	FacingDir = uint8(Mod6(int32(FacingDir) + StepsSigned));
	CurrYawDeg = DirToYaw(FacingDir);
	UE_LOG(LogAGArm, Verbose, TEXT("ApplyRotateSteps: steps=%d OldDir=%d NewDir=%d CurrYaw=%.2f"),
		StepsSigned, (int32)Old, (int32)FacingDir, CurrYawDeg);
}

FHex ABasicArm::DirStep(uint8 Dir) const
{
	const uint8 D = Dir % 6;
	// Pointy-top neighbors (q,r): E(1,0), NE(1,-1), NW(0,-1), W(-1,0), SW(-1,1), SE(0,1)
	static const FHex Pointy[6] = {
		FHex(1,  0), FHex(1, -1), FHex(0, -1),
		FHex(-1,  0), FHex(-1,  1), FHex(0,  1)
	};
	// Flat-top (only used if you switch orientation)
	static const FHex Flat[6] = {
		FHex(1, -1), FHex(1,  0), FHex(0,  1),
		FHex(-1,  1), FHex(-1,  0), FHex(0, -1)
	};

	const bool bPointy = !Driver || Driver->GetGridOrientation() == EHexOrientation::PointyTop;
	return bPointy ? Pointy[D] : Flat[D];
}

// ABasicArm.cpp
FHex ABasicArm::ComputeTargetHex() const
{
	FHex T(Q, R);
	const int32 N = FMath::Max(1, ReachTiles);
	for (int32 i = 0; i < N; ++i)
		T = HexUtil::Neighbor(T, static_cast<EHexDir>(FacingDir));
	return T;
}


void ABasicArm::ExpandResetAtPC()
{
	// Expand "Reset" into: (Drop if holding) + Reach adjustments + Rotation adjustments back to starting values
	TArray<FArmInstr> Expansion;

	// Always drop first if holding
	if (bHolding)
	{
		Expansion.Add({ EArmCmd::Drop, 0 });
	}

	// Handle reach reset: calculate difference between current and starting reach
	const int32 CurrentReach = ReachTiles;
	const int32 TargetReach = FMath::Clamp(StartingReach, 1, 6);
	const int32 ReachDiff = TargetReach - CurrentReach;

	if (ReachDiff > 0)
	{
		// Need to extend
		for (int32 i = 0; i < ReachDiff; ++i)
		{
			Expansion.Add({ EArmCmd::Extend, 1 });
		}
	}
	else if (ReachDiff < 0)
	{
		// Need to retract
		for (int32 i = 0; i < -ReachDiff; ++i)
		{
			Expansion.Add({ EArmCmd::Retract, 1 });
		}
	}

	// Handle rotation reset: find shortest path to StartingRotation
	const uint8 CurrentRotation = FacingDir;
	const uint8 TargetRotation = StartingRotation % 6;
	const int32 DiffCW = Mod6(int32(TargetRotation) - int32(CurrentRotation)); // 0..5 steps CW
	const int32 DiffCCW = Mod6(int32(CurrentRotation) - int32(TargetRotation)); // 0..5 steps CCW
	const bool bUseCW = (DiffCW <= DiffCCW);
	const int32 RotationSteps = bUseCW ? DiffCW : DiffCCW;

	for (int32 i = 0; i < RotationSteps; ++i)
	{
		Expansion.Add({ bUseCW ? EArmCmd::RotateCW : EArmCmd::RotateCCW, 1 });
	}

	// Remove the Reset command and insert the expansion
	Program.RemoveAt(PC);
	if (Expansion.Num() > 0)
	{
		Program.Insert(Expansion, PC);
	}

	// Log the expansion for debugging
	UE_LOG(LogAGArm, Verbose, TEXT("BasicArm '%s' Reset expanded: Rotation(%d->%d) Reach(%d->%d) Holding=%d ExpansionSteps=%d"),
		*GetName(),
		int32(CurrentRotation), int32(TargetRotation),
		CurrentReach, TargetReach,
		bHolding ? 1 : 0,
		Expansion.Num());
}

void ABasicArm::StepProgram()
{
	// Consume waiting ticks
	if (WaitLeft > 0)
	{
		--WaitLeft;
		return;
	}

	if (Program.Num() == 0) return;

	// Clamp/wrap PC
	if (PC >= Program.Num())
	{
		if (bLoopProgram) PC = 0;
		else return;
	}

	const FArmInstr I = Program[PC];

}

void ABasicArm::ShowHeldOrb(bool bShow)
{
	if (!HeldOrbComp)
	{
		UE_LOG(LogAGArm, Warning, TEXT("Arm '%s': HeldOrbComp is null, cannot show held orb."), *GetName());
		return;
	}

	// Remove previous orb meshes
	TArray<USceneComponent*> ChildComponents;
	HeldOrbComp->GetChildrenComponents(false, ChildComponents);
	//for (USceneComponent* Child : ChildComponents)
	//{
	//	Child->DestroyComponent();
	//}

	if (bShow && HeldOrb.Orbs.Num() > 0)
	{
		if (bDebugLogs)
		{
			UE_LOG(LogAGArm, Log, TEXT("Arm '%s': Showing held orb with %d sub-orbs."), *GetName(), HeldOrb.Orbs.Num());
		}
		HeldOrbComp->AttachToComponent(BaseComp, FAttachmentTransformRules::SnapToTargetIncludingScale, TEXT("GripSocket"));

		for (const FOrbData& Orb : HeldOrb.Orbs)
		{
			UStaticMeshComponent* OrbMesh = NewObject<UStaticMeshComponent>(this);
			OrbMesh->RegisterComponent();
			OrbMesh->AttachToComponent(HeldOrbComp, FAttachmentTransformRules::KeepRelativeTransform);
			OrbMesh->SetRelativeLocation(FVector(Orb.Offset, 0.f));
			OrbMesh->SetMaterial(0, GetMaterialForOrbColor(Orb.Type));
			OrbMesh->SetRelativeScale3D(HeldOrbScale);

			if (bDebugLogs)
			{
				UE_LOG(LogAGArm, Verbose, TEXT("Arm '%s': Created orb mesh for type=%d color=%d offset=(%.2f, %.2f)"), *GetName(), Orb.Type, Orb.Color, Orb.Offset.X, Orb.Offset.Y);
			}
		}
		//HeldOrbComp->SetVisibility(true, true);
	}
	else
	{
		if (bShow)
		{
			UE_LOG(LogAGArm, Warning, TEXT("Arm '%s': Tried to show held orb, but HeldOrb.Orbs.Num() == 0"), *GetName());
		}
		HeldOrbComp->DetachFromComponent(FDetachmentTransformRules::KeepWorldTransform);
		//HeldOrbComp->SetVisibility(false, true);
	}
}

void ABasicArm::InitializeOnGrid(AOrbSimDriver* InDriver, int32 InQ, int32 InR, uint8 InFacing)
{
	Driver = InDriver;
	Q = InQ;
	R = InR;
	FacingDir = InFacing % 6;
	StartFacingDir = FacingDir;

	// Initialize starting values for Reset command
	// These will be the values the arm resets to
	StartingRotation = InFacing % 6;
	StartingReach = ReachTiles;

	Sim = (Driver ? Driver->GetSim() : nullptr);

	// refresh visuals immediately
	CurrYawDeg = DirToYaw(FacingDir);
	LastYawDeg = CurrYawDeg;
	RefreshPose(1.f);
}

UStaticMesh* ABasicArm::GetMeshForOrbType(uint8 OrbType) const
{
	// Use centralized material manager
	if (UOrbMaterialManager* MaterialManager = UOrbMaterialManager::FindInWorld(this))
	{
		UStaticMesh* CentralizedMesh = MaterialManager->GetOrbMesh();
		if (CentralizedMesh)
		{
			return CentralizedMesh;
		}
	}

	// Final fallback to engine default
	return nullptr;
}

UMaterialInterface* ABasicArm::GetMaterialForOrbColor(uint8 OrbType) const
{
	// Use centralized material manager
	if (UOrbMaterialManager* MaterialManager = UOrbMaterialManager::FindInWorld(this))
	{
		// Use the orb type directly as the material key
		EOrbType EOrbTypeValue = (EOrbType)OrbType;
		UMaterialInterface* CentralizedMaterial = MaterialManager->GetMaterialForOrbType(EOrbTypeValue);
		if (CentralizedMaterial)
		{
			return CentralizedMaterial;
		}

		// Try the default material from the centralized system
		UMaterialInterface* DefaultMaterial = MaterialManager->GetDefaultOrbMaterial();
		if (DefaultMaterial)
		{
			return DefaultMaterial;
		}
	}

	// Final fallback to engine default
	return nullptr;
}

// Add helper functions near other helpers (e.g. after GetMaterialForOrbColor)
UStaticMesh* ABasicArm::ResolveOrbMesh(const FItem& Item) const
{
	// Use centralized material manager
	if (UOrbMaterialManager* MaterialManager = UOrbMaterialManager::FindInWorld(this))
	{
		// Get the correct orb type from the item
		EOrbType OrbType = EOrbType::BaseA; // Default
		if (Item.bIsCluster && Item.Orbs.Num() > 0)
		{
			OrbType = (EOrbType)Item.Orbs[0].Type;
		}
		else if (!Item.bIsCluster)
		{
			OrbType = Item.Single.Type;
		}

		// For now, all orb types use the same mesh, but this could be extended
		UStaticMesh* CentralizedMesh = MaterialManager->GetOrbMesh();
		if (CentralizedMesh)
		{
			return CentralizedMesh;
		}
	}

	// Final fallback to engine default
	return nullptr;
}

UMaterialInterface* ABasicArm::ResolveOrbMaterial(const FItem& Item) const
{
	// Use centralized material manager
	if (UOrbMaterialManager* MaterialManager = UOrbMaterialManager::FindInWorld(this))
	{
		EOrbType OrbType = EOrbType::BaseA; // Default
		if (Item.bIsCluster && Item.Orbs.Num() > 0)
		{
			OrbType = (EOrbType)Item.Orbs[0].Type;
		}
		else if (!Item.bIsCluster)
		{
			OrbType = Item.Single.Type;
		}

		UMaterialInterface* CentralizedMaterial = MaterialManager->GetMaterialForOrbType(OrbType);
		if (CentralizedMaterial)
		{
			return CentralizedMaterial;
		}

		// Try the default material from the centralized system
		UMaterialInterface* DefaultMaterial = MaterialManager->GetDefaultOrbMaterial();
		if (DefaultMaterial)
		{
			return DefaultMaterial;
		}
	}

	// Final fallback to engine default
	return nullptr;
}

// Replace AttachHeldOrb() with more robust version
void ABasicArm::AttachHeldOrb()
{
	if (!bHolding)
	{
		DetachHeldOrb();
		return;
	}

	if (!HeldOrbComp)
	{
		HeldOrbComp = NewObject<UStaticMeshComponent>(this, TEXT("HeldOrb"));
		HeldOrbComp->RegisterComponent();
	}

	USceneComponent* ParentForHeld = nullptr;

	if (bUseGripSocket && BaseComp && BaseComp->DoesSocketExist(TEXT("GripSocket")))
	{
		ParentForHeld = BaseComp;
		HeldOrbComp->AttachToComponent(BaseComp,
			FAttachmentTransformRules::SnapToTargetIncludingScale,
			TEXT("GripSocket"));
	}
	else
	{
		// Fallback: procedural pivot
		ParentForHeld = GripPivotComp ? GripPivotComp : BaseComp;
		if (GripPivotComp && !bUseGripSocket)
		{
			GripPivotComp->SetRelativeLocation(ComputeGripLocalOffset());
		}
		HeldOrbComp->AttachToComponent(ParentForHeld,
			FAttachmentTransformRules::SnapToTargetIncludingScale);
	}

	HeldOrbComp->SetMobility(EComponentMobility::Movable);
	HeldOrbComp->SetVisibility(true, true);
	HeldOrbComp->SetRelativeScale3D(HeldOrbScale);

	// Clear old children
	TArray<USceneComponent*> ChildComps;
	HeldOrbComp->GetChildrenComponents(false, ChildComps);
	for (USceneComponent* C : ChildComps)
	{
		if (C) C->DestroyComponent();
	}

	const float Tile = (Driver ? Driver->GetTileSize() : 100.f);
	const float OffsetBaseScale = (bScaleClusterOffsetsByTile ? Tile : 1.f) * ClusterOffsetScale;

	if (HeldOrb.bIsCluster)
	{
		// Optional container mesh
		if (bShowClusterContainerMesh)
		{
			UStaticMesh* Container = ResolveOrbMesh(HeldOrb);
			HeldOrbComp->SetStaticMesh(Container);
		}
		else
		{
			HeldOrbComp->SetStaticMesh(nullptr);
		}

		if (!bShowClusterContainerMesh && !HeldOrbComp->GetStaticMesh())
		{
			// Accept empty parent; just holds sub-orbs.
		}


		// Opus Magnum approach: position sub-orbs relative to the grabbed orb
		// The grabbed orb should be at the grab point, others positioned relative to it
		// UE_LOG(LogAGArm, Log, TEXT("Attaching cluster with %d sub-orbs, GrabFacing=%d"), 
		// 	HeldOrb.Orbs.Num(), (int32)HeldOrb.GrabFacing);
		for (const FOrbData& Sub : HeldOrb.Orbs)
		{
			UStaticMeshComponent* SubMesh = NewObject<UStaticMeshComponent>(this);
			SubMesh->RegisterComponent();
			SubMesh->AttachToComponent(HeldOrbComp, FAttachmentTransformRules::KeepRelativeTransform);

			UStaticMesh* SM = GetMeshForOrbType(Sub.Type);
			SubMesh->SetStaticMesh(SM);

			UMaterialInterface* Mat = GetMaterialForOrbColor(Sub.Type);
			if (Mat)
			{
				SubMesh->SetMaterial(0, Mat);
				// Ensure the material renders as opaque (not transparent)
				SubMesh->SetRenderCustomDepth(false);
			}

			// Calculate visual offset from hex coordinates (dQ, dR)
			// Convert hex axial coordinates to world offset using the SAME logic as simulation
			const EHexOrientation GridOrientation = (Driver ? Driver->GetGridOrientation() : EHexOrientation::PointyTop);
			const FVector2D HexOffset = HexCoord::AxialToXY(FHexAxial(Sub.dQ, Sub.dR), Tile, GridOrientation);
			FVector2D Off2D = HexOffset * ClusterOffsetScale;

			// Apply rotation based on GrabFacing to make offsets relative to arm direction
			if (HeldOrb.GrabFacing >= 0)
			{
				const float GrabYawDeg = 60.f * (HeldOrb.GrabFacing % 6);
				const float YawRad = FMath::DegreesToRadians(GrabYawDeg);
				const float CosA = FMath::Cos(YawRad);
				const float SinA = FMath::Sin(YawRad);
				const float RotX = Off2D.X * CosA - Off2D.Y * SinA;
				const float RotY = Off2D.X * SinA + Off2D.Y * CosA;
				Off2D = FVector2D(RotX, RotY);
			}

			if (bInvertClusterVisualFacing)
			{
				// 180° flip around Z so visual layout matches logical facing.
				Off2D *= -1.f;
			}

			SubMesh->SetRelativeLocation(FVector(Off2D.X, Off2D.Y, 0.f));


			SubMesh->SetRelativeScale3D(HeldOrbScale);

			HeldClusterComps.Add(SubMesh);
		}

		// Generate bonded orb meshes for held cluster
		if (bShowBondedOrbs && GetBondedOrbMesh() && Sim)
		{
			GenerateBondedOrbMeshes();
		}

	}
	else
	{
		// Handle single orbs the same way as clusters - create a sub-orb mesh
		// This ensures consistent material handling between single orbs and clusters
		HeldOrbComp->SetStaticMesh(nullptr); // Clear the main component mesh

		// Create a sub-orb mesh for the single orb
		UStaticMeshComponent* SubMesh = NewObject<UStaticMeshComponent>(this);
		SubMesh->RegisterComponent();
		SubMesh->AttachToComponent(HeldOrbComp, FAttachmentTransformRules::KeepRelativeTransform);
		SubMesh->SetRelativeLocation(FVector::ZeroVector); // Single orb at center
		SubMesh->SetRelativeScale3D(HeldOrbScale);

		UStaticMesh* SM = GetMeshForOrbType((uint8)HeldOrb.Single.Type);
		SubMesh->SetStaticMesh(SM);

		UMaterialInterface* Mat = GetMaterialForOrbColor((uint8)HeldOrb.Single.Type);
		if (Mat)
		{
			SubMesh->SetMaterial(0, Mat);
			// Ensure the material renders as opaque (not transparent)
			SubMesh->SetRenderCustomDepth(false);
		}

		HeldClusterComps.Add(SubMesh);
		if (bDebugLogs)
		{
			UE_LOG(LogAGArm, Verbose, TEXT("Arm '%s': Attached single orb Id=%d Type=%d"), *GetName(), HeldOrb.Id, (int32)HeldOrb.Single.Type);
		}
	}
}

// UpdateHeldOrbVisual(): relax Id requirement & add logging
void ABasicArm::UpdateHeldOrbVisual()
{
	const int32 CurrentId = bHolding ? (HeldOrb.Id == 0 ? -1 : HeldOrb.Id) : 0; // -1 denotes held with no Id
	const int32 CurrentOrbCount = bHolding ? HeldOrb.Orbs.Num() : 0;

	// Check if the held item changed OR if the cluster composition changed
	bool bNeedsUpdate = (CurrentId != LastHeldItemId) || (CurrentOrbCount != LastHeldOrbCount);

	if (!bNeedsUpdate)
		return;

	if (!bHolding)
	{
		DetachHeldOrb();
		LastHeldItemId = 0;
		LastHeldOrbCount = 0;
		return;
	}

	AttachHeldOrb(); // always when holding
	LastHeldItemId = CurrentId;
	LastHeldOrbCount = CurrentOrbCount;
}

// --- Sync / detach implementations added ---

void ABasicArm::SyncFromSim(const FArmState& SimState)
{
	// Update orientation
	FacingDir = static_cast<uint8>(SimState.Facing);
	CurrYawDeg = DirToYaw(FacingDir);

	// Reach (clamped the same way we push)
	if (ReachTiles != SimState.ArmLen)
	{
		ReachTiles = SimState.ArmLen;
	}

	// Sync wait state from simulation
	WaitLeft = SimState.WaitCounter;

	// Sync program counter from simulation
	PC = SimState.PC;

	// Holding state + item payload
	bHolding = SimState.bHolding;
	if (bHolding)
	{
		HeldOrb = SimState.Held;   // assumes FArmState has Held (or adapt if different field name)
	}
	else
	{
		HeldOrb = FItem();
	}

	UpdateHeldOrbVisual();
}

void ABasicArm::SyncFromCurrentSim()
{
	if (!Sim || SimArmIndex < 0 || SimArmIndex >= Sim->Arms.Num())
	{
		return;
	}
	SyncFromSim(Sim->Arms[SimArmIndex]);
}

void ABasicArm::UpdateHeldClusterOnly()
{
	if (!Sim || SimArmIndex < 0 || SimArmIndex >= Sim->Arms.Num())
	{
		return;
	}

	// Only update the held orb data and visual, don't touch orientation or other state
	const FArmState& SimState = Sim->Arms[SimArmIndex];

	// Update holding state and item payload only
	bool bWasHolding = bHolding;
	bHolding = SimState.bHolding;

	if (bHolding)
	{
		HeldOrb = SimState.Held;
	}
	else
	{
		HeldOrb = FItem();
	}

	// Only update visual if holding state changed or cluster composition changed
	const int32 NewOrbCount = bHolding ? HeldOrb.Orbs.Num() : 0;
	const bool bClusterChanged = (NewOrbCount != LastHeldOrbCount);



	if (bWasHolding != bHolding || (bHolding && bClusterChanged))
	{
		UpdateHeldOrbVisual();
	}
}

void ABasicArm::DetachHeldOrb()
{
	if (!HeldOrbComp) return;

	// Destroy any child components (cluster meshes)
	TArray<USceneComponent*> ChildComps;
	HeldOrbComp->GetChildrenComponents(false, ChildComps);
	for (USceneComponent* C : ChildComps)
	{
		if (C && C != HeldOrbComp)
		{
			C->DestroyComponent();
		}
	}

	HeldOrbComp->SetStaticMesh(nullptr);
	HeldOrbComp->SetVisibility(false, true);
	HeldOrbComp->DetachFromComponent(FDetachmentTransformRules::KeepWorldTransform);
}

void ABasicArm::UpdateHeldClusterTransforms(float /*CurrentYawDeg*/)
{
	// Opus Magnum approach: cluster rotates naturally with the arm
	// The cluster is attached to HeldOrbComp which rotates with the arm
	// No per-tick updates needed - the attachment system handles rotation
}

FVector ABasicArm::ComputeGripLocalOffset() const
{
	const float Tile = (Driver ? Driver->GetTileSize() : 100.f);
	const float Dist = (GripDistanceOverride > 0.f)
		? GripDistanceOverride
		: (float)FMath::Max(1, ReachTiles) * Tile;

	// Forward = +X (assumed arm mesh forward axis)
	return FVector(Dist, GripSideOffset, GripZOffset);
}

void ABasicArm::GenerateBondedOrbMeshes()
{
	UStaticMesh* BondedOrbMesh = GetBondedOrbMesh();
	if (!bHolding || !HeldOrb.bIsCluster || !BondedOrbMesh || !Sim)
	{
		return;
	}

	// Get tile size from driver
	const float Tile = (Driver ? Driver->GetTileSize() : 100.f);

	// Clear existing bonded orb meshes
	TArray<USceneComponent*> ChildComps;
	HeldOrbComp->GetChildrenComponents(false, ChildComps);
	for (USceneComponent* C : ChildComps)
	{
		if (C && C->GetName().StartsWith(TEXT("BondedOrb_")))
		{
			C->DestroyComponent();
		}
	}

	// Get the bonds for this held cluster
	const TArray<USimWorldHex::FBond>& Bonds = Sim->GetBonds();

	// Find bonds that involve sub-orbs in this held cluster
	for (const auto& Bond : Bonds)
	{
		if (!Bond.bActive) continue;

		// Check if both items are in this held cluster
		bool bItemAInCluster = false;
		bool bItemBInCluster = false;
		FVector PosA, PosB;

		for (const FOrbData& Sub : HeldOrb.Orbs)
		{
			if (Sub.ItemId == Bond.ItemA)
			{
				bItemAInCluster = true;
				// Calculate position using the same logic as sub-orb positioning
				const EHexOrientation GridOrientation = (Driver ? Driver->GetGridOrientation() : EHexOrientation::PointyTop);
				const FVector2D HexOffset = HexCoord::AxialToXY(FHexAxial(Sub.dQ, Sub.dR), Tile, GridOrientation);
				FVector2D Off2D = HexOffset * ClusterOffsetScale;

				// Apply rotation based on GrabFacing
				if (HeldOrb.GrabFacing >= 0)
				{
					const float GrabYawDeg = 60.f * (HeldOrb.GrabFacing % 6);
					const float YawRad = FMath::DegreesToRadians(GrabYawDeg);
					const float CosA = FMath::Cos(YawRad);
					const float SinA = FMath::Sin(YawRad);
					const float RotX = Off2D.X * CosA - Off2D.Y * SinA;
					const float RotY = Off2D.X * SinA + Off2D.Y * CosA;
					Off2D = FVector2D(RotX, RotY);
				}

				if (bInvertClusterVisualFacing)
				{
					Off2D *= -1.f;
				}

				PosA = FVector(Off2D.X, Off2D.Y, 0.f);
			}

			if (Sub.ItemId == Bond.ItemB)
			{
				bItemBInCluster = true;
				// Calculate position using the same logic as sub-orb positioning
				const EHexOrientation GridOrientation = (Driver ? Driver->GetGridOrientation() : EHexOrientation::PointyTop);
				const FVector2D HexOffset = HexCoord::AxialToXY(FHexAxial(Sub.dQ, Sub.dR), Tile, GridOrientation);
				FVector2D Off2D = HexOffset * ClusterOffsetScale;

				// Apply rotation based on GrabFacing
				if (HeldOrb.GrabFacing >= 0)
				{
					const float GrabYawDeg = 60.f * (HeldOrb.GrabFacing % 6);
					const float YawRad = FMath::DegreesToRadians(GrabYawDeg);
					const float CosA = FMath::Cos(YawRad);
					const float SinA = FMath::Sin(YawRad);
					const float RotX = Off2D.X * CosA - Off2D.Y * SinA;
					const float RotY = Off2D.X * SinA + Off2D.Y * CosA;
					Off2D = FVector2D(RotX, RotY);
				}

				if (bInvertClusterVisualFacing)
				{
					Off2D *= -1.f;
				}

				PosB = FVector(Off2D.X, Off2D.Y, 0.f);
			}
		}

		// If both items are in this cluster, create a bonded orb mesh
		if (bItemAInCluster && bItemBInCluster)
		{
			UStaticMeshComponent* BondMesh = NewObject<UStaticMeshComponent>(this, *FString::Printf(TEXT("BondedOrb_%d_%d"), Bond.ItemA, Bond.ItemB));
			BondMesh->RegisterComponent();
			BondMesh->AttachToComponent(HeldOrbComp, FAttachmentTransformRules::KeepRelativeTransform);
			BondMesh->SetStaticMesh(BondedOrbMesh);

			UMaterialInterface* BondedOrbMaterial = GetBondedOrbMaterial();
			if (BondedOrbMaterial)
			{
				BondMesh->SetMaterial(0, BondedOrbMaterial);
			}

			// Calculate midpoint and direction
			FVector Midpoint = (PosA + PosB) * 0.5f;
			FVector Direction = (PosB - PosA).GetSafeNormal();
			float Distance = FVector::Dist(PosA, PosB);

			// For a cylinder that's standing up by default, rotate it to align with the bond direction
			FVector Up = Direction;
			FVector Right = FVector::CrossProduct(Up, FVector::UpVector).GetSafeNormal();
			if (Right.IsNearlyZero())
			{
				Right = FVector::RightVector;
			}
			FVector Forward = FVector::CrossProduct(Right, Up).GetSafeNormal();

			// Create rotation matrix from the three axes
			FMatrix RotationMatrix = FMatrix(Forward, Right, Up, FVector::ZeroVector);
			FRotator Rotation = FQuat(RotationMatrix).Rotator();

			// Scale the cylinder to fit the bond length
			float BondedOrbScale = GetBondedOrbScale();
			FVector Scale = FVector(BondedOrbScale, BondedOrbScale, 1.f);

			// Set the transform
			BondMesh->SetRelativeLocation(Midpoint);
			BondMesh->SetRelativeRotation(Rotation);
			BondMesh->SetRelativeScale3D(Scale);
		}
	}
}

UStaticMesh* ABasicArm::GetBondedOrbMesh() const
{
	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			if (UOrbRenderer* Renderer = (*It)->FindComponentByClass<UOrbRenderer>())
			{
				return Renderer->BondedOrbMesh;
			}
		}
	}
	return nullptr;
}

UMaterialInterface* ABasicArm::GetBondedOrbMaterial() const
{
	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			if (UOrbRenderer* Renderer = (*It)->FindComponentByClass<UOrbRenderer>())
			{
				return Renderer->BondedOrbMaterial;
			}
		}
	}
	return nullptr;
}

float ABasicArm::GetBondedOrbScale() const
{
	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			if (UOrbRenderer* Renderer = (*It)->FindComponentByClass<UOrbRenderer>())
			{
				return Renderer->BondedOrbScale;
			}
		}
	}
	return 0.15f; // Default fallback
}

