#include "Actors/BasicWalker.h"
#include "Actors/OrbSimDriver.h"
#include "Framework/GameTickManager.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "Sim/SimWorldHex.h"
#include "EngineUtils.h"

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
	}

	// Interpolate visual position
	float Alpha = bMoving ? MovementProgress : 1.0f;
	RefreshPose(Alpha);
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
	if (!Driver) return GetActorLocation();
	return Driver->MapHexToWorld(H);
}

void ABasicWalker::RefreshPose(float InAlpha)
{
	const FVector CurrentWorld = HexToWorld(CurrentHex) + FVector(0, 0, BaseZ);
	const FVector TargetWorld = HexToWorld(TargetHex) + FVector(0, 0, BaseZ);
	const FVector InterpolatedWorld = FMath::Lerp(CurrentWorld, TargetWorld, InAlpha);

	if (BodyComp)
	{
		BodyComp->SetWorldLocation(InterpolatedWorld);
	}
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

