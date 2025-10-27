#include "UI/ArmTimelineWidget.h"
#include "Blueprint/UserWidget.h"
#include "Engine/Texture2D.h"
#include "Components/WidgetComponent.h"
#include "Sim/ArmTypes.h"
#include "Actors/BasicArm.h"
#include "Actors/HexTopDownPawn.h"
#include "Framework/GameManager.h"
#include "UI/ArmTimelineUMGWidget.h"
#include "AGLogChannels.h"
#include "UMG.h"

UArmTimelineWidget::UArmTimelineWidget()
{
	// Enable tick updates for timeline widgets
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
	
	// Set up the widget component
	SetWidgetSpace(EWidgetSpace::World);
	SetDrawAtDesiredSize(true);
	SetDrawSize(FVector2D(400.0f, 100.0f));
	SetPivot(FVector2D(0.5f, 0.5f));
	
	// Start hidden by default - visibility will be controlled by GameManager
	SetVisibility(false);
	
	// Initialize default command textures (will be set by GameManager)
	CommandTextures.Empty();
}

void UArmTimelineWidget::BeginPlay()
{
	Super::BeginPlay();
	
	InitializeWidget();
	
}

void UArmTimelineWidget::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	
	// Reduced frequency tick - only update every 30th frame for performance
	static int32 TickCounter = 0;
	TickCounter++;
	
	if (TickCounter % 30 == 0) // Update every 30th frame
	{
		if (TimelineWidgetInstance && bTimelineVisible)
		{
			UpdateWidgetContent();
		}
	}
}

void UArmTimelineWidget::OnSimulationStep(float StepSeconds)
{
	// Update widget content only when simulation steps forward
	if (TimelineWidgetInstance && bTimelineVisible)
	{
		UpdateWidgetContent();
		UE_LOG(LogTemp, Verbose, TEXT("ArmTimelineWidget: Updated on simulation step (%.3fs) - Widget visible: %s"), 
			StepSeconds, IsVisible() ? TEXT("true") : TEXT("false"));
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("ArmTimelineWidget: OnSimulationStep called but widget not ready - Instance: %s, Visible: %s"), 
			TimelineWidgetInstance ? TEXT("exists") : TEXT("NULL"),
			bTimelineVisible ? TEXT("true") : TEXT("false"));
	}
}

void UArmTimelineWidget::SetArmProgram(const TArray<FArmInstr>& Program)
{
	ArmProgram = Program;
	CurrentCommandIndex = -1;
	CurrentCommandProgress = 0.0f;
	
	// Only log when program actually changes
	if (ArmProgram.Num() != Program.Num())
	{
	}
	
	if (TimelineWidgetInstance)
	{
		UpdateWidgetContent();
	}
}

void UArmTimelineWidget::SetCurrentCommand(int32 CommandIndex, float Progress)
{
	CurrentCommandIndex = FMath::Clamp(CommandIndex, -1, ArmProgram.Num() - 1);
	CurrentCommandProgress = FMath::Clamp(Progress, 0.0f, 1.0f);
	
	if (TimelineWidgetInstance)
	{
		UpdateWidgetContent();
	}
}

void UArmTimelineWidget::SetCommandTextures(const TMap<EArmCmd, UTexture2D*>& Textures)
{
	CommandTextures = Textures;
	
	
	if (TimelineWidgetInstance)
	{
		UpdateWidgetContent();
	}
}

void UArmTimelineWidget::SetTimelineWidgetVisible(bool bIsVisible)
{
	bTimelineVisible = bIsVisible;
	SetVisibility(bIsVisible);
	
}

void UArmTimelineWidget::SetTimelineScale(float Scale)
{
	TimelineScale = FMath::Max(0.1f, Scale);
	SetWorldScale3D(FVector(TimelineScale));
	
}

void UArmTimelineWidget::SetTimelineOffset(const FVector& Offset)
{
	TimelineOffset = Offset;
	SetRelativeLocation(Offset);
	
	// For top-down view, rotate the widget to face up (towards camera)
	// This ensures the widget is readable from above
	// Add 180° Y rotation to flip text right-side up
	SetRelativeRotation(FRotator(90.0f, 180.0f, 0.0f));
	
}

void UArmTimelineWidget::InitializeWidget()
{
	if (!TimelineWidgetClass)
	{
		UE_LOG(LogTemp, Warning, TEXT("ArmTimelineWidget: No TimelineWidgetClass set"));
		return;
	}
	
	// Create the widget instance
	
	TimelineWidgetInstance = CreateWidget<UUserWidget>(GetWorld(), TimelineWidgetClass);
	if (TimelineWidgetInstance)
	{
		SetWidget(TimelineWidgetInstance);
		
		// If it's an ArmTimelineUMGWidget, we can call its specific functions
		if (UArmTimelineUMGWidget* UMGWidget = Cast<UArmTimelineUMGWidget>(TimelineWidgetInstance))
		{
			UMGWidget->SetArmProgram(ArmProgram);
			UMGWidget->SetCommandTextures(CommandTextures);
			UMGWidget->SetDefaultCommandTexture(DefaultCommandTexture);
			UMGWidget->SetIconSize(IconSize);
			UMGWidget->SetIconBackgroundColor(IconBackgroundColor);
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("ArmTimelineWidget: Created generic widget, not UMG widget"));
		}
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("ArmTimelineWidget: Failed to create widget instance - TimelineWidgetClass is %s"), 
			TimelineWidgetClass ? TEXT("valid") : TEXT("NULL"));
	}
}

void UArmTimelineWidget::UpdateWidgetContent()
{
	if (!TimelineWidgetInstance)
	{
		return;
	}
	
	// Try to update the UMG widget if it's the right type
	if (UArmTimelineUMGWidget* UMGWidget = Cast<UArmTimelineUMGWidget>(TimelineWidgetInstance))
	{
		// Only update if content has changed
		static int32 LastCommandIndex = -1;
		static float LastCommandProgress = -1.0f;
		static int32 LastProgramSize = -1;
		
		bool bContentChanged = (CurrentCommandIndex != LastCommandIndex) || 
		                      (CurrentCommandProgress != LastCommandProgress) ||
		                      (ArmProgram.Num() != LastProgramSize);
		
		if (bContentChanged)
		{
			UMGWidget->SetArmProgram(ArmProgram);
			UMGWidget->SetCurrentCommand(CurrentCommandIndex, CurrentCommandProgress);
			UMGWidget->SetCommandTextures(CommandTextures);
			UMGWidget->SetDefaultCommandTexture(DefaultCommandTexture);
			
			// Update static variables
			LastCommandIndex = CurrentCommandIndex;
			LastCommandProgress = CurrentCommandProgress;
			LastProgramSize = ArmProgram.Num();
			
			UE_LOG(LogTemp, Verbose, TEXT("ArmTimelineWidget: Updated UMG widget - Command %d/%d, Progress %.2f"), 
				CurrentCommandIndex, ArmProgram.Num(), CurrentCommandProgress);
		}
	}
}

UTexture2D* UArmTimelineWidget::GetTextureForCommand(EArmCmd Command) const
{
	if (const UTexture2D* const* FoundTexture = CommandTextures.Find(Command))
	{
		return const_cast<UTexture2D*>(*FoundTexture);
	}
	
	return DefaultCommandTexture;
}

FString UArmTimelineWidget::GetCommandDisplayName(EArmCmd Command) const
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

void UArmTimelineWidget::SetTimelineWidgetClass(TSubclassOf<UUserWidget> NewWidgetClass)
{
	TimelineWidgetClass = NewWidgetClass;
	
	// If we already have a widget instance, recreate it with the new class
	if (TimelineWidgetInstance)
	{
		TimelineWidgetInstance = nullptr;
		SetWidget(nullptr);
	}
	
	// Create new widget instance if we have a valid class
	if (TimelineWidgetClass)
	{
		TimelineWidgetInstance = CreateWidget<UUserWidget>(GetWorld(), TimelineWidgetClass);
		if (TimelineWidgetInstance)
		{
			SetWidget(TimelineWidgetInstance);
			
			// Apply stored icon properties to the new widget
			if (UArmTimelineUMGWidget* UMGWidget = Cast<UArmTimelineUMGWidget>(TimelineWidgetInstance))
			{
				UMGWidget->SetIconSize(IconSize);
				UMGWidget->SetIconBackgroundColor(IconBackgroundColor);
			}
		}
	}
	
}


void UArmTimelineWidget::UpdateWidgetPosition(const FVector& ArmLocation, const FRotator& ArmRotation)
{
	// Calculate the widget position relative to the arm
	FVector WidgetLocation = ArmLocation + TimelineOffset;
	
	// Try to position the timeline to avoid blocking line of sight to the arm
	WidgetLocation = CalculateOptimalTimelinePosition(ArmLocation);
	
	// Check if we should rotate for top-down view
	FRotator WidgetRotation = FRotator::ZeroRotator; // Default rotation
	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<AGameManager> It(World); It; ++It)
		{
			AGameManager* GameManager = *It;
			if (GameManager && GameManager->bRotateWidgetsForTopDown)
			{
				// For top-down view, keep the widget facing up regardless of arm rotation
				WidgetRotation = FRotator(90.0f, 180.0f, 0.0f);
				break;
			}
			else
			{
				// For side view, follow the arm's rotation
				WidgetRotation = ArmRotation;
				break;
			}
		}
	}
	else
	{
		// Default to top-down rotation
		WidgetRotation = FRotator(90.0f, 180.0f, 0.0f);
	}
	
	// Set the world transform
	SetWorldLocation(WidgetLocation);
	SetWorldRotation(WidgetRotation);
	
	// Only update scale if enough time has passed (to prevent excessive updates)
	float CurrentTime = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
	if (FMath::Abs(CurrentTime - LastScaleUpdateTime) > 0.1f)
	{
		// Calculate distance-based scaling
		CachedDistanceScale = CalculateDistanceBasedScale(WidgetLocation);
		LastScaleUpdateTime = CurrentTime;
	}
	
	float FinalScale = TimelineScale * CachedDistanceScale;
	
	// Apply scale
	SetWorldScale3D(FVector(FinalScale, FinalScale, FinalScale));
}

void UArmTimelineWidget::UpdateWidgetPositionFromHex(const FHex& HexCoord, float BaseZ, const FRotator& ArmRotation)
{
	// Get the arm's world position from hex coordinates
	// This requires access to the driver's HexToWorld function
	FVector ArmWorldPosition;
	
	// Try to get the world position from the arm
	if (AActor* Owner = GetOwner())
	{
		if (ABasicArm* Arm = Cast<ABasicArm>(Owner))
		{
			// Use the arm's public function to get world position
			ArmWorldPosition = Arm->GetArmWorldPosition();
		}
		else
		{
			// Fallback to owner's location
			ArmWorldPosition = Owner->GetActorLocation();
		}
	}
	else
	{
		// Fallback to component's world location
		ArmWorldPosition = GetComponentLocation();
	}
	
	// Calculate the widget position relative to the arm
	FVector WidgetLocation = ArmWorldPosition + TimelineOffset;
	
	// Check if we should rotate for top-down view
	FRotator WidgetRotation = FRotator::ZeroRotator; // Default rotation
	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<AGameManager> It(World); It; ++It)
		{
			AGameManager* GameManager = *It;
			if (GameManager && GameManager->bRotateWidgetsForTopDown)
			{
				// For top-down view, keep the widget facing up regardless of arm rotation
				WidgetRotation = FRotator(90.0f, 180.0f, 0.0f);
				break;
			}
			else
			{
				// For side view, follow the arm's rotation
				WidgetRotation = ArmRotation;
				break;
			}
		}
	}
	else
	{
		// Default to top-down rotation
		WidgetRotation = FRotator(90.0f, 180.0f, 0.0f);
	}
	
	// Set the world transform
	SetWorldLocation(WidgetLocation);
	SetWorldRotation(WidgetRotation);
	
	// Only update scale if enough time has passed (to prevent excessive updates)
	float CurrentTime = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
	if (FMath::Abs(CurrentTime - LastScaleUpdateTime) > 0.1f)
	{
		// Calculate distance-based scaling
		CachedDistanceScale = CalculateDistanceBasedScale(WidgetLocation);
		LastScaleUpdateTime = CurrentTime;
	}
	
	float FinalScale = TimelineScale * CachedDistanceScale;
	
	// Apply scale
	SetWorldScale3D(FVector(FinalScale, FinalScale, FinalScale));
}

void UArmTimelineWidget::SetDefaultCommandTexture(UTexture2D* DefaultTexture)
{
	DefaultCommandTexture = DefaultTexture;
	
	
	if (TimelineWidgetInstance)
	{
		UpdateWidgetContent();
	}
}

void UArmTimelineWidget::SetIconSize(float NewIconSize)
{
	IconSize = NewIconSize;
	
	// Pass to UMG widget if it's the right type
	if (UArmTimelineUMGWidget* UMGWidget = Cast<UArmTimelineUMGWidget>(TimelineWidgetInstance))
	{
		UMGWidget->SetIconSize(IconSize);
	}
}

void UArmTimelineWidget::SetIconBackgroundColor(const FLinearColor& NewIconBackgroundColor)
{
	IconBackgroundColor = NewIconBackgroundColor;
	
	// Pass to UMG widget if it's the right type
	if (UArmTimelineUMGWidget* UMGWidget = Cast<UArmTimelineUMGWidget>(TimelineWidgetInstance))
	{
		UMGWidget->SetIconBackgroundColor(IconBackgroundColor);
	}
}

void UArmTimelineWidget::SetNormalCommandColor(const FLinearColor& NormalColor)
{
	NormalCommandColor = NormalColor;
	
	// Pass to UMG widget if it's the right type
	if (UArmTimelineUMGWidget* UMGWidget = Cast<UArmTimelineUMGWidget>(TimelineWidgetInstance))
	{
		UMGWidget->SetNormalCommandColor(NormalColor);
	}
}

void UArmTimelineWidget::SetCurrentCommandColor(const FLinearColor& CurrentColor)
{
	CurrentCommandColor = CurrentColor;
	
	// Pass to UMG widget if it's the right type
	if (UArmTimelineUMGWidget* UMGWidget = Cast<UArmTimelineUMGWidget>(TimelineWidgetInstance))
	{
		UMGWidget->SetCurrentCommandColor(CurrentColor);
	}
}

void UArmTimelineWidget::SetCompletedCommandColor(const FLinearColor& CompletedColor)
{
	CompletedCommandColor = CompletedColor;
	
	// Pass to UMG widget if it's the right type
	if (UArmTimelineUMGWidget* UMGWidget = Cast<UArmTimelineUMGWidget>(TimelineWidgetInstance))
	{
		UMGWidget->SetCompletedCommandColor(CompletedColor);
	}
}

FVector UArmTimelineWidget::CalculateOptimalTimelinePosition(const FVector& ArmLocation)
{
	// Check if smart positioning is enabled
	bool bSmartPositioningEnabled = true;
	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<AGameManager> It(World); It; ++It)
		{
			AGameManager* GameManager = *It;
			if (GameManager)
			{
				bSmartPositioningEnabled = GameManager->bUseSmartTimelinePositioning;
				break;
			}
		}
	}
	
	// If smart positioning is disabled, use default offset
	if (!bSmartPositioningEnabled)
	{
		return ArmLocation + TimelineOffset;
	}
	
	// Get the player's camera position and direction
	FVector PlayerLocation;
	FVector PlayerDirection;
	
	if (UWorld* World = GetWorld())
	{
		// Find the player controller
		APlayerController* PC = World->GetFirstPlayerController();
		if (PC && PC->GetPawn())
		{
			PlayerLocation = PC->GetPawn()->GetActorLocation();
			
			// Get the camera direction (where the player is looking)
			FRotator CameraRotation = PC->GetControlRotation();
			PlayerDirection = CameraRotation.Vector();
		}
	}
	
	// If we couldn't get player info, use default offset
	if (PlayerLocation.IsZero())
	{
		return ArmLocation + TimelineOffset;
	}
	
	// Calculate the vector from arm to player
	FVector ArmToPlayer = PlayerLocation - ArmLocation;
	ArmToPlayer.Normalize();
	
	// Calculate the line of sight vector (from player to arm)
	FVector LineOfSight = ArmLocation - PlayerLocation;
	LineOfSight.Normalize();
	
	// Try different positions around the arm to avoid blocking line of sight
	TArray<FVector> CandidatePositions;
	
	// Position 1: To the right of the line of sight
	FVector RightOffset = FVector::CrossProduct(LineOfSight, FVector::UpVector);
	RightOffset.Normalize();
	CandidatePositions.Add(ArmLocation + RightOffset * 200.0f + FVector(0, 0, 150.0f));
	
	// Position 2: To the left of the line of sight
	FVector LeftOffset = -RightOffset;
	CandidatePositions.Add(ArmLocation + LeftOffset * 200.0f + FVector(0, 0, 150.0f));
	
	// Position 3: Above the arm
	CandidatePositions.Add(ArmLocation + FVector(0, 0, 300.0f));
	
	// Position 4: Behind the arm (opposite to line of sight)
	FVector BehindOffset = -LineOfSight;
	CandidatePositions.Add(ArmLocation + BehindOffset * 200.0f + FVector(0, 0, 150.0f));
	
	// Find the best position that doesn't block the line of sight
	FVector BestPosition = ArmLocation + TimelineOffset; // Default fallback
	float BestScore = -1.0f;
	
	for (const FVector& CandidatePos : CandidatePositions)
	{
		// Calculate how much this position would block the line of sight
		FVector CandidateToPlayer = PlayerLocation - CandidatePos;
		FVector CandidateToArm = ArmLocation - CandidatePos;
		
		CandidateToPlayer.Normalize();
		CandidateToArm.Normalize();
		
		// Calculate the angle between the candidate position and the line of sight
		float DotProduct = FVector::DotProduct(CandidateToPlayer, LineOfSight);
		float Angle = FMath::Acos(FMath::Clamp(DotProduct, -1.0f, 1.0f));
		
		// Prefer positions that are further from the line of sight
		float Score = Angle; // Higher angle = better (less blocking)
		
		// Bonus for positions that are above the arm (less likely to block)
		if (CandidatePos.Z > ArmLocation.Z)
		{
			Score += 0.5f;
		}
		
		// Bonus for positions that are to the side rather than in front/behind
		FVector SideVector = FVector::CrossProduct(LineOfSight, FVector::UpVector);
		float SideDot = FMath::Abs(FVector::DotProduct((CandidatePos - ArmLocation).GetSafeNormal(), SideVector));
		Score += SideDot * 0.3f;
		
		if (Score > BestScore)
		{
			BestScore = Score;
			BestPosition = CandidatePos;
		}
	}
	
	return BestPosition;
}

void UArmTimelineWidget::SetMaxCommandIcons(int32 MaxIcons)
{
	MaxDisplayedCommands = FMath::Clamp(MaxIcons, 1, 20);
	
	// Update the UMG widget if it exists
	if (UArmTimelineUMGWidget* UMGWidget = Cast<UArmTimelineUMGWidget>(TimelineWidgetInstance))
	{
		UMGWidget->SetMaxDisplayedCommands(MaxDisplayedCommands);
	}
}

float UArmTimelineWidget::CalculateDistanceBasedScale(const FVector& WidgetLocation)
{
	// Check if distance-based scaling is enabled
	bool bDistanceScalingEnabled = true;
	float MinArmLength = 600.0f;
	float MaxArmLength = 6000.0f;
	float MinScale = 0.2f;
	float MaxScale = 2.0f;
	
	// Get settings from GameManager
	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<AGameManager> It(World); It; ++It)
		{
			AGameManager* GameManager = *It;
			if (GameManager)
			{
				bDistanceScalingEnabled = GameManager->bUseDistanceBasedScaling;
				MinArmLength = GameManager->TimelineMinDistance;
				MaxArmLength = GameManager->TimelineMaxDistance;
				MinScale = GameManager->TimelineMinScale;
				MaxScale = GameManager->TimelineMaxScale;
				break;
			}
		}
	}
	
	// If distance scaling is disabled, return default scale
	if (!bDistanceScalingEnabled)
	{
		return 1.0f;
	}
	
	// Get the player's camera arm length from HexTopDownPawn
	float CameraArmLength = 3000.0f; // Default fallback
	if (UWorld* World = GetWorld())
	{
		APlayerController* PC = World->GetFirstPlayerController();
		if (PC && PC->GetPawn())
		{
			// Try to cast to HexTopDownPawn to get the SpringArm length
			if (AHexTopDownPawn* HexPawn = Cast<AHexTopDownPawn>(PC->GetPawn()))
			{
				if (HexPawn->SpringArm)
				{
					CameraArmLength = HexPawn->SpringArm->TargetArmLength;
				}
			}
		}
	}
	
	// Clamp arm length to our range
	CameraArmLength = FMath::Clamp(CameraArmLength, MinArmLength, MaxArmLength);
	
	// Calculate scale based on camera arm length (closer = smaller scale)
	// Note: Shorter arm length = closer camera = smaller timeline scale
	float NormalizedArmLength = (CameraArmLength - MinArmLength) / (MaxArmLength - MinArmLength);
	float Scale = FMath::Lerp(MinScale, MaxScale, NormalizedArmLength);
	
	return Scale;
}