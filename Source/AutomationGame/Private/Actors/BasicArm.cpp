#include "Actors/BasicArm.h"
#include "Actors/OrbSimDriver.h"
#include "Framework/GameTickManager.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"

ABasicArm::ABasicArm()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;
}

void ABasicArm::EnsureComps()
{
	if (!RootComponent)
	{
		auto* Root = NewObject<USceneComponent>(this, TEXT("Root"));
		SetRootComponent(Root);
		Root->RegisterComponent();
	}

	if (!BaseComp)
	{
		BaseComp = NewObject<UStaticMeshComponent>(this, TEXT("ArmBase"));
		BaseComp->SetupAttachment(RootComponent);
		BaseComp->RegisterComponent();
		BaseComp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	}
	if (!HandComp)
	{
		HandComp = NewObject<UStaticMeshComponent>(this, TEXT("ArmHand"));
		HandComp->SetupAttachment(BaseComp);
		HandComp->RegisterComponent();
		HandComp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	}
}

void ABasicArm::RefreshMeshes()
{
	if (BaseComp && BaseMesh) BaseComp->SetStaticMesh(BaseMesh);
	if (HandComp && HandMesh) HandComp->SetStaticMesh(HandMesh);
}

void ABasicArm::OnConstruction(const FTransform& Xform)
{
	Super::OnConstruction(Xform);
	EnsureComps();
	RefreshMeshes();

	// Set initial yaws
	CurrYawDeg = DirToYaw(FacingDir);
	LastYawDeg = CurrYawDeg;

	RefreshPose(0.f);
}

void ABasicArm::BeginPlay()
{
	Super::BeginPlay();

	EnsureComps();
	RefreshMeshes();

	// Autodetect driver if not assigned
	if (!Driver)
	{
		if (UWorld* W = GetWorld())
			for (TActorIterator<AOrbSimDriver> It(W); It; ++It) { Driver = *It; break; }
	}

	// Bind to fixed tick (pause/play friendly)
	if (AGameTickManager* M = AGameTickManager::Get(this))
	{
		M->OnFixedTick.AddDynamic(this, &ABasicArm::HandleFixedTickSeconds);
	}

	// Initial pose
	CurrYawDeg = DirToYaw(FacingDir);
	LastYawDeg = CurrYawDeg;
	RefreshPose(0.f);
}

void ABasicArm::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	// advance interpolation inside the current fixed step
	if (StepSecondsCur > 0.f)
	{
		StepAlpha = FMath::Min(1.f, StepAlpha + (DeltaSeconds / StepSecondsCur));
	}
	RefreshPose(StepAlpha);
}

void ABasicArm::HandleFixedTickSeconds(float StepSeconds)
{
	StepSecondsCur = FMath::Max(0.0001f, StepSeconds);
	StepAlpha = 0.f;
	LastYawDeg = CurrYawDeg;

	if (bRunning)
	{
		StepProgram();
	}

	// Recompute target yaw from FacingDir after possible rotate
	CurrYawDeg = DirToYaw(FacingDir);
}

void ABasicArm::SetProgram(const TArray<FArmInstr>& InProgram, bool bLoop)
{
	Program = InProgram;
	bLoopProgram = bLoop;
	ResetProgram();
}

void ABasicArm::Start() { bRunning = true; }
void ABasicArm::Stop() { bRunning = false; }
void ABasicArm::ResetProgram()
{
	PC = 0;
	WaitLeft = 0;
	bHolding = false;
}

void ABasicArm::ExecuteCurrent()
{
	if (Program.IsValidIndex(PC))
	{
		const FArmInstr& I = Program[PC];
		switch (I.Cmd)
		{
		case EArmCmd::Grab:      bHolding = true;  break;
		case EArmCmd::Drop:      bHolding = false; break;
		case EArmCmd::RotateCW:  ApplyRotateSteps(+FMath::Max(1, I.Arg)); break;
		case EArmCmd::RotateCCW: ApplyRotateSteps(-FMath::Max(1, I.Arg)); break;
		case EArmCmd::Wait:      /*ignored here*/ break;
		default: break;
		}
		PC++;
		if (PC >= Program.Num())
		{
			if (bLoopProgram) PC = 0; else bRunning = false;
		}
	}
}

void ABasicArm::StepProgram()
{
	if (WaitLeft > 0)
	{
		WaitLeft--;
		return;
	}

	if (!Program.IsValidIndex(PC)) { if (bLoopProgram) PC = 0; else { bRunning = false; } return; }

	const FArmInstr& I = Program[PC];

	switch (I.Cmd)
	{
	case EArmCmd::NoOp:
		break;
	case EArmCmd::Grab:
		bHolding = true;
		break;
	case EArmCmd::Drop:
		bHolding = false;
		break;
	case EArmCmd::RotateCW:
		ApplyRotateSteps(+FMath::Max(1, I.Arg));
		break;
	case EArmCmd::RotateCCW:
		ApplyRotateSteps(-FMath::Max(1, I.Arg));
		break;
	case EArmCmd::Wait:
		WaitLeft = FMath::Max(1, I.Arg);
		break;
	default: break;
	}

	// advance PC unless we just started a Wait
	if (I.Cmd != EArmCmd::Wait)
	{
		PC++;
		if (PC >= Program.Num())
		{
			if (bLoopProgram) PC = 0; else bRunning = false;
		}
	}
}

void ABasicArm::ApplyRotateSteps(int32 StepsSigned)
{
	// Wrap into 0..5
	const int32 Steps = (StepsSigned % 6 + 6) % 6;
	FacingDir = uint8((int32(FacingDir) + StepsSigned) % 6);
	if (int32(FacingDir) < 0) FacingDir = uint8(int32(FacingDir) + 6);
	// CurrYawDeg will be recomputed after this instruction (in HandleFixedTickSeconds)
}

FVector ABasicArm::HexToWorld(const FHex& H) const
{
	if (!Driver) return GetActorLocation();

	const float Tile = Driver->GetTileSize();
	const float Yaw = Driver->GetGridYaw();
	const EHexOrientation Ori = Driver->GetGridOrientation();
	const AActor* Anchor = Driver->GetGridAnchor();
	const FVector AnchorLoc = Anchor ? Anchor->GetActorLocation() : FVector::ZeroVector;

	const FVector2D XY = HexCoord::AxialToXY(FHexAxial(H.Q, H.R), Tile, Ori);
	const FVector2D RotXY = HexCoord::Rotate2D(XY, Yaw);
	return AnchorLoc + FVector(RotXY.X, RotXY.Y, 0.f);
}

float ABasicArm::DirToYaw(uint8 Dir) const
{
	const float GridYaw = Driver ? Driver->GetGridYaw() : 0.f;
	return GridYaw + 60.f * float(Dir % 6);
}

void ABasicArm::RefreshPose(float InAlpha)
{
	EnsureComps();

	const FVector Pivot = HexToWorld(FHex{ Q, R }) + FVector(0, 0, BaseZ);

	// Yaw interpolates from LastYawDeg to CurrYawDeg within the step (looks smooth even when paused/resumed)
	const float YawNow = FMath::Lerp(LastYawDeg, CurrYawDeg, FMath::Clamp(InAlpha, 0.f, 1.f));

	if (BaseComp)
	{
		BaseComp->SetWorldTransform(FTransform(
			FRotator(0.f, YawNow, 0.f),
			Pivot,
			BaseScale));
	}

	if (HandComp)
	{
		// Hand sits forward along facing, at HandReachUU * tile
		const float Tile = Driver ? Driver->GetTileSize() : 100.f;
		const FVector Dir = FRotator(0.f, YawNow, 0.f).RotateVector(FVector(1, 0, 0));
		const FVector HandLoc = Pivot + Dir * (Tile * HandReachUU);

		HandComp->SetWorldTransform(FTransform(
			FRotator::ZeroRotator,
			HandLoc,
			HandScale));

		HandComp->SetVisibility(bHolding, true); // show while “holding” something
	}
}
