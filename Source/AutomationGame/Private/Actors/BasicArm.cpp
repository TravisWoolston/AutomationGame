#include "Actors/BasicArm.h"
#include "Actors/OrbSimDriver.h"
#include "Framework/GameTickManager.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/World.h"

// Top of file (after includes)
static inline float NormDeg(float D) { return FMath::UnwindDegrees(D); }
namespace { inline int32 Mod6(int32 V) { return (V % 6 + 6) % 6; } }

ABasicArm::ABasicArm()
{
	PrimaryActorTick.bCanEverTick = true;   // we interpolate visuals in Tick
	USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("ArmRoot"));
	SetRootComponent(Root);

	BaseComp = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("ArmBase"));
	BaseComp->SetupAttachment(Root);
	BaseComp->SetMobility(EComponentMobility::Movable);

	HandComp = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("ArmHand"));
	HandComp->SetupAttachment(BaseComp);
	HandComp->SetMobility(EComponentMobility::Movable);
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

	CurrYawDeg = DirToYaw(FacingDir);
	LastYawDeg = CurrYawDeg;

	Sim = (Driver ? Driver->GetSim() : nullptr);                        // <- new
	if (!Sim)
	{
		UE_LOG(LogTemp, Warning, TEXT("BasicArm '%s': No Sim found; Grab/Drop will be stubbed."), *GetName()); // <- new
	}
	if (AGameTickManager* M = AGameTickManager::Get(this))
	{
		// guard against double-binding
		M->OnFixedTick.RemoveDynamic(this, &ABasicArm::HandleFixedTickSeconds);
		M->OnFixedTick.AddDynamic(this, &ABasicArm::HandleFixedTickSeconds);
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("BasicArm '%s': No GameTickManager; will not step."), *GetName());
	}

	RefreshPose(1.f);
}


void ABasicArm::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	// Smooth between last and current yaw each fixed step
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
		BaseComp->SetupAttachment(GetRootComponent() ? GetRootComponent() : (USceneComponent*)CreateDefaultSubobject<USceneComponent>(TEXT("Root")));
		BaseComp->RegisterComponent();
	}
	if (!HandComp)
	{
		HandComp = NewObject<UStaticMeshComponent>(this, TEXT("ArmHand"));
		HandComp->SetupAttachment(BaseComp);
		HandComp->RegisterComponent();
	}
}

void ABasicArm::RefreshMeshes()
{
	if (BaseComp) { BaseComp->SetStaticMesh(BaseMesh); BaseComp->SetRelativeScale3D(BaseScale); }
	if (HandComp) { HandComp->SetStaticMesh(HandMesh); HandComp->SetRelativeScale3D(HandScale); }
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
	return NormDeg(GridYaw + 60.f * float(Dir % 6));
}


void ABasicArm::RefreshPose(float InAlpha)
{
	const FVector BaseWorld = HexToWorld(FHex(Q, R)) + FVector(0, 0, BaseZ);

	const float StartYaw = NormDeg(LastYawDeg);
	const float TargetYaw = NormDeg(CurrYawDeg);
	const float Delta = FMath::FindDeltaAngleDegrees(StartYaw, TargetYaw);
	const float Yaw = NormDeg(StartYaw + Delta * InAlpha);
	const FRotator BaseRot(0.f, Yaw, 0.f);

	if (BaseComp)
	{
		BaseComp->SetWorldLocation(BaseWorld);
		BaseComp->SetWorldRotation(BaseRot);
	}

	const float Tile = Driver ? Driver->GetTileSize() : 100.f;
	const FVector Forward = FRotator(0.f, Yaw, 0.f).RotateVector(FVector(1, 0, 0));
	const FVector HandWorld = BaseWorld + Forward * (Tile * HandReachUU);

	if (HandComp)
	{
		HandComp->SetWorldLocation(HandWorld);
		HandComp->SetWorldRotation(BaseRot);
	}
}
void ABasicArm::TryGrabAtTarget()
{
	const FHex H = ComputeTargetHex();
	if (!Sim)
	{
		// Stub: pretend success so you can test arm logic
		bHolding = true;
		UE_LOG(LogTemp, Log, TEXT("BasicArm '%s': [STUB] Grab at (%d,%d) -> holding=1"), *GetName(), H.Q, H.R);
		return;
	}

	// TODO: replace with real Sim call when available, e.g. Sim->TryArmPick(H, OutPayload)
	// For now: optimistic toggle with log
	bHolding = true;
	UE_LOG(LogTemp, Log, TEXT("BasicArm '%s': Grab at (%d,%d) -> holding=1"), *GetName(), H.Q, H.R);
}

void ABasicArm::TryDropAtTarget()
{
	const FHex H = ComputeTargetHex();
	if (!Sim)
	{
		bHolding = false;
		UE_LOG(LogTemp, Log, TEXT("BasicArm '%s': [STUB] Drop at (%d,%d) -> holding=0"), *GetName(), H.Q, H.R);
		return;
	}

	// TODO: replace with real Sim call when available, e.g. Sim->TryArmPlace(H, Payload)
	bHolding = false;
	UE_LOG(LogTemp, Log, TEXT("BasicArm '%s': Drop at (%d,%d) -> holding=0"), *GetName(), H.Q, H.R);
}

void ABasicArm::SetProgram(const TArray<FArmInstr>& InProgram, bool bLoop /*= true*/)
{
	Program = InProgram;
	bLoopProgram = bLoop;
	PC = 0;
	WaitLeft = 0;
}

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
	// Do not change FacingDir or bHolding here; Reset is a program cmd for that.
}

void ABasicArm::ExecuteCurrent()
{
	// One immediate step ignoring Wait
	const int32 SavedWait = WaitLeft;
	WaitLeft = 0;
	StepProgram();
	WaitLeft = SavedWait;
}

void ABasicArm::HandleFixedTickSeconds(float StepSeconds)
{
	if (!bRunning) { StepAlpha = 1.f; return; }

	StepSecondsCur = StepSeconds;

	// Lock the rendered yaw as the new "start" of the step (wrap-safe)
	const float RenderedNow = NormDeg(
		LastYawDeg + FMath::FindDeltaAngleDegrees(LastYawDeg, CurrYawDeg) * StepAlpha
	);
	LastYawDeg = RenderedNow;
	StepAlpha = 0.f;

	StepProgram();   // may update CurrYawDeg
}


void ABasicArm::ApplyRotateSteps(int32 StepsSigned)
{
	if (StepsSigned == 0) return;
	FacingDir = uint8(Mod6(int32(FacingDir) + StepsSigned));
	CurrYawDeg = NormDeg(DirToYaw(FacingDir));
}

FHex ABasicArm::DirStep(uint8 Dir) const  // returns axial step for the given dir
{
	const uint8 D = Dir % 6;
	// Our Dir->Yaw uses 0=+X (east), +60° CW each step. For axial:
	// Pointy-top neighbors (q,r): E(1,0), NE(1,-1), NW(0,-1), W(-1,0), SW(-1,1), SE(0,1)
	static const FHex Pointy[6] = {
		FHex(1,  0), FHex(1, -1), FHex(0, -1),
		FHex(-1,  0), FHex(-1,  1), FHex(0,  1)
	};
	// Flat-top neighbors if you ever switch orientation:
	// E(1,-1), ESE(1,0), WSE(0,1), W(-1,1), WNW(-1,0), ENW(0,-1)
	static const FHex Flat[6] = {
		FHex(1, -1), FHex(1,  0), FHex(0,  1),
		FHex(-1,  1), FHex(-1,  0), FHex(0, -1)
	};

	const bool bPointy = !Driver || Driver->GetGridOrientation() == EHexOrientation::PointyTop;
	return bPointy ? Pointy[D] : Flat[D];
}

FHex ABasicArm::ComputeTargetHex() const
{
	FHex T(Q, R);
	const FHex Step = DirStep(FacingDir);
	const int32 N = FMath::Max(1, ReachTiles);
	for (int32 i = 0; i < N; ++i)
	{
		T.Q += Step.Q;
		T.R += Step.R;
	}
	return T;
}
void ABasicArm::ExpandResetAtPC()
{
	// Drop first if holding
	TArray<FArmInstr> Expansion;
	if (bHolding)
	{
		Expansion.Add({ EArmCmd::Drop, 0 });
	}

	// Compute minimal rotation to StartFacingDir
	const int32 DiffCW = Mod6(int32(StartFacingDir) - int32(FacingDir)); // 0..5 steps CW
	const int32 DiffCCW = Mod6(int32(FacingDir) - int32(StartFacingDir)); // 0..5 steps CCW

	const bool bUseCW = (DiffCW <= DiffCCW);
	const int32 Steps = bUseCW ? DiffCW : DiffCCW;

	// Expand into single-step rotates so the program shows/executes exactly what happens
	for (int32 i = 0; i < Steps; ++i)
	{
		Expansion.Add({ bUseCW ? EArmCmd::RotateCW : EArmCmd::RotateCCW, 1 });
	}

	// Replace Reset at PC with the expansion
	Program.RemoveAt(PC);
	if (Expansion.Num() > 0)
	{
		Program.Insert(Expansion, PC);
	}

	// (Intentionally do not ++PC; next fixed tick will execute the first inserted op)
	UE_LOG(LogTemp, Verbose, TEXT("BasicArm '%s' Reset expanded: Holding=%d, From=%d To=%d, Dir=%s, Steps=%d"),
		*GetName(), bHolding ? 1 : 0, int32(FacingDir), int32(StartFacingDir),
		bUseCW ? TEXT("CW") : TEXT("CCW"), Steps);
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

	switch (I.Cmd)
	{
	case EArmCmd::NoOp:
		++PC;
		break;

	case EArmCmd::Wait:
		WaitLeft = FMath::Max(0, I.Arg - 1); // consume one tick now; wait the rest
		++PC;
		break;

	case EArmCmd::Grab:
		if (!bHolding) { TryGrabAtTarget(); }  // <- new
		++PC;
		break;

	case EArmCmd::Drop:
		if (bHolding) { TryDropAtTarget(); }   // <- new
		++PC;
		break;

	case EArmCmd::RotateCW:
		ApplyRotateSteps(FMath::Max(1, I.Arg));
		++PC;
		break;

	case EArmCmd::RotateCCW:
		ApplyRotateSteps(-FMath::Max(1, I.Arg));
		++PC;
		break;

	case EArmCmd::Reset:
		ExpandResetAtPC();
		// no PC++ here; we just rewrote the stream at PC
		break;

	default:
		++PC;
		break;
	}
}
