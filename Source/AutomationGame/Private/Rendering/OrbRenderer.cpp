#include "Rendering/OrbRenderer.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Actors/OrbSimDriver.h"
#include "Sim/SimWorldHex.h"
#include "Sim/HexTypes.h"
#include "Sim/HexCoord.h"
#include "Engine/World.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "UObject/ConstructorHelpers.h"
#include "Engine/Engine.h"
#include "GameFramework/Actor.h"

UOrbRenderer::UOrbRenderer()
{
	PrimaryComponentTick.bCanEverTick = true;

	static ConstructorHelpers::FObjectFinder<UStaticMesh> SphereMesh(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> BasicMat(TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	if (!OrbMesh && SphereMesh.Succeeded()) OrbMesh = SphereMesh.Object;
	if (!OrbMaterial && BasicMat.Succeeded()) OrbMaterial = BasicMat.Object;
}

void UOrbRenderer::OnRegister()
{
	Super::OnRegister();
	EnsureISM();
}

void UOrbRenderer::BeginPlay()
{
	Super::BeginPlay();

	EnsureISM();

	if (UWorld* W = GetWorld())
	{
		Driver = AOrbSimDriver::Find(W);
		if (Driver) Sim = Driver->GetSim();
	}

	RebuildCurrPos();
	PrevPos = CurrPos;
}

void UOrbRenderer::EnsureISM()
{
	if (ISM) return;

	AActor* Owner = GetOwner();
	if (!Owner) return;

	ISM = NewObject<UInstancedStaticMeshComponent>(Owner, TEXT("OrbRenderer_ISM"));
	ISM->RegisterComponent();

	if (Owner->GetRootComponent())
	{
		ISM->AttachToComponent(Owner->GetRootComponent(), FAttachmentTransformRules::KeepRelativeTransform);
	}

	if (OrbMesh) ISM->SetStaticMesh(OrbMesh);
	if (OrbMaterial) ISM->SetMaterial(0, OrbMaterial);
	ISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
}

AActor* UOrbRenderer::GetAnchor() const
{
	if (bOverrideAnchor && AnchorOverride) return AnchorOverride;
	return Driver ? Driver->GetGridAnchor() : nullptr;
}

void UOrbRenderer::TickComponent(float DeltaTime, enum ELevelTick, FActorComponentTickFunction*)
{
	if (bAutoReacquire && (!Driver || !Sim))
	{
		if (UWorld* W = GetWorld())
		{
			Driver = AOrbSimDriver::Find(W);
			if (Driver) Sim = Driver->GetSim();
			if (Sim) { RebuildCurrPos(); PrevPos = CurrPos; }
		}
	}
	if (!Driver) return;
	const float Alpha = Driver->GetStepAlpha();

	// If a new step started, update only moved orbs
	if (Alpha < PrevAlpha)
	{
		// Instead of: PrevPos = CurrPos;
		// Do:
		for (const auto& KV : CurrPos)
		{
			int32 Id = KV.Key;
			const FVector& Curr = KV.Value;
			const FVector* Prev = PrevPos.Find(Id);
			if (!Prev || !Prev->Equals(Curr, 0.01f))
			{
				PrevPos.Add(Id, Curr); // Only update if moved
			}
		}
		PrevRot = CurrRot;
		RebuildCurrPos();
	}
	PrevAlpha = Alpha;

	RebuildInstances();
}

void UOrbRenderer::RebuildCurrPos()
{
	CurrPos.Reset();
	CurrRot.Reset();
	if (!Sim) return;

	const AActor* Anchor = GetAnchor();
	const FVector AnchorV = Anchor ? Anchor->GetActorLocation() : FVector::ZeroVector;
	const float Tile = Driver ? Driver->GetTileSize() : Sim->TileSizeUU;
	const float Yaw = Driver ? Driver->GetGridYaw() : 0.f;
	const EHexOrientation Ori = Driver ? Driver->GetGridOrientation() : EHexOrientation::PointyTop;

	for (const auto& KV : Sim->ItemMap)
	{
		const int32 Id = KV.Value.Id;
		if (Id == 0) continue;
		const FHexAxial A(KV.Key.Q, KV.Key.R);
		const FVector2D LocalXY = HexCoord::AxialToXY(A, Tile, Ori);
		const FVector2D RotXY = HexCoord::Rotate2D(LocalXY, Yaw);
		const FVector Loc = AnchorV + FVector(RotXY.X, RotXY.Y, 0.f);
		CurrPos.Add(Id, Loc);
	}

	for (const FArmState& S : Sim->Arms)
	{
		if (!S.bHolding) continue;
		if (S.Held.Id != 0)
		{
			UE_LOG(LogTemp, Log, TEXT("Rendering held orb Id=%d at arm hand"), S.Held.Id);
		}
		FHex Grip = S.BaseHex;
		for (int i = 0; i < S.ArmLen; ++i) Grip = HexUtil::Neighbor(Grip, S.Facing);
		const FHexAxial A(Grip.Q, Grip.R);
		const FVector2D LocalXY = HexCoord::AxialToXY(A, Tile, Ori);
		const FVector2D RotXY = HexCoord::Rotate2D(LocalXY, Yaw);
		const FVector Loc = AnchorV + FVector(RotXY.X, RotXY.Y, 0.f);

		float OrbYaw = DirToYaw(static_cast<uint8>(S.Facing));
		FRotator OrbRot(0.f, OrbYaw, 0.f);

		if (S.Held.Id != 0)
		{
			CurrPos.Add(S.Held.Id, Loc);
			CurrRot.Add(S.Held.Id, OrbRot);
		}
	}
}

void UOrbRenderer::RebuildInstances()
{
	EnsureISM();
	if (!ISM || !Driver) return;
	const float Alpha = Driver->GetStepAlpha();

	TSet<int32> Keys;
	PrevPos.GetKeys(Keys);
	TArray<int32> CurrKeys; CurrPos.GetKeys(CurrKeys);
	for (int32 K : CurrKeys) Keys.Add(K);

	ISM->ClearInstances();
	if (!OrbMesh) return;

	for (int32 Id : Keys)
	{
		const FVector* A = PrevPos.Find(Id);
		const FVector* B = CurrPos.Find(Id);
		const FRotator* RotA = PrevRot.Find(Id);
		const FRotator* RotB = CurrRot.Find(Id);

		const FVector From = A ? *A : (B ? *B : FVector::ZeroVector);
		const FVector To = B ? *B : From;

		FVector P;
		// Check if this orb is currently held by an arm
		bool bHeldByArm = false;
		FVector ArmBase;
		for (const FArmState& S : Sim->Arms)
		{
			if (S.bHolding && S.Held.Id == Id)
			{
				// Arm base world position
				FHex BaseHex = S.BaseHex;
				const AActor* Anchor = GetAnchor();
				const FVector AnchorV = Anchor ? Anchor->GetActorLocation() : FVector::ZeroVector;
				const float Tile = Driver ? Driver->GetTileSize() : Sim->TileSizeUU;
				const EHexOrientation Ori = Driver ? Driver->GetGridOrientation() : EHexOrientation::PointyTop;
				const FVector2D LocalXY = HexCoord::AxialToXY(FHexAxial(BaseHex.Q, BaseHex.R), Tile, Ori);
				const FVector2D RotXY = HexCoord::Rotate2D(LocalXY, Driver->GetGridYaw());
				ArmBase = AnchorV + FVector(RotXY.X, RotXY.Y, 0.f);
				bHeldByArm = true;
				break;
			}
		}

		//if (bHeldByArm)
		//{
		//	// Interpolate along arc from From to To, centered at ArmBase
		//	P = InterpArc(ArmBase, From, To, Alpha);
		//}
	//else
	//{
		// Belt or normal movement: straight lerp
		P = FMath::Lerp(From, To, Alpha);
	//}

	FRotator RA = RotA ? *RotA : FRotator::ZeroRotator;
	FRotator RB = RotB ? *RotB : RA;
	FRotator R = FMath::Lerp(RA, RB, Alpha);

	ISM->AddInstance(FTransform(R, P, FVector(OrbScale)));
	}
}

float UOrbRenderer::DirToYaw(uint8 Dir) const
{
	const float GridYaw = Driver ? Driver->GetGridYaw() : 0.f;
	return FMath::UnwindDegrees(GridYaw + 60.f * float(Dir % 6));
}


