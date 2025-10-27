#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "ArmTimelineEditor.h"
#include "Framework/GameManager.h"
#include "Sim/ArmTypes.h"
#include "ArmTimelineEditorWidget.generated.h"

class UArmTimelineWidget;
class UArmTimelineUMGWidget;
class ABasicArm;
class UTexture2D;

// Forward declaration for EArmCmd defined in BasicArm.h
enum class EArmCmd : uint8;

/**
 * Configuration for a command slot in the editor
 */
USTRUCT(BlueprintType)
struct FCommandSlotConfig
{
	GENERATED_BODY()

	/** Command type */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Command Config")
	ECmd Command;

	/** Icon to display for this command */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Command Config")
	UTexture2D* Icon = nullptr;

	/** Input key to press to trigger this command */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Command Config")
	FKey InputKey = EKeys::Invalid;

	/** Default argument value for this command */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Command Config")
	int32 DefaultArg = 0;

	FCommandSlotConfig()
		: Command(ECmd::Wait)
		, Icon(nullptr)
		, InputKey(EKeys::Invalid)
		, DefaultArg(0)
	{
	}
};

/**
 * Base widget for timeline editors that implement IArmTimelineEditor
 * Provides C++ implementation that Blueprint can override if needed
 */
UCLASS(BlueprintType, Blueprintable)
class AUTOMATIONGAME_API UArmTimelineEditorWidget : public UUserWidget, public IArmTimelineEditor
{
	GENERATED_BODY()

public:
	UArmTimelineEditorWidget(const FObjectInitializer& ObjectInitializer);

	virtual void NativeConstruct() override;
	virtual FReply NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent) override;

	// IArmTimelineEditor Interface Implementation
	UFUNCTION(BlueprintCallable, BlueprintNativeEvent, Category = "Timeline Editor")
	UArmTimelineWidget* GetActiveTimeline() const;
	virtual UArmTimelineWidget* GetActiveTimeline_Implementation() const;

	UFUNCTION(BlueprintCallable, BlueprintNativeEvent, Category = "Timeline Editor")
	void SetActiveTimeline(UArmTimelineWidget* Timeline);
	virtual void SetActiveTimeline_Implementation(UArmTimelineWidget* Timeline);

	UFUNCTION(BlueprintCallable, BlueprintNativeEvent, Category = "Timeline Editor")
	int32 GetCursorPosition() const;
	virtual int32 GetCursorPosition_Implementation() const;

	UFUNCTION(BlueprintCallable, BlueprintNativeEvent, Category = "Timeline Editor")
	void SetCursorPosition(int32 Position);
	virtual void SetCursorPosition_Implementation(int32 Position);

	UFUNCTION(BlueprintCallable, BlueprintNativeEvent, Category = "Timeline Editor")
	bool InsertCommand(int32 CommandIndex, ECmd Command, int32 Arg = 0);
	virtual bool InsertCommand_Implementation(int32 CommandIndex, ECmd Command, int32 Arg = 0);

	UFUNCTION(BlueprintCallable, BlueprintNativeEvent, Category = "Timeline Editor")
	bool RemoveCommand(int32 CommandIndex);
	virtual bool RemoveCommand_Implementation(int32 CommandIndex);

	UFUNCTION(BlueprintCallable, BlueprintNativeEvent, Category = "Timeline Editor")
	UTexture2D* GetCommandIcon(ECmd Command) const;
	virtual UTexture2D* GetCommandIcon_Implementation(ECmd Command) const;

	UFUNCTION(BlueprintCallable, BlueprintNativeEvent, Category = "Timeline Editor")
	void SetCommandIcon(ECmd Command, UTexture2D* Icon);
	virtual void SetCommandIcon_Implementation(ECmd Command, UTexture2D* Icon);

	UFUNCTION(BlueprintCallable, BlueprintNativeEvent, Category = "Timeline Editor")
	TMap<ECmd, UTexture2D*> GetAllCommandIcons() const;
	virtual TMap<ECmd, UTexture2D*> GetAllCommandIcons_Implementation() const;

	UFUNCTION(BlueprintCallable, BlueprintNativeEvent, Category = "Timeline Editor")
	ABasicArm* GetActiveArm() const;
	virtual ABasicArm* GetActiveArm_Implementation() const;

	/** Handle command button click or key press */
	UFUNCTION(BlueprintCallable, Category = "Timeline Editor")
	void HandleCommandInput(ECmd Command, int32 Arg = 0);

	/** Handle input from keyboard */
	UFUNCTION(BlueprintCallable, Category = "Timeline Editor")
	void HandleKeyInput(const FKey& PressedKey);

	/** Get all command slot configurations */
	UFUNCTION(BlueprintPure, Category = "Timeline Editor")
	TArray<FCommandSlotConfig> GetCommandSlotConfigs() const { return CommandSlotConfigs; }

	/** Set command slot configurations */
	UFUNCTION(BlueprintCallable, Category = "Timeline Editor")
	void SetCommandSlotConfigs(const TArray<FCommandSlotConfig>& Configs);

	/** Get number of command slots */
	UFUNCTION(BlueprintPure, Category = "Timeline Editor")
	int32 GetCommandSlotCount() const { return CommandSlotConfigs.Num(); }

	/** Get command slot configuration at index */
	UFUNCTION(BlueprintPure, Category = "Timeline Editor")
	FCommandSlotConfig GetCommandSlotConfig(int32 Index) const;

	/** Call when a command button is clicked in the editor */
	UFUNCTION(BlueprintCallable, Category = "Timeline Editor")
	void OnCommandButtonClicked(int32 SlotIndex);

public:
	/** Command slot configurations (icons and input keys for each command) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Timeline Editor")
	TArray<FCommandSlotConfig> CommandSlotConfigs;

	/** Optional: Manually set button widgets for each command slot */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Timeline Editor")
	TArray<class UButton*> CommandButtons;

protected:
	/** Get the GameManager from the world */
	AGameManager* GetGameManager() const;

	/** Get the UMG widget instance from the active timeline */
	UArmTimelineUMGWidget* GetActiveUMGWidget() const;

private:
	/** Cached reference to GameManager */
	UPROPERTY()
	AGameManager* CachedGameManager = nullptr;

	/** Cursor position in the active timeline */
	int32 CurrentCursorPosition = 0;
};
