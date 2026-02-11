#include "UI/ArmTimelineUMGWidget.h"
#include "AGLogChannels.h"
#include "Actors/BasicArm.h"
#include "Actors/OrbSimDriver.h"
#include "Components/ActorComponent.h"
#include "Components/Button.h"
#include "Components/EditableTextBox.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Image.h"
#include "Components/PanelWidget.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Engine/Texture2D.h"
#include "EngineUtils.h"
#include "Framework/GameManager.h"
#include "Sim/ArmTypes.h"
#include "Styling/CoreStyle.h"
#include "UI/ArmTimelineWidget.h"
#include "UMG.h"

UArmTimelineUMGWidget::UArmTimelineUMGWidget(
    const FObjectInitializer &ObjectInitializer)
    : Super(ObjectInitializer) {
  // Initialize default values
  CurrentCommandIndex = -1;
  CurrentCommandProgress = 0.0f;
  MaxDisplayedCommands = 10;
  CurrentCommandColor = FLinearColor::Yellow;
  NormalCommandColor = FLinearColor::White;
  CompletedCommandColor = FLinearColor::Green;
  SelectedCommandColor = FLinearColor::Blue;
  ListBorderColor = FLinearColor(0.2f, 0.2f, 0.2f, 0.8f);
  ListBorderThickness = 2.0f;
  bPulseCurrentCommand = true;
  LastCommandIndex = -1;
  LastProgress = 0.0f;
}

void UArmTimelineUMGWidget::NativeConstruct() {
  Super::NativeConstruct();

  // Disable clipping on the root widget to prevent cropping
  SetClipping(EWidgetClipping::Inherit);

  // Set a large minimum desired size to ensure the widget can expand to show
  // all content This helps prevent cropping when there are many command icons
  SetMinimumDesiredSize(FVector2D(4000.0f, 400.0f));

  // Ensure CommandIconsBox doesn't clip its content
  if (CommandIconsBox) {
    CommandIconsBox->SetClipping(EWidgetClipping::Inherit);
  }

  InitializeWidget();
}

void UArmTimelineUMGWidget::NativeTick(const FGeometry &MyGeometry,
                                       float InDeltaTime) {
  Super::NativeTick(MyGeometry, InDeltaTime);

  // Lerp scroll position
  if (!FMath::IsNearlyEqual(CurrentScrollIndex, TargetScrollIndex, 0.001f)) {
    CurrentScrollIndex = FMath::FInterpTo(CurrentScrollIndex, TargetScrollIndex,
                                          InDeltaTime, 10.0f);

    if (IconScrollContainer) {
      // Calculate the visual offset based on the fractional part of the scroll
      // index
      float Fraction = CurrentScrollIndex - (float)VisibleStartIndex;
      float OffsetX = -Fraction * GetUnitWidth();

      IconScrollContainer->SetRenderTranslation(FVector2D(OffsetX, 0.0f));
    }
  }
}

void UArmTimelineUMGWidget::SynchronizeProperties() {
  Super::SynchronizeProperties();

  // If we are in the designer and have no program, add some mock data for
  // preview
  if (IsDesignTime()) {
    if (ArmProgram.Num() == 0) {
      TArray<FArmInstr> MockProgram;
      MockProgram.Add({EArmCmd::Grab, 0});
      MockProgram.Add({EArmCmd::RotateCW, 1});
      MockProgram.Add({EArmCmd::Extend, 1});
      MockProgram.Add({EArmCmd::Drop, 0});
      MockProgram.Add({EArmCmd::Reset, 0});

      // Don't call SetArmProgram as it triggers callbacks we might not want in
      // designer
      ArmProgram = MockProgram;
    }

    // Force icon recreation in designer to reflect property changes immediately
    bIconsCreated = false;
  }

  UpdateWidgetContent();
}

void UArmTimelineUMGWidget::NativeDestruct() {
  Super::NativeDestruct();

  // Clean up command icon widgets
  for (UWidget *Widget : CommandIconWidgets) {
    if (Widget && Widget->IsValidLowLevel()) {
      Widget->RemoveFromParent();
    }
  }
  CommandIconWidgets.Empty();
}

void UArmTimelineUMGWidget::SetArmProgram(const TArray<FArmInstr> &Program) {
  // Only update if the program actually changed
  if (ArmProgram != Program) {
    ArmProgram = Program;
    CurrentCommandIndex = -1;
    CurrentCommandProgress = 0.0f;

    // Clear existing icons so they get recreated with new program
    for (UWidget *Widget : CommandIconWidgets) {
      if (Widget && Widget->IsValidLowLevel()) {
        Widget->RemoveFromParent();
      }
    }
    CommandIconWidgets.Empty();
    bIconsCreated = false; // Reset caching flag since program changed

    // Ensure minimum desired size is maintained to prevent shrinking
    SetMinimumDesiredSize(FVector2D(4000.0f, 400.0f));

    UpdateWidgetContent();
    OnProgramChanged();
  }
}

void UArmTimelineUMGWidget::ToggleCursorPreview() {
  if (bIsEditingMode) {
    DisableEditingMode();
  } else {
    EnableEditingMode();
  }
}

void UArmTimelineUMGWidget::InsertDummyCommand() {
  if (bIsEditingMode) {
    InsertCommandAtCursor(EArmCmd::NoOp);
  }
}

void UArmTimelineUMGWidget::PreviewMoveCursorLeft() {
  if (bIsEditingMode) {
    MoveCursorLeft();
  }
}

void UArmTimelineUMGWidget::PreviewMoveCursorRight() {
  if (bIsEditingMode) {
    MoveCursorRight();
  }
}

void UArmTimelineUMGWidget::SetCurrentCommand(int32 CommandIndex,
                                              float Progress) {
  CurrentCommandIndex = FMath::Clamp(CommandIndex, -1, ArmProgram.Num() - 1);
  CurrentCommandProgress = FMath::Clamp(Progress, 0.0f, 1.0f);

  // Only update if the command index actually changed or progress changed
  // significantly
  bool bCommandChanged = (CurrentCommandIndex != LastCommandIndex);
  bool bProgressChanged = FMath::Abs(CurrentCommandProgress - LastProgress) >
                          0.05f; // More frequent progress updates (5%)

  if (bCommandChanged || bProgressChanged) {
    UpdateWidgetContent();
    OnCurrentCommandChanged();

    LastCommandIndex = CurrentCommandIndex;
    LastProgress = CurrentCommandProgress;
  }
}

void UArmTimelineUMGWidget::SetCommandTextures(
    const TMap<EArmCmd, UTexture2D *> &Textures) {
  CommandTextures = Textures;

  // Only log once to reduce spam
  static bool bLoggedTextures = false;
  if (!bLoggedTextures) {
    bLoggedTextures = true;
  }

  UpdateWidgetContent();
  OnCommandTexturesUpdated();
}

void UArmTimelineUMGWidget::SetDefaultCommandTexture(
    UTexture2D *DefaultTexture) {
  // Only update if texture actually changed
  if (DefaultCommandTexture != DefaultTexture) {
    DefaultCommandTexture = DefaultTexture;

    UpdateWidgetContent();
  }
}

void UArmTimelineUMGWidget::SetMaxDisplayedCommands(int32 MaxCommands) {
  MaxDisplayedCommands = FMath::Clamp(MaxCommands, 1, 20);

  UpdateWidgetContent();
}

void UArmTimelineUMGWidget::SetCommandColors(
    const FLinearColor &CurrentColor, const FLinearColor &NormalColor,
    const FLinearColor &CompletedColor) {
  CurrentCommandColor = CurrentColor;
  NormalCommandColor = NormalColor;
  CompletedCommandColor = CompletedColor;

  UpdateWidgetContent();
}

void UArmTimelineUMGWidget::SetIconSize(float NewIconSize) {
  IconSize = FMath::Clamp(NewIconSize, 16.0f, 128.0f);

  UpdateWidgetContent();
}

void UArmTimelineUMGWidget::SetIconSpacing(float InIconSpacing) {
  IconSpacing = FMath::Max(0.0f, InIconSpacing);
  UpdateWidgetContent();
}

void UArmTimelineUMGWidget::SetListBorderColor(
    const FLinearColor &BorderColor) {
  ListBorderColor = BorderColor;
  UpdateWidgetContent();
}

void UArmTimelineUMGWidget::SetListBorderThickness(float Thickness) {
  ListBorderThickness = Thickness;
  UpdateWidgetContent();
}

void UArmTimelineUMGWidget::SetPulseCurrentCommand(bool bPulse) {
  bPulseCurrentCommand = bPulse;
  UpdateWidgetContent();
}

void UArmTimelineUMGWidget::SetIconBackgroundColor(
    const FLinearColor &NewIconBackgroundColor) {
  IconBackgroundColor = NewIconBackgroundColor;

  UpdateWidgetContent();
}

void UArmTimelineUMGWidget::InitializeWidget() {
  // Initialize any default values or setup

  // Bind reset button if it exists
  if (ResetButton) {
    ResetButton->OnClicked.AddDynamic(
        this, &UArmTimelineUMGWidget::TriggerImmediateReset);
  }

  OnWidgetConstructed();
}

void UArmTimelineUMGWidget::UpdateWidgetContent() {
  // Use caching flag to prevent unnecessary recreation
  bool bNeedsRecreation = !bIconsCreated;

  // Check if icon count matches expected count for current program
  if (!bNeedsRecreation) {
    int32 ExpectedIconCount =
        FMath::Min(ArmProgram.Num(), MaxDisplayedCommands);
    if (CommandIconWidgets.Num() != ExpectedIconCount) {
      bNeedsRecreation = true;
    }
  }

  if (bNeedsRecreation) {
    // Recreate all UI elements (Icons, Reset Button, Property Inputs)
    CreateCommandIcons();
    // bIconsCreated is set to true inside CreateCommandIcons
  } else {
    // Just update visual states of existing icons
    UpdateCommandIcons();
  }

  // Update property input values if arm exists
  if (ABasicArm *Arm = GetAssociatedArm()) {
    if (ReachInput && !ReachInput->HasKeyboardFocus()) {
      FText TargetText = FText::AsNumber(Arm->StartingReach);
      if (!ReachInput->GetText().EqualTo(TargetText)) {
        ReachInput->SetText(TargetText);
      }
    }
    if (RotationInput && !RotationInput->HasKeyboardFocus()) {
      FText TargetText = FText::AsNumber(Arm->StartingRotation);
      if (!RotationInput->GetText().EqualTo(TargetText)) {
        RotationInput->SetText(TargetText);
      }
    }
  }

  // UE_LOG(LogTemp, Verbose, TEXT("ArmTimelineUMGWidget: UpdateWidgetContent -
  // %d commands, current %d"),
  //	ArmProgram.Num(), CurrentCommandIndex);
}

UTexture2D *UArmTimelineUMGWidget::GetTextureForCommand(EArmCmd Command) const {
  if (const UTexture2D *const *FoundTexture = CommandTextures.Find(Command)) {
    return const_cast<UTexture2D *>(*FoundTexture);
  }

  return DefaultCommandTexture;
}

FString UArmTimelineUMGWidget::GetCommandDisplayName(EArmCmd Command) const {
  switch (Command) {
  case EArmCmd::NoOp:
    return TEXT("NoOp");
  case EArmCmd::Grab:
    return TEXT("Grab");
  case EArmCmd::Drop:
    return TEXT("Drop");
  case EArmCmd::RotateCW:
    return TEXT("Rotate CW");
  case EArmCmd::RotateCCW:
    return TEXT("Rotate CCW");
  case EArmCmd::Wait:
    return TEXT("Wait");
  case EArmCmd::Reset:
    return TEXT("Reset");
  case EArmCmd::Extend:
    return TEXT("Extend");
  case EArmCmd::Retract:
    return TEXT("Retract");
  case EArmCmd::RotateHeldCW:
    return TEXT("Rotate Held CW");
  case EArmCmd::RotateHeldCCW:
    return TEXT("Rotate Held CCW");
  case EArmCmd::WaitForBond:
    return TEXT("Wait For Bond");
  default:
    return TEXT("Unknown");
  }
}

void UArmTimelineUMGWidget::CreateCommandIcons() {
  // Clear existing icons
  for (UWidget *Widget : CommandIconWidgets) {
    if (Widget && Widget->IsValidLowLevel()) {
      Widget->RemoveFromParent();
    }
  }
  CommandIconWidgets.Empty();

  // Clear and remove existing reset button if it exists
  if (ResetButton && ResetButton->IsValidLowLevel()) {
    ResetButton->RemoveFromParent();
    ResetButton = nullptr;
  }

  // Clear and remove existing property inputs if they exist
  if (PropertyInputBox && PropertyInputBox->IsValidLowLevel()) {
    PropertyInputBox->RemoveFromParent();
    PropertyInputBox = nullptr;
    ReachInput = nullptr;
    RotationInput = nullptr;
  }

  // Also clear the container so borders/inner boxes don't accumulate
  if (CommandIconsBox) {
    CommandIconsBox->ClearChildren();
  }

  if (!CommandIconsBox) {
    UE_LOG(LogTemp, Warning,
           TEXT("ArmTimelineUMGWidget: CommandIconsBox is null"));
    return;
  }

  // Add a border around the entire command list
  UBorder *CommandListBorder = NewObject<UBorder>(this);
  if (CommandListBorder) {
    // Set border properties for the command list
    CommandListBorder->SetBrushColor(ListBorderColor);
    CommandListBorder->SetPadding(
        FMargin(ListBorderThickness)); // Border thickness

    // Create a horizontal box inside the border to arrange icons
    UHorizontalBox *InnerHorizontalBox = NewObject<UHorizontalBox>(this);
    if (InnerHorizontalBox) {

      // Add the horizontal box to the border
      CommandListBorder->AddChild(InnerHorizontalBox);
      IconScrollContainer = InnerHorizontalBox;

      // Add the border to the command icons box
      CommandIconsBox->AddChild(CommandListBorder);

      // Update target scroll first to determine window
      UpdateScrollTarget();

      // Discrete start index for the sliding window
      int32 StartIndex = FMath::FloorToInt(TargetScrollIndex);

      // Cache metrics for cursor math
      CommandListLeftPadding = 8.0f; // border padding set above
      VisibleStartIndex = StartIndex;

      // Create icons for the displayed commands
      int32 NumCommandsToShow =
          FMath::Min(ArmProgram.Num() - StartIndex, MaxDisplayedCommands);

      for (int32 i = 0; i < NumCommandsToShow; ++i) {
        int32 CommandIndex = StartIndex + i;
        UWidget *IconWidget =
            CreateCommandIcon(ArmProgram[CommandIndex].Cmd, CommandIndex);
        if (IconWidget) {
          CommandIconWidgets.Add(IconWidget);

          // Add to the inner horizontal box with proper spacing
          UHorizontalBoxSlot *BoxSlot =
              InnerHorizontalBox->AddChildToHorizontalBox(IconWidget);
          if (BoxSlot) {
            const float HalfGap = IconSpacing * 0.5f;
            BoxSlot->SetPadding(FMargin(HalfGap, 0.0f, HalfGap, 0.0f));
            BoxSlot->SetHorizontalAlignment(HAlign_Center);
            BoxSlot->SetVerticalAlignment(VAlign_Center);
          }
        }
      }
    }
  } else {
    // Fallback: create icons directly in the horizontal box using sliding
    // window
    int32 NumCommandsToShow =
        FMath::Min(ArmProgram.Num(), MaxDisplayedCommands);
    int32 StartIndex = 0;

    // Show a window around the current command
    if (CurrentCommandIndex >= 0 && ArmProgram.Num() > MaxDisplayedCommands) {
      // Center the window around the current command
      StartIndex =
          FMath::Max(0, CurrentCommandIndex - MaxDisplayedCommands / 2);
      StartIndex =
          FMath::Min(StartIndex, ArmProgram.Num() - MaxDisplayedCommands);
    }

    // Cache metrics for cursor math (no outer border used here)
    CommandListLeftPadding = 0.0f;
    VisibleStartIndex = StartIndex;

    for (int32 i = 0; i < NumCommandsToShow; ++i) {
      int32 CommandIndex = StartIndex + i;
      UWidget *IconWidget =
          CreateCommandIcon(ArmProgram[CommandIndex].Cmd, CommandIndex);
      if (IconWidget) {
        CommandIconWidgets.Add(IconWidget);

        // Add to horizontal box
        UHorizontalBoxSlot *BoxSlot =
            CommandIconsBox->AddChildToHorizontalBox(IconWidget);
        if (BoxSlot) {
          const float HalfGap = IconSpacing * 0.5f;
          BoxSlot->SetPadding(FMargin(HalfGap, 0.0f, HalfGap, 0.0f));
          BoxSlot->SetHorizontalAlignment(HAlign_Center);
          BoxSlot->SetVerticalAlignment(VAlign_Center);
        }
      }
    }
  }

  // Create reset button beneath CommandIconsBox
  CreateResetButton();

  // Create property inputs beneath reset button
  CreatePropertyInputs();

  bIconsCreated = true;
}

void UArmTimelineUMGWidget::UpdateCommandIcons() {
  // Update scrolling target based on cursor or execution
  UpdateScrollTarget();

  // Discrete start index for the sliding window
  int32 StartIndex = FMath::FloorToInt(TargetScrollIndex);

  // If the window shifted discretely, we might need to recreate icons
  // (or we can just update them if they already exist in the correct count)
  if (StartIndex != VisibleStartIndex) {
    // Force recreation if start index changed to ensure icons match
    // instructions
    bIconsCreated = false;
    CreateCommandIcons();
    return;
  }

  // Cache metrics for cursor math
  VisibleStartIndex = StartIndex;

  // Update existing icons
  for (int32 i = 0; i < CommandIconWidgets.Num(); ++i) {
    if (CommandIconWidgets[i]) {
      int32 CommandIndex = StartIndex + i;
      if (CommandIndex < ArmProgram.Num()) {
        bool bIsCurrent = (CommandIndex == CurrentCommandIndex);
        bool bIsCompleted = (CommandIndex < CurrentCommandIndex);
        float Progress = (bIsCurrent) ? CurrentCommandProgress
                                      : (bIsCompleted ? 1.0f : 0.0f);

        // Handle the SizeBox wrapper if present
        UWidget *TargetWidget = CommandIconWidgets[i];
        if (USizeBox *SB = Cast<USizeBox>(TargetWidget)) {
          TargetWidget = SB->GetContent();
        }

        UpdateCommandIcon(TargetWidget, ArmProgram[CommandIndex].Cmd,
                          CommandIndex, bIsCurrent, bIsCompleted, Progress);
      }
    }
  }
}

UWidget *UArmTimelineUMGWidget::CreateCommandIcon(EArmCmd Command,
                                                  int32 Index) {
  // Create a SizeBox to keep the layout slot fixed size and prevent "bouncing"
  USizeBox *IconContainerBox = NewObject<USizeBox>(this);
  if (!IconContainerBox)
    return nullptr;

  // Set a fixed size for the slot that can accommodate the pulsed/expanded icon
  float MaxExpectedSize = IconSize * 1.5f;
  IconContainerBox->SetWidthOverride(MaxExpectedSize);
  IconContainerBox->SetHeightOverride(MaxExpectedSize);

  // Create a border widget to wrap the icon
  UBorder *BorderWidget = NewObject<UBorder>(this);
  if (!BorderWidget) {
    return nullptr;
  }

  // Create the image widget for the command icon
  UImage *ImageWidget = NewObject<UImage>(this);
  if (!ImageWidget) {
    return nullptr;
  }

  // Get the texture for this command
  UTexture2D *CommandTexture = GetTextureForCommand(Command);
  if (CommandTexture) {
    ImageWidget->SetBrushFromTexture(CommandTexture);
  } else {
    // Use a default texture or create a colored rectangle
    // Only log this warning once per command type to reduce spam
    static TSet<EArmCmd> LoggedMissingTextures;
    if (!LoggedMissingTextures.Contains(Command)) {
      UE_LOG(LogTemp, Warning,
             TEXT("ArmTimelineUMGWidget: No texture found for command %d"),
             (int32)Command);
      LoggedMissingTextures.Add(Command);
    }
  }

  // Set size using configurable IconSize
  ImageWidget->SetDesiredSizeOverride(FVector2D(IconSize, IconSize));

  // Add the image widget to the border
  BorderWidget->AddChild(ImageWidget);

  // Set border initial state
  bool bIsCurrent = (Index == CurrentCommandIndex);
  bool bIsCompleted = (Index < CurrentCommandIndex);
  float Progress =
      (bIsCurrent) ? CurrentCommandProgress : (bIsCompleted ? 1.0f : 0.0f);

  UpdateCommandIcon(BorderWidget, Command, Index, bIsCurrent, bIsCompleted,
                    Progress);

  // Add the border to the size box and return the box
  IconContainerBox->AddChild(BorderWidget);
  return IconContainerBox;
}

void UArmTimelineUMGWidget::UpdateCommandIcon(UWidget *IconWidget,
                                              EArmCmd Command, int32 Index,
                                              bool bIsCurrent,
                                              bool bIsCompleted,
                                              float Progress) {
  // Handle border widget (which contains the image widget)
  if (UBorder *BorderWidget = Cast<UBorder>(IconWidget)) {
    // Get the image widget from the border
    UImage *ImageWidget = nullptr;
    if (BorderWidget->GetChildrenCount() > 0) {
      ImageWidget = Cast<UImage>(BorderWidget->GetChildAt(0));
    }

    if (ImageWidget) {
      // Update the texture
      UTexture2D *CommandTexture = GetTextureForCommand(Command);
      if (CommandTexture) {
        ImageWidget->SetBrushFromTexture(CommandTexture);
      }

      // Apply size based on command state
      if (!bIsCurrent) {
        // Non-current commands use configurable size
        ImageWidget->SetDesiredSizeOverride(FVector2D(IconSize, IconSize));
      } else {
        // Current command uses configurable size with slight scaling
        float SizeMultiplier = 1.0f + Progress * 0.2f;

        // Apply pulse effect if enabled
        if (bPulseCurrentCommand) {
          float PulseTime = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
          float PulseFactor =
              (FMath::Sin(PulseTime * 10.0f) + 1.0f) * 0.5f; // 0 to 1 sine wave
          SizeMultiplier += PulseFactor * 0.1f; // Add up to 10% extra scale
        }

        float FinalSize = IconSize * SizeMultiplier;
        ImageWidget->SetDesiredSizeOverride(FVector2D(FinalSize, FinalSize));
      }
    }

    // Set border properties based on command state
    FLinearColor BorderColor;
    float BorderThickness = 2.0f;

    // Command that would be removed by delete command
    int32 IndexToRemove =
        FMath::Clamp(CursorPosition, 0, FMath::Max(0, ArmProgram.Num() - 1));
    bool bIsSelected =
        (ArmProgram.Num() > 0 && Index == IndexToRemove && bIsEditingMode);

    if (bIsSelected) {
      // Selected/Affected command gets a distinct blue border
      BorderColor = SelectedCommandColor;
      BorderThickness = 4.0f;
    } else if (bIsCurrent) {
      // Current command gets a bright border
      BorderColor = CurrentCommandColor;
      BorderThickness = 3.0f;
    } else if (bIsCompleted) {
      // Completed commands get a green border
      BorderColor = FLinearColor::Green;
    } else {
      // Regular commands get a subtle border
      BorderColor = FLinearColor(0.5f, 0.5f, 0.5f, 0.8f);
    }

    // Apply border color and thickness
    BorderWidget->SetBrushColor(BorderColor);

    // Set border thickness using padding
    BorderWidget->SetPadding(FMargin(BorderThickness));

  }
  // Fallback for direct image widgets (shouldn't happen with new structure)
  else if (UImage *ImageWidget = Cast<UImage>(IconWidget)) {
    // Update the texture and apply color in the correct order
    UTexture2D *CommandTexture = GetTextureForCommand(Command);
    if (CommandTexture) {
      ImageWidget->SetBrushFromTexture(CommandTexture);
    }

    // Apply size based on command state
    if (!bIsCurrent) {
      // Non-current commands use configurable size
      ImageWidget->SetDesiredSizeOverride(FVector2D(IconSize, IconSize));
    } else {
      // Current command uses configurable size with slight scaling
      float SizeMultiplier = 1.0f + Progress * 0.2f;

      // Apply pulse effect if enabled
      if (bPulseCurrentCommand) {
        float PulseTime = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
        float PulseFactor =
            (FMath::Sin(PulseTime * 10.0f) + 1.0f) * 0.5f; // 0 to 1 sine wave
        SizeMultiplier += PulseFactor * 0.1f; // Add up to 10% extra scale
      }

      float FinalSize = IconSize * SizeMultiplier;
      ImageWidget->SetDesiredSizeOverride(FVector2D(FinalSize, FinalSize));
    }
  }

  // Call Blueprint implementation for any additional customizations
  OnUpdateCommandIcon(IconWidget, Command, Index, bIsCurrent, bIsCompleted,
                      Progress);
}

void UArmTimelineUMGWidget::SetNormalCommandColor(
    const FLinearColor &NormalColor) {
  NormalCommandColor = NormalColor;
}

void UArmTimelineUMGWidget::SetCurrentCommandColor(
    const FLinearColor &CurrentColor) {
  CurrentCommandColor = CurrentColor;
}

void UArmTimelineUMGWidget::SetCompletedCommandColor(
    const FLinearColor &CompletedColor) {
  CompletedCommandColor = CompletedColor;
}

bool UArmTimelineUMGWidget::InsertCommandAtCursor(EArmCmd Command, int32 Arg) {
  // Clamp cursor position to valid range
  CursorPosition = FMath::Clamp(CursorPosition, 0, ArmProgram.Num());

  // Create the new instruction
  FArmInstr NewInstr;
  NewInstr.Cmd = Command;
  NewInstr.Arg = Arg;

  // Insert at cursor position
  ArmProgram.Insert(NewInstr, CursorPosition);

  // Move cursor to next position
  CursorPosition++;

  // Synchronize the program back to the arm
  SyncProgramToArm();

  // Update the widget content (check if recreation is needed)
  UpdateWidgetContent();

  return true;
}

bool UArmTimelineUMGWidget::RemoveCommandAt(int32 Index) {
  // Check if index is valid
  if (Index < 0 || Index >= ArmProgram.Num()) {
    return false;
  }

  // Remove the command
  ArmProgram.RemoveAt(Index);

  // Adjust cursor position if needed
  if (CursorPosition >= ArmProgram.Num()) {
    CursorPosition = ArmProgram.Num();
  }

  // Ensure minimum desired size is maintained to prevent shrinking when
  // commands are removed
  SetMinimumDesiredSize(FVector2D(4000.0f, 400.0f));

  // Synchronize the program back to the arm
  SyncProgramToArm();

  // Update the widget content
  bIconsCreated = false;
  UpdateWidgetContent();

  return true;
}

FArmInstr UArmTimelineUMGWidget::GetCommandAt(int32 Index) const {
  if (Index >= 0 && Index < ArmProgram.Num()) {
    return ArmProgram[Index];
  }

  return FArmInstr();
}

void UArmTimelineUMGWidget::MoveCursorLeft() {
  if (CursorPosition > 0) {
    SetCursorPosition(CursorPosition - 1);
  }
}

void UArmTimelineUMGWidget::MoveCursorRight() {
  if (CursorPosition < ArmProgram.Num()) {
    SetCursorPosition(CursorPosition + 1);
  } else {
    SetCursorPosition(0);
  }
}

void UArmTimelineUMGWidget::SetCursorPosition(int32 Position) {
  CursorPosition = FMath::Clamp(Position, 0, ArmProgram.Num());

  // Update scroll target to follow cursor
  UpdateScrollTarget();

  // Refresh icon borders to show/move the selected command highlight
  UpdateCommandIcons();

  // Notify Blueprint to update the cursor visual (if needed for additional
  // effects)
  OnCursorPositionChanged(CursorPosition);
}

int32 UArmTimelineUMGWidget::GetCursor() const { return CursorPosition; }

void UArmTimelineUMGWidget::EnableEditingMode() {
  bIsEditingMode = true;
  UpdateWidgetContent();
}

void UArmTimelineUMGWidget::DisableEditingMode() {
  bIsEditingMode = false;
  UpdateWidgetContent();
}

void UArmTimelineUMGWidget::UpdateScrollTarget() {
  int32 FocusIndex = -1;

  // Prioritize CursorPosition if in editing mode
  if (bIsEditingMode) {
    FocusIndex = CursorPosition;
  } else if (CurrentCommandIndex >= 0) {
    FocusIndex = CurrentCommandIndex;
  }

  if (FocusIndex >= 0 && ArmProgram.Num() > MaxDisplayedCommands) {
    // Center the FocusIndex in the display window
    float NewTargetScroll =
        (float)FocusIndex - (float)MaxDisplayedCommands / 2.0f;
    TargetScrollIndex =
        FMath::Clamp(NewTargetScroll, 0.0f,
                     (float)ArmProgram.Num() - (float)MaxDisplayedCommands);
  } else {
    TargetScrollIndex = 0.0f;
  }
}

ABasicArm *UArmTimelineUMGWidget::GetAssociatedArm() const {
  return AssociatedArm;
}

void UArmTimelineUMGWidget::SetAssociatedArm(ABasicArm *Arm) {
  AssociatedArm = Arm;

  // Refresh content to show arm properties
  UpdateWidgetContent();
}

void UArmTimelineUMGWidget::TriggerImmediateReset() {
  // Get the associated arm
  ABasicArm *Arm = GetAssociatedArm();
  if (!Arm) {
    UE_LOG(LogTemp, Warning,
           TEXT("TriggerImmediateReset: No associated arm found"));
    return;
  }

  // Get the simulation from the driver
  UWorld *World = GetWorld();
  if (!World) {
    UE_LOG(LogTemp, Warning, TEXT("TriggerImmediateReset: No world found"));
    return;
  }

  AOrbSimDriver *Driver = AOrbSimDriver::Find(World);
  if (!Driver) {
    UE_LOG(LogTemp, Warning,
           TEXT("TriggerImmediateReset: No OrbSimDriver found"));
    return;
  }

  USimWorldHex *Sim = Driver->GetSim();
  if (!Sim) {
    UE_LOG(LogTemp, Warning, TEXT("TriggerImmediateReset: No Sim found"));
    return;
  }

  // Check if the arm has a valid SimArmIndex
  if (Arm->SimArmIndex < 0 || Arm->SimArmIndex >= Sim->Arms.Num()) {
    UE_LOG(
        LogTemp, Warning,
        TEXT("TriggerImmediateReset: Invalid SimArmIndex %d (Sim has %d arms)"),
        Arm->SimArmIndex, Sim->Arms.Num());
    return;
  }

  // Get the arm state from the simulation
  FArmState &ArmState = Sim->Arms[Arm->SimArmIndex];

  // Check if the tape is valid
  if (ArmState.Tape.Num() == 0) {
    UE_LOG(LogTemp, Warning, TEXT("TriggerImmediateReset: Arm has empty tape"));
    return;
  }

  // Insert a Reset command at the current PC position to execute immediately
  FOp ResetOp;
  ResetOp.Cmd = ECmd::Reset;
  ResetOp.Arg = 0;

  // Clear any wait counters so the reset executes immediately
  ArmState.WaitCounter = 0;

  // Insert the reset command at the current PC position
  // This ensures it executes on the next simulation tick
  // The simulation will expand it into drop + rotation + reach adjustments and
  // execute them immediately
  int32 InsertIndex = ArmState.PC;
  ArmState.Tape.Insert(ResetOp, InsertIndex);

  // PC now points to the Reset command we just inserted
  // The simulation will execute it on the next tick and expand it immediately

  UE_LOG(LogTemp, Log,
         TEXT("TriggerImmediateReset: Inserted Reset command at tape index %d "
              "(PC is now %d, pointing at Reset)"),
         InsertIndex, ArmState.PC);
}

void UArmTimelineUMGWidget::CreateResetButton() {
  if (!CommandIconsBox) {
    return;
  }

  // Get the parent widget of CommandIconsBox
  UWidget *ParentWidget = CommandIconsBox->GetParent();
  if (!ParentWidget) {
    UE_LOG(LogTemp, Warning,
           TEXT("CreateResetButton: CommandIconsBox has no parent widget"));
    return;
  }

  // Try to get the parent as a VerticalBox (most common layout)
  UVerticalBox *ParentVerticalBox = Cast<UVerticalBox>(ParentWidget);

  // If parent is not a VerticalBox, try to find a VerticalBox ancestor or
  // create one
  if (!ParentVerticalBox) {
    // Try to find a VerticalBox in the parent hierarchy
    UWidget *CurrentParent = ParentWidget;
    while (CurrentParent) {
      ParentVerticalBox = Cast<UVerticalBox>(CurrentParent);
      if (ParentVerticalBox) {
        break;
      }
      CurrentParent = CurrentParent->GetParent();
    }
  }

  // If we still don't have a VerticalBox, wrap CommandIconsBox in one
  if (!ParentVerticalBox) {
    // Create a new VerticalBox and move CommandIconsBox into it
    ParentVerticalBox = NewObject<UVerticalBox>(this);
    if (!ParentVerticalBox) {
      UE_LOG(LogTemp, Warning,
             TEXT("CreateResetButton: Failed to create VerticalBox"));
      return;
    }

    // Set container to not block hits to its children
    ParentVerticalBox->SetVisibility(ESlateVisibility::SelfHitTestInvisible);

    // Remove CommandIconsBox from its current parent
    CommandIconsBox->RemoveFromParent();

    // Add CommandIconsBox to the new VerticalBox
    UVerticalBoxSlot *IconsSlot =
        ParentVerticalBox->AddChildToVerticalBox(CommandIconsBox);
    if (IconsSlot) {
      IconsSlot->SetHorizontalAlignment(HAlign_Fill);
      IconsSlot->SetVerticalAlignment(VAlign_Top);
    }

    // Add the new VerticalBox to the original parent
    if (ParentWidget) {
      // Try to add to various container types
      if (UVerticalBox *VBox = Cast<UVerticalBox>(ParentWidget)) {
        UVerticalBoxSlot *VerticalSlot =
            VBox->AddChildToVerticalBox(ParentVerticalBox);
        if (VerticalSlot) {
          VerticalSlot->SetHorizontalAlignment(HAlign_Fill);
          VerticalSlot->SetVerticalAlignment(VAlign_Top);
        }
      } else if (UHorizontalBox *HBox = Cast<UHorizontalBox>(ParentWidget)) {
        UHorizontalBoxSlot *HorizontalSlot =
            HBox->AddChildToHorizontalBox(ParentVerticalBox);
        if (HorizontalSlot) {
          HorizontalSlot->SetHorizontalAlignment(HAlign_Fill);
          HorizontalSlot->SetVerticalAlignment(VAlign_Center);
        }
      } else if (UCanvasPanel *Canvas = Cast<UCanvasPanel>(ParentWidget)) {
        // Specifically handle CanvasPanel which is common for root widgets
        UCanvasPanelSlot *CanvasSlot =
            Canvas->AddChildToCanvas(ParentVerticalBox);
        if (CanvasSlot) {
          // Center it in the canvas and size to content
          CanvasSlot->SetAnchors(FAnchors(0.5f, 0.5f));
          CanvasSlot->SetAlignment(FVector2D(0.5f, 0.5f));
          CanvasSlot->SetAutoSize(true);
        }
      } else {
        // Fallback: try to add as child using UPanelWidget interface
        if (UPanelWidget *PanelWidget = Cast<UPanelWidget>(ParentWidget)) {
          PanelWidget->AddChild(ParentVerticalBox);
        } else {
          UE_LOG(LogTemp, Warning,
                 TEXT("CreateResetButton: Parent widget is not a panel widget, "
                      "cannot add child"));
        }
      }
    }
  }

  // Create the reset button
  ResetButton = NewObject<UButton>(this);
  if (!ResetButton) {
    UE_LOG(LogTemp, Warning,
           TEXT("CreateResetButton: Failed to create button"));
    return;
  }

  // Set button properties
  ResetButton->SetIsEnabled(true);

  // Create text label for the button
  UTextBlock *ButtonText = NewObject<UTextBlock>(this);
  if (ButtonText) {
    ButtonText->SetText(FText::FromString(TEXT("Reset")));
    ButtonText->SetColorAndOpacity(LabelTextColor);
    ButtonText->SetJustification(ETextJustify::Center);

    // Set font size
    FSlateFontInfo FontInfo = ButtonText->GetFont();
    FontInfo.Size = LabelFontSize;
    ButtonText->SetFont(FontInfo);

    ResetButton->AddChild(ButtonText);
  }

  // Bind the click event
  ResetButton->OnClicked.AddDynamic(
      this, &UArmTimelineUMGWidget::TriggerImmediateReset);

  // Add the button to the VerticalBox below CommandIconsBox
  if (ParentVerticalBox) {
    UVerticalBoxSlot *ButtonSlot =
        ParentVerticalBox->AddChildToVerticalBox(ResetButton);
    if (ButtonSlot) {
      ButtonSlot->SetHorizontalAlignment(HAlign_Center);
      ButtonSlot->SetVerticalAlignment(VAlign_Top);
      ButtonSlot->SetPadding(ResetButtonPadding);

      // Set button size
      ButtonSlot->SetSize(FSlateChildSize(ESlateSizeRule::Automatic));
    }
  } else {
    UE_LOG(
        LogTemp, Warning,
        TEXT("CreateResetButton: Could not find or create VerticalBox parent"));
  }
}

void UArmTimelineUMGWidget::CheckEditingMode() {
  // Enable editing mode by default when cursor position is valid
  if (!bIsEditingMode && CursorPosition >= 0) {
    EnableEditingMode();
  }
}

void UArmTimelineUMGWidget::SyncProgramToArm() {
  // Only sync if we're in editing mode
  if (!bIsEditingMode) {
    UE_LOG(LogTemp, Warning,
           TEXT("SyncProgramToArm: Editing mode not enabled "
                "(bIsEditingMode=false)"));
    return;
  }

  // Get the associated arm and update its Program
  // If AssociatedArm is not set, try to get it from the GameManager's active
  // timeline
  if (!AssociatedArm) {
    UE_LOG(LogTemp, Warning,
           TEXT("SyncProgramToArm: AssociatedArm not set, attempting to find "
                "from GameManager active timeline..."));

    // Try to get the arm from the GameManager's active timeline
    if (UWorld *World = GetWorld()) {
      for (TActorIterator<AGameManager> It(World); It; ++It) {
        AGameManager *GameManager = *It;
        if (GameManager) {
          ABasicArm *ActiveArm = GameManager->GetActiveArm();
          if (ActiveArm) {
            AssociatedArm = ActiveArm;
            UE_LOG(LogTemp, Log,
                   TEXT("SyncProgramToArm: Found associated arm from "
                        "GameManager: %s"),
                   *ActiveArm->GetName());
            break;
          }
        }
      }
    }

    if (!AssociatedArm) {
      UE_LOG(LogTemp, Warning,
             TEXT("SyncProgramToArm: Could not find associated arm from "
                  "GameManager"));
      return;
    }
  }

  UE_LOG(LogTemp, Log,
         TEXT("SyncProgramToArm: Syncing %d commands to arm '%s'"),
         ArmProgram.Num(), *AssociatedArm->GetName());
  UE_LOG(LogTemp, Log,
         TEXT("SyncProgramToArm: Current arm Program has %d commands"),
         AssociatedArm->Program.Num());

  AssociatedArm->SetProgram(ArmProgram, AssociatedArm->bLoopProgram);

  // Force a pose refresh so the arm visibly resets to the start of the new
  // program
  AssociatedArm->RefreshPose(1.0f);

  UE_LOG(
      LogTemp, Log,
      TEXT("SyncProgramToArm: After SetProgram, arm Program has %d commands"),
      AssociatedArm->Program.Num());
}

void UArmTimelineUMGWidget::CreatePropertyInputs() {
  if (!CommandIconsBox)
    return;

  // Find the VerticalBox created by CreateResetButton
  UVerticalBox *ParentVerticalBox = nullptr;
  UWidget *ParentWidget = CommandIconsBox->GetParent();
  while (ParentWidget) {
    ParentVerticalBox = Cast<UVerticalBox>(ParentWidget);
    if (ParentVerticalBox)
      break;
    ParentWidget = ParentWidget->GetParent();
  }

  if (!ParentVerticalBox)
    return;

  // Create a horizontal box for the inputs
  UHorizontalBox *InputHBox = NewObject<UHorizontalBox>(this);
  if (!InputHBox)
    return;
  InputHBox->SetVisibility(ESlateVisibility::SelfHitTestInvisible);

  // Common label font
  FSlateFontInfo LabelFont;
  const ISlateStyle &CoreStyle = FCoreStyle::Get();
  const FTextBlockStyle &DefaultStyle =
      CoreStyle.GetWidgetStyle<FTextBlockStyle>("NormalText");
  LabelFont = DefaultStyle.Font;
  LabelFont.Size = LabelFontSize;

  // Reach Input
  UVerticalBox *ReachVBox = NewObject<UVerticalBox>(this);
  UTextBlock *ReachLabel = NewObject<UTextBlock>(this);
  ReachLabel->SetText(FText::FromString(TEXT("Start Reach")));
  ReachLabel->SetFont(LabelFont);
  ReachLabel->SetColorAndOpacity(LabelTextColor);
  ReachVBox->AddChildToVerticalBox(ReachLabel);

  ReachInput = NewObject<UEditableTextBox>(this);
  ReachInput->SetToolTipText(FText::FromString(TEXT("Starting Reach (1-6)")));
  ReachInput->WidgetStyle.BackgroundColor = FSlateColor(InputBackgroundColor);
  ReachInput->OnTextCommitted.AddDynamic(
      this, &UArmTimelineUMGWidget::OnReachCommitted);

  // Constrain width of input boxes
  USizeBox *ReachSizeBox = NewObject<USizeBox>(this);
  ReachSizeBox->SetWidthOverride(InputBoxWidth);
  ReachSizeBox->SetHeightOverride(40.0f);
  ReachSizeBox->SetContent(ReachInput);
  ReachSizeBox->SetVisibility(ESlateVisibility::SelfHitTestInvisible);

  UVerticalBoxSlot *ReachInputSlot =
      ReachVBox->AddChildToVerticalBox(ReachSizeBox);
  if (ReachInputSlot) {
    ReachInputSlot->SetPadding(FMargin(0, 2, 0, 0));
    ReachInputSlot->SetHorizontalAlignment(HAlign_Center);
  }
  ReachVBox->SetVisibility(ESlateVisibility::SelfHitTestInvisible);

  UHorizontalBoxSlot *ReachHSlot =
      InputHBox->AddChildToHorizontalBox(ReachVBox);
  if (ReachHSlot) {
    ReachHSlot->SetPadding(FMargin(10, 0, 10, 0));
    ReachHSlot->SetSize(FSlateChildSize(ESlateSizeRule::Automatic));
  }

  // Rotation Input
  UVerticalBox *RotationVBox = NewObject<UVerticalBox>(this);
  UTextBlock *RotationLabel = NewObject<UTextBlock>(this);
  RotationLabel->SetText(FText::FromString(TEXT("Start Rot")));
  RotationLabel->SetFont(LabelFont);
  RotationLabel->SetColorAndOpacity(LabelTextColor);
  RotationVBox->AddChildToVerticalBox(RotationLabel);

  RotationInput = NewObject<UEditableTextBox>(this);
  RotationInput->SetToolTipText(
      FText::FromString(TEXT("Starting Rotation (0-5)")));
  RotationInput->WidgetStyle.BackgroundColor =
      FSlateColor(InputBackgroundColor);
  RotationInput->OnTextCommitted.AddDynamic(
      this, &UArmTimelineUMGWidget::OnRotationCommitted);

  // Constrain width of input boxes
  USizeBox *RotationSizeBox = NewObject<USizeBox>(this);
  RotationSizeBox->SetWidthOverride(InputBoxWidth);
  RotationSizeBox->SetHeightOverride(40.0f);
  RotationSizeBox->SetContent(RotationInput);
  RotationSizeBox->SetVisibility(ESlateVisibility::SelfHitTestInvisible);

  UVerticalBoxSlot *RotInputSlot =
      RotationVBox->AddChildToVerticalBox(RotationSizeBox);
  if (RotInputSlot) {
    RotInputSlot->SetPadding(FMargin(0, 2, 0, 0));
    RotInputSlot->SetHorizontalAlignment(HAlign_Center);
  }
  RotationVBox->SetVisibility(ESlateVisibility::SelfHitTestInvisible);

  UHorizontalBoxSlot *RotationHSlot =
      InputHBox->AddChildToHorizontalBox(RotationVBox);
  if (RotationHSlot) {
    RotationHSlot->SetPadding(FMargin(10, 0, 10, 0));
    RotationHSlot->SetSize(FSlateChildSize(ESlateSizeRule::Automatic));
  }

  // Add the horizontal box to the VerticalBox
  UVerticalBoxSlot *InputSlot =
      ParentVerticalBox->AddChildToVerticalBox(InputHBox);
  if (InputSlot) {
    InputSlot->SetPadding(PropertyInputPadding);
    InputSlot->SetHorizontalAlignment(HAlign_Center);
  }

  // Store for future cleanup
  PropertyInputBox = InputHBox;

  // Initialize values
  if (ABasicArm *Arm = GetAssociatedArm()) {
    ReachInput->SetText(FText::AsNumber(Arm->StartingReach));
    RotationInput->SetText(FText::AsNumber(Arm->StartingRotation));
  }
}

void UArmTimelineUMGWidget::OnReachCommitted(const FText &Text,
                                             ETextCommit::Type CommitMethod) {
  if (CommitMethod == ETextCommit::OnEnter ||
      CommitMethod == ETextCommit::OnUserMovedFocus) {
    int32 NewReach = FCString::Atoi(*Text.ToString());
    if (ABasicArm *Arm = GetAssociatedArm()) {
      Arm->SetStartingReach(NewReach);
    }
  }
}

void UArmTimelineUMGWidget::OnRotationCommitted(
    const FText &Text, ETextCommit::Type CommitMethod) {
  if (CommitMethod == ETextCommit::OnEnter ||
      CommitMethod == ETextCommit::OnUserMovedFocus) {
    int32 NewRotation = FCString::Atoi(*Text.ToString());
    if (ABasicArm *Arm = GetAssociatedArm()) {
      Arm->SetStartingRotation(NewRotation);
    }
  }
}
