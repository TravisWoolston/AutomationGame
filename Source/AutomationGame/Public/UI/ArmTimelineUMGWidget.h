#pragma once


#include "Blueprint/UserWidget.h"
#include "Components/HorizontalBox.h"
#include "Components/Image.h"
#include "Components/TextBlock.h"
#include "CoreMinimal.h"
#include "Sim/ArmTypes.h"
#include "UMG.h"
// .generated.h must be the last include
#include "ArmTimelineUMGWidget.generated.h"

class UTexture2D;

/**
 * UMG Widget for displaying arm command timeline
 * Shows command icons in a horizontal layout with highlighting
 */
UCLASS(BlueprintType, Blueprintable)
class AUTOMATIONGAME_API UArmTimelineUMGWidget : public UUserWidget {
  GENERATED_BODY()

public:
  UArmTimelineUMGWidget(const FObjectInitializer &ObjectInitializer);

  virtual void NativeConstruct() override;
  virtual void NativeDestruct() override;
  virtual void NativeTick(const FGeometry &MyGeometry,
                          float InDeltaTime) override;

  /** Set the arm program to display */
  UFUNCTION(BlueprintCallable, Category = "Arm Timeline")
  void SetArmProgram(const TArray<FArmInstr> &Program);

  /** Update the current command being executed */
  UFUNCTION(BlueprintCallable, Category = "Arm Timeline")
  void SetCurrentCommand(int32 CommandIndex, float Progress = 0.0f);

  /** Set the command textures for each command type */
  UFUNCTION(BlueprintCallable, Category = "Arm Timeline")
  void SetCommandTextures(const TMap<EArmCmd, UTexture2D *> &Textures);

  /** Set the default command texture */
  UFUNCTION(BlueprintCallable, Category = "Arm Timeline")
  void SetDefaultCommandTexture(UTexture2D *DefaultTexture);

  /** Set the maximum number of commands to display */
  UFUNCTION(BlueprintCallable, Category = "Arm Timeline")
  void SetMaxDisplayedCommands(int32 MaxCommands);

  /** Set the colors for different command states */
  UFUNCTION(BlueprintCallable, Category = "Arm Timeline")
  void SetCommandColors(const FLinearColor &CurrentColor,
                        const FLinearColor &NormalColor,
                        const FLinearColor &CompletedColor);

  /** Set the icon size for command icons */
  UFUNCTION(BlueprintCallable, Category = "Arm Timeline")
  void SetIconSize(float IconSize);

  /** Set spacing between command icons (in pixels) */
  UFUNCTION(BlueprintCallable, Category = "Arm Timeline")
  void SetIconSpacing(float InIconSpacing);

  /** Set the background color for command icons */
  UFUNCTION(BlueprintCallable, Category = "Arm Timeline")
  void SetIconBackgroundColor(const FLinearColor &IconBackgroundColor);

  /** Set the border color for the command list */
  UFUNCTION(BlueprintCallable, Category = "Arm Timeline")
  void SetListBorderColor(const FLinearColor &BorderColor);

  /** Set the border thickness for the command list */
  UFUNCTION(BlueprintCallable, Category = "Arm Timeline")
  void SetListBorderThickness(float Thickness);

  /** Set whether to pulse the current command */
  UFUNCTION(BlueprintCallable, Category = "Arm Timeline")
  void SetPulseCurrentCommand(bool bPulse);

  /** Set the normal command color */
  UFUNCTION(BlueprintCallable, Category = "Arm Timeline")
  void SetNormalCommandColor(const FLinearColor &NormalColor);

  /** Set the current command color */
  UFUNCTION(BlueprintCallable, Category = "Arm Timeline")
  void SetCurrentCommandColor(const FLinearColor &CurrentColor);

  /** Set the completed command color */
  UFUNCTION(BlueprintCallable, Category = "Arm Timeline")
  void SetCompletedCommandColor(const FLinearColor &CompletedColor);

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

  /** Called when cursor position changes (implement in Blueprint to update
   * visual) */
  UFUNCTION(BlueprintImplementableEvent, Category = "Timeline Editor")
  void OnCursorPositionChanged(int32 NewPosition);

  /** Create a command icon widget */
  UWidget *CreateCommandIcon(EArmCmd Command, int32 Index);

  /** Update a command icon widget */
  UFUNCTION(BlueprintCallable, Category = "Arm Timeline")
  void UpdateCommandIcon(UWidget *IconWidget, EArmCmd Command, int32 Index,
                         bool bIsCurrent, bool bIsCompleted, float Progress);

  /** Blueprint implementable event for custom icon updates */
  UFUNCTION(BlueprintImplementableEvent, Category = "Arm Timeline")
  void OnUpdateCommandIcon(UWidget *IconWidget, EArmCmd Command, int32 Index,
                           bool bIsCurrent, bool bIsCompleted, float Progress);

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
  TMap<EArmCmd, UTexture2D *> CommandTextures;

  /** Default texture for unknown commands */
  UPROPERTY(BlueprintReadOnly, Category = "Arm Timeline")
  UTexture2D *DefaultCommandTexture = nullptr;

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

  /** Spacing between command icons (total gap between neighbors) */
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Arm Timeline")
  float IconSpacing = 10.0f;

  /** Background color for command icons */
  UPROPERTY(BlueprintReadOnly, Category = "Arm Timeline")
  FLinearColor IconBackgroundColor = FLinearColor(0.2f, 0.2f, 0.2f, 0.8f);

  /** Border color for the command list */
  UPROPERTY(BlueprintReadOnly, Category = "Arm Timeline")
  FLinearColor ListBorderColor = FLinearColor(0.2f, 0.2f, 0.2f, 0.8f);

  /** Border thickness for the command list */
  UPROPERTY(BlueprintReadOnly, Category = "Arm Timeline")
  float ListBorderThickness = 2.0f;

  /** Whether to pulse the current command icon */
  UPROPERTY(BlueprintReadOnly, Category = "Arm Timeline")
  bool bPulseCurrentCommand = true;

  /** Cached left padding used for the command list visual */
  UPROPERTY(BlueprintReadOnly, Category = "Arm Timeline")
  float CommandListLeftPadding = 0.0f;

  /** Cached first visible index (used by sliding window) */
  UPROPERTY(BlueprintReadOnly, Category = "Arm Timeline")
  int32 VisibleStartIndex = 0;

  /** Insert a command at the cursor position */
  UFUNCTION(BlueprintCallable, Category = "Timeline Editor")
  bool InsertCommandAtCursor(EArmCmd Command, int32 Arg = 0);

  /** Remove a command at the specified index */
  UFUNCTION(BlueprintCallable, Category = "Timeline Editor")
  bool RemoveCommandAt(int32 Index);

  /** Get the command at a specific index */
  UFUNCTION(BlueprintCallable, Category = "Timeline Editor")
  FArmInstr GetCommandAt(int32 Index) const;

  /** Move cursor to the left */
  UFUNCTION(BlueprintCallable, Category = "Timeline Editor")
  void MoveCursorLeft();

  /** Move cursor to the right */
  UFUNCTION(BlueprintCallable, Category = "Timeline Editor")
  void MoveCursorRight();

  /** Set cursor to a specific position */
  UFUNCTION(BlueprintCallable, Category = "Timeline Editor")
  void SetCursorPosition(int32 Position);

  /** Get cursor position */
  UFUNCTION(BlueprintCallable, Category = "Timeline Editor")
  int32 GetCursor() const;

  /** Enable editing mode */
  UFUNCTION(BlueprintCallable, Category = "Timeline Editor")
  void EnableEditingMode();

  /** Disable editing mode */
  UFUNCTION(BlueprintCallable, Category = "Timeline Editor")
  void DisableEditingMode();

  /** Get the cursor visual line widget */
  UFUNCTION(BlueprintPure, Category = "Timeline Editor")
  class UBorder *GetCursorLine() const;

  /** Width of one command cell (icon + spacing) for cursor math */
  UFUNCTION(BlueprintPure, Category = "Timeline Editor")
  float GetUnitWidth() const { return IconSize + IconSpacing; }

  /** Left padding applied before the first icon for cursor math */
  UFUNCTION(BlueprintPure, Category = "Timeline Editor")
  float GetListLeftPadding() const { return CommandListLeftPadding; }

  /** First visible program index in the current sliding window */
  UFUNCTION(BlueprintPure, Category = "Timeline Editor")
  int32 GetVisibleStartIndex() const { return VisibleStartIndex; }

  /** Synchronize the current ArmProgram back to the associated arm's Program */
  UFUNCTION(BlueprintCallable, Category = "Timeline Editor")
  void SyncProgramToArm();

  /** Get the associated arm */
  UFUNCTION(BlueprintPure, Category = "Timeline Editor")
  class ABasicArm *GetAssociatedArm() const;

  /** Set the associated arm */
  UFUNCTION(BlueprintCallable, Category = "Timeline Editor")
  void SetAssociatedArm(class ABasicArm *Arm);

  /** Update target scroll index based on current execution or cursor position
   */
  void UpdateScrollTarget();

  /** Cursor position in the command list (where commands will be inserted) */
  UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Timeline Editor")
  int32 CursorPosition = 0;

  /** Whether the timeline is in editing mode */
  UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Timeline Editor")
  bool bIsEditingMode = false;

  /** Command icon textures for editing interface */
  UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Timeline Editor")
  TMap<EArmCmd, UTexture2D *> EditorCommandIcons;

  /** Horizontal box containing command icons */
  UPROPERTY(BlueprintReadOnly, meta = (BindWidget))
  UHorizontalBox *CommandIconsBox = nullptr;

  /** Cursor indicator widget (visual line showing where commands will be
   * inserted) */
  UPROPERTY(BlueprintReadOnly, meta = (BindWidget))
  UWidget *CursorIndicator = nullptr;

  /** Horizontal box for cursor indicator */
  UPROPERTY(BlueprintReadOnly, meta = (BindWidget))
  UHorizontalBox *CursorBox = nullptr;

  /** Reset button to immediately trigger reset command (created
   * programmatically) */
  UPROPERTY()
  class UButton *ResetButton = nullptr;

  /** Input for starting reach distance */
  UPROPERTY()
  class UEditableTextBox *ReachInput = nullptr;

  /** Input for starting rotation */
  UPROPERTY()
  class UEditableTextBox *RotationInput = nullptr;

  /** Container for property inputs (created programmatically) */
  UPROPERTY()
  class UHorizontalBox *PropertyInputBox = nullptr;

  /** Trigger an immediate reset command for the associated arm */
  UFUNCTION(BlueprintCallable, Category = "Timeline Editor")
  void TriggerImmediateReset();

  /** Handle starting reach change */
  UFUNCTION()
  void OnReachCommitted(const FText &Text, ETextCommit::Type CommitMethod);

  /** Handle starting rotation change */
  UFUNCTION()
  void OnRotationCommitted(const FText &Text, ETextCommit::Type CommitMethod);

private:
  /** Array of command icon widgets */
  UPROPERTY()
  TArray<UWidget *> CommandIconWidgets;

  /** Whether icons have been created and are stable */
  bool bIconsCreated = false;

  /** Initialize the widget */
  void InitializeWidget();

  /** Update the widget content */
  void UpdateWidgetContent();

  /** Check if editing mode should be active based on timeline visibility */
  void CheckEditingMode();

  /** Get texture for a command type */
  UTexture2D *GetTextureForCommand(EArmCmd Command) const;

  /** Get display name for a command */
  FString GetCommandDisplayName(EArmCmd Command) const;

  /** Create command icon widgets */
  void CreateCommandIcons();

  /** Update command icon widgets */
  void UpdateCommandIcons();

  /** Update the cursor indicator position (including smooth scrolling offset)
   */
  void UpdateCursorIndicator();

  /** Update the cursor indicator position */
  void SetCursorIndicatorPosition();

  /** Create the reset button programmatically */
  void CreateResetButton();

  /** Create property inputs (reach/rotation) programmatically */
  void CreatePropertyInputs();

private:
  /** Pointer to the arm this timeline is associated with */
  UPROPERTY()
  class ABasicArm *AssociatedArm = nullptr;

  /** Visual scroll interpolation */
  float CurrentScrollIndex = 0.0f;
  float TargetScrollIndex = 0.0f;

  /** Last seen command index/progress for throttling updates */
  int32 LastCommandIndex = -1;
  float LastProgress = 0.0f;

  /** The container that holds the icons and gets shifted for smooth scrolling
   */
  UPROPERTY()
  UWidget *IconScrollContainer = nullptr;
};
