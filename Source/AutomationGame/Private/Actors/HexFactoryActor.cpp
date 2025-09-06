#include "Actors/HexFactoryActor.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Actors/OrbSimDriver.h"
#include "Actors/HexBoardActor.h"
#include "Sim/SimWorldHex.h"
#include "Sim/HexCoord.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "DrawDebugHelpers.h"

AHexFactoryActor::AHexFactoryActor()
{
	PrimaryActorTick.bCanEverTick = true;

	USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);

	TileISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("TileISM"));
	TileISM->SetupAttachment(Root);
	TileISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	BeltISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("BeltISM"));
	BeltISM->SetupAttachment(Root);
	BeltISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	BondISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("BondISM"));
	BondISM->SetupAttachment(Root);
	BondISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
}

void AHexFactoryActor::EnsureComponents()
{
	if (!GetRootComponent())
	{
		USceneComponent* Root = NewObject<USceneComponent>(this, TEXT("Root_Runtime"));
		Root->RegisterComponent();
		SetRootComponent(Root);
	}
	auto EnsureISM = [&](UInstancedStaticMeshComponent*& Comp, const TCHAR* Name)
		{
			if (!Comp)
			{
				Comp = NewObject<UInstancedStaticMeshComponent>(this, Name);
				Comp->SetupAttachment(GetRootComponent());
				Comp->RegisterComponent();
				Comp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			}
			Comp->SetVisibility(true, true);
		};
	EnsureISM(TileISM, TEXT("TileISM_Runtime"));
	EnsureISM(BeltISM, TEXT("BeltISM_Runtime"));
	EnsureISM(BondISM, TEXT("BondISM_Runtime"));
}

AActor* AHexFactoryActor::GetAnchor() const
{
	return Driver ? Driver->GetGridAnchor() : nullptr;
}

void AHexFactoryActor::BeginPlay()
{
	Super::BeginPlay();
	EnsureComponents();

	if (UWorld* W = GetWorld())
	{
		Driver = AOrbSimDriver::Find(W);
		if (Driver) Sim = Driver->GetSim();
		for (TActorIterator<AHexBoardActor> It(W); It; ++It)
		{
			Board = *It;
			break;
		}
	}

	RebuildInternal();
}

void AHexFactoryActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!Sim) return;

	const bool bBoardChanged = (Board && Board->BoardRevision != LastSeenBoardRevision);
	if (!bAutoRefresh && !bBoardChanged) return;

	if (Sim->TileRevision != LastSeenRevision || bBoardChanged)
	{
		RebuildInternal();
	}
}

void AHexFactoryActor::Rebuild()
{
	RebuildInternal();
}

FVector AHexFactoryActor::HexToWorld(const FHex& H) const
{
	const AActor* Anchor = GetAnchor();
	const FVector AnchorV = Anchor ? Anchor->GetActorLocation() : FVector::ZeroVector;
	const float Tile = Driver ? Driver->GetTileSize() : 100.f;
	const float GridYaw = Driver ? Driver->GetGridYaw() : 0.f;
	const EHexOrientation Ori = Driver ? Driver->GetGridOrientation() : EHexOrientation::PointyTop;

	const FVector2D LocalXY = HexCoord::AxialToXY(FHexAxial(H.Q, H.R), Tile, Ori);
	const FVector2D RotXY = HexCoord::Rotate2D(LocalXY, GridYaw);
	return AnchorV + FVector(RotXY.X, RotXY.Y, 0.f);
}

void AHexFactoryActor::RebuildInternal()
{
	EnsureComponents();
	if (!Sim) return;

	LastSeenRevision = Sim->TileRevision;
	LastSeenBoardRevision = Board ? Board->BoardRevision : -1;

	if (TileMesh) TileISM->SetStaticMesh(TileMesh);
	if (TileMaterial) TileISM->SetMaterial(0, TileMaterial);
	if (BeltMesh) BeltISM->SetStaticMesh(BeltMesh);
	if (BeltMaterial) BeltISM->SetMaterial(0, BeltMaterial);
	if (BondMesh) BondISM->SetStaticMesh(BondMesh);
	if (BondMaterial) BondISM->SetMaterial(0, BondMaterial);

	TileISM->ClearInstances();
	BeltISM->ClearInstances();
	BondISM->ClearInstances();

	RebuildTiles();
	RebuildBelts();
	RebuildBonds();   // NEW
}

void AHexFactoryActor::RebuildTiles()
{
	if (!Board) return;
	const float Tile = Driver ? Driver->GetTileSize() : 100.f;
	const float GridYaw = Driver ? Driver->GetGridYaw() : 0.f;
	const EHexOrientation Ori = Driver ? Driver->GetGridOrientation() : EHexOrientation::PointyTop;
	const AActor* Anchor = GetAnchor();
	const FVector AnchorV = Anchor ? Anchor->GetActorLocation() : FVector::ZeroVector;

	for (const FHex& H : Board->GetAllHexes())
	{
		const FVector2D LocalXY = HexCoord::AxialToXY(FHexAxial(H.Q, H.R), Tile, Ori);
		const FVector2D RotXY = HexCoord::Rotate2D(LocalXY, GridYaw);
		const FVector Loc = AnchorV + FVector(RotXY.X, RotXY.Y, 0.f);
		if (TileMesh)
			TileISM->AddInstance(FTransform(FRotator::ZeroRotator, Loc, FVector(1.f)));
	}
}

void AHexFactoryActor::DrawBeltArrow(const FVector& Center, float TileRadiusUU, float YawDeg)
{
	const float Len = TileRadiusUU * DebugArrowLenScale;
	const FVector Dir = FRotator(0.f, YawDeg, 0.f).RotateVector(FVector(1, 0, 0));
	const FVector A = Center + FVector(0, 0, BeltZOffset + 2.f);
	const FVector B = A + Dir * Len;
	DrawDebugDirectionalArrow(GetWorld(), A, B, 16.f, FColor::Yellow, false, 1.0f, 0, DebugArrowThickness);
}

void AHexFactoryActor::DrawBondArrow(const FVector& A, const FVector& B)
{
	DrawDebugLine(GetWorld(), A + FVector(0, 0, BondZOffset + 2.f), B + FVector(0, 0, BondZOffset + 2.f), FColor::Cyan, false, 1.0f, 0, 2.f);
}

void AHexFactoryActor::RebuildBelts()
{
	if (!Sim) return;
	const float Tile = Driver ? Driver->GetTileSize() : 100.f;
	const float GridYaw = Driver ? Driver->GetGridYaw() : 0.f;
	const EHexOrientation Ori = Driver ? Driver->GetGridOrientation() : EHexOrientation::PointyTop;
	const AActor* Anchor = GetAnchor();
	const FVector AnchorV = Anchor ? Anchor->GetActorLocation() : FVector::ZeroVector;

	auto ForwardYaw = [&](const FHex& Base, uint8 Dir)->float
		{
			const FHex Fwd = HexUtil::Neighbor(Base, (EHexDir)(Dir % 6));
			const FVector2D Axy = HexCoord::Rotate2D(HexCoord::AxialToXY(FHexAxial(Base.Q, Base.R), Tile, Ori), GridYaw);
			const FVector2D Bxy = HexCoord::Rotate2D(HexCoord::AxialToXY(FHexAxial(Fwd.Q, Fwd.R), Tile, Ori), GridYaw);
			const FVector2D Delta = Bxy - Axy;
			return Delta.IsNearlyZero() ? 0.f : FMath::RadiansToDegrees(FMath::Atan2(Delta.Y, Delta.X));
		};

	for (const auto& KV : Sim->TileMap)
	{
		const FHex& H = KV.Key;
		const FTile& T = KV.Value;
		if (T.Kind != ETileKind::Belt) continue;

		const FVector2D LocalXY = HexCoord::AxialToXY(FHexAxial(H.Q, H.R), Tile, Ori);
		const FVector2D RotXY = HexCoord::Rotate2D(LocalXY, GridYaw);
		const FVector Loc = AnchorV + FVector(RotXY.X, RotXY.Y, BeltZOffset);
		const float Yaw = ForwardYaw(H, (uint8)(T.Arg & 0xFF));

		if (BeltMesh)
		{
			BeltISM->AddInstance(FTransform(FRotator(0.f, Yaw, 0.f), Loc, BeltScale));
			if (bAlwaysDrawBeltArrows)
				DrawBeltArrow(Loc, Tile, Yaw);
		}
	}
}

void AHexFactoryActor::RebuildBonds()
{
	if (!Sim) return;
	const float Tile = Driver ? Driver->GetTileSize() : 100.f;
	const float GridYaw = Driver ? Driver->GetGridYaw() : 0.f;
	const EHexOrientation Ori = Driver ? Driver->GetGridOrientation() : EHexOrientation::PointyTop;
	const AActor* Anchor = GetAnchor();
	const FVector AnchorV = Anchor ? Anchor->GetActorLocation() : FVector::ZeroVector;

	for (const auto& KV : Sim->TileMap)
	{
		const FHex& H = KV.Key;
		const FTile& T = KV.Value;
		if (T.Kind != ETileKind::Bond) continue;

		const uint8 Dir = (uint8)(T.Arg & 0xFF);
		const FHex N = HexUtil::Neighbor(H, (EHexDir)(Dir % 6));

		// World positions of each hex center
		const FVector2D AxyLocal = HexCoord::AxialToXY(FHexAxial(H.Q, H.R), Tile, Ori);
		const FVector2D BxyLocal = HexCoord::AxialToXY(FHexAxial(N.Q, N.R), Tile, Ori);
		const FVector2D Axy = HexCoord::Rotate2D(AxyLocal, GridYaw);
		const FVector2D Bxy = HexCoord::Rotate2D(BxyLocal, GridYaw);
		const FVector WorldA = AnchorV + FVector(Axy.X, Axy.Y, BondZOffset);
		const FVector WorldB = AnchorV + FVector(Bxy.X, Bxy.Y, BondZOffset);
		const FVector Mid = (WorldA + WorldB) * 0.5f;
		const FVector2D Delta2D(Bxy - Axy);
		const float Yaw = Delta2D.IsNearlyZero()
			? 0.f
			: FMath::RadiansToDegrees(FMath::Atan2(Delta2D.Y, Delta2D.X));

		if (BondMesh)
		{
			BondISM->AddInstance(FTransform(FRotator(0.f, Yaw, 0.f), Mid, BondScale));
			if (bDrawBondDebugArrows)
				DrawBondArrow(WorldA, WorldB);
		}
	}
}
