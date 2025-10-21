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

	// Create icons for the displayed commands
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
	// Create a simple image widget for the command icon
	UImage* ImageWidget = NewObject<UImage>(this);
	if (ImageWidget)
	{
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
		
		// Add background color to make icons more visible
		ImageWidget->SetColorAndOpacity(IconBackgroundColor);
		
		// Set initial state
		bool bIsCurrent = (Index == CurrentCommandIndex);
		bool bIsCompleted = (Index < CurrentCommandIndex);
		float Progress = (bIsCurrent) ? CurrentCommandProgress : (bIsCompleted ? 1.0f : 0.0f);
		
		// Call Blueprint implementation or use C++ fallback
		UpdateCommandIcon(ImageWidget, Command, Index, bIsCurrent, bIsCompleted, Progress);
		
		// UE_LOG(LogTemp, Log, TEXT("ArmTimelineUMGWidget: Created icon for command %d at index %d"), (int32)Command, Index);
	}
	
	return ImageWidget;
}

void UArmTimelineUMGWidget::UpdateCommandIcon(UWidget* IconWidget, EArmCmd Command, int32 Index, bool bIsCurrent, bool bIsCompleted, float Progress)
{
	// C++ implementation - this should take precedence
	if (UImage* ImageWidget = Cast<UImage>(IconWidget))
	{
		// UE_LOG(LogTemp, Warning, TEXT("ArmTimelineUMGWidget: UpdateCommandIcon called for index %d, size=%.1f, color=(%.2f,%.2f,%.2f,%.2f)"), 
		// 	Index, IconSize, IconBackgroundColor.R, IconBackgroundColor.G, IconBackgroundColor.B, IconBackgroundColor.A);
		// Update visual state based on progress
		FLinearColor IconColor = NormalCommandColor;
		if (bIsCompleted)
		{
			IconColor = CompletedCommandColor;
		}
		else if (bIsCurrent)
		{
			IconColor = FLinearColor::LerpUsingHSV(NormalCommandColor, CurrentCommandColor, Progress);
		}
		
		// Apply background color for visibility, but keep current command size unchanged
		FLinearColor FinalColor = FLinearColor::LerpUsingHSV(IconBackgroundColor, IconColor, 0.7f);
		
		// UE_LOG(LogTemp, Verbose, TEXT("ArmTimelineUMGWidget: Icon %d color - Background: R=%.2f G=%.2f B=%.2f A=%.2f, Command: R=%.2f G=%.2f B=%.2f A=%.2f, Final: R=%.2f G=%.2f B=%.2f A=%.2f"), 
		//	Index, IconBackgroundColor.R, IconBackgroundColor.G, IconBackgroundColor.B, IconBackgroundColor.A,
		//	IconColor.R, IconColor.G, IconColor.B, IconColor.A,
		//	FinalColor.R, FinalColor.G, FinalColor.B, FinalColor.A);
		
		// Update the texture and apply color in the correct order
		UTexture2D* CommandTexture = GetTextureForCommand(Command);
		if (CommandTexture)
		{
			ImageWidget->SetBrushFromTexture(CommandTexture);
		}
		
		// Apply tint color after setting texture to ensure it's not overridden
		FSlateBrush Brush = ImageWidget->GetBrush();
		Brush.TintColor = FinalColor;
		ImageWidget->SetBrush(Brush);
		
		
		// Apply size based on command state
		if (!bIsCurrent)
		{
			// Non-current commands use configurable size
			ImageWidget->SetDesiredSizeOverride(FVector2D(IconSize, IconSize));
			// UE_LOG(LogTemp, Verbose, TEXT("ArmTimelineUMGWidget: Set non-current icon size to %.1f"), IconSize);
		}
		else
		{
			// Current command uses configurable size with slight scaling
			float SizeMultiplier = 1.0f + Progress * 0.2f;
			float FinalSize = IconSize * SizeMultiplier;
			ImageWidget->SetDesiredSizeOverride(FVector2D(FinalSize, FinalSize));
			// UE_LOG(LogTemp, Verbose, TEXT("ArmTimelineUMGWidget: Set current icon size to %.1f (base %.1f * %.2f)"), FinalSize, IconSize, SizeMultiplier);
		}
		
		// UE_LOG(LogTemp, Verbose, TEXT("ArmTimelineUMGWidget: Updated icon %d - Current: %s, Completed: %s, Progress: %.2f"), 
		//	Index, bIsCurrent ? TEXT("Yes") : TEXT("No"), bIsCompleted ? TEXT("Yes") : TEXT("No"), Progress);
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
