#pragma once
#include "CoreMinimal.h"
#include "GameFramework/SaveGame.h"
#include "Editor/MachineBlueprint.h"
#include "MachineBlueprintSave.generated.h"

UCLASS()
class AUTOMATIONGAME_API UMachineBlueprintSave : public USaveGame
{
    GENERATED_BODY()
public:
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Save")
    FText DisplayName;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Save")
    TArray<FMachinePart> Parts;
};
