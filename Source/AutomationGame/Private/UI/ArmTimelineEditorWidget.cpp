#include "UI/ArmTimelineEditorWidget.h"
#include "UI/ArmTimelineWidget.h"
#include "UI/ArmTimelineUMGWidget.h"
#include "Framework/GameManager.h"
#include "Actors/BasicArm.h"
#include "Framework/Application/SlateApplication.h"
#include "Components/CanvasPanelSlot.h"
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
}

FReply UArmTimelineEditorWidget::NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent)
{
	// Handle key input for command insertion
	HandleKeyInput(InKeyEvent.GetKey());
	
	// Always handled
	return FReply::Handled();
}

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
	
	// Insert the command (cast from ECmd to EArmCmd)
	return UMGWidget->InsertCommandAtCursor(static_cast<EArmCmd>(Command), Arg);
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
	UArmTimelineUMGWidget* UMGWidget = GetActiveUMGWidget();
	if (!UMGWidget)
	{
		return;
	}
	
	// Get cursor position
	int32 CursorPos = CurrentCursorPosition;
	
	// Insert command at cursor
	UMGWidget->SetCursorPosition(CursorPos);
		UMGWidget->InsertCommandAtCursor(static_cast<EArmCmd>(Command), Arg);
	
	// Update local cursor position
	CurrentCursorPosition = UMGWidget->GetCursor();
}

void UArmTimelineEditorWidget::HandleKeyInput(const FKey& PressedKey)
{
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
		HandleCommandInput(Config.Command, Config.DefaultArg);
	}
}
