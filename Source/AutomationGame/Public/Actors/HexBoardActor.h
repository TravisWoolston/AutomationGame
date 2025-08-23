#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Sim/HexTypes.h"
#include "HexBoardActor.generated.h"

/** Authoritative set of existing hex cells in the world. */
UCLASS()
class AUTOMATIONGAME_API AHexBoardActor : public AActor
{
    GENERATED_BODY()
public:
    AHexBoardActor();
    virtual void BeginPlay() override;

    /** Generate a filled hex of this radius (0 = just origin) at BeginPlay. */
    UPROPERTY(EditAnywhere, Category="Board")
    int32 InitialRingRadius = 6;

    UPROPERTY(EditAnywhere, Category="Board")
    bool bGenerateOnBeginPlay = true;

    /** Bumps whenever terrain changes. Use to trigger visual rebuilds. */
    UPROPERTY(VisibleAnywhere, Category="Board")
    int32 BoardRevision = 0;

    /** Returns true if the given axial hex exists on the board. */
    UFUNCTION(BlueprintCallable, Category="Board")
    bool HasHex(FHex H) const;

    /** Add/remove hex cells (in code or via BP). */
    UFUNCTION(BlueprintCallable, Category="Board")
    void AddHex(FHex H);

    UFUNCTION(BlueprintCallable, Category="Board")
    void RemoveHex(FHex H);

    /** Iterate all hexes. */
    const TSet<FHex>& GetAllHexes() const { return Terrain; }

    /** Find the single board actor in the level. */
    static AHexBoardActor* Find(const UWorld* World);

private:
    TSet<FHex> Terrain;
    void GenerateFilledRadius(int32 Radius);
};
