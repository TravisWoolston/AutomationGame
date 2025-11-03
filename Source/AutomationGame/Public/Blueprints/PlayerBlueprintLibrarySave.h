#pragma once
#include "CoreMinimal.h"
#include "GameFramework/SaveGame.h"
#include "Blueprints/BlueprintTile.h"
#include "PlayerBlueprintLibrarySave.generated.h"

USTRUCT(BlueprintType)
struct AUTOMATIONGAME_API FBlueprintData
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Save")
    TArray<FBlueprintTile> Tiles;
};

UCLASS()
class AUTOMATIONGAME_API UPlayerBlueprintLibrarySave : public USaveGame
{
    GENERATED_BODY()
public:
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Save")
    TArray<FBlueprintData> SavedBlueprints;
};

