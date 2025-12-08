#include "UI/ArmTimelineUMGWidget.h"
#include "UI/ArmTimelineWidget.h"
#include "Engine/Texture2D.h"
#include "Components/Image.h"
#include "Components/TextBlock.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/Button.h"
#include "Components/PanelWidget.h"
#include "Sim/ArmTypes.h"
#include "Actors/BasicArm.h"
#include "Actors/OrbSimDriver.h"
#include "Framework/GameManager.h"
#include "AGLogChannels.h"
#include "UMG.h"
#include "Components/ActorComponent.h"
#include "EngineUtils.h"

UArmTimelineUMGWidget::UArmTimelineUMGWidget(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// Initialize default values
	CurrentCommandIndex = -1;
	CurrentCommandProgress = 0.0f;
	MaxDisplayedCommands = 10;
	CurrentCommandColor = FLinearColor::Yellow;
	NormalCommandColor = FLinearColor::White;
	CompletedCommandColor = FLinearColor::Green;
	ListBorderColor = FLinearColor(0.2f, 0.2f, 0.2f, 0.8f);
	ListBorderThickness = 2.0f;
	bPulseCurrentCommand = true;
}

void UArmTimelineUMGWidget::NativeConstruct()
{
	Super::NativeConstruct();
	
	// Disable clipping on the root widget to prevent cropping
	SetClipping(EWidgetClipping::Inherit);
	
	// Set a large minimum desired size to ensure the widget can expand to show all content
	// This helps prevent cropping when there are many command icons
	SetMinimumDesiredSize(FVector2D(4000.0f, 400.0f));
	
	// Ensure CommandIconsBox doesn't clip its content
	if (CommandIconsBox)
	{
		CommandIconsBox->SetClipping(EWidgetClipping::Inherit);
	}
	
	InitializeWidget();
	
}

void UArmTimelineUMGWidget::NativeDestruct()
{
	Super::NativeDestruct();
	
	// Clean up command icon widgets
	for (UWidget* Widget : CommandIconWidgets)
	{
		if (Widget && Widget->IsValidLowLevel())
		{
			Widget->RemoveFromParent();
		}
	}
	CommandIconWidgets.Empty();
	
}

void UArmTimelineUMGWidget::SetArmProgram(const TArray<FArmInstr>& Program)
{
	// Only update if the program actually changed
	if (ArmProgram != Program)
	{
		ArmProgram = Program;
		CurrentCommandIndex = -1;
		CurrentCommandProgress = 0.0f;
		
		// Clear existing icons so they get recreated with new program
		for (UWidget* Widget : CommandIconWidgets)
		{
			if (Widget && Widget->IsValidLowLevel())
			{
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

void UArmTimelineUMGWidget::SetCurrentCommand(int32 CommandIndex, float Progress)
{
	CurrentCommandIndex = FMath::Clamp(CommandIndex, -1, ArmProgram.Num() - 1);
	CurrentCommandProgress = FMath::Clamp(Progress, 0.0f, 1.0f);
	
	// UE_LOG(LogTemp, Verbose, TEXT("ArmTimelineUMGWidget: SetCurrentCommand - Index %d, Progress %.2f"), 
	// 	CurrentCommandIndex, CurrentCommandProgress);
	
	// Only update if the command index actually changed or progress changed significantly
	static int32 LastCommandIndex = -1;
	static float LastProgress = 0.0f;
	
	bool bCommandChanged = (CurrentCommandIndex != LastCommandIndex);
	bool bProgressChanged = FMath::Abs(CurrentCommandProgress - LastProgress) > 0.1f; // Only update if progress changed by more than 10%
	
	if (bCommandChanged || bProgressChanged)
	{
		UpdateWidgetContent();
		OnCurrentCommandChanged();
		
		LastCommandIndex = CurrentCommandIndex;
		LastProgress = CurrentCommandProgress;
	}
}

void UArmTimelineUMGWidget::SetCommandTextures(const TMap<EArmCmd, UTexture2D*>& Textures)
{
	CommandTextures = Textures;
	
	// Only log once to reduce spam
	static bool bLoggedTextures = false;
	if (!bLoggedTextures)
	{
		bLoggedTextures = true;
	}
	
	UpdateWidgetContent();
	OnCommandTexturesUpdated();
}

void UArmTimelineUMGWidget::SetDefaultCommandTexture(UTexture2D* DefaultTexture)
{
	// Only update if texture actually changed
	if (DefaultCommandTexture != DefaultTexture)
	{
		DefaultCommandTexture = DefaultTexture;
		
		
		UpdateWidgetContent();
	}
}

void UArmTimelineUMGWidget::SetMaxDisplayedCommands(int32 MaxCommands)
{
	MaxDisplayedCommands = FMath::Clamp(MaxCommands, 1, 20);
	
	
	UpdateWidgetContent();
}

void UArmTimelineUMGWidget::SetCommandColors(const FLinearColor& CurrentColor, const FLinearColor& NormalColor, const FLinearColor& CompletedColor)
{
	CurrentCommandColor = CurrentColor;
	NormalCommandColor = NormalColor;
	CompletedCommandColor = CompletedColor;
	
	
	UpdateWidgetContent();
}

void UArmTimelineUMGWidget::SetIconSize(float NewIconSize)
{
	IconSize = FMath::Clamp(NewIconSize, 16.0f, 128.0f);
	
	
	UpdateWidgetContent();
}

void UArmTimelineUMGWidget::SetIconSpacing(float InIconSpacing)
{
    IconSpacing = FMath::Max(0.0f, InIconSpacing);
    UpdateWidgetContent();
}

void UArmTimelineUMGWidget::SetListBorderColor(const FLinearColor& BorderColor)
{
	ListBorderColor = BorderColor;
	UpdateWidgetContent();
}

void UArmTimelineUMGWidget::SetListBorderThickness(float Thickness)
{
	ListBorderThickness = Thickness;
	UpdateWidgetContent();
}

void UArmTimelineUMGWidget::SetPulseCurrentCommand(bool bPulse)
{
	bPulseCurrentCommand = bPulse;
	UpdateWidgetContent();
}

void UArmTimelineUMGWidget::SetIconBackgroundColor(const FLinearColor& NewIconBackgroundColor)
{
	IconBackgroundColor = NewIconBackgroundColor;
	
	
	UpdateWidgetContent();
}

void UArmTimelineUMGWidget::InitializeWidget()
{
	// Initialize any default values or setup
	
	// Bind reset button if it exists
	if (ResetButton)
	{
		ResetButton->OnClicked.AddDynamic(this, &UArmTimelineUMGWidget::TriggerImmediateReset);
	}
	
	OnWidgetConstructed();
}

void UArmTimelineUMGWidget::UpdateWidgetContent()
{
	// Don't create icons if there are no commands
	if (ArmProgram.Num() == 0)
	{
		// Clear any existing icons
		for (UWidget* Widget : CommandIconWidgets)
		{
			if (Widget && Widget->IsValidLowLevel())
			{
				Widget->RemoveFromParent();
			}
		}
		CommandIconWidgets.Empty();
		bIconsCreated = false;
		return;
	}





	// Use caching flag to prevent unnecessary recreation
	bool bNeedsRecreation = !bIconsCreated || (CommandIconWidgets.Num() == 0);
	
	// Check if program size changed (recreate if size is different)
	if (!bNeedsRecreation && ArmProgram.Num() > 0)
	{
		int32 CurrentIconCount = CommandIconWidgets.Num();
		int32 ProgramSize = FMath::Min(ArmProgram.Num(), MaxDisplayedCommands);
		if (CurrentIconCount != ProgramSize)
		{
			bNeedsRecreation = true;
		}
	}
	
	if (bNeedsRecreation)
	{
		// No icons exist or program changed, create them
		CreateCommandIcons();
		bIconsCreated = true;
	}
	else
	{
		// Icons exist and program is stable, just update them
		UpdateCommandIcons();
	}
	
	// UE_LOG(LogTemp, Verbose, TEXT("ArmTimelineUMGWidget: UpdateWidgetContent - %d commands, current %d"), 
	//	ArmProgram.Num(), CurrentCommandIndex);
}

UTexture2D* UArmTimelineUMGWidget::GetTextureForCommand(EArmCmd Command) const
{
	if (const UTexture2D* const* FoundTexture = CommandTextures.Find(Command))
	{
		return const_cast<UTexture2D*>(*FoundTexture);
	}
	
	return DefaultCommandTexture;
}

FString UArmTimelineUMGWidget::GetCommandDisplayName(EArmCmd Command) const
{
	switch (Command)
	{
	case EArmCmd::NoOp:         return TEXT("NoOp");
	case EArmCmd::Grab:         return TEXT("Grab");
	case EArmCmd::Drop:         return TEXT("Drop");
	case EArmCmd::RotateCW:     return TEXT("Rotate CW");
	case EArmCmd::RotateCCW:    return TEXT("Rotate CCW");
	case EArmCmd::Wait:         return TEXT("Wait");
	case EArmCmd::Reset:        return TEXT("Reset");
	case EArmCmd::Extend:       return TEXT("Extend");
	case EArmCmd::Retract:      return TEXT("Retract");
	case EArmCmd::RotateHeldCW: return TEXT("Rotate Held CW");
	case EArmCmd::RotateHeldCCW: return TEXT("Rotate Held CCW");
	case EArmCmd::WaitForBond:  return TEXT("Wait For Bond");
	default:                    return TEXT("Unknown");
	}
}

void UArmTimelineUMGWidget::CreateCommandIcons()
{
	// Clear existing icons
    for (UWidget* Widget : CommandIconWidgets)
    {
        if (Widget && Widget->IsValidLowLevel())
        {
            Widget->RemoveFromParent();
        }
    }
    CommandIconWidgets.Empty();

    // Clear and remove existing reset button if it exists
    if (ResetButton && ResetButton->IsValidLowLevel())
    {
        ResetButton->RemoveFromParent();
        ResetButton = nullptr;
    }

    // Also clear the container so borders/inner boxes don't accumulate
    if (CommandIconsBox)
    {
        CommandIconsBox->ClearChildren();
    }

	if (!CommandIconsBox)
	{
		UE_LOG(LogTemp, Warning, TEXT("ArmTimelineUMGWidget: CommandIconsBox is null"));
		return;
	}

	// Add a border around the entire command list
	UBorder* CommandListBorder = NewObject<UBorder>(this);
	if (CommandListBorder)
	{
		// Set border properties for the command list
		CommandListBorder->SetBrushColor(ListBorderColor);
		CommandListBorder->SetPadding(FMargin(ListBorderThickness)); // Border thickness
		
		// Create a horizontal box inside the border to arrange icons
		UHorizontalBox* InnerHorizontalBox = NewObject<UHorizontalBox>(this);
		if (InnerHorizontalBox)
		{
			// Add the horizontal box to the border
			CommandListBorder->AddChild(InnerHorizontalBox);
			
			// Add the border to the command icons box
			CommandIconsBox->AddChild(CommandListBorder);
			
			// Create icons for the displayed commands using sliding window around current command
            int32 NumCommandsToShow = FMath::Min(ArmProgram.Num(), MaxDisplayedCommands);
            int32 StartIndex = 0;
			
			// Show a window around the current command
			if (CurrentCommandIndex >= 0 && ArmProgram.Num() > MaxDisplayedCommands)
			{
				// Center the window around the current command
				StartIndex = FMath::Max(0, CurrentCommandIndex - MaxDisplayedCommands / 2);
				StartIndex = FMath::Min(StartIndex, ArmProgram.Num() - MaxDisplayedCommands);
			}
			
            // Cache metrics for cursor math
            CommandListLeftPadding = 8.0f; // border padding set above
            VisibleStartIndex = StartIndex;

            for (int32 i = 0; i < NumCommandsToShow; ++i)
			{
				int32 CommandIndex = StartIndex + i;
				UWidget* IconWidget = CreateCommandIcon(ArmProgram[CommandIndex].Cmd, CommandIndex);
				if (IconWidget)
				{
					CommandIconWidgets.Add(IconWidget);
					
                    // Add to the inner horizontal box with proper spacing
					UHorizontalBoxSlot* BoxSlot = InnerHorizontalBox->AddChildToHorizontalBox(IconWidget);
					if (BoxSlot)
					{
                        const float HalfGap = IconSpacing * 0.5f;
                        BoxSlot->SetPadding(FMargin(HalfGap, 0.0f, HalfGap, 0.0f));
						BoxSlot->SetHorizontalAlignment(HAlign_Center);
						BoxSlot->SetVerticalAlignment(VAlign_Center);
					}
				}
			}
		}
	}
	else
	{
		// Fallback: create icons directly in the horizontal box using sliding window
        int32 NumCommandsToShow = FMath::Min(ArmProgram.Num(), MaxDisplayedCommands);
        int32 StartIndex = 0;
		
		// Show a window around the current command
		if (CurrentCommandIndex >= 0 && ArmProgram.Num() > MaxDisplayedCommands)
		{
			// Center the window around the current command
			StartIndex = FMath::Max(0, CurrentCommandIndex - MaxDisplayedCommands / 2);
			StartIndex = FMath::Min(StartIndex, ArmProgram.Num() - MaxDisplayedCommands);
		}
		
        // Cache metrics for cursor math (no outer border used here)
        CommandListLeftPadding = 0.0f;
        VisibleStartIndex = StartIndex;

        for (int32 i = 0; i < NumCommandsToShow; ++i)
		{
			int32 CommandIndex = StartIndex + i;
			UWidget* IconWidget = CreateCommandIcon(ArmProgram[CommandIndex].Cmd, CommandIndex);
			if (IconWidget)
			{
				CommandIconWidgets.Add(IconWidget);
				
                // Add to horizontal box
				UHorizontalBoxSlot* BoxSlot = CommandIconsBox->AddChildToHorizontalBox(IconWidget);
				if (BoxSlot)
				{
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
}

void UArmTimelineUMGWidget::UpdateCommandIcons()
{
	// Calculate the sliding window start index around the current command
    int32 StartIndex = 0;
	if (CurrentCommandIndex >= 0 && ArmProgram.Num() > MaxDisplayedCommands)
	{
		// Center the window around the current command
		StartIndex = FMath::Max(0, CurrentCommandIndex - MaxDisplayedCommands / 2);
		StartIndex = FMath::Min(StartIndex, ArmProgram.Num() - MaxDisplayedCommands);
	}
	
    // Cache metrics for cursor math
    VisibleStartIndex = StartIndex;

    // Update existing icons
	for (int32 i = 0; i < CommandIconWidgets.Num(); ++i)
	{
		if (CommandIconWidgets[i])
		{
			int32 CommandIndex = StartIndex + i;
			if (CommandIndex < ArmProgram.Num())
			{
				bool bIsCurrent = (CommandIndex == CurrentCommandIndex);
				bool bIsCompleted = (CommandIndex < CurrentCommandIndex);
				float Progress = (bIsCurrent) ? CurrentCommandProgress : (bIsCompleted ? 1.0f : 0.0f);
				
				UpdateCommandIcon(CommandIconWidgets[i], ArmProgram[CommandIndex].Cmd, CommandIndex, bIsCurrent, bIsCompleted, Progress);
			}
		}
	}
}

UWidget* UArmTimelineUMGWidget::CreateCommandIcon(EArmCmd Command, int32 Index)
{
	// Create a border widget to wrap the icon
	UBorder* BorderWidget = NewObject<UBorder>(this);
	if (!BorderWidget)
	{
		return nullptr;
	}
	
	// Create the image widget for the command icon
	UImage* ImageWidget = NewObject<UImage>(this);
	if (!ImageWidget)
	{
		return nullptr;
	}
	
	// Get the texture for this command
	UTexture2D* CommandTexture = GetTextureForCommand(Command);
	if (CommandTexture)
	{
		ImageWidget->SetBrushFromTexture(CommandTexture);
	}
	else
	{
		// Use a default texture or create a colored rectangle
		// Only log this warning once per command type to reduce spam
		static TSet<EArmCmd> LoggedMissingTextures;
		if (!LoggedMissingTextures.Contains(Command))
		{
			UE_LOG(LogTemp, Warning, TEXT("ArmTimelineUMGWidget: No texture found for command %d"), (int32)Command);
			LoggedMissingTextures.Add(Command);
		}
	}
	
	// Set size using configurable IconSize
	ImageWidget->SetDesiredSizeOverride(FVector2D(IconSize, IconSize));
	
	// Add the image widget to the border
	BorderWidget->AddChild(ImageWidget);
	
	// Set initial state
	bool bIsCurrent = (Index == CurrentCommandIndex);
	bool bIsCompleted = (Index < CurrentCommandIndex);
	float Progress = (bIsCurrent) ? CurrentCommandProgress : (bIsCompleted ? 1.0f : 0.0f);
	
	// Call Blueprint implementation or use C++ fallback
	UpdateCommandIcon(BorderWidget, Command, Index, bIsCurrent, bIsCompleted, Progress);
	
	return BorderWidget;
}

void UArmTimelineUMGWidget::UpdateCommandIcon(UWidget* IconWidget, EArmCmd Command, int32 Index, bool bIsCurrent, bool bIsCompleted, float Progress)
{
	// Handle border widget (which contains the image widget)
	if (UBorder* BorderWidget = Cast<UBorder>(IconWidget))
	{
		// Get the image widget from the border
		UImage* ImageWidget = nullptr;
		if (BorderWidget->GetChildrenCount() > 0)
		{
			ImageWidget = Cast<UImage>(BorderWidget->GetChildAt(0));
		}
		
		if (ImageWidget)
		{
			// Update the texture
			UTexture2D* CommandTexture = GetTextureForCommand(Command);
			if (CommandTexture)
			{
				ImageWidget->SetBrushFromTexture(CommandTexture);
			}
			
			// Apply size based on command state
			if (!bIsCurrent)
			{
				// Non-current commands use configurable size
				ImageWidget->SetDesiredSizeOverride(FVector2D(IconSize, IconSize));
			}
			else
			{
			// Current command uses configurable size with slight scaling
			float SizeMultiplier = 1.0f + Progress * 0.2f;

			// Apply pulse effect if enabled
			if (bPulseCurrentCommand)
			{
				float PulseTime = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
				float PulseFactor = (FMath::Sin(PulseTime * 10.0f) + 1.0f) * 0.5f; // 0 to 1 sine wave
				SizeMultiplier += PulseFactor * 0.1f; // Add up to 10% extra scale
			}

			float FinalSize = IconSize * SizeMultiplier;
			ImageWidget->SetDesiredSizeOverride(FVector2D(FinalSize, FinalSize));
			}
		}
		
		// Set border properties based on command state
		FLinearColor BorderColor;
		float BorderThickness = 2.0f;
		
		if (bIsCurrent)
		{
			// Current command gets a bright border
			BorderColor = FLinearColor::Yellow;
			BorderThickness = 3.0f;
		}
		else if (bIsCompleted)
		{
			// Completed commands get a green border
			BorderColor = FLinearColor::Green;
		}
		else
		{
			// Regular commands get a subtle border
			BorderColor = FLinearColor(0.5f, 0.5f, 0.5f, 0.8f);
		}
		
		// Apply border color and thickness
		BorderWidget->SetBrushColor(BorderColor);
		
		// Set border thickness using padding
		BorderWidget->SetPadding(FMargin(BorderThickness));
		
	}
	// Fallback for direct image widgets (shouldn't happen with new structure)
	else if (UImage* ImageWidget = Cast<UImage>(IconWidget))
	{
		// Update the texture and apply color in the correct order
		UTexture2D* CommandTexture = GetTextureForCommand(Command);
		if (CommandTexture)
		{
			ImageWidget->SetBrushFromTexture(CommandTexture);
		}
		
		// Apply size based on command state
		if (!bIsCurrent)
		{
			// Non-current commands use configurable size
			ImageWidget->SetDesiredSizeOverride(FVector2D(IconSize, IconSize));
		}
		else
		{
			// Current command uses configurable size with slight scaling
			float SizeMultiplier = 1.0f + Progress * 0.2f;
			
			// Apply pulse effect if enabled
			if (bPulseCurrentCommand)
			{
				float PulseTime = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
				float PulseFactor = (FMath::Sin(PulseTime * 10.0f) + 1.0f) * 0.5f; // 0 to 1 sine wave
				SizeMultiplier += PulseFactor * 0.1f; // Add up to 10% extra scale
			}

			float FinalSize = IconSize * SizeMultiplier;
			ImageWidget->SetDesiredSizeOverride(FVector2D(FinalSize, FinalSize));
		}
	}
	
	// Call Blueprint implementation for any additional customizations
	OnUpdateCommandIcon(IconWidget, Command, Index, bIsCurrent, bIsCompleted, Progress);
}

void UArmTimelineUMGWidget::SetNormalCommandColor(const FLinearColor& NormalColor)
{
	NormalCommandColor = NormalColor;
}

void UArmTimelineUMGWidget::SetCurrentCommandColor(const FLinearColor& CurrentColor)
{
	CurrentCommandColor = CurrentColor;
}

void UArmTimelineUMGWidget::SetCompletedCommandColor(const FLinearColor& CompletedColor)
{
	CompletedCommandColor = CompletedColor;
}

bool UArmTimelineUMGWidget::InsertCommandAtCursor(EArmCmd Command, int32 Arg)
{
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
	UpdateCursorIndicator();
	
	return true;
}

bool UArmTimelineUMGWidget::RemoveCommandAt(int32 Index)
{
	// Check if index is valid
	if (Index < 0 || Index >= ArmProgram.Num())
	{
		return false;
	}
	
	// Remove the command
	ArmProgram.RemoveAt(Index);
	
	// Adjust cursor position if needed
	if (CursorPosition >= ArmProgram.Num())
	{
		CursorPosition = ArmProgram.Num();
	}
	
	// Ensure minimum desired size is maintained to prevent shrinking when commands are removed
	SetMinimumDesiredSize(FVector2D(4000.0f, 400.0f));
	
	// Synchronize the program back to the arm
	SyncProgramToArm();
	
	// Update the widget content
	bIconsCreated = false;
	UpdateWidgetContent();
	UpdateCursorIndicator();
	
	return true;
}

FArmInstr UArmTimelineUMGWidget::GetCommandAt(int32 Index) const
{
	if (Index >= 0 && Index < ArmProgram.Num())
	{
		return ArmProgram[Index];
	}
	
	return FArmInstr();
}

void UArmTimelineUMGWidget::MoveCursorLeft()
{
	if (CursorPosition > 0)
	{
		CursorPosition--;
		UpdateCursorIndicator();
	}
}

void UArmTimelineUMGWidget::MoveCursorRight()
{
	if (CursorPosition < ArmProgram.Num())
	{
		CursorPosition++;
		UpdateCursorIndicator();
	}
	else {
		CursorPosition = 0;
		UpdateCursorIndicator();
	}
}

void UArmTimelineUMGWidget::SetCursorPosition(int32 Position)
{
	CursorPosition = FMath::Clamp(Position, 0, ArmProgram.Num());
	UpdateCursorIndicator();
}

int32 UArmTimelineUMGWidget::GetCursor() const
{
	return CursorPosition;
}

void UArmTimelineUMGWidget::EnableEditingMode()
{
	bIsEditingMode = true;
	UpdateCursorIndicator();
}

void UArmTimelineUMGWidget::DisableEditingMode()
{
	bIsEditingMode = false;
	UpdateCursorIndicator();
}

void UArmTimelineUMGWidget::UpdateCursorIndicator()
{
	// Clamp cursor position to valid range
	CursorPosition = FMath::Clamp(CursorPosition, 0, ArmProgram.Num());
	
	// Notify Blueprint to update the cursor visual
	OnCursorPositionChanged(CursorPosition);
}

ABasicArm* UArmTimelineUMGWidget::GetAssociatedArm() const
{
	return AssociatedArm;
}

void UArmTimelineUMGWidget::SetAssociatedArm(ABasicArm* Arm)
{
	AssociatedArm = Arm;
}

UBorder* UArmTimelineUMGWidget::GetCursorLine() const
{
	return Cast<UBorder>(CursorIndicator);
}

void UArmTimelineUMGWidget::TriggerImmediateReset()
{
	// Get the associated arm
	ABasicArm* Arm = GetAssociatedArm();
	if (!Arm)
	{
		UE_LOG(LogTemp, Warning, TEXT("TriggerImmediateReset: No associated arm found"));
		return;
	}

	// Get the simulation from the driver
	UWorld* World = GetWorld();
	if (!World)
	{
		UE_LOG(LogTemp, Warning, TEXT("TriggerImmediateReset: No world found"));
		return;
	}

	AOrbSimDriver* Driver = AOrbSimDriver::Find(World);
	if (!Driver)
	{
		UE_LOG(LogTemp, Warning, TEXT("TriggerImmediateReset: No OrbSimDriver found"));
		return;
	}

	USimWorldHex* Sim = Driver->GetSim();
	if (!Sim)
	{
		UE_LOG(LogTemp, Warning, TEXT("TriggerImmediateReset: No Sim found"));
		return;
	}

	// Check if the arm has a valid SimArmIndex
	if (Arm->SimArmIndex < 0 || Arm->SimArmIndex >= Sim->Arms.Num())
	{
		UE_LOG(LogTemp, Warning, TEXT("TriggerImmediateReset: Invalid SimArmIndex %d (Sim has %d arms)"), 
			Arm->SimArmIndex, Sim->Arms.Num());
		return;
	}

	// Get the arm state from the simulation
	FArmState& ArmState = Sim->Arms[Arm->SimArmIndex];
	
	// Check if the tape is valid
	if (ArmState.Tape.Num() == 0)
	{
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
	// The simulation will expand it into drop + rotation + reach adjustments and execute them immediately
	int32 InsertIndex = ArmState.PC;
	ArmState.Tape.Insert(ResetOp, InsertIndex);
	
	// PC now points to the Reset command we just inserted
	// The simulation will execute it on the next tick and expand it immediately
	
	UE_LOG(LogTemp, Log, TEXT("TriggerImmediateReset: Inserted Reset command at tape index %d (PC is now %d, pointing at Reset)"), 
		InsertIndex, ArmState.PC);
}

void UArmTimelineUMGWidget::CreateResetButton()
{
	if (!CommandIconsBox)
	{
		return;
	}

	// Get the parent widget of CommandIconsBox
	UWidget* ParentWidget = CommandIconsBox->GetParent();
	if (!ParentWidget)
	{
		UE_LOG(LogTemp, Warning, TEXT("CreateResetButton: CommandIconsBox has no parent widget"));
		return;
	}

	// Try to get the parent as a VerticalBox (most common layout)
	UVerticalBox* ParentVerticalBox = Cast<UVerticalBox>(ParentWidget);
	
	// If parent is not a VerticalBox, try to find a VerticalBox ancestor or create one
	if (!ParentVerticalBox)
	{
		// Try to find a VerticalBox in the parent hierarchy
		UWidget* CurrentParent = ParentWidget;
		while (CurrentParent)
		{
			ParentVerticalBox = Cast<UVerticalBox>(CurrentParent);
			if (ParentVerticalBox)
			{
				break;
			}
			CurrentParent = CurrentParent->GetParent();
		}
	}

	// If we still don't have a VerticalBox, wrap CommandIconsBox in one
	if (!ParentVerticalBox)
	{
		// Create a new VerticalBox and move CommandIconsBox into it
		ParentVerticalBox = NewObject<UVerticalBox>(this);
		if (!ParentVerticalBox)
		{
			UE_LOG(LogTemp, Warning, TEXT("CreateResetButton: Failed to create VerticalBox"));
			return;
		}

		// Remove CommandIconsBox from its current parent
		CommandIconsBox->RemoveFromParent();
		
		// Add CommandIconsBox to the new VerticalBox
		UVerticalBoxSlot* IconsSlot = ParentVerticalBox->AddChildToVerticalBox(CommandIconsBox);
		if (IconsSlot)
		{
			IconsSlot->SetHorizontalAlignment(HAlign_Fill);
			IconsSlot->SetVerticalAlignment(VAlign_Top);
		}

		// Add the new VerticalBox to the original parent
		if (ParentWidget)
		{
			// Try to add to various container types
			if (UVerticalBox* VBox = Cast<UVerticalBox>(ParentWidget))
			{
				UVerticalBoxSlot* VerticalSlot = VBox->AddChildToVerticalBox(ParentVerticalBox);
				if (VerticalSlot)
				{
					VerticalSlot->SetHorizontalAlignment(HAlign_Fill);
					VerticalSlot->SetVerticalAlignment(VAlign_Top);
				}
			}
			else if (UHorizontalBox* HBox = Cast<UHorizontalBox>(ParentWidget))
			{
				UHorizontalBoxSlot* HorizontalSlot = HBox->AddChildToHorizontalBox(ParentVerticalBox);
				if (HorizontalSlot)
				{
					HorizontalSlot->SetHorizontalAlignment(HAlign_Fill);
					HorizontalSlot->SetVerticalAlignment(VAlign_Center);
				}
			}
			else
			{
				// Fallback: try to add as child using UPanelWidget interface
				if (UPanelWidget* PanelWidget = Cast<UPanelWidget>(ParentWidget))
				{
					PanelWidget->AddChild(ParentVerticalBox);
				}
				else
				{
					UE_LOG(LogTemp, Warning, TEXT("CreateResetButton: Parent widget is not a panel widget, cannot add child"));
				}
			}
		}
	}

	// Create the reset button
	ResetButton = NewObject<UButton>(this);
	if (!ResetButton)
	{
		UE_LOG(LogTemp, Warning, TEXT("CreateResetButton: Failed to create button"));
		return;
	}

	// Set button properties
	ResetButton->SetIsEnabled(true);
	
	// Create text label for the button
	UTextBlock* ButtonText = NewObject<UTextBlock>(this);
	if (ButtonText)
	{
		ButtonText->SetText(FText::FromString(TEXT("Reset")));
		ButtonText->SetColorAndOpacity(FLinearColor::White);
		ButtonText->SetJustification(ETextJustify::Center);
		
		// Set font size
		FSlateFontInfo FontInfo = ButtonText->GetFont();
		FontInfo.Size = 14;
		ButtonText->SetFont(FontInfo);
		
		ResetButton->AddChild(ButtonText);
	}

	// Bind the click event
	ResetButton->OnClicked.AddDynamic(this, &UArmTimelineUMGWidget::TriggerImmediateReset);

	// Add the button to the VerticalBox below CommandIconsBox
	if (ParentVerticalBox)
	{
		UVerticalBoxSlot* ButtonSlot = ParentVerticalBox->AddChildToVerticalBox(ResetButton);
		if (ButtonSlot)
		{
			ButtonSlot->SetHorizontalAlignment(HAlign_Center);
			ButtonSlot->SetVerticalAlignment(VAlign_Top);
			ButtonSlot->SetPadding(FMargin(4.0f, 8.0f, 4.0f, 4.0f));
			
			// Set button size
			ButtonSlot->SetSize(FSlateChildSize(ESlateSizeRule::Automatic));
		}
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("CreateResetButton: Could not find or create VerticalBox parent"));
	}
}

void UArmTimelineUMGWidget::CheckEditingMode()
{
	// Enable editing mode by default when cursor position is valid
	if (!bIsEditingMode && CursorPosition >= 0)
	{
		EnableEditingMode();
	}
}

void UArmTimelineUMGWidget::SyncProgramToArm()
{
	// Only sync if we're in editing mode
	if (!bIsEditingMode)
	{
		UE_LOG(LogTemp, Warning, TEXT("SyncProgramToArm: Editing mode not enabled (bIsEditingMode=false)"));
		return;
	}
	
	// Get the associated arm and update its Program
	// If AssociatedArm is not set, try to get it from the GameManager's active timeline
	if (!AssociatedArm)
	{
		UE_LOG(LogTemp, Warning, TEXT("SyncProgramToArm: AssociatedArm not set, attempting to find from GameManager active timeline..."));
		
		// Try to get the arm from the GameManager's active timeline
		if (UWorld* World = GetWorld())
		{
			for (TActorIterator<AGameManager> It(World); It; ++It)
			{
				AGameManager* GameManager = *It;
				if (GameManager)
				{
					ABasicArm* ActiveArm = GameManager->GetActiveArm();
					if (ActiveArm)
					{
						AssociatedArm = ActiveArm;
						UE_LOG(LogTemp, Log, TEXT("SyncProgramToArm: Found associated arm from GameManager: %s"), *ActiveArm->GetName());
						break;
					}
				}
			}
		}
		
		if (!AssociatedArm)
		{
			UE_LOG(LogTemp, Warning, TEXT("SyncProgramToArm: Could not find associated arm from GameManager"));
			return;
		}
	}
	
	UE_LOG(LogTemp, Log, TEXT("SyncProgramToArm: Syncing %d commands to arm '%s'"), ArmProgram.Num(), *AssociatedArm->GetName());
	UE_LOG(LogTemp, Log, TEXT("SyncProgramToArm: Current arm Program has %d commands"), AssociatedArm->Program.Num());
	
	AssociatedArm->SetProgram(ArmProgram, AssociatedArm->bLoopProgram);
	
	UE_LOG(LogTemp, Log, TEXT("SyncProgramToArm: After SetProgram, arm Program has %d commands"), AssociatedArm->Program.Num());
}
