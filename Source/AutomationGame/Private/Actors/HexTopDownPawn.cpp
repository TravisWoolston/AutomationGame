#include "Actors/HexTopDownPawn.h"
#include "GameFramework/SpringArmComponent.h"
#include "Camera/CameraComponent.h"
#include "Components/HexPlacementComponent.h"
#include "Components/StaticMeshComponent.h"
#include "GameFramework/PlayerController.h"
#include "Actors/OrbSimDriver.h"
#include "Actors/BasicArm.h"
#include "Sim/SimWorldHex.h"
#include "Sim/HexCoord.h"
#include "Sim/HexTypes.h"
#include "Sim/ArmTypes.h"
#include "DrawDebugHelpers.h"
#include "UI/AutomationGameHUD.h"
#include "Framework/GameManager.h"
#include "Framework/GameTickManager.h"
#include "Framework/TestLevelDirector.h"
#include "UI/ArmTimelineWidget.h"
#include "UI/ArmTimelineUMGWidget.h"
#include "UI/ArmTimelineEditorWidget.h"
#include "EngineUtils.h"
#include "Data/OrbMaterialManager.h"
#include "Materials/MaterialInstanceDynamic.h"

AHexTopDownPawn::AHexTopDownPawn()
{
	PrimaryActorTick.bCanEverTick = true;

	RootComponent = CreateDefaultSubobject<USceneComponent>("Root");

	SpringArm = CreateDefaultSubobject<USpringArmComponent>("SpringArm");
	SpringArm->SetupAttachment(RootComponent);
	SpringArm->TargetArmLength = 2000.f;
	SpringArm->bDoCollisionTest = false;
	SpringArm->SetRelativeRotation(FRotator(-60.f, 0.f, 0.f));

	Camera = CreateDefaultSubobject<UCameraComponent>("Camera");
	Camera->SetupAttachment(SpringArm);
	Camera->bUsePawnControlRotation = false;

	Placer = CreateDefaultSubobject<UHexPlacementComponent>("HexPlacer");
	Placer->bUseInputPolling = false; // we drive it via input events
}

void AHexTopDownPawn::ToggleEditorUI()
{
    const bool bShouldShow = (TimelineEditorWidget == nullptr);
    ShowTimelineEditor(bShouldShow);
}

void AHexTopDownPawn::BeginPlay()
{
	Super::BeginPlay();
	
	// Ensure placement preview is enabled by default
	if (Placer)
	{
		Placer->SetBlueprintMode(false);
	}
	
	// Bind to GameTickManager for NextTick functionality
	AGameTickManager* TickManager = AGameTickManager::Get(this);
	if (TickManager)
	{
		TickManager->OnFixedTick.AddDynamic(this, &AHexTopDownPawn::HandleNextTickStop);
	}
	
    // Editor UI now toggled explicitly via action
}

void AHexTopDownPawn::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	
    // Editor visibility is now managed by explicit calls to UpdateTimelineEditorVisibility()

    // Forward Timeline Editor hotkeys without needing input actions
    if (TimelineEditorWidget)
    {
        APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
        if (PC)
        {
            // Remove key first so it can override
            if (TimelineEditorWidget->RemoveButtonInputKey.IsValid() &&
                PC->WasInputKeyJustPressed(TimelineEditorWidget->RemoveButtonInputKey))
            {
                TimelineEditorWidget->HandleKeyInput(TimelineEditorWidget->RemoveButtonInputKey);
            }

            // Command slot keys
            const TArray<FCommandSlotConfig> SlotConfigs = TimelineEditorWidget->GetCommandSlotConfigs();
            for (const FCommandSlotConfig& Cfg : SlotConfigs)
            {
                if (Cfg.InputKey.IsValid() && PC->WasInputKeyJustPressed(Cfg.InputKey))
                {
                    TimelineEditorWidget->HandleKeyInput(Cfg.InputKey);
                    // Do not break; allow multiple keys same frame if user pressed them
                }
            }
        }
    }

	AGameManager* GM = nullptr; 
	if (UWorld* W = GetWorld()) 
	{ 
		for (TActorIterator<AGameManager> It(W); It; ++It)
		{ 
			GM = *It; 
			break; 
		} 
	}
	const bool bSphere = (GM && GM->bSphereGrid);
	if (bSphere)
	{
		const FVector Center = GM->GridAnchor ? GM->GridAnchor->GetActorLocation() : GM->GetActorLocation();
		FVector Pos = GetActorLocation();
		FVector RadVec = Pos - Center; 
		float Radius = RadVec.Size(); 
		if (Radius < 1.f) Radius = 1.f; 
		RadVec /= Radius; // normal
		// Build an orthonormal frame with Up=RadVec; choose an arbitrary reference to build East/North (longitude/latitude tangents)
		FVector Up = RadVec;
		FVector East = FVector::CrossProduct(FVector::UpVector, Up); 
		if (East.IsNearlyZero()) East = FVector::CrossProduct(FVector::RightVector, Up); 
		East.Normalize();
		FVector North = FVector::CrossProduct(Up, East).GetSafeNormal(); // points toward local north pole

		// Convert input axes into small angular rotations: AxisX = yaw around world up? Better: move along East (longitude), AxisY along North (latitude)
		float AngularSpeed = MoveSpeed / Radius; // rad/sec equivalent so that linear ground speed ~ MoveSpeed
		float dLon = AxisX * AngularSpeed * DeltaSeconds; // rotation around North axis? Actually East tangent corresponds to increasing longitude
		float dLat = AxisY * AngularSpeed * DeltaSeconds; // move toward poles

		// Apply latitude change: rotate RadVec toward North by dLat
		if (!FMath::IsNearlyZero(dLat))
		{
			RadVec = (RadVec * FMath::Cos(dLat) + North * FMath::Sin(dLat)).GetSafeNormal();
		}
		// Recompute East & North after latitude change
		Up = RadVec; 
		East = FVector::CrossProduct(FVector::UpVector, Up); 
		if (East.IsNearlyZero()) East = FVector::CrossProduct(FVector::RightVector, Up); 
		East.Normalize(); 
		North = FVector::CrossProduct(Up, East).GetSafeNormal();
		// Apply longitude change: rotate around Up axis by dLon using East/North plane
		if (!FMath::IsNearlyZero(dLon))
		{
			RadVec = (RadVec * FMath::Cos(dLon) + East * FMath::Sin(dLon)).GetSafeNormal();
		}
		SetActorLocation(Center + RadVec * Radius);
	}
	else
	{
		// Planar panning
		const FVector Right = GetActorRightVector();
		const FVector Fwd = GetActorForwardVector();
		FVector Delta = (Right * AxisX + Fwd * AxisY) * MoveSpeed * DeltaSeconds;
		AddActorWorldOffset(Delta, true);
	}

	// Zoom (disabled when TypeSelectionActive is held)
	if (!bTypeSelectionActive)
	{
		SpringArm->TargetArmLength = FMath::Clamp(SpringArm->TargetArmLength - AxisZoom * ZoomSpeed * DeltaSeconds, MinArm, MaxArm);
	}
	
	// Update selection if active
	UpdateSelection();
	UpdateDeletionSelection();
	
	// Update HUD selection box if active (both blueprint and deletion use the same box)
	if ((bBlueprintSelectionActive || bDeletionSelectionActive) && GetWorld())
	{
		APlayerController* PC = GetWorld()->GetFirstPlayerController();
		if (PC)
		{
			AAutomationGameHUD* GameHUD = Cast<AAutomationGameHUD>(PC->GetHUD());
			if (GameHUD)
			{
				FVector2D MousePosition;
				if (PC->GetMousePosition(MousePosition.X, MousePosition.Y))
				{
					GameHUD->UpdateSelectionBox(MousePosition);
				}
			}
		}
	}
	
	// Update blueprint preview meshes if in placement mode
	if (bBlueprintPlacementMode && BlueprintTiles.Num() > 0 && GetWorld())
	{
		UpdateBlueprintPreviewMeshes();
	}
	else
	{
		ClearBlueprintPreviewComponents();
	}
}

void AHexTopDownPawn::SetupPlayerInputComponent(UInputComponent* IC)
{
	Super::SetupPlayerInputComponent(IC);
	// Axis (set up in Project Settings → Input)
	IC->BindAxis("MoveRight", this, &AHexTopDownPawn::MoveX);
	IC->BindAxis("MoveForward", this, &AHexTopDownPawn::MoveY);
	IC->BindAxis("Zoom", this, &AHexTopDownPawn::Zoom);

    // Actions
	IC->BindAction("Place", IE_Pressed, this, &AHexTopDownPawn::PlacePressed);
	IC->BindAction("Place", IE_Released, this, &AHexTopDownPawn::PlaceReleased);
	IC->BindAction("Remove", IE_Pressed, this, &AHexTopDownPawn::RemovePressed);

    // Editor UI toggle (user assigns key in Project Settings)
    IC->BindAction("EditorUIToggle", IE_Pressed, this, &AHexTopDownPawn::ToggleEditorUI);
	
	// Blueprint mode (hold to select, toggle for placement)
	IC->BindAction("BlueprintMode", IE_Pressed, this, &AHexTopDownPawn::BlueprintModePressed);
	IC->BindAction("BlueprintMode", IE_Released, this, &AHexTopDownPawn::BlueprintModeReleased);
	
	// Deletion mode (hold to select, then delete on release)
	IC->BindAction("DeletionMode", IE_Pressed, this, &AHexTopDownPawn::DeletionModePressed);
	IC->BindAction("DeletionMode", IE_Released, this, &AHexTopDownPawn::DeletionModeReleased);
	
	// Blueprint placement actions (use same keys as regular rotation)
	// IC->BindAction("BlueprintRotateCW", IE_Pressed, this, &AHexTopDownPawn::RotateBlueprintCW);
	// IC->BindAction("BlueprintRotateCCW", IE_Pressed, this, &AHexTopDownPawn::RotateBlueprintCCW);

	IC->BindAction("RotateCW", IE_Pressed, this, &AHexTopDownPawn::RotCW);
	IC->BindAction("RotateCCW", IE_Pressed, this, &AHexTopDownPawn::RotCCW);

	IC->BindAction("Kind1", IE_Pressed, this, &AHexTopDownPawn::SetInputMode1);
	IC->BindAction("Kind2", IE_Pressed, this, &AHexTopDownPawn::SetInputMode2);
	IC->BindAction("Kind3", IE_Pressed, this, &AHexTopDownPawn::SetInputMode3);
	IC->BindAction("Kind4", IE_Pressed, this, &AHexTopDownPawn::SetInputMode4);
	IC->BindAction("Kind5", IE_Pressed, this, &AHexTopDownPawn::SetInputMode5);
	IC->BindAction("Kind6", IE_Pressed, this, &AHexTopDownPawn::SetInputMode6);
	IC->BindAction("Kind7", IE_Pressed, this, &AHexTopDownPawn::SetInputMode7);
	IC->BindAction("Kind8", IE_Pressed, this, &AHexTopDownPawn::SetInputMode8);

	// Type cycling (works for both Input and Output based on current placement mode)
	IC->BindAction("CycleNextType", IE_Pressed, this, &AHexTopDownPawn::CycleInputTypeNext);
	IC->BindAction("CyclePreviousType", IE_Pressed, this, &AHexTopDownPawn::CycleInputTypePrevious);
	
	// Glyph cycling (Belt, Bond, Unbond)
	IC->BindAction("CycleGlyphType", IE_Pressed, this, &AHexTopDownPawn::CycleGlyphType);
	
	// Selection modifier key (Left Alt)
	IC->BindAction("TypeSelectionActive", IE_Pressed, this, &AHexTopDownPawn::SelectionActivePressed);
	IC->BindAction("TypeSelectionActive", IE_Released, this, &AHexTopDownPawn::SelectionActiveReleased);
	
	// Simulation control
	IC->BindAction("NextTick", IE_Pressed, this, &AHexTopDownPawn::NextTick);
	
	// Timeline control
	IC->BindAction("ToggleArmTimelines", IE_Pressed, this, &AHexTopDownPawn::ToggleArmTimelines);
	
	// Timeline editing - cursor control
	IC->BindAction("NextCommand", IE_Pressed, this, &AHexTopDownPawn::NextCommand);
	IC->BindAction("PrevCommand", IE_Pressed, this, &AHexTopDownPawn::PrevCommand);
}

void AHexTopDownPawn::SetInputMode1() { if (Placer) Placer->SetPlaceKind(EPlaceKind::Input); }
void AHexTopDownPawn::SetInputMode2() { if (Placer) Placer->SetPlaceKind(EPlaceKind::Output); }
void AHexTopDownPawn::SetInputMode3() { if (Placer) Placer->SetPlaceKind(EPlaceKind::Belt); }
void AHexTopDownPawn::SetInputMode4() { if (Placer) Placer->SetPlaceKind(EPlaceKind::Arm); }
void AHexTopDownPawn::SetInputMode5() { if (Placer) Placer->SetPlaceKind(EPlaceKind::Bond); }
void AHexTopDownPawn::SetInputMode6() { if (Placer) Placer->SetPlaceKind(EPlaceKind::Unbond); }
void AHexTopDownPawn::SetInputMode7() { if (Placer) Placer->SetPlaceKind(EPlaceKind::ClusterOutput); }
void AHexTopDownPawn::SetInputMode8() 
{ 
	// Find GameManager to get DifficultyValue
	AGameManager* GameManager = nullptr;
	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<AGameManager> It(World); It; ++It)
		{
			GameManager = *It;
			break;
		}
	}
	
	if (!GameManager)
	{
		UE_LOG(LogTemp, Warning, TEXT("SetInputMode8: No GameManager found"));
		if (GEngine)
		{
			GEngine->AddOnScreenDebugMessage(-1, 3.0f, FColor::Red, TEXT("No GameManager found"));
		}
		return;
	}
	
	// Get the simulation to access templates
	AOrbSimDriver* Driver = AOrbSimDriver::Find(GetWorld());
	if (!Driver)
	{
		UE_LOG(LogTemp, Warning, TEXT("SetInputMode8: No OrbSimDriver found"));
		return;
	}
	
	USimWorldHex* Sim = Driver->GetSim();
	if (!Sim)
	{
		UE_LOG(LogTemp, Warning, TEXT("SetInputMode8: No Sim found"));
		return;
	}
	
	// Generate a random cluster based on difficulty
	int32 TemplateId = GameManager->GenerateRandomCluster();
	
	if (TemplateId < 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("SetInputMode8: Failed to generate cluster"));
		if (GEngine)
		{
			GEngine->AddOnScreenDebugMessage(-1, 3.0f, FColor::Red, TEXT("Failed to generate cluster"));
		}
		return;
	}
	
	UE_LOG(LogTemp, Log, TEXT("SetInputMode8: Generated cluster template with ID %d"), TemplateId);
	
	// Get the template to convert it into a blueprint
	const USimWorldHex::FClusterTemplate* Template = Sim->GetClusterTemplates().Find((uint8)TemplateId);
	if (!Template)
	{
		UE_LOG(LogTemp, Warning, TEXT("SetInputMode8: Template %d not found"), TemplateId);
		if (GEngine)
		{
			GEngine->AddOnScreenDebugMessage(-1, 3.0f, FColor::Red, TEXT("Template not found"));
		}
		return;
	}
	
	// Clear any existing blueprint
	ClearSelection();
	
	// Convert template into blueprint tiles (ClusterOutput tiles)
	for (const FOrbData& Orb : Template->Orbs)
	{
		// Create a FBlueprintTile for each orb in the cluster
		FBlueprintTile BlueprintTile;
		BlueprintTile.LocalHex = FHex(Orb.dQ, Orb.dR);
		BlueprintTile.TileType = ETileKind::ClusterOutput;
		BlueprintTile.TileDirection = (uint8)TemplateId; // Store template ID in TileDirection
		BlueprintTiles.Add(BlueprintTile);
	}
	
	// Enter blueprint placement mode
	bBlueprintPlacementMode = true;
	bBlueprintMode = false;
	BlueprintRotation = 0;
	BlueprintPlacementOffset = FHex(0, 0);
	
	// Disable HexPlacementComponent preview when in blueprint mode
	if (Placer)
	{
		Placer->SetBlueprintMode(true);
	}
	
	if (GEngine)
	{
		FString Msg = FString::Printf(TEXT("Generated random cluster (ID: %d, Difficulty: %d) - %d tiles ready to place"), TemplateId, GameManager->DifficultyValue, BlueprintTiles.Num());
		GEngine->AddOnScreenDebugMessage(-1, 3.0f, FColor::Green, Msg);
	}
}

void AHexTopDownPawn::SelectionActivePressed() { bTypeSelectionActive = true; }
void AHexTopDownPawn::SelectionActiveReleased() { bTypeSelectionActive = false; }

void AHexTopDownPawn::CycleInputTypeNext() 
{ 
	// Check if we're in Kind8 blueprint placement mode with TypeSelectionActive
	if (bBlueprintPlacementMode && bTypeSelectionActive && BlueprintTiles.Num() > 0)
	{
		// Find GameManager to adjust DifficultyValue
		AGameManager* GameManager = nullptr;
		if (UWorld* World = GetWorld())
		{
			for (TActorIterator<AGameManager> It(World); It; ++It)
			{
				GameManager = *It;
				break;
			}
		}
		
		if (GameManager)
		{
			// Increase difficulty (decrease means harder, but the value increases)
			GameManager->DifficultyValue = FMath::Min(GameManager->DifficultyValue + 1, 100);
			UE_LOG(LogTemp, Log, TEXT("Kind8 difficulty increased to %d"), GameManager->DifficultyValue);
			
			// Regenerate the cluster
			SetInputMode8();
		}
	}
	else if (Placer)
	{
		Placer->CycleNextType();
	}
}

void AHexTopDownPawn::CycleInputTypePrevious() 
{ 
	// Check if we're in Kind8 blueprint placement mode with TypeSelectionActive
	if (bBlueprintPlacementMode && bTypeSelectionActive && BlueprintTiles.Num() > 0)
	{
		// Find GameManager to adjust DifficultyValue
		AGameManager* GameManager = nullptr;
		if (UWorld* World = GetWorld())
		{
			for (TActorIterator<AGameManager> It(World); It; ++It)
			{
				GameManager = *It;
				break;
			}
		}
		
		if (GameManager)
		{
			// Decrease difficulty (increase means easier, but the value decreases)
			GameManager->DifficultyValue = FMath::Max(GameManager->DifficultyValue - 1, 4);
			UE_LOG(LogTemp, Log, TEXT("Kind8 difficulty decreased to %d"), GameManager->DifficultyValue);
			
			// Regenerate the cluster
			SetInputMode8();
		}
	}
	else if (Placer)
	{
		Placer->CyclePreviousType();
	}
}

void AHexTopDownPawn::NextTick()
{
	// Find the GameTickManager
	AGameTickManager* TickManager = AGameTickManager::Get(this);
	if (!TickManager)
	{
		UE_LOG(LogTemp, Warning, TEXT("NextTick: No GameTickManager found"));
		return;
	}
	
	// Set flag to pause on the next GTM tick
	bPauseOnNextTick = true;
	
	// If paused, start playing so we can reach the next tick
	if (!TickManager->IsPlaying())
	{
		TickManager->Play();
		UE_LOG(LogTemp, Log, TEXT("NextTick: Playing until next tick"));
	}
	else
	{
		// Already playing, will pause on next tick
		UE_LOG(LogTemp, Log, TEXT("NextTick: Will pause on next tick"));
	}
}

void AHexTopDownPawn::HandleNextTickStop(float StepSeconds)
{
	if (bPauseOnNextTick)
	{
		AGameTickManager* TickManager = AGameTickManager::Get(this);
		if (TickManager)
		{
			TickManager->Pause();
			UE_LOG(LogTemp, Log, TEXT("NextTick: Paused at tick boundary"));
		}
		bPauseOnNextTick = false;
	}
}

void AHexTopDownPawn::ToggleArmTimelines()
{
	// Find the GameManager
	AGameManager* GameManager = nullptr;
	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<AGameManager> It(World); It; ++It)
		{
			GameManager = *It;
			break;
		}
	}

	if (GameManager)
	{
        // Toggle the timeline visibility
		bool bNewVisibility = !GameManager->bShowArmTimelines;
		GameManager->SetTimelineVisibility(bNewVisibility);
        // Editor UI no longer auto-toggles here
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("ToggleArmTimelines: No GameManager found"));
	}
}

void AHexTopDownPawn::NextCommand()
{
	// Find the GameManager and get the active timeline
	AGameManager* GameManager = nullptr;
	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<AGameManager> It(World); It; ++It)
		{
			GameManager = *It;
			break;
		}
	}
	
	if (GameManager)
	{
		UArmTimelineWidget* ActiveTimeline = GameManager->GetActiveTimeline();
		if (ActiveTimeline)
		{
			UUserWidget* WidgetInstance = ActiveTimeline->GetTimelineWidgetInstance();
			if (WidgetInstance)
			{
				UArmTimelineUMGWidget* UMGWidget = Cast<UArmTimelineUMGWidget>(WidgetInstance);
				if (UMGWidget)
				{
					UE_LOG(LogTemp, Log, TEXT("NextCommand: Moving cursor right"));
					UMGWidget->MoveCursorRight();
				}
			}
		}
	}
}

void AHexTopDownPawn::PrevCommand()
{
	// Find the GameManager and get the active timeline
	AGameManager* GameManager = nullptr;
	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<AGameManager> It(World); It; ++It)
		{
			GameManager = *It;
			break;
		}
	}
	
	if (GameManager)
	{
		UArmTimelineWidget* ActiveTimeline = GameManager->GetActiveTimeline();
		if (ActiveTimeline)
		{
			UUserWidget* WidgetInstance = ActiveTimeline->GetTimelineWidgetInstance();
			if (WidgetInstance)
			{
				UArmTimelineUMGWidget* UMGWidget = Cast<UArmTimelineUMGWidget>(WidgetInstance);
				if (UMGWidget)
				{
					UMGWidget->MoveCursorLeft();
				}
			}
		}
	}
}

ABasicArm* AHexTopDownPawn::FindArmAtHex(const FHex& Hex)
{
	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<ABasicArm> It(World); It; ++It)
		{
			ABasicArm* Arm = *It;
			if (Arm && Arm->Q == Hex.Q && Arm->R == Hex.R)
			{
				return Arm;
			}
		}
	}
	return nullptr;
}

void AHexTopDownPawn::ShowTimelineEditor(bool bShow)
{
	if (!GetWorld())
	{
		return;
	}
	
	APlayerController* PC = GetWorld()->GetFirstPlayerController();
	if (!PC)
	{
		return;
	}
	
	if (bShow)
	{
		// Remove existing editor if it exists
		if (TimelineEditorWidget)
		{
			TimelineEditorWidget->RemoveFromParent();
			TimelineEditorWidget = nullptr;
		}
		
		// Create new editor widget
		if (TimelineEditorWidgetClass)
		{
			TimelineEditorWidget = CreateWidget<UArmTimelineEditorWidget>(GetWorld(), TimelineEditorWidgetClass);
			if (TimelineEditorWidget)
			{
				TimelineEditorWidget->AddToViewport();
				
				// Use GameAndUI mode to allow both game and widget interaction
				// Don't lock mouse to viewport or capture focus
				// FInputModeGameAndUI InputMode;
				// InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
				// InputMode.SetHideCursorDuringCapture(false);
				// PC->SetInputMode(InputMode);
				PC->bShowMouseCursor = true;
			}
		}
	}
	else
	{
		// Remove the editor widget from viewport
		if (TimelineEditorWidget)
		{
			TimelineEditorWidget->RemoveFromParent();
			TimelineEditorWidget = nullptr;
			
			// Keep GameAndUI mode since we might open another timeline
			// This prevents the double-click issue
			// FInputModeGameAndUI InputMode;
			// InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
			// InputMode.SetHideCursorDuringCapture(false);
			// PC->SetInputMode(InputMode);
			// PC->bShowMouseCursor = true;
		}
	}
}

void AHexTopDownPawn::SetActiveTimelineForEditing(ABasicArm* Arm)
{
	if (!Arm || !Arm->TimelineWidget)
	{
		return;
	}
	
	// Find the GameManager and set the active timeline
	AGameManager* GameManager = nullptr;
	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<AGameManager> It(World); It; ++It)
		{
			GameManager = *It;
			break;
		}
	}
	
	if (GameManager)
	{
		// Set this arm's timeline as the active one
		GameManager->SetActiveTimeline(Arm->TimelineWidget);
	}
}

void AHexTopDownPawn::UpdateTimelineEditorVisibility()
{
    // No-op: editor UI is toggled explicitly via EditorUIToggle action
}

void AHexTopDownPawn::ToggleArmTimeline(ABasicArm* Arm)
{
	if (Arm && Arm->TimelineWidget)
	{
		// Toggle the timeline visibility using the public method
		bool bCurrentVisibility = Arm->TimelineWidget->bTimelineVisible;
		Arm->TimelineWidget->SetTimelineWidgetVisible(!bCurrentVisibility);
		
        // If making timeline visible, set it as the active timeline (editor UI is independent)
		if (!bCurrentVisibility)
		{
			SetActiveTimelineForEditing(Arm);
			
			// Enable editing mode on the UMG widget when timeline becomes visible
			if (UUserWidget* WidgetInstance = Arm->TimelineWidget->GetTimelineWidgetInstance())
			{
				if (UArmTimelineUMGWidget* UMGWidget = Cast<UArmTimelineUMGWidget>(WidgetInstance))
				{
					UMGWidget->EnableEditingMode();
				}
			}
		}
		else
		{
			// Making timeline invisible - clear the active timeline and update editor visibility
			AGameManager* GameManager = nullptr;
			if (UWorld* World = GetWorld())
			{
				for (TActorIterator<AGameManager> It(World); It; ++It)
				{
					GameManager = *It;
					break;
				}
			}
			
			if (GameManager)
			{
				// Clear the active timeline if it's the one being hidden
				UArmTimelineWidget* ActiveTimeline = GameManager->GetActiveTimeline();
				if (ActiveTimeline == Arm->TimelineWidget)
				{
					GameManager->SetActiveTimeline(nullptr);
				}
			}
			
			// Disable editing mode on the UMG widget when timeline becomes hidden
			if (UUserWidget* WidgetInstance = Arm->TimelineWidget->GetTimelineWidgetInstance())
			{
				if (UArmTimelineUMGWidget* UMGWidget = Cast<UArmTimelineUMGWidget>(WidgetInstance))
				{
					UMGWidget->DisableEditingMode();
				}
			}
			
            // Editor UI visibility is independent; do nothing here
		}
	}
}


void AHexTopDownPawn::StartSelection()
{
	UE_LOG(LogTemp, Warning, TEXT("StartSelection called, blueprint mode: %s"), bBlueprintMode ? TEXT("ON") : TEXT("OFF"));
	
	if (!bBlueprintMode) return;
	
	APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	if (!PC) return;
	
	FVector2D MousePosition;
	if (PC->GetMousePosition(MousePosition.X, MousePosition.Y))
	{
		SelectionStart = MousePosition;
		SelectionEnd = MousePosition;
		bBlueprintSelectionActive = true;
		
		// Start HUD selection box
		AAutomationGameHUD* GameHUD = Cast<AAutomationGameHUD>(PC->GetHUD());
		if (GameHUD)
		{
			GameHUD->StartSelectionBox(MousePosition);
		}
		
		UE_LOG(LogTemp, Warning, TEXT("Selection started at (%f, %f)"), MousePosition.X, MousePosition.Y);
	}
	else
	{
		// UE_LOG(LogTemp, Warning, TEXT("Failed to get mouse position for selection"));
	}
}

void AHexTopDownPawn::EndSelection()
{
	UE_LOG(LogTemp, Warning, TEXT("EndSelection called, blueprint mode: %s, selection active: %s"), 
		bBlueprintMode ? TEXT("ON") : TEXT("OFF"), bBlueprintSelectionActive ? TEXT("YES") : TEXT("NO"));
	
	if (!bBlueprintMode || !bBlueprintSelectionActive) return;
	
	bBlueprintSelectionActive = false;
	
	// End HUD selection box
	APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	if (PC)
	{
		AAutomationGameHUD* GameHUD = Cast<AAutomationGameHUD>(PC->GetHUD());
		if (GameHUD)
		{
			GameHUD->EndSelectionBox();
		}
	}
	
	CreateBlueprint();
}

void AHexTopDownPawn::UpdateSelection()
{
	if (!bBlueprintSelectionActive || !GetWorld()) return;
	
	FVector2D MousePosition;
	if (GetWorld()->GetFirstPlayerController() && 
		GetWorld()->GetFirstPlayerController()->GetMousePosition(MousePosition.X, MousePosition.Y))
	{
		SelectionEnd = MousePosition;
		
		// Update selected tiles based on current selection box
		SelectedTiles.Empty();
		
		// Get tile size and grid info from driver
		AOrbSimDriver* Driver = AOrbSimDriver::Find(GetWorld());
		float TileSize = Driver ? Driver->GetTileSize() : 100.f;
		EHexOrientation Orientation = Driver ? Driver->GetGridOrientation() : EHexOrientation::PointyTop;
		float GridPlaneZ = Driver ? Driver->GetGridPlaneZ(this) : 0.f;
		
		// Convert screen coordinates to world coordinates by projecting rays to the grid plane
		FVector WorldStart, WorldEnd;
		FVector WorldStartDir, WorldEndDir;
		
		// Deproject both screen points to get rays
		GetWorld()->GetFirstPlayerController()->DeprojectScreenPositionToWorld(
			SelectionStart.X, SelectionStart.Y, WorldStart, WorldStartDir);
		GetWorld()->GetFirstPlayerController()->DeprojectScreenPositionToWorld(
			SelectionEnd.X, SelectionEnd.Y, WorldEnd, WorldEndDir);
		
		// Project the rays down to the grid plane (Z = GridPlaneZ)
		if (FMath::Abs(WorldStartDir.Z) > KINDA_SMALL_NUMBER)
		{
			float T = (GridPlaneZ - WorldStart.Z) / WorldStartDir.Z;
			WorldStart = WorldStart + WorldStartDir * T;
		}
		if (FMath::Abs(WorldEndDir.Z) > KINDA_SMALL_NUMBER)
		{
			float T = (GridPlaneZ - WorldEnd.Z) / WorldEndDir.Z;
			WorldEnd = WorldEnd + WorldEndDir * T;
		}
		
		// Convert world coordinates to hex coordinates
		FHexAxial HexStartAxial = HexCoord::XYToAxialRound(FVector2D(WorldStart.X, WorldStart.Y), TileSize, Orientation);
		FHexAxial HexEndAxial = HexCoord::XYToAxialRound(FVector2D(WorldEnd.X, WorldEnd.Y), TileSize, Orientation);
		FHex HexStart(HexStartAxial.Q, HexStartAxial.R);
		FHex HexEnd(HexEndAxial.Q, HexEndAxial.R);
		
		// Debug: Log the selection area with grid plane info
		UE_LOG(LogTemp, Warning, TEXT("Selection area: Screen(%f,%f) to (%f,%f) -> World(%f,%f,%.1f) to (%f,%f,%.1f) -> Hex(%d,%d) to (%d,%d) [GridZ=%.1f]"), 
			SelectionStart.X, SelectionStart.Y, SelectionEnd.X, SelectionEnd.Y,
			WorldStart.X, WorldStart.Y, WorldStart.Z, WorldEnd.X, WorldEnd.Y, WorldEnd.Z,
			HexStart.Q, HexStart.R, HexEnd.Q, HexEnd.R, GridPlaneZ);
		
		// Use improved hexagonal selection algorithm with actual world positions
		GetHexesInSelectionArea(HexStart, HexEnd, WorldStart, WorldEnd, SelectedTiles);
		
		UE_LOG(LogTemp, Warning, TEXT("Blueprint selection: %d tiles selected"), SelectedTiles.Num());
	}
}

void AHexTopDownPawn::BlueprintModePressed()
{
	UE_LOG(LogTemp, Warning, TEXT("BlueprintMode pressed"));
	if (bBlueprintPlacementMode)
	{
		// If we're in placement mode, toggle to exit
		bBlueprintPlacementMode = false;
		bBlueprintMode = false;
		ClearSelection();
	}
	else
	{
		// Enter blueprint selection mode temporarily
		bBlueprintMode = true;
	}
}

void AHexTopDownPawn::BlueprintModeReleased()
{
	UE_LOG(LogTemp, Warning, TEXT("BlueprintMode released"));
	if (bBlueprintSelectionActive)
	{
		// End selection and create blueprint
		EndSelection();
	}
	else if (!bBlueprintPlacementMode)
	{
		// Exit blueprint mode if not in placement mode
		bBlueprintMode = false;
	}
}

void AHexTopDownPawn::ToggleBlueprintMode()
{
	bBlueprintMode = !bBlueprintMode;
	UE_LOG(LogTemp, Warning, TEXT("Blueprint mode toggled: %s"), bBlueprintMode ? TEXT("ON") : TEXT("OFF"));
	
	if (!bBlueprintMode)
	{
		ClearSelection();
		bBlueprintPlacementMode = false;
	}
	else
	{
		// Enter blueprint mode - clear any existing blueprint
		ClearSelection();
		bBlueprintPlacementMode = false;
	}
}

void AHexTopDownPawn::CreateBlueprint()
{
	UE_LOG(LogTemp, Warning, TEXT("CreateBlueprint called with %d selected tiles"), SelectedTiles.Num());
	
	if (SelectedTiles.Num() == 0) 
	{
		UE_LOG(LogTemp, Warning, TEXT("No tiles selected, exiting CreateBlueprint"));
		return;
	}
	
	BlueprintTiles.Empty();
	
	// Find the simulation world
	AOrbSimDriver* Driver = AOrbSimDriver::Find(GetWorld());
	if (!Driver) return;
	
	USimWorldHex* Sim = Driver->GetSim();
	if (!Sim) return;
	
	// Find the minimum hex coordinates to use as the blueprint origin
	FHex MinHex = SelectedTiles[0];
	for (const FHex& Hex : SelectedTiles)
	{
		MinHex.Q = FMath::Min(MinHex.Q, Hex.Q);
		MinHex.R = FMath::Min(MinHex.R, Hex.R);
	}
	
	// Debug: Log what we're checking
	UE_LOG(LogTemp, Warning, TEXT("Checking %d arms and %d tiles in simulation"), Sim->Arms.Num(), Sim->TileMap.Num());
	
	// Convert selected tiles to blueprint tiles
	for (const FHex& Hex : SelectedTiles)
	{
		UE_LOG(LogTemp, Warning, TEXT("Checking hex (%d,%d) for content"), Hex.Q, Hex.R);
		
		// Calculate local hex coordinates relative to blueprint origin
		FHex LocalHex(Hex.Q - MinHex.Q, Hex.R - MinHex.R);
		
		// Check if there's an arm at this position first
		bool bFoundArm = false;
		for (int32 ArmIndex = 0; ArmIndex < Sim->Arms.Num(); ArmIndex++)
		{
			const FArmState& ArmState = Sim->Arms[ArmIndex];
			if (ArmState.BaseHex.Q == Hex.Q && ArmState.BaseHex.R == Hex.R)
			{
				// Found the arm - capture its program and HOME facing/reach (reset position)
				// Get the starting values from the arm actor
				uint8 StartingRot = (uint8)ArmState.HomeFacing; // Default to HomeFacing
				int32 StartingRch = ArmState.HomeArmLen;        // Default to HomeArmLen

				// Find the arm actor to get its original program and starting values
				ABasicArm* ArmActor = nullptr;
				if (UWorld* World = GetWorld())
				{
					for (TActorIterator<ABasicArm> It(World); It; ++It)
					{
						ABasicArm* TestArm = *It;
						if (TestArm && TestArm->SimArmIndex == ArmIndex)
						{
							ArmActor = TestArm;
							break;
						}
					}
				}

				// Use the original program from the arm actor, not the current simulation tape
				// The simulation tape may contain temporary commands generated by Reset commands
				TArray<FArmInstr> CapturedProgram;
				if (ArmActor)
				{
					CapturedProgram = ArmActor->Program; // Use original program, not ArmState.Tape
					StartingRot = ArmActor->StartingRotation;
					StartingRch = ArmActor->StartingReach;
				}
				else
				{
					// Fallback: convert from simulation tape if arm actor not found
					CapturedProgram = ConvertOpsToProgram(ArmState.Tape);
					UE_LOG(LogTemp, Warning, TEXT("Blueprint: Arm actor not found, using simulation tape (may include Reset-generated commands)"));
				}

				FBlueprintTile BlueprintTile(LocalHex, (uint8)ArmState.HomeFacing, ArmState.HomeArmLen, StartingRot, StartingRch, CapturedProgram, true); // Assume looping for now
				BlueprintTiles.Add(BlueprintTile);
				

				bFoundArm = true;
				break;
			}
		}
		
		// If no arm found, check for regular tiles
		if (!bFoundArm)
		{
			FTile* Tile = Sim->TileMap.Find(Hex);
			if (Tile)
			{
				// Get tile direction from the tile's Arg field
				uint8 TileDirection = Tile->Arg;
				
				// Regular tile (not an arm)
				BlueprintTiles.Add(FBlueprintTile(LocalHex, Tile->Kind, TileDirection));
				
				// Debug log for ClusterOutput tiles
				if (Tile->Kind == ETileKind::ClusterOutput)
				{
					UE_LOG(LogTemp, Warning, TEXT("CreateBlueprint: Captured ClusterOutput at (%d,%d) with TileDirection=%d (from Tile->Arg)"), 
						Hex.Q, Hex.R, TileDirection);
				}
			}
			else
			{
				UE_LOG(LogTemp, Warning, TEXT("No tile or arm found at (%d,%d)"), Hex.Q, Hex.R);
			}
		}
	}
	
	UE_LOG(LogTemp, Log, TEXT("Created blueprint with %d tiles"), BlueprintTiles.Num());
	
	// Enable blueprint placement mode after creating a blueprint
	if (BlueprintTiles.Num() > 0)
	{
		bBlueprintPlacementMode = true;
		BlueprintRotation = 0;
		BlueprintPlacementOffset = FHex(0, 0);
		
		// Disable HexPlacementComponent preview when in blueprint mode
		if (Placer)
		{
			Placer->SetBlueprintMode(true);
		}
	}
}

void AHexTopDownPawn::ClearSelection()
{
	SelectedTiles.Empty();
	BlueprintTiles.Empty();
	bBlueprintSelectionActive = false;
	ClearBlueprintPreviewComponents();
}

void AHexTopDownPawn::CancelBlueprintPlacement()
{
	UE_LOG(LogTemp, Warning, TEXT("Canceling blueprint placement"));
	
	// Clear preview components before exiting
	ClearBlueprintPreviewComponents();
	
	// Exit blueprint placement mode and return to normal kind placement
	bBlueprintPlacementMode = false;
	bBlueprintMode = false;
	ClearSelection();
	
	// Re-enable HexPlacementComponent preview when exiting blueprint mode
	if (Placer)
	{
		Placer->SetBlueprintMode(false);
	}
	
	// Clear any HUD selection box that might still be active
	APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	if (PC)
	{
		AAutomationGameHUD* GameHUD = Cast<AAutomationGameHUD>(PC->GetHUD());
		if (GameHUD)
		{
			GameHUD->EndSelectionBox();
		}
	}
}

void AHexTopDownPawn::PlaceBlueprint()
{
	if (BlueprintTiles.Num() == 0) return;
	
	// Find the simulation world
	AOrbSimDriver* Driver = AOrbSimDriver::Find(GetWorld());
	if (!Driver) return;
	
	USimWorldHex* Sim = Driver->GetSim();
	if (!Sim) return;
	
	// Get the current mouse position in world coordinates
	FVector2D MousePosition;
	if (!GetWorld()->GetFirstPlayerController() || 
		!GetWorld()->GetFirstPlayerController()->GetMousePosition(MousePosition.X, MousePosition.Y))
	{
		return;
	}
	
	// Get tile size and grid info from driver
	float TileSize = Driver ? Driver->GetTileSize() : 100.f;
	EHexOrientation Orientation = Driver ? Driver->GetGridOrientation() : EHexOrientation::PointyTop;
	float GridPlaneZ = Driver ? Driver->GetGridPlaneZ(this) : 0.f;
	
	// Convert mouse position to world coordinates by projecting ray to the grid plane
	FVector WorldPosition, WorldDirection;
	GetWorld()->GetFirstPlayerController()->DeprojectScreenPositionToWorld(
		MousePosition.X, MousePosition.Y, WorldPosition, WorldDirection);
	
	// Project the ray down to the grid plane (Z = GridPlaneZ)
	if (FMath::Abs(WorldDirection.Z) > KINDA_SMALL_NUMBER)
	{
		float T = (GridPlaneZ - WorldPosition.Z) / WorldDirection.Z;
		WorldPosition = WorldPosition + WorldDirection * T;
	}
	
	// Convert world coordinates to hex coordinates
	FHexAxial TargetHexAxial = HexCoord::XYToAxialRound(FVector2D(WorldPosition.X, WorldPosition.Y), TileSize, Orientation);
	FHex TargetHex(TargetHexAxial.Q, TargetHexAxial.R);
	
	// Calculate the center of the blueprint to center it at the mouse position
	FHex BlueprintCenter(0, 0);
	if (BlueprintTiles.Num() > 0)
	{
		// Find the bounds of the blueprint
		FHex MinHex(BlueprintTiles[0].LocalHex.Q, BlueprintTiles[0].LocalHex.R);
		FHex MaxHex(BlueprintTiles[0].LocalHex.Q, BlueprintTiles[0].LocalHex.R);
		
		for (const FBlueprintTile& Tile : BlueprintTiles)
		{
			MinHex.Q = FMath::Min(MinHex.Q, Tile.LocalHex.Q);
			MinHex.R = FMath::Min(MinHex.R, Tile.LocalHex.R);
			MaxHex.Q = FMath::Max(MaxHex.Q, Tile.LocalHex.Q);
			MaxHex.R = FMath::Max(MaxHex.R, Tile.LocalHex.R);
		}
		
		// Calculate center as the average of min and max
		BlueprintCenter.Q = (MinHex.Q + MaxHex.Q) / 2;
		BlueprintCenter.R = (MinHex.R + MaxHex.R) / 2;
	}
	
	// Apply blueprint rotation to the placement offset
	FHex RotatedOffset = BlueprintPlacementOffset;
	if (BlueprintRotation > 0)
	{
		// Rotate the offset by the blueprint rotation
		for (uint8 i = 0; i < BlueprintRotation; i++)
		{
			RotatedOffset = HexUtil::Neighbor(RotatedOffset, EHexDir::D1); // 60 degree rotation
		}
	}
	
	// Place each tile in the blueprint
	for (const FBlueprintTile& BlueprintTile : BlueprintTiles)
	{
		// Calculate the offset of this tile from the blueprint center
		FHex OffsetFromCenter = FHex(
			BlueprintTile.LocalHex.Q - BlueprintCenter.Q,
			BlueprintTile.LocalHex.R - BlueprintCenter.R
		);
		
		// Rotate the offset by BlueprintRotation * 60 degrees
		// Use proper hexagonal rotation formula
		FHex RotatedTileOffset = OffsetFromCenter;
		if (BlueprintRotation > 0)
		{
			// Hexagonal rotation formula for 60-degree increments
			// Each rotation step rotates the hex coordinates by 60 degrees clockwise
			for (uint8 j = 0; j < BlueprintRotation; j++)
			{
				int32 NewQ = RotatedTileOffset.Q + RotatedTileOffset.R;
				int32 NewR = -RotatedTileOffset.Q;
				RotatedTileOffset = FHex(NewQ, NewR);
			}
		}
		
		// Calculate the final hex position
		// Apply the rotated offset to the mouse position (which becomes the new center)
		FHex FinalHex = TargetHex;
		FinalHex.Q += RotatedTileOffset.Q;
		FinalHex.R += RotatedTileOffset.R;
		
		if (BlueprintTile.bIsArm)
		{
			// Use GameManager to spawn arm (same as HexPlacementComponent does)
			// Find GameManager and let it handle both spawning and simulation
			AGameManager* GameManager = nullptr;
			if (UWorld* World = GetWorld())
			{
				for (TActorIterator<AGameManager> It(World); It; ++It)
				{
					GameManager = *It;
					break;
				}
			}
			
			if (GameManager)
			{
				// Apply blueprint rotation to both the arm facing and starting rotation
				uint8 RotatedArmFacing = (BlueprintTile.ArmFacing + BlueprintRotation) % 6;
				uint8 RotatedStartingRotation = (BlueprintTile.StartingRotation + BlueprintRotation) % 6;
				
				UE_LOG(LogTemp, Warning, TEXT("Blueprint: Placing arm at (%d,%d) with facing=%d startRot=%d (rotated from facing=%d startRot=%d)"), 
					FinalHex.Q, FinalHex.R, RotatedArmFacing, RotatedStartingRotation, BlueprintTile.ArmFacing, BlueprintTile.StartingRotation);
				
				// Use GameManager to spawn the arm with the rotated arm facing (home position)
				ABasicArm* SpawnedArm = GameManager->SpawnBasicArmAt(
					FinalHex, RotatedArmFacing, BlueprintTile.ArmProgram, BlueprintTile.bArmLoopProgram);
				
				if (SpawnedArm)
				{
					// Set the captured reach distance (reset position)
					SpawnedArm->ReachTiles = BlueprintTile.StartingReach;
					SpawnedArm->StartingReach = BlueprintTile.StartingReach;
					
					// Set the rotation values
					SpawnedArm->FacingDir = RotatedStartingRotation;
					SpawnedArm->StartingRotation = RotatedStartingRotation;

					// Also update the simulation to match the captured reset reach and rotation
					if (SpawnedArm->SimArmIndex >= 0 && SpawnedArm->SimArmIndex < Sim->Arms.Num())
					{
						FArmState& ArmState = Sim->Arms[SpawnedArm->SimArmIndex];
						ArmState.Facing = (EHexDir)RotatedStartingRotation;  // Current facing is the starting rotation
						ArmState.HomeFacing = (EHexDir)RotatedArmFacing;     // Home facing is where it resets to
						ArmState.ArmLen = BlueprintTile.StartingReach;
						ArmState.HomeArmLen = BlueprintTile.StartingReach;

						UE_LOG(LogTemp, Warning, TEXT("Blueprint: Sim state for spawned arm - HomeFacing=%d, StartingRotation=%d, Blueprint rotation=%d"), 
							RotatedArmFacing, RotatedStartingRotation, BlueprintRotation);
					}
					
					UE_LOG(LogTemp, Warning, TEXT("Blueprint: Spawned arm at (%d,%d) - FacingDir=%d StartFacingDir=%d SimIndex=%d"), 
						FinalHex.Q, FinalHex.R, SpawnedArm->FacingDir, SpawnedArm->GetStartFacingDir(), SpawnedArm->SimArmIndex);
				}
				else
				{
					UE_LOG(LogTemp, Warning, TEXT("GameManager failed to spawn arm at (%d,%d)"), FinalHex.Q, FinalHex.R);
				}
			}
			else
			{
				UE_LOG(LogTemp, Warning, TEXT("No GameManager found - cannot spawn arm at (%d,%d)"), FinalHex.Q, FinalHex.R);
			}
		}
		else
		{
			// Create a regular tile
			FTile NewTile;
			NewTile.Kind = BlueprintTile.TileType;
			
			// Apply blueprint rotation to tile direction for directional tiles
			uint8 RotatedTileDirection = BlueprintTile.TileDirection;
			switch (BlueprintTile.TileType)
			{
			case ETileKind::Belt:
			case ETileKind::Bond:
			case ETileKind::Unbond:
				RotatedTileDirection = (BlueprintTile.TileDirection + BlueprintRotation) % 6;
				break;
			default:
				// Non-directional tiles don't need direction rotation
				break;
			}
			
			// For ClusterOutput tiles in blueprints, check if TileDirection is a template ID
			// If a template with that ID exists, this is a Kind8 multi-tile cluster placement
			if (BlueprintTile.TileType == ETileKind::ClusterOutput)
			{
				const USimWorldHex::FClusterTemplate* Template = Sim->GetClusterTemplates().Find(BlueprintTile.TileDirection);
				// if (Template)
				// {
				// 	// This is a Kind8 placement - find the orb type from the template
				// 	// Find matching orb in template by dQ/dR from LocalHex
				// 	uint8 OrbTypeForThisTile = 0;
				// 	FHex LocalPos = BlueprintTile.LocalHex;
				// 	for (const FOrbData& Orb : Template->Orbs)
				// 	{
				// 		if (Orb.dQ == LocalPos.Q && Orb.dR == LocalPos.R)
				// 		{
				// 			OrbTypeForThisTile = Orb.Type;
				// 			break;
				// 		}
				// 	}
				// 	NewTile.Arg = OrbTypeForThisTile;
				// }
				// else
				// {
					// This is a regular copy/paste - TileDirection stores the orb type directly
					NewTile.Arg = RotatedTileDirection;
					UE_LOG(LogTemp, Warning, TEXT("PlaceBlueprint: ClusterOutput copy/paste - TileDirection=%d, NewTile.Arg=%d"), 
						BlueprintTile.TileDirection, NewTile.Arg);
				// }
			}
			else
			{
				// For other tile types, TileDirection is the direction/type
				NewTile.Arg = RotatedTileDirection;
			}
			
			// Place the tile in the simulation
			Sim->PlaceTile(FinalHex, NewTile);
			
			UE_LOG(LogTemp, Log, TEXT("Placed tile at (%d,%d) type %d direction %d (original %d + blueprint rotation %d)"), 
				FinalHex.Q, FinalHex.R, (int32)BlueprintTile.TileType, NewTile.Arg, BlueprintTile.TileDirection, BlueprintRotation);
		}
	}
	
	// Force GameManager to rebuild grid to reflect tile changes
	if (UWorld* GameWorld = GetWorld())
	{
		for (TActorIterator<AGameManager> It(GameWorld); It; ++It)
		{
			AGameManager* GameManager = *It;
			if (GameManager)
			{
				GameManager->Rebuild();
				break;
			}
		}
	}
	
	UE_LOG(LogTemp, Log, TEXT("Placed blueprint with %d tiles"), BlueprintTiles.Num());
	
	// Exit blueprint placement mode after successful placement
	bBlueprintPlacementMode = false;
	bBlueprintMode = false;
	ClearSelection();
	
	// Re-enable HexPlacementComponent preview
	if (Placer)
	{
		Placer->SetBlueprintMode(false);
	}
}

void AHexTopDownPawn::RotateBlueprintCW()
{
	if (BlueprintTiles.Num() == 0) return;
	
	BlueprintRotation = (BlueprintRotation + 1) % 6;
	UE_LOG(LogTemp, Log, TEXT("Blueprint rotated CW, new rotation: %d"), BlueprintRotation);
}

void AHexTopDownPawn::RotateBlueprintCCW()
{
	if (BlueprintTiles.Num() == 0) return;
	
	BlueprintRotation = (BlueprintRotation + 5) % 6; // +5 is equivalent to -1 mod 6
	UE_LOG(LogTemp, Log, TEXT("Blueprint rotated CCW, new rotation: %d"), BlueprintRotation);
}

void AHexTopDownPawn::UpdateBlueprintPreview()
{
	// TODO: Implement blueprint preview visualization
	// This would show a ghost/preview of where the blueprint would be placed
}

TArray<FArmInstr> AHexTopDownPawn::ConvertOpsToProgram(const TArray<FOp>& Ops)
{
	TArray<FArmInstr> Out;
	for (const FOp& Op : Ops)
	{
		FArmInstr Instr;
		switch (Op.Cmd)
		{
		case ECmd::Wait:     
			Instr.Cmd = EArmCmd::Wait; 
			Instr.Arg = FMath::Max(1, Op.Arg); 
			break;
		case ECmd::Grab:     
			Instr.Cmd = EArmCmd::Grab; 
			Instr.Arg = 0; 
			break;
		case ECmd::Drop:     
			Instr.Cmd = EArmCmd::Drop; 
			Instr.Arg = 0; 
			break;
		case ECmd::RotateR:  
			Instr.Cmd = EArmCmd::RotateCW; 
			Instr.Arg = FMath::Max(1, Op.Arg); 
			break;
		case ECmd::RotateL:  
			Instr.Cmd = EArmCmd::RotateCCW; 
			Instr.Arg = FMath::Max(1, Op.Arg); 
			break;
		case ECmd::Extend:   
			Instr.Cmd = EArmCmd::Extend; 
			Instr.Arg = FMath::Max(1, Op.Arg); 
			break;
		case ECmd::Retract:  
			Instr.Cmd = EArmCmd::Retract; 
			Instr.Arg = FMath::Max(1, Op.Arg); 
			break;
		case ECmd::Reset:    
			Instr.Cmd = EArmCmd::Reset; 
			Instr.Arg = 0; 
			break;
		case ECmd::RotateHeldL:  
			Instr.Cmd = EArmCmd::RotateHeldCCW; 
			Instr.Arg = FMath::Max(1, Op.Arg); 
			break;
		case ECmd::RotateHeldR:  
			Instr.Cmd = EArmCmd::RotateHeldCW; 
			Instr.Arg = FMath::Max(1, Op.Arg); 
			break;
		case ECmd::WaitForBond:  
			Instr.Cmd = EArmCmd::WaitForBond; 
			Instr.Arg = 0; 
			break;
		default:             
			Instr.Cmd = EArmCmd::Wait; 
			Instr.Arg = 1; 
			break;
		}
		Out.Add(Instr);
	}
	return Out;
}

void AHexTopDownPawn::DeletionModePressed()
{
	UE_LOG(LogTemp, Warning, TEXT("DeletionMode pressed - entering deletion selection mode"));
	bDeletionMode = true;
}

void AHexTopDownPawn::DeletionModeReleased()
{
	UE_LOG(LogTemp, Warning, TEXT("DeletionMode released"));
	if (bDeletionSelectionActive)
	{
		// End deletion selection and delete selected tiles
		EndDeletionSelection();
	}
	else
	{
		// Exit deletion mode
		bDeletionMode = false;
	}
}

void AHexTopDownPawn::DeleteTileAndOrbs(const FHex& Hex)
{
	// Find the simulation world
	AOrbSimDriver* Driver = AOrbSimDriver::Find(GetWorld());
	if (!Driver) return;
	
	USimWorldHex* Sim = Driver->GetSim();
	if (!Sim) return;
	
	UE_LOG(LogTemp, Log, TEXT("Deleting tile and orbs at (%d,%d)"), Hex.Q, Hex.R);
	
	// 1. Remove any orbs/items at this hex
	FItem RemovedItem;
	if (Sim->TryTakeItem(Hex, RemovedItem))
	{
		UE_LOG(LogTemp, Log, TEXT("Removed orb/item ID %d at (%d,%d)"), RemovedItem.Id, Hex.Q, Hex.R);
	}
	
	// 2. Remove any arms at this hex (check both simulation and world actors)
	// First remove from simulation
	int32 RemovedArmIndex = -1;
	for (int32 ArmIndex = Sim->Arms.Num() - 1; ArmIndex >= 0; ArmIndex--)
	{
		const FArmState& ArmState = Sim->Arms[ArmIndex];
		if (ArmState.BaseHex.Q == Hex.Q && ArmState.BaseHex.R == Hex.R)
		{
			UE_LOG(LogTemp, Warning, TEXT("HexTopDownPawn::DeleteTileAndOrbs - Removing arm from simulation at (%d,%d) SimIndex=%d (total arms before: %d)"), 
				Hex.Q, Hex.R, ArmIndex, Sim->Arms.Num());
			RemovedArmIndex = ArmIndex;
			Sim->Arms.RemoveAt(ArmIndex);
			UE_LOG(LogTemp, Warning, TEXT("HexTopDownPawn::DeleteTileAndOrbs - After removal, total arms: %d"), Sim->Arms.Num());
			break;
		}
	}
	
	// CRITICAL: Update SimArmIndex for all arms that were shifted down by the removal
	if (RemovedArmIndex >= 0)
	{
		if (UWorld* World = GetWorld())
		{
			for (TActorIterator<ABasicArm> It(World); It; ++It)
			{
				ABasicArm* Arm = *It;
				if (Arm && Arm->SimArmIndex > RemovedArmIndex)
				{
					Arm->SimArmIndex--; // Shift down by 1
					UE_LOG(LogTemp, Warning, TEXT("HexTopDownPawn::DeleteTileAndOrbs - Updated arm at (%d,%d) SimIndex: %d -> %d"), 
						Arm->Q, Arm->R, Arm->SimArmIndex + 1, Arm->SimArmIndex);
				}
			}
		}
	}
	
	// Remove runtime spawned arm actors at this hex
	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<ABasicArm> It(World); It; ++It)
		{
			ABasicArm* Arm = *It;
			if (Arm && Arm->Q == Hex.Q && Arm->R == Hex.R)
			{
				UE_LOG(LogTemp, Log, TEXT("Destroying arm actor at (%d,%d)"), Hex.Q, Hex.R);
				Arm->Destroy();
				break;
			}
		}
	}
	
	// 3. Remove the tile itself
	// First, check if this is a ClusterOutput tile and clean up its partnerships
	if (FTile* Tile = Sim->TileMap.Find(Hex))
	{
		if (Tile->Kind == ETileKind::ClusterOutput)
		{
			// Find and remove all partnerships involving this tile
			TArray<FHex> PartnersToRemove;
			for (const auto& PartnerKV : Sim->GetIOBondPartners())
			{
				if (PartnerKV.Key == Hex || PartnerKV.Value == Hex)
				{
					PartnersToRemove.Add(PartnerKV.Key);
				}
			}
			
			TMap<FHex, FHex>& IOBondPartners = Sim->GetIOBondPartnersMutable();
			for (const FHex& PartnerHex : PartnersToRemove)
			{
				IOBondPartners.Remove(PartnerHex);
				UE_LOG(LogTemp, Log, TEXT("Removed IOBondPartners entry for (%d,%d)"), PartnerHex.Q, PartnerHex.R);
			}
			
			// Remove any merged tile info entries involving this hex
			TArray<FHex> MergedToRemove;
			for (const auto& MergedKV : Sim->GetMergedClusterOutputs())
			{
				if (MergedKV.Value.Hex1 == Hex || MergedKV.Value.Hex2 == Hex)
				{
					MergedToRemove.Add(MergedKV.Key);
				}
			}
			
			TMap<FHex, FMergedTileInfo>& MergedClusterOutputs = Sim->GetMergedClusterOutputsMutable();
			for (const FHex& MergedHex : MergedToRemove)
			{
				MergedClusterOutputs.Remove(MergedHex);
				UE_LOG(LogTemp, Log, TEXT("Removed MergedClusterOutputs entry for (%d,%d)"), MergedHex.Q, MergedHex.R);
			}
			
			UE_LOG(LogTemp, Log, TEXT("Cleaned up ClusterOutput partnerships for (%d,%d)"), Hex.Q, Hex.R);
		}
	}
	
	if (Sim->TileMap.Remove(Hex) > 0)
	{
		UE_LOG(LogTemp, Log, TEXT("Removed tile at (%d,%d)"), Hex.Q, Hex.R);
		Sim->TileRevision++; // Increment revision to trigger visual updates
	}
}

void AHexTopDownPawn::DeleteSelectedTilesAndOrbs()
{
	UE_LOG(LogTemp, Log, TEXT("Deleting %d selected tiles and their orbs"), SelectedTiles.Num());
	
	for (const FHex& Hex : SelectedTiles)
	{
		DeleteTileAndOrbs(Hex);
	}
}

void AHexTopDownPawn::RemoveTileAndOrbsAtCursor()
{
	if (!Placer) return;
	
	// Use the placement component's hover hex detection
	if (Placer->IsHoverValid())
	{
		FHex CursorHex = Placer->GetHoverHex();
		UE_LOG(LogTemp, Log, TEXT("Removing tile and orbs at cursor position (%d,%d)"), CursorHex.Q, CursorHex.R);
		DeleteTileAndOrbs(CursorHex);
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("No valid hex under cursor for removal"));
	}
}

void AHexTopDownPawn::StartDeletionSelection()
{
	UE_LOG(LogTemp, Warning, TEXT("StartDeletionSelection called"));
	
	if (!bDeletionMode) return;
	
	APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	if (!PC) return;
	
	FVector2D MousePosition;
	if (PC->GetMousePosition(MousePosition.X, MousePosition.Y))
	{
		SelectionStart = MousePosition;
		SelectionEnd = MousePosition;
		bDeletionSelectionActive = true;
		
		// Start HUD selection box (reuse the same box but with different purpose)
		AAutomationGameHUD* GameHUD = Cast<AAutomationGameHUD>(PC->GetHUD());
		if (GameHUD)
		{
			GameHUD->StartSelectionBox(MousePosition);
		}
		
		UE_LOG(LogTemp, Warning, TEXT("Deletion selection started at (%f, %f)"), MousePosition.X, MousePosition.Y);
	}
	else
	{
		// UE_LOG(LogTemp, Warning, TEXT("Failed to get mouse position for deletion selection"));
	}
}

void AHexTopDownPawn::EndDeletionSelection()
{
	UE_LOG(LogTemp, Warning, TEXT("EndDeletionSelection called"));
	
	if (!bDeletionMode || !bDeletionSelectionActive) return;
	
	bDeletionSelectionActive = false;
	bDeletionMode = false; // Exit deletion mode after selection
	
	// End HUD selection box
	APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	if (PC)
	{
		AAutomationGameHUD* GameHUD = Cast<AAutomationGameHUD>(PC->GetHUD());
		if (GameHUD)
		{
			GameHUD->EndSelectionBox();
		}
	}
	
	// Delete selected tiles immediately
	if (SelectedTiles.Num() > 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("Deleting %d selected tiles"), SelectedTiles.Num());
		DeleteSelectedTilesAndOrbs();
		ClearSelection();
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("No tiles selected for deletion"));
	}
}

void AHexTopDownPawn::UpdateDeletionSelection()
{
	if (!bDeletionSelectionActive || !GetWorld()) return;
	
	// Use the same logic as UpdateSelection but for deletion
	FVector2D MousePosition;
	if (GetWorld()->GetFirstPlayerController() && 
		GetWorld()->GetFirstPlayerController()->GetMousePosition(MousePosition.X, MousePosition.Y))
	{
		SelectionEnd = MousePosition;
		
		// Update selected tiles based on current selection box
		SelectedTiles.Empty();
		
		// Get tile size and grid info from driver
		AOrbSimDriver* Driver = AOrbSimDriver::Find(GetWorld());
		float TileSize = Driver ? Driver->GetTileSize() : 100.f;
		EHexOrientation Orientation = Driver ? Driver->GetGridOrientation() : EHexOrientation::PointyTop;
		float GridPlaneZ = Driver ? Driver->GetGridPlaneZ(this) : 0.f;
		
		// Convert screen coordinates to world coordinates by projecting rays to the grid plane
		FVector WorldStart, WorldEnd;
		FVector WorldStartDir, WorldEndDir;
		
		// Deproject both screen points to get rays
		GetWorld()->GetFirstPlayerController()->DeprojectScreenPositionToWorld(
			SelectionStart.X, SelectionStart.Y, WorldStart, WorldStartDir);
		GetWorld()->GetFirstPlayerController()->DeprojectScreenPositionToWorld(
			SelectionEnd.X, SelectionEnd.Y, WorldEnd, WorldEndDir);
		
		// Project the rays down to the grid plane (Z = GridPlaneZ)
		if (FMath::Abs(WorldStartDir.Z) > KINDA_SMALL_NUMBER)
		{
			float T = (GridPlaneZ - WorldStart.Z) / WorldStartDir.Z;
			WorldStart = WorldStart + WorldStartDir * T;
		}
		if (FMath::Abs(WorldEndDir.Z) > KINDA_SMALL_NUMBER)
		{
			float T = (GridPlaneZ - WorldEnd.Z) / WorldEndDir.Z;
			WorldEnd = WorldEnd + WorldEndDir * T;
		}
		
		// Convert world coordinates to hex coordinates
		FHexAxial HexStartAxial = HexCoord::XYToAxialRound(FVector2D(WorldStart.X, WorldStart.Y), TileSize, Orientation);
		FHexAxial HexEndAxial = HexCoord::XYToAxialRound(FVector2D(WorldEnd.X, WorldEnd.Y), TileSize, Orientation);
		FHex HexStart(HexStartAxial.Q, HexStartAxial.R);
		FHex HexEnd(HexEndAxial.Q, HexEndAxial.R);
		
		// Use improved hexagonal selection algorithm with actual world positions
		GetHexesInSelectionArea(HexStart, HexEnd, WorldStart, WorldEnd, SelectedTiles);
		
		UE_LOG(LogTemp, Warning, TEXT("Deletion selection: %d tiles selected"), SelectedTiles.Num());
	}
}

void AHexTopDownPawn::GetHexesInSelectionArea(const FHex& HexStart, const FHex& HexEnd, const FVector& WorldStart, const FVector& WorldEnd, TArray<FHex>& OutHexes)
{
	OutHexes.Empty();
	
	// Get the driver to access tile size and orientation
	AOrbSimDriver* Driver = AOrbSimDriver::Find(GetWorld());
	if (!Driver) return;
	
	float TileSize = Driver->GetTileSize();
	EHexOrientation Orientation = Driver->GetGridOrientation();
	
	// Use the actual world positions from the camera projection
	FVector2D StartWorld2D(WorldStart.X, WorldStart.Y);
	FVector2D EndWorld2D(WorldEnd.X, WorldEnd.Y);
	
	// Create a proper selection rectangle in world space using the actual projected positions
	FVector2D MinWorld(FMath::Min(StartWorld2D.X, EndWorld2D.X), FMath::Min(StartWorld2D.Y, EndWorld2D.Y));
	FVector2D MaxWorld(FMath::Max(StartWorld2D.X, EndWorld2D.X), FMath::Max(StartWorld2D.Y, EndWorld2D.Y));
	
	// Calculate a much larger search area to ensure we don't miss any tiles
	// Use the world bounds to determine a reasonable hex search area
	int32 SearchRadius = FMath::CeilToInt(FMath::Max(MaxWorld.X - MinWorld.X, MaxWorld.Y - MinWorld.Y) / TileSize) + 5;
	
	// Calculate center of selection area in hex coordinates
	int32 CenterQ = (HexStart.Q + HexEnd.Q) / 2;
	int32 CenterR = (HexStart.R + HexEnd.R) / 2;
	
	// Search in a hexagonal pattern around the center
	int32 MinQ = CenterQ - SearchRadius;
	int32 MaxQ = CenterQ + SearchRadius;
	int32 MinR = CenterR - SearchRadius;
	int32 MaxR = CenterR + SearchRadius;
	
	UE_LOG(LogTemp, Warning, TEXT("GetHexesInSelectionArea: World bounds (%.1f,%.1f) to (%.1f,%.1f), Search radius %d, Hex bounds Q(%d-%d) R(%d-%d)"), 
		MinWorld.X, MinWorld.Y, MaxWorld.X, MaxWorld.Y, SearchRadius, MinQ, MaxQ, MinR, MaxR);
	
	int32 TilesChecked = 0;
	int32 TilesAdded = 0;
	
	// Check all hexes in the expanded range
	for (int32 q = MinQ; q <= MaxQ; q++)
	{
		for (int32 r = MinR; r <= MaxR; r++)
		{
			FHex TestHex(q, r);
			TilesChecked++;
			
			// Convert this hex to world position using the same method as the selection
			FVector2D HexWorld = HexCoord::AxialToXY(FHexAxial(q, r), TileSize, Orientation);
			
			// Check if this hex center is within the actual selection rectangle
			bool bInSelection = (HexWorld.X >= MinWorld.X && HexWorld.X <= MaxWorld.X && 
								HexWorld.Y >= MinWorld.Y && HexWorld.Y <= MaxWorld.Y);
			
			if (bInSelection)
			{
				OutHexes.Add(TestHex);
				TilesAdded++;
				UE_LOG(LogTemp, Log, TEXT("  Added hex (%d,%d) at world (%.1f,%.1f)"), q, r, HexWorld.X, HexWorld.Y);
			}
		}
	}
	
	UE_LOG(LogTemp, Warning, TEXT("GetHexesInSelectionArea: Checked %d hexes, found %d in selection area"), TilesChecked, TilesAdded);
}

void AHexTopDownPawn::EnsureBlueprintPreviewComponents()
{
	// Create preview components for each blueprint tile if they don't exist
	while (BlueprintPreviewComponents.Num() < BlueprintTiles.Num())
	{
		UStaticMeshComponent* PreviewComp = NewObject<UStaticMeshComponent>(this);
		PreviewComp->SetMobility(EComponentMobility::Movable);
		PreviewComp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		PreviewComp->SetVisibility(false);
		PreviewComp->AttachToComponent(RootComponent, FAttachmentTransformRules::KeepRelativeTransform);
		PreviewComp->RegisterComponent();
		BlueprintPreviewComponents.Add(PreviewComp);
	}
	
	// Remove excess components
	while (BlueprintPreviewComponents.Num() > BlueprintTiles.Num())
	{
		UStaticMeshComponent* PreviewComp = BlueprintPreviewComponents.Pop();
		if (PreviewComp)
		{
			PreviewComp->DestroyComponent();
		}
	}
}

void AHexTopDownPawn::UpdateBlueprintPreviewMeshes()
{
	if (!bBlueprintPlacementMode || BlueprintTiles.Num() == 0 || !GetWorld())
	{
		ClearBlueprintPreviewComponents();
		return;
	}
	
	EnsureBlueprintPreviewComponents();
	
	// Get tile size and grid info from driver
	AOrbSimDriver* Driver = AOrbSimDriver::Find(GetWorld());
	if (!Driver) return;
	
	float TileSize = Driver->GetTileSize();
	EHexOrientation Orientation = Driver->GetGridOrientation();
	float GridPlaneZ = Driver->GetGridPlaneZ(this);
	
	// Get mouse position and convert to hex coordinates
	FVector2D MousePosition;
	if (!GetWorld()->GetFirstPlayerController() || 
		!GetWorld()->GetFirstPlayerController()->GetMousePosition(MousePosition.X, MousePosition.Y))
	{
		return;
	}
	
	// Convert mouse position to world coordinates
	FVector WorldPosition, WorldDirection;
	GetWorld()->GetFirstPlayerController()->DeprojectScreenPositionToWorld(
		MousePosition.X, MousePosition.Y, WorldPosition, WorldDirection);
	
	// Project the ray down to the grid plane
	if (FMath::Abs(WorldDirection.Z) > KINDA_SMALL_NUMBER)
	{
		float T = (GridPlaneZ - WorldPosition.Z) / WorldDirection.Z;
		WorldPosition = WorldPosition + WorldDirection * T;
	}
	
	// Convert world coordinates to hex coordinates
	FHexAxial TargetHexAxial = HexCoord::XYToAxialRound(FVector2D(WorldPosition.X, WorldPosition.Y), TileSize, Orientation);
	FHex TargetHex(TargetHexAxial.Q, TargetHexAxial.R);
	
	// Calculate the center of the blueprint
	FHex BlueprintCenter(0, 0);
	if (BlueprintTiles.Num() > 0)
	{
		FHex MinHex(BlueprintTiles[0].LocalHex.Q, BlueprintTiles[0].LocalHex.R);
		FHex MaxHex(BlueprintTiles[0].LocalHex.Q, BlueprintTiles[0].LocalHex.R);
		
		for (const FBlueprintTile& Tile : BlueprintTiles)
		{
			MinHex.Q = FMath::Min(MinHex.Q, Tile.LocalHex.Q);
			MinHex.R = FMath::Min(MinHex.R, Tile.LocalHex.R);
			MaxHex.Q = FMath::Max(MaxHex.Q, Tile.LocalHex.Q);
			MaxHex.R = FMath::Max(MaxHex.R, Tile.LocalHex.R);
		}
		
		BlueprintCenter.Q = (MinHex.Q + MaxHex.Q) / 2;
		BlueprintCenter.R = (MinHex.R + MaxHex.R) / 2;
	}
	
	// Apply blueprint rotation to the placement offset
	FHex RotatedOffset = BlueprintPlacementOffset;
	if (BlueprintRotation > 0)
	{
		for (uint8 i = 0; i < BlueprintRotation; i++)
		{
			RotatedOffset = HexUtil::Neighbor(RotatedOffset, EHexDir::D1); // 60 degree rotation
		}
	}
	
	// Update each preview mesh
	for (int32 i = 0; i < BlueprintTiles.Num() && i < BlueprintPreviewComponents.Num(); i++)
	{
		const FBlueprintTile& BlueprintTile = BlueprintTiles[i];
		UStaticMeshComponent* PreviewComp = BlueprintPreviewComponents[i];
		
		if (!PreviewComp) continue;
		
		// Calculate the offset of this tile from the blueprint center
		FHex OffsetFromCenter = FHex(
			BlueprintTile.LocalHex.Q - BlueprintCenter.Q,
			BlueprintTile.LocalHex.R - BlueprintCenter.R
		);
		
		// Rotate the offset by BlueprintRotation * 60 degrees
		// Use proper hexagonal rotation formula
		FHex RotatedTileOffset = OffsetFromCenter;
		if (BlueprintRotation > 0)
		{
			// Hexagonal rotation formula for 60-degree increments
			// Each rotation step rotates the hex coordinates by 60 degrees clockwise
			for (uint8 j = 0; j < BlueprintRotation; j++)
			{
				int32 NewQ = RotatedTileOffset.Q + RotatedTileOffset.R;
				int32 NewR = -RotatedTileOffset.Q;
				RotatedTileOffset = FHex(NewQ, NewR);
			}
		}
		
		// Calculate the preview hex position
		// Apply the rotated offset to the mouse position (which becomes the new center)
		FHex PreviewHex = TargetHex;
		PreviewHex.Q += RotatedTileOffset.Q;
		PreviewHex.R += RotatedTileOffset.R;
		
		// Convert hex to world position
		FVector PreviewWorldPos;
		if (Driver->IsSphereEnabled())
		{
			PreviewWorldPos = Driver->MapHexToWorld(PreviewHex);
		}
		else
		{
			FVector2D PreviewWorldPos2D = HexCoord::AxialToXY(FHexAxial(PreviewHex.Q, PreviewHex.R), TileSize, Orientation);
			PreviewWorldPos = FVector(PreviewWorldPos2D.X, PreviewWorldPos2D.Y, GridPlaneZ);
		}
		
		// Get the appropriate mesh for this tile type
		UStaticMesh* PreviewMesh = nullptr;
		float PreviewYaw = 0.f;
		
		if (BlueprintTile.bIsArm)
		{
			// For arms, use ArmBasePreviewMesh from HexPlacementComponent
			if (Placer)
			{
				PreviewMesh = Placer->ArmBasePreviewMesh ? Placer->ArmBasePreviewMesh : Placer->GenericTilePreviewMesh;
			}
			// Apply arm rotation using StartingRotation (facing direction)
			PreviewYaw = (BlueprintTile.StartingRotation + BlueprintRotation) * 60.0f;
			if (Placer)
			{
				PreviewYaw = FMath::UnwindDegrees(PreviewYaw + Placer->ArmPreviewFacingFixDeg);
			}
		}
		else
		{
			// Get mesh based on tile type - use actual meshes like single tile placement does
			switch (BlueprintTile.TileType)
			{
			case ETileKind::Input:
				if (Placer)
				{
					PreviewMesh = Placer->GetActualOrbMesh();
				}
				break;
		case ETileKind::Output:
			if (Placer)
			{
				PreviewMesh = Placer->GetActualOutputOrbMesh();
				// For output tiles, handle grid orientation
				if (Orientation == EHexOrientation::FlatTop)
				{
					PreviewYaw = 30.0f;
				}
			}
			break;
		case ETileKind::ClusterOutput:
			if (Placer)
			{
				PreviewMesh = Placer->GetActualOutputOrbMesh();
				// For output tiles, handle grid orientation
				if (Orientation == EHexOrientation::FlatTop)
				{
					PreviewYaw = 30.0f;
				}
			}
			break;
		case ETileKind::Belt:
				if (Placer)
				{
					PreviewMesh = Placer->GetActualBeltMesh();
				}
				// Apply belt direction rotation
				PreviewYaw = (BlueprintTile.TileDirection + BlueprintRotation) * 60.0f;
				break;
			case ETileKind::Bond:
				if (Placer)
				{
					PreviewMesh = Placer->BondPreviewMesh ? Placer->BondPreviewMesh : Placer->GenericTilePreviewMesh;
				}
				// Apply bond direction rotation
				PreviewYaw = (BlueprintTile.TileDirection + BlueprintRotation) * 60.0f;
				break;
			case ETileKind::Unbond:
				if (Placer)
				{
					PreviewMesh = Placer->UnbondPreviewMesh ? Placer->UnbondPreviewMesh : Placer->GenericTilePreviewMesh;
				}
				// Apply unbond direction rotation
				PreviewYaw = (BlueprintTile.TileDirection + BlueprintRotation) * 60.0f;
				break;
			default:
				if (Placer)
				{
					PreviewMesh = Placer->GenericTilePreviewMesh;
				}
				break;
			}
		}
		
		// Set the mesh and position
		if (PreviewMesh)
		{
			PreviewComp->SetStaticMesh(PreviewMesh);
			PreviewComp->SetWorldLocationAndRotation(PreviewWorldPos, FRotator(0.f, PreviewYaw, 0.f));
			PreviewComp->SetVisibility(true);
			
			// Apply preview material based on tile type
			if (BlueprintTile.TileType == ETileKind::ClusterOutput)
			{
				// For ClusterOutput tiles, get the material from the template
				UMaterialInterface* MaterialToApply = nullptr;
				
				// Check if this is a Kind8 generated cluster (TileDirection contains template ID)
				if (Placer && Driver)
				{
					USimWorldHex* Sim = Driver->GetSim();
					if (Sim)
					{
						const USimWorldHex::FClusterTemplate* Template = Sim->GetClusterTemplates().Find(BlueprintTile.TileDirection);
						// if (Template)
						// {
						// 	// Find the orb type for this specific tile position
						// 	FHex LocalPos = BlueprintTile.LocalHex;
						// 	for (const FOrbData& Orb : Template->Orbs)
						// 	{
						// 		if (Orb.dQ == LocalPos.Q && Orb.dR == LocalPos.R)
						// 		{
						// 			// Found the matching orb - get its material
						// 			if (UOrbMaterialManager* MaterialManager = UOrbMaterialManager::FindInWorld(this))
						// 			{
						// 				MaterialToApply = MaterialManager->GetMaterialForOrbType((EOrbType)Orb.Type);
						// 			}
						// 			break;
						// 		}
						// 	}
						// }
						// else
						// {
							// This is a copy/paste ClusterOutput - TileDirection stores the orb type directly
							if (UOrbMaterialManager* MaterialManager = UOrbMaterialManager::FindInWorld(this))
							{
								MaterialToApply = MaterialManager->GetMaterialForOrbType((EOrbType)BlueprintTile.TileDirection);
							}
						}
					// }
				}
				
				// Apply the material with half opacity if found
				if (MaterialToApply)
				{
					UMaterialInstanceDynamic* OutputMID = UMaterialInstanceDynamic::Create(MaterialToApply, this);
					if (OutputMID)
					{
						OutputMID->SetScalarParameterValue(FName("Opacity"), 0.5f);
						PreviewComp->SetMaterial(0, OutputMID);
					}
					else
					{
						PreviewComp->SetMaterial(0, MaterialToApply);
					}
				}
				else if (Placer && Placer->PreviewBaseMaterial)
				{
					// Fallback to generic preview material
					PreviewComp->SetMaterial(0, Placer->PreviewBaseMaterial);
				}
			}
			else if (BlueprintTile.TileType == ETileKind::Output)
			{
				// For Output tiles, get material based on TileDirection (which stores the orb type)
				if (UOrbMaterialManager* MaterialManager = UOrbMaterialManager::FindInWorld(this))
				{
					UMaterialInterface* MaterialToApply = MaterialManager->GetMaterialForOrbType((EOrbType)BlueprintTile.TileDirection);
					if (MaterialToApply)
					{
						UMaterialInstanceDynamic* OutputMID = UMaterialInstanceDynamic::Create(MaterialToApply, this);
						if (OutputMID)
						{
							OutputMID->SetScalarParameterValue(FName("Opacity"), 0.5f);
							PreviewComp->SetMaterial(0, OutputMID);
						}
						else
						{
							PreviewComp->SetMaterial(0, MaterialToApply);
						}
					}
					else if (Placer && Placer->PreviewBaseMaterial)
					{
						PreviewComp->SetMaterial(0, Placer->PreviewBaseMaterial);
					}
				}
				else if (Placer && Placer->PreviewBaseMaterial)
				{
					PreviewComp->SetMaterial(0, Placer->PreviewBaseMaterial);
				}
			}
			else if (Placer && Placer->PreviewBaseMaterial)
			{
				// Apply generic preview material for other tiles
				PreviewComp->SetMaterial(0, Placer->PreviewBaseMaterial);
			}
		}
		else
		{
			PreviewComp->SetVisibility(false);
		}
	}
}

void AHexTopDownPawn::ClearBlueprintPreviewComponents()
{
	for (UStaticMeshComponent* PreviewComp : BlueprintPreviewComponents)
	{
		if (PreviewComp)
		{
			PreviewComp->SetVisibility(false);
		}
	}
}
