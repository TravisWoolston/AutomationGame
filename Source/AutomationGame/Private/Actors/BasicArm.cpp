#include "Actors/BasicArm.h"
#include "Actors/OrbSimDriver.h"
#include "Framework/GameTickManager.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/World.h"

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

	// Keep CurrYaw in sync with FacingDir if edited in details
	CurrYawDeg = DirToYaw(FacingDir);
	LastYawDeg = CurrYawDeg;
	RefreshPose(1.f);
}

void ABasicArm::BeginPlay()
{
	Super::BeginPlay();

	EnsureComps();
	RefreshMeshes();

	// Capture the starting direction for Reset
	StartFacingDir = FacingDir;
	CurrYawDeg = DirToYaw(FacingDir);
	LastYawDeg = CurrYawDeg;

	// Hook shared fixed tick
	if (AGameTickManager* M = AGameTickManager::Get(this))
	{
		M->OnFixedTick.AddDynamic(this, &ABasicArm::HandleFixedTickSeconds);
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("BasicArm '%s': No GameTickManager found; will not step."), *GetName());
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
	return GridYaw + 60.f * float(Dir % 6);
}

void ABasicArm::RefreshPose(float InAlpha)
{
	const FVector BaseWorld = HexToWorld(FHex(Q, R)) + FVector(0, 0, BaseZ);

	// Yaw interpolation
	const float Yaw = FMath::Lerp(LastYawDeg, CurrYawDeg, InAlpha);
	const FRotator BaseRot(0.f, Yaw, 0.f);

	if (BaseComp)
	{
		BaseComp->SetWorldLocation(BaseWorld);
		BaseComp->SetWorldRotation(BaseRot);
	}

	// Hand sits forward along the facing direction
	const float Tile = Driver ? Driver->GetTileSize() : 100.f;
	const FVector Forward = FRotator(0.f, Yaw, 0.f).RotateVector(FVector(1, 0, 0));
	const FVector HandWorld = BaseWorld + Forward * (Tile * HandReachUU);

	if (HandComp)
	{
		HandComp->SetWorldLocation(HandWorld);
		HandComp->SetWorldRotation(BaseRot);
	}
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

	// Begin a new step interpolation window
	StepSecondsCur = StepSeconds;
	StepAlpha = 0.f;
	LastYawDeg = CurrYawDeg;

	StepProgram();

	// Recompute current yaw from Facing
	CurrYawDeg = DirToYaw(FacingDir);
}

void ABasicArm::ApplyRotateSteps(int32 StepsSigned)
{
	if (StepsSigned == 0) return;
	FacingDir = uint8(Mod6(int32(FacingDir) + StepsSigned));
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
		if (!bHolding)
		{
			// TODO: interact with Sim to actually pick up
			bHolding = true;
		}
		++PC;
		break;

	case EArmCmd::Drop:
		if (bHolding)
		{
			// TODO: interact with Sim to actually place
			bHolding = false;
		}
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
