#pragma once
#include "CoreMinimal.h"
#include "MachineEditor_TickHook.generated.h"

class AGameTickManager;

USTRUCT()
struct FMachineEditorTickState
{
    GENERATED_BODY()
    /** External manager if found in level; can be null. */
    UPROPERTY(Transient) AGameTickManager* Manager = nullptr;
    /** Local accumulator fallback when no manager is present. */
    double LocalAccum = 0.0;
    /** Local play state fallback. */
    uint8 bLocalPlaying : 1;
    FMachineEditorTickState(){ bLocalPlaying = false; }
};
