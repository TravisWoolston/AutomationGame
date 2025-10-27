#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "Sim/ArmTypes.h"
#include "ArmTimelineEditor.generated.h"

class ABasicArm;
class UArmTimelineWidget;

/**
 * Interface for timeline editors that can edit arm command lists
 */
UINTERFACE(MinimalAPI, Blueprintable)
class UArmTimelineEditor : public UInterface
{
	GENERATED_BODY()
};

class AUTOMATIONGAME_API IArmTimelineEditor
{
	GENERATED_BODY()

public:
	/** Get the currently active arm timeline being edited */
	UFUNCTION(BlueprintImplementableEvent, BlueprintCallable, Category = "Timeline Editor")
	UArmTimelineWidget* GetActiveTimeline() const;

	/** Set the active timeline to edit */
	UFUNCTION(BlueprintImplementableEvent, BlueprintCallable, Category = "Timeline Editor")
	void SetActiveTimeline(UArmTimelineWidget* Timeline);

	/** Get the cursor position in the command list (where commands will be inserted) */
	UFUNCTION(BlueprintImplementableEvent, BlueprintCallable, Category = "Timeline Editor")
	int32 GetCursorPosition() const;

	/** Set the cursor position in the command list */
	UFUNCTION(BlueprintImplementableEvent, BlueprintCallable, Category = "Timeline Editor")
	void SetCursorPosition(int32 Position);

	/** Insert a command at the cursor position */
	UFUNCTION(BlueprintImplementableEvent, BlueprintCallable, Category = "Timeline Editor")
	bool InsertCommand(int32 CommandIndex, ECmd Command, int32 Arg = 0);

	/** Remove a command at the specified index */
	UFUNCTION(BlueprintImplementableEvent, BlueprintCallable, Category = "Timeline Editor")
	bool RemoveCommand(int32 CommandIndex);

	/** Get the command icon texture for a given command type */
	UFUNCTION(BlueprintImplementableEvent, BlueprintCallable, Category = "Timeline Editor")
	UTexture2D* GetCommandIcon(ECmd Command) const;

	/** Set the command icon texture for a given command type */
	UFUNCTION(BlueprintImplementableEvent, BlueprintCallable, Category = "Timeline Editor")
	void SetCommandIcon(ECmd Command, UTexture2D* Icon);

	/** Get all command icons */
	UFUNCTION(BlueprintImplementableEvent, BlueprintCallable, Category = "Timeline Editor")
	TMap<ECmd, UTexture2D*> GetAllCommandIcons() const;

	/** Get the arm associated with the active timeline */
	UFUNCTION(BlueprintImplementableEvent, BlueprintCallable, Category = "Timeline Editor")
	ABasicArm* GetActiveArm() const;
};
