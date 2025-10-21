#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Components/HorizontalBox.h"
#include "Components/Image.h"
#include "Components/TextBlock.h"
#include "Sim/ArmTypes.h"
#include "UMG.h"
#include "ArmTimelineUMGWidget.generated.h"

class UTexture2D;

/**
 * UMG Widget for displaying arm command timeline
 * Shows command icons in a horizontal layout with highlighting
 */
UCLASS(BlueprintType, Blueprintable)
class AUTOMATIONGAME_API UArmTimelineUMGWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	UArmTimelineUMGWidget(const FObjectInitializer& ObjectInitializer);

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

	/** Set the default command texture */
	UFUNCTION(BlueprintCallable, Category = "Arm Timeline")
	void SetDefaultCommandTexture(UTexture2D* DefaultTexture);

	/** Set the maximum number of commands to display */
	UFUNCTION(BlueprintCallable, Category = "Arm Timeline")
	void SetMaxDisplayedCommands(int32 MaxCommands);

	/** Set the colors for different command states */
	UFUNCTION(BlueprintCallable, Category = "Arm Timeline")
	void SetCommandColors(const FLinearColor& CurrentColor, const FLinearColor& NormalColor, const FLinearColor& CompletedColor);

	/** Set the icon size for command icons */
	UFUNCTION(BlueprintCallable, Category = "Arm Timeline")
	void SetIconSize(float IconSize);

	/** Set the background color for command icons */
	UFUNCTION(BlueprintCallable, Category = "Arm Timeline")
	void SetIconBackgroundColor(const FLinearColor& IconBackgroundColor);

	/** Set the normal command color */
	UFUNCTION(BlueprintCallable, Category = "Arm Timeline")
	void SetNormalCommandColor(const FLinearColor& NormalColor);

	/** Set the current command color */
	UFUNCTION(BlueprintCallable, Category = "Arm Timeline")
	void SetCurrentCommandColor(const FLinearColor& CurrentColor);

	/** Set the completed command color */
	UFUNCTION(BlueprintCallable, Category = "Arm Timeline")
	void SetCompletedCommandColor(const FLinearColor& CompletedColor);

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

	/** Create a command icon widget */
	UWidget* CreateCommandIcon(EArmCmd Command, int32 Index);

	/** Update a command icon widget */
	UFUNCTION(BlueprintCallable, Category = "Arm Timeline")
	void UpdateCommandIcon(UWidget* IconWidget, EArmCmd Command, int32 Index, bool bIsCurrent, bool bIsCompleted, float Progress);

	/** Blueprint implementable event for custom icon updates */
	UFUNCTION(BlueprintImplementableEvent, Category = "Arm Timeline")
	void OnUpdateCommandIcon(UWidget* IconWidget, EArmCmd Command, int32 Index, bool bIsCurrent, bool bIsCompleted, float Progress);

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

	/** Size of command icons */
	UPROPERTY(BlueprintReadOnly, Category = "Arm Timeline")
	float IconSize = 32.0f;

	/** Background color for command icons */
	UPROPERTY(BlueprintReadOnly, Category = "Arm Timeline")
	FLinearColor IconBackgroundColor = FLinearColor(0.2f, 0.2f, 0.2f, 0.8f);

	/** Horizontal box containing command icons */
	UPROPERTY(BlueprintReadOnly, meta = (BindWidget))
	UHorizontalBox* CommandIconsBox = nullptr;

	/** Text block showing current command name */
	UPROPERTY(BlueprintReadOnly, meta = (BindWidget))
	UTextBlock* CurrentCommandText = nullptr;

	/** Text block showing progress */
	UPROPERTY(BlueprintReadOnly, meta = (BindWidget))
	UTextBlock* ProgressText = nullptr;

private:
	/** Array of command icon widgets */
	UPROPERTY()
	TArray<UWidget*> CommandIconWidgets;

	/** Whether icons have been created and are stable */
	bool bIconsCreated = false;

	/** Initialize the widget */
	void InitializeWidget();

	/** Update the widget content */
	void UpdateWidgetContent();

	/** Get texture for a command type */
	UTexture2D* GetTextureForCommand(EArmCmd Command) const;

	/** Get display name for a command */
	FString GetCommandDisplayName(EArmCmd Command) const;

	/** Create command icon widgets */
	void CreateCommandIcons();

	/** Update command icon widgets */
	void UpdateCommandIcons();
};
