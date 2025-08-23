#pragma once
#include "AutomationGameMinimal.h"
#include "GameFramework/Actor.h"
#include "GridTestHarness.generated.h"

class UHexPlacementComponent;
class UOrbRenderer;
class AOrbSimDriver;

/** Drop this actor into the level for a self-contained test: click to place Inputs, see orbs spawn. */
UCLASS()
class AUTOMATIONGAME_API AGridTestHarness : public AActor
{
    GENERATED_BODY()
public:
    AGridTestHarness();

    virtual void BeginPlay() override;

    /** Auto-spawn an OrbSimDriver if not found. */
    UPROPERTY(EditAnywhere, Category="Harness") bool bAutoSpawnDriver = true;

    /** Use this actor as the grid anchor if the driver has none. */
    UPROPERTY(EditAnywhere, Category="Harness") bool bUseSelfAsAnchorIfUnset = true;

    /** Make the cursor visible and enable click events at BeginPlay. */
    UPROPERTY(EditAnywhere, Category="Harness") bool bEnableMouse = true;

    /** Placement component (polling on, Input kind by default). */
    UPROPERTY(VisibleAnywhere) UHexPlacementComponent* Placement;

    /** Orb renderer (falls back to Engine sphere/material). */
    UPROPERTY(VisibleAnywhere) UOrbRenderer* Renderer;
};
