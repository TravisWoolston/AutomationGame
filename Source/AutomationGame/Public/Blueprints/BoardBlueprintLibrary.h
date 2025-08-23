#pragma once
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Sim/HexTypes.h"
#include "BoardBlueprintLibrary.generated.h"

class AHexBoardActor;

UCLASS()
class AUTOMATIONGAME_API UBoardBlueprintLibrary : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()
public:
    UFUNCTION(BlueprintCallable, BlueprintPure, Category="Board", meta=(WorldContext="WorldContextObject"))
    static bool BoardHasHex(const UObject* WorldContextObject, FHex H);

    UFUNCTION(BlueprintCallable, Category="Board", meta=(WorldContext="WorldContextObject"))
    static void BoardAddHex(const UObject* WorldContextObject, FHex H);

    UFUNCTION(BlueprintCallable, Category="Board", meta=(WorldContext="WorldContextObject"))
    static void BoardRemoveHex(const UObject* WorldContextObject, FHex H);
};
