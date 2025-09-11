#include "Actors/BasicArm.h"
#include "Actors/OrbSimDriver.h"
#include "Framework/GameTickManager.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/World.h"
#include "AGLogChannels.h"
#include "Sim/SimWorldHex.h"
#include "DrawDebugHelpers.h"

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
	case EArmCmd::Reset:     return TEXT("Reset");
	default:                 return TEXT("<?>");
	}
}

ABasicArm::ABasicArm()
{
	PrimaryActorTick.bCanEverTick = true;   // we interpolate visuals in Tick

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

	// Sync from sim
	if (Sim && SimArmIndex >= 0 && SimArmIndex < Sim->Arms.Num())
	{
		const FArmState& SimState = Sim->Arms[SimArmIndex];
		SyncFromSim(SimState);
	}

	// Advance StepAlpha for interpolation
	if (StepSecondsCur > 0.f)
	{
		StepAlpha = FMath::Clamp(StepAlpha + DeltaSeconds / StepSecondsCur, 0.f, 1.f);
	}
	else
	{
		StepAlpha = 1.f;
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

	// NOW pull new sim state (sets CurrYawDeg)
	SyncFromSim();

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
		HeldOrbComp->SetStaticMesh(HeldOrbMesh);
		HeldOrbComp->SetRelativeScale3D(HeldOrbScale);
		HeldOrbComp->SetVisibility(bHolding && HeldOrbMesh != nullptr, true);
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
	// Apply easing to alpha
	float EasedAlpha = FMath::InterpEaseInOut(0.f, 1.f, InAlpha, 2);

	const FVector BaseWorld = HexToWorld(FHex(Q, R)) + FVector(0, 0, BaseZ);

	const float StartYaw = NormDeg(LastYawDeg);
	const float TargetYaw = NormDeg(CurrYawDeg);
	const float Delta = FMath::FindDeltaAngleDegrees(StartYaw, TargetYaw);
	const float Yaw = NormDeg(StartYaw + Delta * EasedAlpha);

	UE_LOG(LogAGArm, Verbose, TEXT("RefreshPose: Q=%d R=%d LastYaw=%.2f CurrYaw=%.2f StepAlpha=%.2f EasedAlpha=%.2f Delta=%.2f Yaw=%.2f"),
		Q, R, StartYaw, TargetYaw, InAlpha, EasedAlpha, Delta, Yaw);

	const FRotator BaseRot(0.f, Yaw + BaseYawOffsetDeg, 0.f);

	if (BaseComp)
	{
		BaseComp->SetWorldLocation(BaseWorld);
		BaseComp->SetWorldRotation(BaseRot);
	}

	// --- NEW: precise hand (grab/drop) target world location ---
	FVector HandWorld = BaseWorld; // fallback
	{
		const FHex TargetHex = ComputeTargetHex();
		HandWorld = HexToWorld(TargetHex);
		// Apply same vertical offsets we use for grip pivot
		HandWorld.Z += GripZOffset;
	}

	// Keep GripPivotComp at exact hand world position (unless using a socket)
	if (GripPivotComp && !bUseGripSocket)
	{
		// Convert to BaseComp local so animation / rotation stays consistent
		if (BaseComp)
		{
			const FTransform BaseXform = BaseComp->GetComponentTransform();
			const FVector Local = BaseXform.InverseTransformPosition(HandWorld);
			GripPivotComp->SetRelativeLocation(Local);
			// Don't set rotation here - let the attachment system handle it automatically
			// The GripPivotLocalRotation should be set once during initialization, not every frame
		}
		else
		{
			GripPivotComp->SetWorldLocation(HandWorld);
			// For world space, we need to apply the full rotation
			const FRotator WorldRotation = FRotator(0.f, Yaw + BaseYawOffsetDeg, 0.f) + GripPivotLocalRotation;
			GripPivotComp->SetWorldRotation(WorldRotation);
		}
	}

	// Optional debug draw
	if (bDebugHandLocation && GetWorld())
	{
		const FVector Start = BaseComp ? BaseComp->GetComponentLocation() : GetActorLocation();
		DrawDebugLine(GetWorld(), Start, HandWorld, FColor::Green, false, 0.f, 0, 2.f);
		DrawDebugSphere(GetWorld(), HandWorld, 6.f, 12, FColor::Yellow, false, 0.f);
	}

	// Opus Magnum approach: cluster rotates naturally with the arm
	// No per-tick updates needed - the attachment system handles rotation
}

void ABasicArm::SetProgram(const TArray<FArmInstr>& InProgram, bool bLoop /*= true*/)
{
	Program = InProgram;
	bLoopProgram = bLoop;
	PC = 0;
	WaitLeft = 0;
	PushProgramToSim(); // NEW
}

static TArray<FOp> BuildOpsFromProgram(const TArray<FArmInstr>& Program)
{
	TArray<FOp> Out;
	if (Program.Num() == 0)
	{
		Out.Add({ ECmd::Wait, 1 }); // harmless idle
		return Out;
	}
	for (const FArmInstr& I : Program)
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
		case EArmCmd::Extend:    Out.Add({ ECmd::Extend,  (uint8)FMath::Clamp(I.Arg == 0 ? 1 : I.Arg, 0, 255) }); break;   // NEW
		case EArmCmd::Retract:   Out.Add({ ECmd::Retract, (uint8)FMath::Clamp(I.Arg == 0 ? 1 : I.Arg, 0, 255) }); break;   // NEW
		default:                 Out.Add({ ECmd::Wait, 1 }); break;
		}
	}
	return Out;
}

// NEW: push actor-side program / facing into sim
void ABasicArm::PushProgramToSim()
{
	if (!Sim || SimArmIndex < 0 || SimArmIndex >= Sim->Arms.Num()) return;
	FArmState& S = Sim->Arms[SimArmIndex];
	S.Tape = BuildOpsFromProgram(Program);
	S.PC = 0;
	S.Facing = static_cast<EHexDir>(FacingDir);
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
		UE_LOG(LogAGArm, Verbose, TEXT("Arm '%s': Loop flag changed=%d"), *GetName(), bLoopProgram ? 1 : 0);
	}
	else if (bReachOrFacingChanged)
	{
		CurrYawDeg = DirToYaw(FacingDir);
		PushRuntimePropertiesToSim();
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
	// Expand “Reset” into: (Drop if holding) + the shortest sequence of Rotate steps back to StartFacingDir.
	TArray<FArmInstr> Expansion;
	if (bHolding)
	{
		Expansion.Add({ EArmCmd::Drop, 0 });
	}

	const int32 DiffCW = Mod6(int32(StartFacingDir) - int32(FacingDir)); // 0..5 steps CW
	const int32 DiffCCW = Mod6(int32(FacingDir) - int32(StartFacingDir)); // 0..5 steps CCW
	const bool bUseCW = (DiffCW <= DiffCCW);
	const int32 Steps = bUseCW ? DiffCW : DiffCCW;

	for (int32 i = 0; i < Steps; ++i)
	{
		Expansion.Add({ bUseCW ? EArmCmd::RotateCW : EArmCmd::RotateCCW, 1 });
	}

	Program.RemoveAt(PC);
	if (Expansion.Num() > 0)
	{
		Program.Insert(Expansion, PC);
	}
	// next fixed tick will execute the first inserted op
	UE_LOG(LogAGArm, Verbose, TEXT("BasicArm '%s' Reset expanded: From=%d To=%d Dir=%s Steps=%d Holding=%d"),
		*GetName(), int32(FacingDir), int32(StartFacingDir), bUseCW ? TEXT("CW") : TEXT("CCW"), Steps, bHolding ? 1 : 0);
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
		UE_LOG(LogAGArm, Log, TEXT("Arm '%s': Showing held orb with %d sub-orbs."), *GetName(), HeldOrb.Orbs.Num());
		HeldOrbComp->AttachToComponent(BaseComp, FAttachmentTransformRules::SnapToTargetIncludingScale, TEXT("GripSocket"));

		for (const FOrbData& Orb : HeldOrb.Orbs)
		{
			UStaticMeshComponent* OrbMesh = NewObject<UStaticMeshComponent>(this);
			OrbMesh->RegisterComponent();
			OrbMesh->AttachToComponent(HeldOrbComp, FAttachmentTransformRules::KeepRelativeTransform);
			OrbMesh->SetRelativeLocation(FVector(Orb.Offset, 0.f));
			OrbMesh->SetMaterial(0, GetMaterialForOrbColor(Orb.Color));
			OrbMesh->SetRelativeScale3D(HeldOrbScale);

			UE_LOG(LogAGArm, Verbose, TEXT("Arm '%s': Created orb mesh for type=%d color=%d offset=(%.2f, %.2f)"), *GetName(), Orb.Type, Orb.Color, Orb.Offset.X, Orb.Offset.Y);
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

	Sim = (Driver ? Driver->GetSim() : nullptr);

	// refresh visuals immediately
	CurrYawDeg = DirToYaw(FacingDir);
	LastYawDeg = CurrYawDeg;
	RefreshPose(1.f);
}

UStaticMesh* ABasicArm::GetMeshForOrbType(uint8 OrbType) const
{
	if (OrbTypeToMesh.Contains(OrbType))
	{
		return OrbTypeToMesh[OrbType];
	}
	// Fallback to default mesh if not found
	return HeldOrbMesh;
}

UMaterialInterface* ABasicArm::GetMaterialForOrbColor(uint8 OrbColor) const
{
	if (OrbColorToMaterial.Contains(OrbColor))
	{
		return OrbColorToMaterial[OrbColor];
	}
	// Fallback to no material if not found
	return nullptr;
}

// Add helper functions near other helpers (e.g. after GetMaterialForOrbColor)
UStaticMesh* ABasicArm::ResolveOrbMesh(const FItem& Item) const
{
	// Prefer cluster: each sub-orb handled individually below; for single just pick once.
	if (Item.bIsCluster)
	{
		// Use HeldOrbMesh as container base (or first sub-orb mesh if available)
		if (HeldOrbMesh) return HeldOrbMesh;
		if (Item.Orbs.Num() > 0)
		{
			uint8 Key = Item.Orbs[0].Type;
			if (OrbTypeToMesh.Contains(Key)) return OrbTypeToMesh[Key];
		}
		return HeldOrbMesh;
	}
	// Single orb
	uint8 Key = Item.Single.Kind != 0 ? Item.Single.Kind : static_cast<uint8>(Item.Single.Type);
	if (OrbTypeToMesh.Contains(Key)) return OrbTypeToMesh[Key];
	return HeldOrbMesh;
}

UMaterialInterface* ABasicArm::ResolveOrbMaterial(const FItem& Item) const
{
	if (Item.bIsCluster)
	{
		if (Item.Orbs.Num() > 0)
		{
			uint8 C = Item.Orbs[0].Color;
			if (OrbColorToMaterial.Contains(C)) return OrbColorToMaterial[C];
		}
		return DefaultOrbMaterial;
	}
	uint8 C = static_cast<uint8>(Item.Single.Color);
	if (OrbColorToMaterial.Contains(C)) return OrbColorToMaterial[C];
	return DefaultOrbMaterial;
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
			if (!Container) Container = HeldOrbMesh;
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
		UE_LOG(LogAGArm, Log, TEXT("Attaching cluster with %d sub-orbs, GrabFacing=%d"), 
			HeldOrb.Orbs.Num(), (int32)HeldOrb.GrabFacing);
		for (const FOrbData& Sub : HeldOrb.Orbs)
		{
			UStaticMeshComponent* SubMesh = NewObject<UStaticMeshComponent>(this);
			SubMesh->RegisterComponent();
			SubMesh->AttachToComponent(HeldOrbComp, FAttachmentTransformRules::KeepRelativeTransform);

			UStaticMesh* SM = OrbTypeToMesh.FindRef(Sub.Type);
			if (!SM) SM = HeldOrbMesh;
			if (!SM)
			{
				static ConstructorHelpers::FObjectFinder<UStaticMesh> SphereMesh(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
				if (SphereMesh.Succeeded()) SM = SphereMesh.Object;
			}
			SubMesh->SetStaticMesh(SM);

			UMaterialInterface* Mat = OrbColorToMaterial.FindRef(Sub.Color);
			if (!Mat) Mat = DefaultOrbMaterial;
			if (Mat) SubMesh->SetMaterial(0, Mat);

			// Rotate the offset based on the arm's facing direction at grab time
			// The offsets are stored in absolute hex coordinates, so we need to rotate them
			// to be relative to the arm's facing direction when grabbed
			FVector2D Off2D = Sub.Offset * OffsetBaseScale;
			
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
			
			// Debug logging to see the offsets
			UE_LOG(LogAGArm, Log, TEXT("Cluster sub-orb: ItemId=%d Offset=(%.2f,%.2f) dQ=%d dR=%d FinalPos=(%.2f,%.2f)"), 
				Sub.ItemId, Sub.Offset.X, Sub.Offset.Y, Sub.dQ, Sub.dR, Off2D.X, Off2D.Y);
			SubMesh->SetRelativeScale3D(HeldOrbScale);

			HeldClusterComps.Add(SubMesh);
		}
		//UpdateHeldClusterTransforms(DirToYaw(FacingDir) + BaseYawOffsetDeg);
		UE_LOG(LogAGArm, Verbose, TEXT("Arm '%s': Attached cluster Id=%d (#=%d) scale(Offsets)=%.2f Tile=%.1f"),
			*GetName(), HeldOrb.Id, HeldOrb.Orbs.Num(), OffsetBaseScale, Tile);
	}
	else
	{
		UStaticMesh* Mesh = ResolveOrbMesh(HeldOrb);
		if (!Mesh) Mesh = HeldOrbMesh;
		if (!Mesh)
		{
			static ConstructorHelpers::FObjectFinder<UStaticMesh> SphereMesh(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
			if (SphereMesh.Succeeded()) Mesh = SphereMesh.Object;
		}
		HeldOrbComp->SetStaticMesh(Mesh);

		UMaterialInterface* Mat = ResolveOrbMaterial(HeldOrb);
		if (!Mat) Mat = DefaultOrbMaterial;
		if (Mat) HeldOrbComp->SetMaterial(0, Mat);
		UE_LOG(LogAGArm, Verbose, TEXT("Arm '%s': Attached single orb Id=%d"), *GetName(), HeldOrb.Id);
	}
}

// UpdateHeldOrbVisual(): relax Id requirement & add logging
void ABasicArm::UpdateHeldOrbVisual()
{
	const int32 CurrentId = bHolding ? (HeldOrb.Id == 0 ? -1 : HeldOrb.Id) : 0; // -1 denotes held with no Id

	if (CurrentId == LastHeldItemId)
		return;

	if (!bHolding)
	{
		DetachHeldOrb();
		LastHeldItemId = 0;
		return;
	}

	AttachHeldOrb(); // always when holding
	LastHeldItemId = CurrentId;
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

void ABasicArm::SyncFromSim()
{
	if (!Sim || SimArmIndex < 0 || SimArmIndex >= Sim->Arms.Num())
	{
		return;
	}
	SyncFromSim(Sim->Arms[SimArmIndex]);
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

