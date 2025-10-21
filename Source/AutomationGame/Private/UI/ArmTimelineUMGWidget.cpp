#include "UI/ArmTimelineUMGWidget.h"
#include "Engine/Texture2D.h"
#include "Components/Image.h"
#include "Components/TextBlock.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Sim/ArmTypes.h"
#include "Actors/BasicArm.h"
#include "AGLogChannels.h"
#include "UMG.h"

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
}

void UArmTimelineUMGWidget::NativeConstruct()
{
	Super::NativeConstruct();
	
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

void UArmTimelineUMGWidget::SetIconBackgroundColor(const FLinearColor& NewIconBackgroundColor)
{
	IconBackgroundColor = NewIconBackgroundColor;
	
	
	UpdateWidgetContent();
}

void UArmTimelineUMGWidget::InitializeWidget()
{
	// Initialize any default values or setup
	
	OnWidgetConstructed();
}

void UArmTimelineUMGWidget::UpdateWidgetContent()
{
	// Update text displays
	if (CurrentCommandText)
	{
		FString CommandName = TEXT("None");
		if (CurrentCommandIndex >= 0 && CurrentCommandIndex < ArmProgram.Num())
		{
			CommandName = GetCommandDisplayName(ArmProgram[CurrentCommandIndex].Cmd);
		}
		CurrentCommandText->SetText(FText::FromString(CommandName));
	}

	if (ProgressText)
	{
		FString ProgressString = FString::Printf(TEXT("Progress: %.1f%%"), CurrentCommandProgress * 100.0f);
		ProgressText->SetText(FText::FromString(ProgressString));
	}

	// Use caching flag to prevent unnecessary recreation
	bool bNeedsRecreation = !bIconsCreated || (CommandIconWidgets.Num() == 0);
	
	// Check if program size changed significantly (more than 2 commands difference)
	if (!bNeedsRecreation && ArmProgram.Num() > 0)
	{
		int32 CurrentIconCount = CommandIconWidgets.Num();
		int32 ProgramSize = FMath::Min(ArmProgram.Num(), MaxDisplayedCommands);
		if (FMath::Abs(CurrentIconCount - ProgramSize) > 2)
		{
			bNeedsRecreation = true;
		}
	}
	
	if (bNeedsRecreation)
	{
		// No icons exist or program changed significantly, create them
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
		FLinearColor BorderColor(0.2f, 0.2f, 0.2f, 0.8f); // Dark gray border
		CommandListBorder->SetBrushColor(BorderColor);
		CommandListBorder->SetPadding(FMargin(8.0f)); // Border thickness
		
		// Create a horizontal box inside the border to arrange icons
		UHorizontalBox* InnerHorizontalBox = NewObject<UHorizontalBox>(this);
		if (InnerHorizontalBox)
		{
			// Add the horizontal box to the border
			CommandListBorder->AddChild(InnerHorizontalBox);
			
			// Add the border to the command icons box
			CommandIconsBox->AddChild(CommandListBorder);
			
			// Create icons for the displayed commands
			int32 NumCommandsToShow = FMath::Min(ArmProgram.Num(), MaxDisplayedCommands);
			for (int32 i = 0; i < NumCommandsToShow; ++i)
			{
				UWidget* IconWidget = CreateCommandIcon(ArmProgram[i].Cmd, i);
				if (IconWidget)
				{
					CommandIconWidgets.Add(IconWidget);
					
					// Add to the inner horizontal box with proper spacing
					UHorizontalBoxSlot* BoxSlot = InnerHorizontalBox->AddChildToHorizontalBox(IconWidget);
					if (BoxSlot)
					{
						BoxSlot->SetPadding(FMargin(5.0f, 0.0f, 5.0f, 0.0f));
						BoxSlot->SetHorizontalAlignment(HAlign_Center);
						BoxSlot->SetVerticalAlignment(VAlign_Center);
					}
				}
			}
		}
	}
	else
	{
		// Fallback: create icons directly in the horizontal box
		int32 NumCommandsToShow = FMath::Min(ArmProgram.Num(), MaxDisplayedCommands);
		for (int32 i = 0; i < NumCommandsToShow; ++i)
		{
			UWidget* IconWidget = CreateCommandIcon(ArmProgram[i].Cmd, i);
			if (IconWidget)
			{
				CommandIconWidgets.Add(IconWidget);
				
				// Add to horizontal box
				UHorizontalBoxSlot* BoxSlot = CommandIconsBox->AddChildToHorizontalBox(IconWidget);
				if (BoxSlot)
				{
					BoxSlot->SetPadding(FMargin(5.0f, 0.0f, 5.0f, 0.0f));
					BoxSlot->SetHorizontalAlignment(HAlign_Center);
					BoxSlot->SetVerticalAlignment(VAlign_Center);
				}
			}
		}
	}
}

void UArmTimelineUMGWidget::UpdateCommandIcons()
{
	// Update existing icons
	for (int32 i = 0; i < CommandIconWidgets.Num(); ++i)
	{
		if (i < ArmProgram.Num() && CommandIconWidgets[i])
		{
			bool bIsCurrent = (i == CurrentCommandIndex);
			bool bIsCompleted = (i < CurrentCommandIndex);
			float Progress = (bIsCurrent) ? CurrentCommandProgress : (bIsCompleted ? 1.0f : 0.0f);
			
			UpdateCommandIcon(CommandIconWidgets[i], ArmProgram[i].Cmd, i, bIsCurrent, bIsCompleted, Progress);
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
