#include "Actors/HexBoardActor.h"
#include "EngineUtils.h"

AHexBoardActor::AHexBoardActor()
{
    PrimaryActorTick.bCanEverTick = false;
#if WITH_EDITORONLY_DATA
    SetActorLabel(TEXT("HexBoardActor"));
#endif
}

void AHexBoardActor::BeginPlay()
{
    Super::BeginPlay();
    if (bGenerateOnBeginPlay && Terrain.Num() == 0 && InitialRingRadius >= 0)
    {
        GenerateFilledRadius(InitialRingRadius);
    }
}

bool AHexBoardActor::HasHex(FHex H) const
{
    return Terrain.Contains(H);
}

void AHexBoardActor::AddHex(FHex H)
{
    if (Terrain.Contains(H)) return;
    Terrain.Add(H);
    ++BoardRevision;
}

void AHexBoardActor::RemoveHex(FHex H)
{
    if (Terrain.Remove(H) > 0)
    {
        ++BoardRevision;
    }
}

AHexBoardActor* AHexBoardActor::Find(const UWorld* World)
{
    if (!World) return nullptr;
    for (TActorIterator<AHexBoardActor> It(World); It; ++It)
    {
        return *It;
    }
    return nullptr;
}

void AHexBoardActor::GenerateFilledRadius(int32 Radius)
{
    // Filled hex of radius R: |q|<=R, |r|<=R, |q+r|<=R
    for (int32 q = -Radius; q <= Radius; ++q)
    {
        for (int32 r = -Radius; r <= Radius; ++r)
        {
            const int32 s = -q - r;
            if (FMath::Max3(FMath::Abs(q), FMath::Abs(r), FMath::Abs(s)) <= Radius)
            {
                Terrain.Add(FHex(q, r));
            }
        }
    }
    ++BoardRevision;
}
