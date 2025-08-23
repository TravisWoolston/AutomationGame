#include "Actors/HexFactoryActor.h"            // own header first
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Actors/OrbSimDriver.h"
#include "Actors/HexBoardActor.h"
#include "Sim/SimWorldHex.h"
#include "Sim/HexTypes.h"
#include "Sim/Tiles.h"
#include "Sim/HexCoord.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "EngineUtils.h"
#include "DrawDebugHelpers.h"

namespace { static inline float DirYawDeg(uint8 Dir) { return 60.f * (Dir % 6); } }

AHexFactoryActor::AHexFactoryActor()
{
    PrimaryActorTick.bCanEverTick = true;

    USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
    SetRootComponent(Root);

    TileISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("TileISM"));
    TileISM->SetupAttachment(Root);
    TileISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    TileISM->SetCastShadow(false);

    BeltISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("BeltISM"));
    BeltISM->SetupAttachment(Root);
    BeltISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    BeltISM->SetCastShadow(false);
}

void AHexFactoryActor::EnsureComponents()
{
    if (!GetRootComponent())
    {
        USceneComponent* Root = NewObject<USceneComponent>(this, TEXT("Root_Runtime"));
        Root->RegisterComponent();
        SetRootComponent(Root);
    }
    if (!TileISM)
    {
        TileISM = NewObject<UInstancedStaticMeshComponent>(this, TEXT("TileISM_Runtime"));
        TileISM->SetupAttachment(GetRootComponent());
        TileISM->RegisterComponent();
        TileISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        TileISM->SetCastShadow(false);
    }
    if (!BeltISM)
    {
        BeltISM = NewObject<UInstancedStaticMeshComponent>(this, TEXT("BeltISM_Runtime"));
        BeltISM->SetupAttachment(GetRootComponent());
        BeltISM->RegisterComponent();
        BeltISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        BeltISM->SetCastShadow(false);
    }
    BeltISM->SetVisibility(true, true);
}

AActor* AHexFactoryActor::GetAnchor() const
{
    if (bOverrideAnchor && AnchorOverride) return AnchorOverride;
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
        if (!Board)
        {
            for (TActorIterator<AHexBoardActor> It(W); It; ++It) { Board = *It; break; }
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

    TileISM->ClearInstances();
    BeltISM->ClearInstances();

    RebuildTiles();
    RebuildBelts();
}

void AHexFactoryActor::RebuildTiles()
{
    // Ground comes from the Board (authoritative), not from the sim.
    if (!Board) return;
    const AActor* Anchor = GetAnchor();
    const FVector AnchorV = Anchor ? Anchor->GetActorLocation() : FVector::ZeroVector;
    const float Tile = Driver ? Driver->GetTileSize() : 100.f;
    const float GridYaw = Driver ? Driver->GetGridYaw() : 0.f;
    const EHexOrientation Ori = Driver ? Driver->GetGridOrientation() : EHexOrientation::PointyTop;

    for (const FHex& H : Board->GetAllHexes())
    {
        const FVector2D LocalXY = HexCoord::AxialToXY(FHexAxial(H.Q,H.R), Tile, Ori);
        const FVector2D RotXY   = HexCoord::Rotate2D(LocalXY, GridYaw);
        const FVector Loc = AnchorV + FVector(RotXY.X, RotXY.Y, 0.f);

        if (TileMesh)
        {
            TileISM->AddInstance(FTransform(FRotator::ZeroRotator, Loc, FVector(1)));
        }
        else
        {
            DrawDebugPoint(GetWorld(), Loc + FVector(0,0,2), 6.f, FColor::Cyan, false, 1.5f);
        }
    }
}

void AHexFactoryActor::DrawBeltArrow(const FVector& Center, float TileRadiusUU, float YawDeg)
{
    const float Len = TileRadiusUU * DebugArrowLenScale;
    const FVector Dir = FRotator(0.f, YawDeg, 0.f).RotateVector(FVector(1,0,0));
    const FVector A = Center + FVector(0,0,BeltZOffset + 2.f);
    const FVector B = A + Dir * Len;
    DrawDebugDirectionalArrow(GetWorld(), A, B, 18.f, FColor::Yellow, false, 1.0f, 0, DebugArrowThickness);
}

void AHexFactoryActor::RebuildBelts()
{
    if (!Sim) return;
    const AActor* Anchor = GetAnchor();
    const FVector AnchorV = Anchor ? Anchor->GetActorLocation() : FVector::ZeroVector;
    const float Tile = Driver ? Driver->GetTileSize() : 100.f;
    const float GridYaw = Driver ? Driver->GetGridYaw() : 0.f;
    const int32 VisOffset = Driver ? Driver->GetBeltVisualOffset() : 0;
    const bool bCW = Driver ? Driver->IsBeltYawClockwise() : true;
    const float Step = bCW ? -60.f : +60.f;
    const EHexOrientation Ori = Driver ? Driver->GetGridOrientation() : EHexOrientation::PointyTop;

    for (const auto& KV : Sim->TileMap)
    {
        const FHex& H = KV.Key;
        const FTile& T = KV.Value;
        if (T.Kind != ETileKind::Belt) continue;

        // Skip drawing belts on hexes that don't physically exist on the board.
        if (Board && !Board->HasHex(H)) continue;

        const uint8 VisDir = uint8((int32(T.Arg) + VisOffset + 6) % 6);
        const float Yaw = Step * float(VisDir) + GridYaw;

        const FVector2D LocalXY = HexCoord::AxialToXY(FHexAxial(H.Q,H.R), Tile, Ori);
        const FVector2D RotXY   = HexCoord::Rotate2D(LocalXY, GridYaw);
        const FVector Loc = AnchorV + FVector(RotXY.X, RotXY.Y, BeltZOffset);

        if (BeltMesh)
        {
            BeltISM->AddInstance(FTransform(FRotator(0.f, Yaw, 0.f), Loc, BeltScale));
            if (bAlwaysDrawBeltArrows) DrawBeltArrow(Loc, Tile, Yaw);
        }
        else
        {
            DrawBeltArrow(Loc, Tile, Yaw);
        }
    }
}
