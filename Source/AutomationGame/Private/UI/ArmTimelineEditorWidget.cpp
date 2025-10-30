#include "UI/ArmTimelineEditorWidget.h"
#include "UI/ArmTimelineWidget.h"
#include "UI/ArmTimelineUMGWidget.h"
#include "Framework/GameManager.h"
#include "Actors/BasicArm.h"
#include "Framework/Application/SlateApplication.h"
#include "Blueprint/SlateBlueprintLibrary.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/Button.h"
#include "Components/Image.h"
#include "Sim/ArmTypes.h"

UArmTimelineEditorWidget::UArmTimelineEditorWidget(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

void UArmTimelineEditorWidget::NativeConstruct()
{
	Super::NativeConstruct();
	
	// Cache reference to GameManager
	CachedGameManager = GetGameManager();
	
	// Make the widget interactive but not block game input
	SetIsFocusable(false);
	SetVisibility(ESlateVisibility::Visible);
}

// FReply UArmTimelineEditorWidget::NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent)
// {
// 	// Handle key input for command insertion
// 	HandleKeyInput(InKeyEvent.GetKey());
	
// 	// Always handled
// 	return FReply::Handled();
// }

UArmTimelineWidget* UArmTimelineEditorWidget::GetActiveTimeline_Implementation() const
{
	if (CachedGameManager)
	{
		return CachedGameManager->GetActiveTimeline();
	}
	return nullptr;
}

void UArmTimelineEditorWidget::SetActiveTimeline_Implementation(UArmTimelineWidget* Timeline)
{
	if (CachedGameManager)
	{
		CachedGameManager->SetActiveTimeline(Timeline);
	}
}

int32 UArmTimelineEditorWidget::GetCursorPosition_Implementation() const
{
	return CurrentCursorPosition;
}

void UArmTimelineEditorWidget::SetCursorPosition_Implementation(int32 Position)
{
	CurrentCursorPosition = Position;
	
	UArmTimelineUMGWidget* UMGWidget = GetActiveUMGWidget();
	if (UMGWidget)
	{
		UMGWidget->SetCursorPosition(Position);
	}
}

bool UArmTimelineEditorWidget::InsertCommand_Implementation(int32 CommandIndex, ECmd Command, int32 Arg)
{
	UArmTimelineUMGWidget* UMGWidget = GetActiveUMGWidget();
	if (!UMGWidget)
	{
		return false;
	}
	
	// Set cursor to the insertion point
	UMGWidget->SetCursorPosition(CommandIndex);
	
	// Convert ECmd to EArmCmd for insertion
	EArmCmd ArmCommand = ConvertECmdToEArmCmd(Command);
	return UMGWidget->InsertCommandAtCursor(ArmCommand, Arg);
}

bool UArmTimelineEditorWidget::RemoveCommand_Implementation(int32 CommandIndex)
{
	UArmTimelineUMGWidget* UMGWidget = GetActiveUMGWidget();
	if (!UMGWidget)
	{
		return false;
	}
	
	return UMGWidget->RemoveCommandAt(CommandIndex);
}

UTexture2D* UArmTimelineEditorWidget::GetCommandIcon_Implementation(ECmd Command) const
{
	UArmTimelineUMGWidget* UMGWidget = GetActiveUMGWidget();
	if (!UMGWidget)
	{
		return nullptr;
	}
	
	EArmCmd ArmCmd = static_cast<EArmCmd>(Command);
	
	// Get from EditorCommandIcons if available, otherwise use regular CommandTextures
	if (UMGWidget->EditorCommandIcons.Contains(ArmCmd))
	{
		return UMGWidget->EditorCommandIcons[ArmCmd];
	}
	
	if (UMGWidget->CommandTextures.Contains(ArmCmd))
	{
		return UMGWidget->CommandTextures[ArmCmd];
	}
	
	return UMGWidget->DefaultCommandTexture;
}

void UArmTimelineEditorWidget::SetCommandIcon_Implementation(ECmd Command, UTexture2D* Icon)
{
	UArmTimelineUMGWidget* UMGWidget = GetActiveUMGWidget();
	if (!UMGWidget)
	{
		return;
	}
	
	EArmCmd ArmCmd = static_cast<EArmCmd>(Command);
	UMGWidget->EditorCommandIcons.Add(ArmCmd, Icon);
}

TMap<ECmd, UTexture2D*> UArmTimelineEditorWidget::GetAllCommandIcons_Implementation() const
{
	UArmTimelineUMGWidget* UMGWidget = GetActiveUMGWidget();
	if (!UMGWidget)
	{
		return TMap<ECmd, UTexture2D*>();
	}
	
	// Convert from EArmCmd to ECmd
	TMap<ECmd, UTexture2D*> AllIcons;
	
	// Add editor icons first (they take precedence)
	for (const auto& Pair : UMGWidget->EditorCommandIcons)
	{
		AllIcons.Add(static_cast<ECmd>(Pair.Key), Pair.Value);
	}
	
	// Add regular command textures
	for (const auto& Pair : UMGWidget->CommandTextures)
	{
		if (!AllIcons.Contains(static_cast<ECmd>(Pair.Key)))
		{
			AllIcons.Add(static_cast<ECmd>(Pair.Key), Pair.Value);
		}
	}
	
	return AllIcons;
}

ABasicArm* UArmTimelineEditorWidget::GetActiveArm_Implementation() const
{
	if (CachedGameManager)
	{
		return CachedGameManager->GetActiveArm();
	}
	return nullptr;
}

AGameManager* UArmTimelineEditorWidget::GetGameManager() const
{
	if (!GetWorld())
	{
		return nullptr;
	}
	
	for (TActorIterator<AGameManager> It(GetWorld()); It; ++It)
	{
		return *It;
	}
	
	return nullptr;
}

UArmTimelineUMGWidget* UArmTimelineEditorWidget::GetActiveUMGWidget() const
{
	UArmTimelineWidget* TimelineWidget = GetActiveTimeline_Implementation();
	if (!TimelineWidget)
	{
		return nullptr;
	}
	
	UUserWidget* WidgetInstance = TimelineWidget->GetTimelineWidgetInstance();
	return Cast<UArmTimelineUMGWidget>(WidgetInstance);
}

void UArmTimelineEditorWidget::HandleCommandInput(ECmd Command, int32 Arg)
{
	UE_LOG(LogTemp, Log, TEXT("HandleCommandInput: Command=%d, Arg=%d"), (int32)Command, Arg);
	
	UArmTimelineUMGWidget* UMGWidget = GetActiveUMGWidget();
	if (!UMGWidget)
	{
		UE_LOG(LogTemp, Warning, TEXT("HandleCommandInput: No active UMG widget"));
		return;
	}
	
	UE_LOG(LogTemp, Log, TEXT("HandleCommandInput: Found UMG widget"));
	
	// Get live cursor position from the UMG widget (avoid stale local value)
	int32 CursorPos = UMGWidget->GetCursor();
	CurrentCursorPosition = CursorPos;

	UE_LOG(LogTemp, Log, TEXT("HandleCommandInput: Cursor position (live)=%d"), CursorPos);
	
	// Insert command at cursor (convert ECmd to EArmCmd)
	EArmCmd ArmCommand = ConvertECmdToEArmCmd(Command);
	UMGWidget->SetCursorPosition(CursorPos);
	UMGWidget->InsertCommandAtCursor(ArmCommand, Arg);
	
	// Update local cursor position
	CurrentCursorPosition = UMGWidget->GetCursor();
	
	UE_LOG(LogTemp, Log, TEXT("HandleCommandInput: Command inserted successfully"));
}

void UArmTimelineEditorWidget::HandleKeyInput(const FKey& PressedKey)
{
    // Remove key takes priority
    if (RemoveButtonInputKey.IsValid() && PressedKey == RemoveButtonInputKey)
    {
        ExecuteRemoveAtCursor();
        return;
    }

	// Search for matching command slot
	for (const FCommandSlotConfig& Config : CommandSlotConfigs)
	{
		if (Config.InputKey == PressedKey && Config.Command != ECmd::Wait)
		{
			HandleCommandInput(Config.Command, Config.DefaultArg);
			break;
		}
	}
}

void UArmTimelineEditorWidget::SetCommandSlotConfigs(const TArray<FCommandSlotConfig>& Configs)
{
	CommandSlotConfigs = Configs;
}

FCommandSlotConfig UArmTimelineEditorWidget::GetCommandSlotConfig(int32 Index) const
{
	if (Index >= 0 && Index < CommandSlotConfigs.Num())
	{
		return CommandSlotConfigs[Index];
	}
	
	return FCommandSlotConfig();
}

void UArmTimelineEditorWidget::OnCommandButtonClicked(int32 SlotIndex)
{
	if (SlotIndex >= 0 && SlotIndex < CommandSlotConfigs.Num())
	{
		const FCommandSlotConfig& Config = CommandSlotConfigs[SlotIndex];
		
		// Log the assigned command for debugging
		FString CommandName = GetCommandDisplayName(Config.Command);
		UE_LOG(LogTemp, Log, TEXT("Editor Button Clicked: Slot %d -> Command '%s' (Arg: %d)"), 
			SlotIndex, *CommandName, Config.DefaultArg);
		
		HandleCommandInput(Config.Command, Config.DefaultArg);
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("Editor Button Clicked: Invalid slot index %d"), SlotIndex);
	}
}

void UArmTimelineEditorWidget::SetCommandSlotButton(int32 SlotIndex, UButton* Button)
{
	if (SlotIndex >= 0 && SlotIndex < CommandSlotConfigs.Num())
	{
		CommandSlotConfigs[SlotIndex].ButtonWidget = Button;
	}
}

void UArmTimelineEditorWidget::GenerateCommandButtons()
{
	if (!CommandButtonsContainer)
	{
		return;
	}

	// Clear existing buttons
	CommandButtonsContainer->ClearChildren();

    // Generate a button for each command slot
	for (int32 i = 0; i < CommandSlotConfigs.Num(); ++i)
	{
		const FCommandSlotConfig& Config = CommandSlotConfigs[i];

        // Create button
        UButton* Button = NewObject<UButton>(this);
        if (Button)
        {
            // Create overlay to stack icon and input key label
            UOverlay* Overlay = NewObject<UOverlay>(this);
            if (Overlay)
            {
                // Icon image
                if (Config.Icon)
                {
                    UImage* IconImage = NewObject<UImage>(this);
                    if (IconImage)
                    {
                        IconImage->SetBrushFromTexture(Config.Icon);
                        IconImage->SetDesiredSizeOverride(FVector2D(48.0f, 48.0f));
                        Overlay->AddChild(IconImage);
                    }
                }

                // Key label (lower-left)
                if (Config.InputKey.IsValid())
                {
                    UTextBlock* KeyText = NewObject<UTextBlock>(this);
                    if (KeyText)
                    {
                        const FText KeyName = Config.InputKey.GetDisplayName();
                        KeyText->SetText(KeyName);
                        KeyText->SetColorAndOpacity(FSlateColor(FLinearColor::White));
                        KeyText->SetShadowOffset(FVector2D(1.f, 1.f));
                        KeyText->SetShadowColorAndOpacity(FLinearColor(0.f, 0.f, 0.f, 0.75f));
                        FSlateFontInfo FontInfo = KeyText->GetFont();
                        FontInfo.Size = 10;
                        KeyText->SetFont(FontInfo);

                        UOverlaySlot* LabelSlot = Overlay->AddChildToOverlay(KeyText);
                        if (LabelSlot)
                        {
                            LabelSlot->SetHorizontalAlignment(HAlign_Left);
                            LabelSlot->SetVerticalAlignment(VAlign_Bottom);
                            LabelSlot->SetPadding(FMargin(4.f, 0.f, 0.f, 4.f));
                        }
                    }
                }

                Button->SetContent(Overlay);
            }

			// Store the button in the config
			CommandSlotConfigs[i].ButtonWidget = Button;
			
			// Bind to the appropriate slot function
			FName FunctionName;
			switch(i)
			{
				case 0: FunctionName = FName(TEXT("ExecuteCommandAtSlot0")); break;
				case 1: FunctionName = FName(TEXT("ExecuteCommandAtSlot1")); break;
				case 2: FunctionName = FName(TEXT("ExecuteCommandAtSlot2")); break;
				case 3: FunctionName = FName(TEXT("ExecuteCommandAtSlot3")); break;
				case 4: FunctionName = FName(TEXT("ExecuteCommandAtSlot4")); break;
				case 5: FunctionName = FName(TEXT("ExecuteCommandAtSlot5")); break;
				case 6: FunctionName = FName(TEXT("ExecuteCommandAtSlot6")); break;
				case 7: FunctionName = FName(TEXT("ExecuteCommandAtSlot7")); break;
				case 8: FunctionName = FName(TEXT("ExecuteCommandAtSlot8")); break;
				case 9: FunctionName = FName(TEXT("ExecuteCommandAtSlot9")); break;
				default: FunctionName = FName(TEXT("ExecuteCommandAtSlot0")); break;
			}
			
			FScriptDelegate Delegate;
			Delegate.BindUFunction(this, FunctionName);
			Button->OnClicked.Add(Delegate);
			
			// Add to vertical box with proper sizing
			UVerticalBoxSlot* ButtonSlot = CommandButtonsContainer->AddChildToVerticalBox(Button);
			if (ButtonSlot)
			{
				ButtonSlot->SetHorizontalAlignment(HAlign_Fill);
				ButtonSlot->SetVerticalAlignment(VAlign_Center);
				ButtonSlot->SetPadding(FMargin(5.0f, 5.0f, 5.0f, 5.0f));
			}

    // Append a single remove button after command slots (at index CommandSlotConfigs.Num())
    if(i == CommandSlotConfigs.Num() - 1) {
        // Destroy previous if any to avoid duplicates
        RemoveButtonWidget = nullptr;

        UButton* RemoveButton = NewObject<UButton>(this);
        if (RemoveButton)
        {
            // Prefer icon if provided, otherwise simple X text
            if (RemoveButtonIcon)
            {
                UImage* XImage = NewObject<UImage>(this);
                if (XImage)
                {
                    XImage->SetBrushFromTexture(RemoveButtonIcon);
                    XImage->SetDesiredSizeOverride(FVector2D(48.0f, 48.0f));
                    RemoveButton->SetContent(XImage);
                }
            }
            else
            {
                UTextBlock* XText = NewObject<UTextBlock>(this);
                if (XText)
                {
                    XText->SetText(FText::FromString(TEXT("X")));
                    RemoveButton->SetContent(XText);
                }
            }

            FScriptDelegate RemoveDelegate;
            RemoveDelegate.BindUFunction(this, FName(TEXT("ExecuteRemoveAtCursor")));
            RemoveButton->OnClicked.Add(RemoveDelegate);

            UVerticalBoxSlot* RemoveButtonSlot = CommandButtonsContainer->AddChildToVerticalBox(RemoveButton);
            if (RemoveButtonSlot)
            {
                RemoveButtonSlot->SetHorizontalAlignment(HAlign_Fill);
                RemoveButtonSlot->SetVerticalAlignment(VAlign_Center);
                RemoveButtonSlot->SetPadding(FMargin(5.0f, 10.0f, 5.0f, 5.0f)); // extra top gap
            }

            RemoveButtonWidget = RemoveButton;
        }
    }
		}
	}
}

UButton* UArmTimelineEditorWidget::GetCommandButton(int32 SlotIndex) const
{
	if (SlotIndex >= 0 && SlotIndex < CommandSlotConfigs.Num())
	{
		return CommandSlotConfigs[SlotIndex].ButtonWidget;
	}
	return nullptr;
}

void UArmTimelineEditorWidget::ExecuteRemoveAtCursor()
{
    UArmTimelineUMGWidget* UMGWidget = GetActiveUMGWidget();
    if (!UMGWidget)
    {
        UE_LOG(LogTemp, Warning, TEXT("ExecuteRemoveAtCursor: No active UMG widget"));
        return;
    }

    const int32 ProgramLength = UMGWidget->ArmProgram.Num();
    if (ProgramLength <= 0)
    {
        UE_LOG(LogTemp, Log, TEXT("ExecuteRemoveAtCursor: Program is empty"));
        return;
    }

    // Remove at the current cursor index; clamp to last valid index if cursor is after the end
    int32 IndexToRemove = FMath::Clamp(UMGWidget->GetCursor(), 0, ProgramLength - 1);

    const bool bRemoved = UMGWidget->RemoveCommandAt(IndexToRemove);
    UE_LOG(LogTemp, Log, TEXT("ExecuteRemoveAtCursor: Remove at index %d -> %s (program size after: %d)"),
        IndexToRemove, bRemoved ? TEXT("Success") : TEXT("Fail"), UMGWidget->ArmProgram.Num());
}

void UArmTimelineEditorWidget::ExecuteCommandAtSlot0() { OnCommandButtonClicked(0); }
void UArmTimelineEditorWidget::ExecuteCommandAtSlot1() { OnCommandButtonClicked(1); }
void UArmTimelineEditorWidget::ExecuteCommandAtSlot2() { OnCommandButtonClicked(2); }
void UArmTimelineEditorWidget::ExecuteCommandAtSlot3() { OnCommandButtonClicked(3); }
void UArmTimelineEditorWidget::ExecuteCommandAtSlot4() { OnCommandButtonClicked(4); }
void UArmTimelineEditorWidget::ExecuteCommandAtSlot5() { OnCommandButtonClicked(5); }
void UArmTimelineEditorWidget::ExecuteCommandAtSlot6() { OnCommandButtonClicked(6); }
void UArmTimelineEditorWidget::ExecuteCommandAtSlot7() { OnCommandButtonClicked(7); }
void UArmTimelineEditorWidget::ExecuteCommandAtSlot8() { OnCommandButtonClicked(8); }
void UArmTimelineEditorWidget::ExecuteCommandAtSlot9() { OnCommandButtonClicked(9); }

FString UArmTimelineEditorWidget::GetCommandDisplayName(ECmd Command) const
{
	switch (Command)
	{
	case ECmd::Wait:         return TEXT("Wait");
	case ECmd::Grab:         return TEXT("Grab");
	case ECmd::Drop:         return TEXT("Drop");
	case ECmd::RotateL:      return TEXT("Rotate CCW");
	case ECmd::RotateR:      return TEXT("Rotate CW");
	case ECmd::Extend:       return TEXT("Extend");
	case ECmd::Retract:      return TEXT("Retract");
	case ECmd::MovePos:      return TEXT("Move Pos");
	case ECmd::MoveNeg:      return TEXT("Move Neg");
	case ECmd::PivotL:       return TEXT("Pivot Left");
	case ECmd::PivotR:       return TEXT("Pivot Right");
	case ECmd::Reset:        return TEXT("Reset");
	case ECmd::Repeat:       return TEXT("Repeat");
	case ECmd::RotateHeldL:  return TEXT("Rotate Held CCW");
	case ECmd::RotateHeldR:  return TEXT("Rotate Held CW");
	case ECmd::WaitForBond:  return TEXT("Wait For Bond");
	default:                 return TEXT("Unknown");
	}
}

EArmCmd UArmTimelineEditorWidget::ConvertECmdToEArmCmd(ECmd Cmd) const
{
	// Convert ECmd (simulation commands) to EArmCmd (user-facing commands)
	switch (Cmd)
	{
	case ECmd::Wait:          return EArmCmd::Wait;
	case ECmd::Grab:          return EArmCmd::Grab;
	case ECmd::Drop:          return EArmCmd::Drop;
	case ECmd::RotateL:      return EArmCmd::RotateCCW;
	case ECmd::RotateR:      return EArmCmd::RotateCW;
	case ECmd::Extend:       return EArmCmd::Extend;
	case ECmd::Retract:      return EArmCmd::Retract;
	case ECmd::Reset:        return EArmCmd::Reset;
	case ECmd::RotateHeldL:  return EArmCmd::RotateHeldCCW;
	case ECmd::RotateHeldR:  return EArmCmd::RotateHeldCW;
	case ECmd::WaitForBond:  return EArmCmd::WaitForBond;
	default:                 return EArmCmd::NoOp;
	}
}

void UArmTimelineEditorWidget::DebugPrintActiveArmInfo()
{
	UArmTimelineUMGWidget* UMGWidget = GetActiveUMGWidget();
	if (!UMGWidget)
	{
		UE_LOG(LogTemp, Warning, TEXT("DebugPrintActiveArmInfo: No active UMG widget"));
		return;
	}
	
	ABasicArm* ActiveArm = UMGWidget->GetAssociatedArm();
	if (!ActiveArm)
	{
		UE_LOG(LogTemp, Warning, TEXT("DebugPrintActiveArmInfo: No associated arm"));
		return;
	}
	
	UE_LOG(LogTemp, Log, TEXT("DebugPrintActiveArmInfo: Active arm=%s, Program size=%d, Editing mode=%d, Cursor position=%d"),
		*ActiveArm->GetName(), ActiveArm->Program.Num(), UMGWidget->bIsEditingMode, UMGWidget->GetCursor());
}
