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

	USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("ArmRoot"));
	SetRootComponent(Root);

	BaseComp = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("ArmBase"));
	BaseComp->SetupAttachment(Root);
	BaseComp->SetMobility(EComponentMobility::Movable);

	HeldOrbComp = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("HeldOrbPreview"));
	HeldOrbComp->SetupAttachment(BaseComp); // Attach directly to BaseComp
	HeldOrbComp->SetMobility(EComponentMobility::Movable);
	HeldOrbComp->SetVisibility(false, true);
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
}

void ABasicArm::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	// Smooth between last and current yaw each fixed step
	if (StepSecondsCur > 0.f)
	{
		StepAlpha += DeltaSeconds / StepSecondsCur;
		StepAlpha = FMath::Clamp(StepAlpha, 0.f, 1.f);
	}
	else
	{
		StepAlpha = 1.f;
	}

	RefreshPose(StepAlpha);

	// --- Debug: Draw line to targeted tile ---
	if (Driver)
	{
		FVector BaseWorld = HexToWorld(FHex(Q, R));
		FHex TargetHex = ComputeTargetHex();
		FVector TargetWorld = HexToWorld(TargetHex);

		// Determine line color based on orb presence
		FColor LineColor = FColor::Red;
		if (Sim && Sim->HasOrbAt(TargetHex))
		{
			LineColor = FColor::Green;
		}

		DrawDebugLine(
			GetWorld(),
			BaseWorld,
			TargetWorld,
			LineColor,
			false,      // persistent lines
			-1.0f,      // lifetime (single frame)
			0,          // depth priority
			4.0f        // thickness
		);

		DrawDebugSphere(
			GetWorld(),
			TargetWorld,
			10.0f,      // radius
			12,         // segments
			FColor::Yellow,
			false,
			-1.0f,
			0,
			2.0f
		);
	}
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
	StepAlpha = 0.f; // Reset only when simulation advances

	StepProgram();   // may update CurrYawDeg
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
	return NormDeg(GridYaw + 60.f * float(Dir % 6));
}

void ABasicArm::RefreshPose(float InAlpha)
{
	const FVector BaseWorld = HexToWorld(FHex(Q, R)) + FVector(0, 0, BaseZ);

	const float StartYaw = NormDeg(LastYawDeg);
	const float TargetYaw = NormDeg(CurrYawDeg);
	const float Delta = FMath::FindDeltaAngleDegrees(StartYaw, TargetYaw);
	const float Yaw = NormDeg(StartYaw + Delta * InAlpha);
	const FRotator BaseRot(0.f, Yaw + BaseYawOffsetDeg, 0.f);

	if (BaseComp)
	{
		BaseComp->SetWorldLocation(BaseWorld);
		BaseComp->SetWorldRotation(BaseRot);
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
}

void ABasicArm::ExecuteCurrent()
{
	const int32 SavedWait = WaitLeft;
	WaitLeft = 0;
	StepProgram();
	WaitLeft = SavedWait;
}



void ABasicArm::ApplyRotateSteps(int32 StepsSigned)
{
	if (StepsSigned == 0) return;
	const uint8 Old = FacingDir;
	FacingDir = uint8(Mod6(int32(FacingDir) + StepsSigned));
	CurrYawDeg = DirToYaw(FacingDir);
	/*UE_LOG(LogAGArm, Verbose, TEXT("Rotate steps=%d : %d -> %d (CurrYaw=%.1f)"),
		StepsSigned, (int32)Old, (int32)FacingDir, CurrYawDeg);*/
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
	//UE_LOG(LogAGArm, Log, TEXT("Arm '%s' Exec %s Arg=%d  (PC=%d/%d)"),
	//	*GetName(), CmdName(I.Cmd), I.Arg, PC, Program.Num()); // <— add

	switch (I.Cmd)
	{
	case EArmCmd::NoOp:
		++PC;
		break;

	case EArmCmd::Wait:
		WaitLeft = FMath::Max(0, I.Arg - 1); // consume one tick now; wait the rest
		++PC;
		break;

		// BasicArm.cpp (in StepProgram, Grab/Drop cases)
	case EArmCmd::Grab:
	{
		if (!bHolding && Sim)
		{
			FItem grabbed;
			const FHex Target = ComputeTargetHex();
			if (Sim->TryArmPickItem(Target, grabbed))
			{
				bHolding = true;
				HeldOrb = grabbed;
				AttachHeldOrb();
			}
		}
		++PC;
		break;
	}

	case EArmCmd::Drop:
	{
		if (bHolding && bAffectSim && Sim)
		{
			const FHex Target = ComputeTargetHex();
			if (Sim->TryArmPlaceItem(Target, FItem{}))
			{
				bHolding = false;
				DetachHeldOrb();
			}
		}
		++PC;
		break;
	}


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
		break;

	default:
		++PC;
		break;
	}
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

void ABasicArm::AttachHeldOrb()
{
	if (HeldOrbComp)
	{
		HeldOrbComp->DestroyComponent();
		HeldOrbComp = nullptr;
	}

	if (!bHolding) return;

	// Handle cluster
	if (HeldOrb.bIsCluster && HeldOrb.Orbs.Num() > 0)
	{
		HeldOrbComp = NewObject<UStaticMeshComponent>(this, TEXT("HeldOrb"));
		HeldOrbComp->RegisterComponent();
		HeldOrbComp->AttachToComponent(BaseComp, FAttachmentTransformRules::SnapToTargetIncludingScale, TEXT("GripSocket"));

		for (const FOrbData& Orb : HeldOrb.Orbs)
		{
			UStaticMeshComponent* OrbMesh = NewObject<UStaticMeshComponent>(this);
			OrbMesh->RegisterComponent();
			OrbMesh->AttachToComponent(HeldOrbComp, FAttachmentTransformRules::KeepRelativeTransform);
			OrbMesh->SetStaticMesh(GetMeshForOrbType(Orb.Type));
			OrbMesh->SetMaterial(0, GetMaterialForOrbColor(Orb.Color));
			OrbMesh->SetRelativeLocation(FVector(Orb.Offset, 0.f));
			OrbMesh->SetRelativeScale3D(HeldOrbScale);
		}
	}
	// Handle single orb
	else if (!HeldOrb.bIsCluster)
	{
		HeldOrbComp = NewObject<UStaticMeshComponent>(this, TEXT("HeldOrb"));
		HeldOrbComp->RegisterComponent();
		HeldOrbComp->AttachToComponent(BaseComp, FAttachmentTransformRules::SnapToTargetIncludingScale, TEXT("GripSocket"));
		HeldOrbComp->SetStaticMesh(GetMeshForOrbType(static_cast<uint8>(HeldOrb.Single.Type)));
		HeldOrbComp->SetMaterial(0, GetMaterialForOrbColor(static_cast<uint8>(HeldOrb.Single.Color)));
		HeldOrbComp->SetRelativeScale3D(HeldOrbScale);
	}
}

void ABasicArm::DetachHeldOrb()
{
	if (HeldOrbComp)
	{
		HeldOrbComp->DestroyComponent();
		HeldOrbComp = nullptr;
	}
}
