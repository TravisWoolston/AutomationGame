#include "Framework/GameManager.h"
#include "Framework/GameTickManager.h"
#include "EngineUtils.h"
#include "Engine/World.h"

AGameManager::AGameManager()
{
    PrimaryActorTick.bCanEverTick = false;
}

void AGameManager::BeginPlay()
{
    Super::BeginPlay();
    AutoFindRefs();
    TryLoadInitial();
}

void AGameManager::AutoFindRefs()
{
    UWorld* W = GetWorld(); if (!W) return;

    if (!TickManager)
        for (TActorIterator<AGameTickManager> It(W); It; ++It) { TickManager = *It; break; }

    if (!OrbSimDriver)
        for (TActorIterator<AActor> It(W); It; ++It)
        {
            AActor* A = *It; if (!A) continue;
            if (A->GetClass()->GetName().Contains(TEXT("OrbSimDriver"))) { OrbSimDriver = A; break; }
        }

    if (!OrbSimDriver && OrbSimDriverClass)
    {
        FActorSpawnParameters S; S.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        OrbSimDriver = W->SpawnActor<AActor>(OrbSimDriverClass, GetActorTransform(), S);
    }
}

void AGameManager::TryLoadInitial()
{
    if (InitialMapToLoad.IsEmpty() || !OrbSimDriver) return;
    const FString Cmd = FString::Printf(TEXT("LoadMap %s"), *InitialMapToLoad);
    if (!OrbSimDriver->CallFunctionByNameWithArguments(*Cmd, *GLog, nullptr, true))
        UE_LOG(LogTemp, Warning, TEXT("GameManager: OrbSimDriver didn't accept Exec 'LoadMap'."));
}