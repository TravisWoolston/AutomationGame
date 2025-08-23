#pragma once
#include "CoreMinimal.h"
#include "Sim/HexTypes.h"
#include "MachineBlueprint.generated.h"

UENUM(BlueprintType)
enum class EMachinePartKind : uint8
{
    Belt       UMETA(DisplayName="Belt"),
    Arm        UMETA(DisplayName="Arm"),
    PortInput  UMETA(DisplayName="Input Port"),
    PortOutput UMETA(DisplayName="Output Port"),
};

USTRUCT(BlueprintType)
struct AUTOMATIONGAME_API FMachinePart
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Machine")
    FHex Hex;

    /** Direction in hex-steps [0..5], 0 = +X in pointy-top convention. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Machine")
    uint8 Dir = 0;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Machine")
    EMachinePartKind Kind = EMachinePartKind::Belt;

    /** Optional integer parameter (e.g., arm length) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Machine")
    int32 ParamI = 0;

    /** Optional float parameter (e.g., speed) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Machine")
    float ParamF = 0.f;
};

/** Lightweight in-memory blueprint object you can create, mutate, save/load. */
UCLASS(BlueprintType)
class AUTOMATIONGAME_API UMachineBlueprint : public UObject
{
    GENERATED_BODY()
public:
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Machine")
    FText DisplayName;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Machine")
    TArray<FMachinePart> Parts;

    /** Returns index if found, or INDEX_NONE. */
    UFUNCTION(BlueprintCallable, BlueprintPure, Category="Machine")
    int32 FindPartAtHex(FHex Hex) const;

    UFUNCTION(BlueprintCallable, Category="Machine")
    void AddOrReplacePart(const FMachinePart& Part);

    UFUNCTION(BlueprintCallable, Category="Machine")
    bool RemovePartAt(FHex Hex);

    UFUNCTION(BlueprintCallable, Category="Machine")
    void Clear();
};
