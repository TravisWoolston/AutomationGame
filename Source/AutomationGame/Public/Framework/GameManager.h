#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "GameManager.generated.h"

class AGameTickManager;

/** Lightweight coordinator to keep the overworld clean. */
UCLASS(BlueprintType)
class AUTOMATIONGAME_API AGameManager : public AActor
{
    GENERATED_BODY()
public:
    AGameManager();

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Refs")
    AGameTickManager* TickManager = nullptr;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Refs")
    AActor* OrbSimDriver = nullptr;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Refs")
    TSubclassOf<AActor> OrbSimDriverClass;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Startup")
    FString InitialMapToLoad;

protected:
    virtual void BeginPlay() override;

private:
    void AutoFindRefs();
    void TryLoadInitial();
};