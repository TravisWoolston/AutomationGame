#include "Blueprints/BoardBlueprintLibrary.h"
#include "Actors/HexBoardActor.h"
#include "Engine/World.h"

static AHexBoardActor* FindBoard(const UObject* Wco)
{
    if (!Wco) return nullptr;
    const UWorld* W = Wco->GetWorld();
    return AHexBoardActor::Find(W);
}

bool UBoardBlueprintLibrary::BoardHasHex(const UObject* WorldContextObject, FHex H)
{
    if (AHexBoardActor* B = FindBoard(WorldContextObject))
        return B->HasHex(H);
    return false;
}

void UBoardBlueprintLibrary::BoardAddHex(const UObject* WorldContextObject, FHex H)
{
    if (AHexBoardActor* B = FindBoard(WorldContextObject))
        B->AddHex(H);
}

void UBoardBlueprintLibrary::BoardRemoveHex(const UObject* WorldContextObject, FHex H)
{
    if (AHexBoardActor* B = FindBoard(WorldContextObject))
        B->RemoveHex(H);
}
