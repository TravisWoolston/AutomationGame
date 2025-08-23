#pragma once
#include "AutomationGameMinimal.h"
#include "Tiles.generated.h"

UENUM(BlueprintType)
enum class ETileKind : uint8
{
    Empty=0,
    Input,
    Output,
    Belt,      // conveyor tile (Arg = direction 0..5)
};

USTRUCT(BlueprintType)
struct AUTOMATIONGAME_API FTile
{
    GENERATED_BODY();
    UPROPERTY(EditAnywhere, BlueprintReadWrite) ETileKind Kind = ETileKind::Empty;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) uint8     Arg  = 0;
};
