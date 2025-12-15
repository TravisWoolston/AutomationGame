#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimInstance.h"
#include "WalkerAnimInstance.generated.h"

class ABasicWalker;

/**
 * Visual foot state for animation updates (includes Holding which EFootStepState lacks)
 * Note: This mirrors EFootVisualState in BasicWalker.h for AnimInstance independence
 */
UENUM(BlueprintType)
enum class EFootAnimState : uint8
{
	Planted  UMETA(DisplayName = "Planted"),
	Raising  UMETA(DisplayName = "Raising"),
	Moving   UMETA(DisplayName = "Moving"),
	Lowering UMETA(DisplayName = "Lowering"),
	Holding  UMETA(DisplayName = "Holding")
};

/**
 * Custom AnimInstance base class for Walker animations.
 * Provides BlueprintImplementableEvent functions that are called by the Walker
 * only when foot animation state changes (optimization - not called every tick when planted).
 * 
 * To use: 
 * 1. Create an Animation Blueprint
 * 2. Reparent it to WalkerAnimInstance (Class Settings → Parent Class)
 * 3. Implement the UpdateFoot*Visual events in your Event Graph
 */
UCLASS()
class AUTOMATIONGAME_API UWalkerAnimInstance : public UAnimInstance
{
	GENERATED_BODY()

public:
	// --- Foot Visual Update Events ---
	// These are called by ABasicWalker only when the foot is animating (Progress < 1)
	// This provides optimization by not updating every tick when feet are planted.
	
	/** Called each tick while D foot is animating (ProgressValue < 1). Implement in Event Graph. */
	UFUNCTION(BlueprintImplementableEvent, Category = "Walker|Foot Events")
	void UpdateFootDVisual(float ProgressValue, EFootAnimState FootState, FVector CurrentAssignedTileWorldPosition, FVector PreviousAssignedTileWorldPosition);
	
	/** Called each tick while L foot is animating (ProgressValue < 1). Implement in Event Graph. */
	UFUNCTION(BlueprintImplementableEvent, Category = "Walker|Foot Events")
	void UpdateFootLVisual(float ProgressValue, EFootAnimState FootState, FVector CurrentAssignedTileWorldPosition, FVector PreviousAssignedTileWorldPosition);
	
	/** Called each tick while R foot is animating (ProgressValue < 1). Implement in Event Graph. */
	UFUNCTION(BlueprintImplementableEvent, Category = "Walker|Foot Events")
	void UpdateFootRVisual(float ProgressValue, EFootAnimState FootState, FVector CurrentAssignedTileWorldPosition, FVector PreviousAssignedTileWorldPosition);
	
	/** Called each tick while U foot is animating (ProgressValue < 1). Implement in Event Graph. */
	UFUNCTION(BlueprintImplementableEvent, Category = "Walker|Foot Events")
	void UpdateFootUVisual(float ProgressValue, EFootAnimState FootState, FVector CurrentAssignedTileWorldPosition, FVector PreviousAssignedTileWorldPosition);

	// --- Convenience: Get owning Walker ---
	
	/** Get the BasicWalker that owns this AnimInstance. Returns nullptr if not owned by a BasicWalker. */
	UFUNCTION(BlueprintPure, Category = "Walker")
	ABasicWalker* GetOwningWalker() const;
};
