#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Sim/ArmTypes.h"
#include "UMG.h"
#include "ArmTimelineWidgetBlueprint.generated.h"

class UTexture2D;
class UImage;
class UTextBlock;
class UHorizontalBox;

/**
 * Blueprint widget for displaying arm command timeline
 * Shows command icons in a horizontal layout with highlighting
 */
UCLASS(BlueprintType, Blueprintable)
class AUTOMATIONGAME_API UArmTimelineWidgetBlueprint : public UUserWidget
{
	GENERATED_BODY()

public:
	UArmTimelineWidgetBlueprint(const FObjectInitializer& ObjectInitializer);

	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;

	/** Set the arm program to display */
	UFUNCTION(BlueprintCallable, Category = "Arm Timeline")
	void SetArmProgram(const TArray<FArmInstr>& Program);

	/** Update the current command being executed */
	UFUNCTION(BlueprintCallable, Category = "Arm Timeline")
	void SetCurrentCommand(int32 CommandIndex, float Progress = 0.0f);

	/** Set the command textures for each command type */
	UFUNCTION(BlueprintCallable, Category = "Arm Timeline")
	void SetCommandTextures(const TMap<EArmCmd, UTexture2D*>& Textures);

	/** Set the maximum number of commands to display */
	UFUNCTION(BlueprintCallable, Category = "Arm Timeline")
	void SetMaxDisplayedCommands(int32 MaxCommands);

	/** Set the colors for different command states */
	UFUNCTION(BlueprintCallable, Category = "Arm Timeline")
	void SetCommandColors(const FLinearColor& CurrentColor, const FLinearColor& NormalColor, const FLinearColor& CompletedColor);

	/** Get the current program */
	UFUNCTION(BlueprintPure, Category = "Arm Timeline")
	const TArray<FArmInstr>& GetArmProgram() const { return ArmProgram; }

	/** Get the current command index */
	UFUNCTION(BlueprintPure, Category = "Arm Timeline")
	int32 GetCurrentCommandIndex() const { return CurrentCommandIndex; }

	/** Get the current command progress (0.0 to 1.0) */
	UFUNCTION(BlueprintPure, Category = "Arm Timeline")
	float GetCurrentCommandProgress() const { return CurrentCommandProgress; }

protected:
	/** Called when the widget is constructed */
	UFUNCTION(BlueprintImplementableEvent, Category = "Arm Timeline")
	void OnWidgetConstructed();

	/** Called when the program changes */
	UFUNCTION(BlueprintImplementableEvent, Category = "Arm Timeline")
	void OnProgramChanged();

	/** Called when the current command changes */
	UFUNCTION(BlueprintImplementableEvent, Category = "Arm Timeline")
	void OnCurrentCommandChanged();

	/** Called when command textures are updated */
	UFUNCTION(BlueprintImplementableEvent, Category = "Arm Timeline")
	void OnCommandTexturesUpdated();

public:
	/** The arm program being displayed */
	UPROPERTY(BlueprintReadOnly, Category = "Arm Timeline")
	TArray<FArmInstr> ArmProgram;

	/** Current command index being executed */
	UPROPERTY(BlueprintReadOnly, Category = "Arm Timeline")
	int32 CurrentCommandIndex = -1;

	/** Progress of current command (0.0 to 1.0) */
	UPROPERTY(BlueprintReadOnly, Category = "Arm Timeline")
	float CurrentCommandProgress = 0.0f;

	/** Command textures for each command type */
	UPROPERTY(BlueprintReadOnly, Category = "Arm Timeline")
	TMap<EArmCmd, UTexture2D*> CommandTextures;

	/** Default texture for unknown commands */
	UPROPERTY(BlueprintReadOnly, Category = "Arm Timeline")
	UTexture2D* DefaultCommandTexture = nullptr;

	/** Maximum number of commands to display */
	UPROPERTY(BlueprintReadOnly, Category = "Arm Timeline")
	int32 MaxDisplayedCommands = 10;

	/** Highlight color for current command */
	UPROPERTY(BlueprintReadOnly, Category = "Arm Timeline")
	FLinearColor CurrentCommandColor = FLinearColor::Yellow;

	/** Normal color for other commands */
	UPROPERTY(BlueprintReadOnly, Category = "Arm Timeline")
	FLinearColor NormalCommandColor = FLinearColor::White;

	/** Completed command color */
	UPROPERTY(BlueprintReadOnly, Category = "Arm Timeline")
	FLinearColor CompletedCommandColor = FLinearColor::Green;

private:
	/** Initialize the widget */
	void InitializeWidget();

	/** Update the widget content */
	void UpdateWidgetContent();

	/** Get texture for a command type */
	UTexture2D* GetTextureForCommand(EArmCmd Command) const;

	/** Get display name for a command */
	FString GetCommandDisplayName(EArmCmd Command) const;
};
