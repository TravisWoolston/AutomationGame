#include "UI/ArmTimelineWidgetBlueprint.h"
#include "Engine/Texture2D.h"
#include "Components/Image.h"
#include "Components/TextBlock.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Sim/ArmTypes.h"
#include "Actors/BasicArm.h"
#include "AGLogChannels.h"
#include "UMG.h"

UArmTimelineWidgetBlueprint::UArmTimelineWidgetBlueprint(const FObjectInitializer& ObjectInitializer)
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

void UArmTimelineWidgetBlueprint::NativeConstruct()
{
	Super::NativeConstruct();
	
	InitializeWidget();
	
	UE_LOG(LogTemp, Log, TEXT("ArmTimelineWidgetBlueprint: NativeConstruct"));
}

void UArmTimelineWidgetBlueprint::NativeDestruct()
{
	Super::NativeDestruct();
	
	UE_LOG(LogTemp, Log, TEXT("ArmTimelineWidgetBlueprint: NativeDestruct"));
}

void UArmTimelineWidgetBlueprint::SetArmProgram(const TArray<FArmInstr>& Program)
{
	ArmProgram = Program;
	CurrentCommandIndex = -1;
	CurrentCommandProgress = 0.0f;
	
	UE_LOG(LogTemp, Log, TEXT("ArmTimelineWidgetBlueprint: SetArmProgram - %d commands"), Program.Num());
	
	UpdateWidgetContent();
	OnProgramChanged();
}

void UArmTimelineWidgetBlueprint::SetCurrentCommand(int32 CommandIndex, float Progress)
{
	CurrentCommandIndex = FMath::Clamp(CommandIndex, -1, ArmProgram.Num() - 1);
	CurrentCommandProgress = FMath::Clamp(Progress, 0.0f, 1.0f);
	
	UE_LOG(LogTemp, Verbose, TEXT("ArmTimelineWidgetBlueprint: SetCurrentCommand - Index %d, Progress %.2f"), 
		CurrentCommandIndex, CurrentCommandProgress);
	
	UpdateWidgetContent();
	OnCurrentCommandChanged();
}

void UArmTimelineWidgetBlueprint::SetCommandTextures(const TMap<EArmCmd, UTexture2D*>& Textures)
{
	CommandTextures = Textures;
	
	UE_LOG(LogTemp, Log, TEXT("ArmTimelineWidgetBlueprint: SetCommandTextures - %d textures set"), Textures.Num());
	
	UpdateWidgetContent();
	OnCommandTexturesUpdated();
}

void UArmTimelineWidgetBlueprint::SetMaxDisplayedCommands(int32 MaxCommands)
{
	MaxDisplayedCommands = FMath::Clamp(MaxCommands, 1, 20);
	
	UE_LOG(LogTemp, Log, TEXT("ArmTimelineWidgetBlueprint: SetMaxDisplayedCommands - %d"), MaxDisplayedCommands);
	
	UpdateWidgetContent();
}

void UArmTimelineWidgetBlueprint::SetCommandColors(const FLinearColor& CurrentColor, const FLinearColor& NormalColor, const FLinearColor& CompletedColor)
{
	CurrentCommandColor = CurrentColor;
	NormalCommandColor = NormalColor;
	CompletedCommandColor = CompletedColor;
	
	UE_LOG(LogTemp, Log, TEXT("ArmTimelineWidgetBlueprint: SetCommandColors"));
	
	UpdateWidgetContent();
}

void UArmTimelineWidgetBlueprint::InitializeWidget()
{
	// Initialize any default values or setup
	UE_LOG(LogTemp, Log, TEXT("ArmTimelineWidgetBlueprint: InitializeWidget"));
	
	OnWidgetConstructed();
}

void UArmTimelineWidgetBlueprint::UpdateWidgetContent()
{
	// This is where the Blueprint implementation would update the UI
	// The actual UI updates are handled in Blueprint
	UE_LOG(LogTemp, Verbose, TEXT("ArmTimelineWidgetBlueprint: UpdateWidgetContent - %d commands, current %d"), 
		ArmProgram.Num(), CurrentCommandIndex);
}

UTexture2D* UArmTimelineWidgetBlueprint::GetTextureForCommand(EArmCmd Command) const
{
	if (const UTexture2D* const* FoundTexture = CommandTextures.Find(Command))
	{
		return const_cast<UTexture2D*>(*FoundTexture);
	}
	
	return DefaultCommandTexture;
}

FString UArmTimelineWidgetBlueprint::GetCommandDisplayName(EArmCmd Command) const
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
