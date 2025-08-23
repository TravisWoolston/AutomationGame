#include "Actors/HexGridActor.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Sim/SimWorldHex.h"
#include "Sim/HexTypes.h"
#include "Sim/HexPick.h"

AHexGridActor::AHexGridActor()
{
    PrimaryActorTick.bCanEverTick = true;
    EnsureISM();
}

void AHexGridActor::EnsureISM()
{
    if (!ISM)
    {
        ISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>("HexGridISM");
        SetRootComponent(ISM);
        ISM->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
        ISM->SetCollisionResponseToAllChannels(ECollisionResponse::ECR_Block);
    }
}

void AHexGridActor::OnConstruction(const FTransform&)
{
    Rebuild();
}

void AHexGridActor::Rebuild()
{
    EnsureISM();
    ISM->ClearInstances();
    if (TileMesh) ISM->SetStaticMesh(TileMesh);
    if (TileMaterial) ISM->SetMaterial(0, TileMaterial);

    const float S = FMath::Max(1.f, TileSizeUU);
    const FVector Origin = GetActorLocation();
    const auto ToWorld = [S, Origin](const FHex& H)->FVector
    {
        // pointy-top axial
        const float x = S * ( (3.f/2.f) * H.Q );
        const float y = S * ( (FMath::Sqrt(3.f)/2.f) * (2*H.R + H.Q) );
        return Origin + FVector(x, y, 0.f);
    };

    // Build a hexagon ring fill up to Radius
    for (int r = -Radius; r <= Radius; ++r)
    {
        for (int q = -Radius; q <= Radius; ++q)
        {
            const int s = -q - r;
            if (FMath::Max3(FMath::Abs(q), FMath::Abs(r), FMath::Abs(s)) > Radius) continue;
            const FVector P = ToWorld(FHex(q, r));
            ISM->AddInstance(FTransform(FRotator::ZeroRotator, P, FVector(1)));
        }
    }
    LastCenter = GetActorLocation();
}

void AHexGridActor::BeginPlay()
{
    Super::BeginPlay();
    Rebuild();
}

void AHexGridActor::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    if (!bAutoExpandAtRuntime) return;
    const float Dist = FVector::Dist2D(LastCenter, GetActorLocation());
    if (Dist > RecenterThresholdUU)
    {
        Rebuild();
    }
}
