#pragma once

#include "CoreMinimal.h"
#include "Components/WidgetComponent.h"
#include "Sim/ArmTypes.h"
#include "Framework/GameTickManager.h"
#include "UMG.h"
#include "ArmTimelineWidget.generated.h"

class UUserWidget;
class UTexture2D;

/**
 * 3D Widget component that displays a timeline of arm commands
 * Shows command icons in sequence with highlighting for current command
 */
UCLASS(BlueprintType, ClassGroup=(Custom), meta=(BlueprintSpawnableComponent))
class AUTOMATIONGAME_API UArmTimelineWidget : public UWidgetComponent
{
	GENERATED_BODY()

public:
	UArmTimelineWidget();

	virtual void BeginPlay() override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	/** Handle simulation step events for performance optimization */
	UFUNCTION()
	void OnSimulationStep(float StepSeconds);

	/** Set the arm program to display */
	UFUNCTION(BlueprintCallable, Category = "Arm Timeline")
	void SetArmProgram(const TArray<FArmInstr>& Program);

	/** Update the current command being executed */
	UFUNCTION(BlueprintCallable, Category = "Arm Timeline")
	void SetCurrentCommand(int32 CommandIndex, float Progress = 0.0f);

	/** Set the command textures for each command type */
	UFUNCTION(BlueprintCallable, Category = "Arm Timeline")
	void SetCommandTextures(const TMap<EArmCmd, UTexture2D*>& Textures);

	/** Get the current program */
	UFUNCTION(BlueprintPure, Category = "Arm Timeline")
	const TArray<FArmInstr>& GetArmProgram() const { return ArmProgram; }

	/** Get the current command index */
	UFUNCTION(BlueprintPure, Category = "Arm Timeline")
	int32 GetCurrentCommandIndex() const { return CurrentCommandIndex; }

	/** Get the current command progress (0.0 to 1.0) */
	UFUNCTION(BlueprintPure, Category = "Arm Timeline")
	float GetCurrentCommandProgress() const { return CurrentCommandProgress; }

	/** Set widget visibility */
	UFUNCTION(BlueprintCallable, Category = "Arm Timeline")
	void SetTimelineWidgetVisible(bool bIsVisible);

	/** Set the scale of the timeline widget */
	UFUNCTION(BlueprintCallable, Category = "Arm Timeline")
	void SetTimelineScale(float Scale);

	/** Set the offset from the arm position */
	UFUNCTION(BlueprintCallable, Category = "Arm Timeline")
	void SetTimelineOffset(const FVector& Offset);

	/** Set the widget class to use for timeline display */
	UFUNCTION(BlueprintCallable, Category = "Arm Timeline")
	void SetTimelineWidgetClass(TSubclassOf<UUserWidget> NewWidgetClass);

	/** Update widget position to follow the arm */
	UFUNCTION(BlueprintCallable, Category = "Arm Timeline")
	void UpdateWidgetPosition(const FVector& ArmLocation, const FRotator& ArmRotation);

	/** Update widget position using arm's hex coordinates */
	UFUNCTION(BlueprintCallable, Category = "Arm Timeline")
	void UpdateWidgetPositionFromHex(const FHex& HexCoord, float BaseZ, const FRotator& ArmRotation);

	/** Set the default command texture */
	UFUNCTION(BlueprintCallable, Category = "Arm Timeline")
	void SetDefaultCommandTexture(UTexture2D* DefaultTexture);

	/** Set the icon size for command icons */
	UFUNCTION(BlueprintCallable, Category = "Arm Timeline")
	void SetIconSize(float IconSize);

	/** Set the background color for command icons */
	UFUNCTION(BlueprintCallable, Category = "Arm Timeline")
	void SetIconBackgroundColor(const FLinearColor& IconBackgroundColor);

public:
	/** Widget class to use for the timeline display */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Arm Timeline")
	TSubclassOf<UUserWidget> TimelineWidgetClass;

	/** Scale of the timeline widget */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Arm Timeline")
	float TimelineScale = 1.0f;

	/** Offset from the arm position */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Arm Timeline")
	FVector TimelineOffset = FVector(0.0f, 0.0f, 100.0f);

	/** Whether the timeline is visible */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Arm Timeline")
	bool bTimelineVisible = true;

	/** Maximum number of commands to display */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Arm Timeline", meta = (ClampMin = "1", ClampMax = "20"))
	int32 MaxDisplayedCommands = 10;

	/** Command textures for each command type */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Arm Timeline")
	TMap<EArmCmd, UTexture2D*> CommandTextures;

	/** Default texture for unknown commands */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Arm Timeline")
	UTexture2D* DefaultCommandTexture = nullptr;

	/** Highlight color for current command */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Arm Timeline")
	FLinearColor CurrentCommandColor = FLinearColor::Yellow;

	/** Normal color for other commands */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Arm Timeline")
	FLinearColor NormalCommandColor = FLinearColor::White;

	/** Completed command color */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Arm Timeline")
	FLinearColor CompletedCommandColor = FLinearColor::Green;

	/** Size of command icons */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Arm Timeline")
	float IconSize = 32.0f;

	/** Background color for command icons */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Arm Timeline")
	FLinearColor IconBackgroundColor = FLinearColor(0.2f, 0.2f, 0.2f, 0.8f);

private:
	/** The arm program being displayed */
	TArray<FArmInstr> ArmProgram;

	/** Current command index being executed */
	int32 CurrentCommandIndex = -1;

	/** Progress of current command (0.0 to 1.0) */
	float CurrentCommandProgress = 0.0f;

	/** Cached widget instance */
	UPROPERTY()
	UUserWidget* TimelineWidgetInstance = nullptr;

	/** Initialize the widget */
	void InitializeWidget();

	/** Update the widget content */
	void UpdateWidgetContent();

	/** Get texture for a command type */
	UTexture2D* GetTextureForCommand(EArmCmd Command) const;

	/** Get display name for a command */
	FString GetCommandDisplayName(EArmCmd Command) const;
};
