#include "Actors/OrbSimDriver.h"
#include "Sim/SimWorldHex.h"
#include "Engine/World.h"
#include "EngineUtils.h"

AOrbSimDriver::AOrbSimDriver()
{
    PrimaryActorTick.bCanEverTick = true;
}

void AOrbSimDriver::BeginPlay()
{
    Super::BeginPlay();
    if (!Sim)
    {
        Sim = NewObject<USimWorldHex>(this);
        Sim->Initialize(20);
    }
    // Align sim spacing to our knob
    if (Sim) Sim->TileSizeUU = TileSizeUU;
}

void AOrbSimDriver::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    if (!Sim) return;

    // Keep sim spacing in sync if user tweaks TileSizeUU in editor during PIE
    if (!FMath::IsNearlyEqual(Sim->TileSizeUU, TileSizeUU))
    {
        Sim->TileSizeUU = TileSizeUU;
        Sim->TileRevision++; // force a visual rebuild
    }

    Accum += DeltaSeconds;
    const float Step = Sim->FixedStepSec;
    while (Accum >= Step)
    {
        Accum -= Step;
        Sim->StepSim();
        StepAlpha = 0.f;
    }
    StepAlpha = Accum / Step;
}

FVector2D AOrbSimDriver::GetGridOriginXY() const
{
    if (GridAnchor)
    {
        const FVector L = GridAnchor->GetActorLocation();
        return FVector2D(L.X, L.Y);
    }
    return FVector2D::ZeroVector;
}

float AOrbSimDriver::GetGridPlaneZ(const AActor* FallbackOwner) const
{
    if (bUseAnchorZForPicking && GridAnchor)
    {
        return GridAnchor->GetActorLocation().Z;
    }
    return FallbackOwner ? FallbackOwner->GetActorLocation().Z : 0.f;
}

AOrbSimDriver* AOrbSimDriver::Find(UWorld* World)
{
    if (!World) return nullptr;
    for (TActorIterator<AOrbSimDriver> It(World); It; ++It)
    {
        return *It;
    }
    return nullptr;
}
