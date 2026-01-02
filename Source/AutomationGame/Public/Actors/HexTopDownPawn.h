#pragma once
#include "AutomationGameMinimal.h"
#include "GameFramework/Pawn.h"
#include "Components/HexPlacementComponent.h"
#include "Sim/HexTypes.h"
#include "Sim/Tiles.h"
#include "Sim/ArmTypes.h"
#include "Actors/BasicArm.h"
#include "Blueprints/BlueprintTile.h"
#include "HexTopDownPawn.generated.h"

class USpringArmComponent;
class UCameraComponent;
class UHexPlacementComponent;
class UStaticMeshComponent;

// Forward declaration
struct FBlueprintData;

UCLASS()
class AUTOMATIONGAME_API AHexTopDownPawn : public APawn
{
	GENERATED_BODY()
public:
	AHexTopDownPawn();

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;

	UPROPERTY(EditAnywhere, Category = "Camera") float MoveSpeed = 1500.f;
	UPROPERTY(EditAnywhere, Category = "Camera") float ZoomSpeed = 1500.f;
	UPROPERTY(EditAnywhere, Category = "Camera") float MinArm = 600.f;
	UPROPERTY(EditAnywhere, Category = "Camera") float MaxArm = 6000.f;

	/** Distance from grid center as multiple of Goldberg radius (for orbital camera). */
	UPROPERTY(EditAnywhere, Category = "Camera|Goldberg")
	float OrbitalDistanceMultiplier = 2.5f;

	UPROPERTY(VisibleAnywhere, Category = "Camera") USpringArmComponent* SpringArm;
	UPROPERTY(VisibleAnywhere, Category = "Camera") UCameraComponent* Camera;
	UPROPERTY(VisibleAnywhere, Category = "Place")  UHexPlacementComponent* Placer;

	// Blueprint preview mesh components
	UPROPERTY(VisibleAnywhere, Category = "Blueprint Preview")
	TArray<UStaticMeshComponent*> BlueprintPreviewComponents;

	// Timeline editor widget class
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Timeline Editor")
	TSubclassOf<class UUserWidget> TimelineEditorWidgetClass = nullptr;
	
	// Timeline editor widget instance (read-only)
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Timeline Editor")
	class UArmTimelineEditorWidget* TimelineEditorWidget = nullptr;

	// Blueprint library management (public so widgets can access)
	void SaveSelectionAsBlueprint();
	void SaveBlueprintToLibrary(const TArray<FBlueprintTile>& Blueprint);
	
	UFUNCTION(BlueprintCallable, Category = "Blueprint Library")
	void LoadBlueprintFromLibrary(int32 Index);
	
	UFUNCTION(BlueprintCallable, Category = "Blueprint Library")
	TArray<FBlueprintData> GetSavedBlueprints() const;
	
	UFUNCTION(BlueprintCallable, Category = "Blueprint Library")
	void RemoveBlueprintFromLibrary(int32 Index);
	
	void SetBlueprintForPlacement(const TArray<FBlueprintTile>& Blueprint);

	// Toggle grid visibility
	void ToggleGridVisibility();
	
	// Toggle walker debug visualization
	void ToggleDebugWalkers();

	// Highlight components
	UPROPERTY()
	UStaticMeshComponent* TileHighlightMesh;
	
	UPROPERTY()
	AActor* LastHighlightedActor;

	UPROPERTY()
	class AHexFactoryActor* CachedHexFactory;

	UPROPERTY()
	class AGameManager* CachedGameManager;

	UPROPERTY()
	class AOrbSimDriver* CachedOrbSimDriver;

	void UpdateHoverHighlight(float DeltaSeconds);

private:
	float AxisX = 0.f, AxisY = 0.f, AxisZoom = 0.f;
	bool bTypeSelectionActive = false;  // For cycling orb types (Alt key)
	bool bBlueprintSelectionActive = false;  // For blueprint drag selection
	bool bDeletionSelectionActive = false;  // For deletion drag selection
	bool bBlueprintMode = false;
	bool bBlueprintPlacementMode = false;
	bool bContinuePlacementActive = false;  // NEW: For continuous placement mode
	bool bDeletionMode = false;
	FVector2D SelectionStart;
	FVector2D SelectionEnd;
	TArray<FHex> SelectedTiles;
	TArray<FBlueprintTile> BlueprintTiles;
	FHex BlueprintPlacementOffset;
	uint8 BlueprintRotation = 0;
	
	void MoveX(float V) { AxisX = V; }
	void MoveY(float V) { AxisY = V; }
	void Zoom(float V) { AxisZoom = V; }

	void PlacePressed() { 
		if (bBlueprintPlacementMode && BlueprintTiles.Num() > 0) {
			PlaceBlueprint();
		} else if (bBlueprintMode && !bBlueprintSelectionActive) {
			StartSelection();
		} else if (bDeletionMode && !bDeletionSelectionActive) {
			StartDeletionSelection();
		} else if (Placer) {
			// Check if there's an arm at the target location first
			if (Placer->IsHoverValid()) {
				FHex TargetHex = Placer->GetHoverHex();
				ABasicArm* ArmAtLocation = FindArmAtHex(TargetHex);
				if (ArmAtLocation) {
					// Don't toggle timeline if we are in the middle of a multi-step placement (like TriggerTile owner selection)
					bool bInMultiStepPlacement = false;
					if (Placer->PlaceKind == EPlaceKind::TriggerTile && Placer->TriggerTilePlacementStep > 0) bInMultiStepPlacement = true;
					if (Placer->PlaceKind == EPlaceKind::RequestBeacon && Placer->BeaconPlacementStep > 0) bInMultiStepPlacement = true;

					if (!bInMultiStepPlacement) {
						// Toggle the individual arm's timeline
						ToggleArmTimeline(ArmAtLocation);
						return;
					}
				}
			}
			Placer->HandlePlace(true); 
		}
	}
	void PlaceReleased() {
		if (bBlueprintMode && bBlueprintSelectionActive) {
			EndSelection();
		} else if (bDeletionMode && bDeletionSelectionActive) {
			EndDeletionSelection();
		}
	}
	void RemovePressed() { 
		if (bBlueprintPlacementMode) {
			// Cancel blueprint placement and return to kind placement
			CancelBlueprintPlacement();
		} else {
			// Enhanced remove - delete tile AND spawned orbs at cursor
			RemoveTileAndOrbsAtCursor();
		}
	}
	
	// Selection functions
	void StartSelection();
	void EndSelection();
	void UpdateSelection();
	void StartDeletionSelection();
	void EndDeletionSelection();
	void UpdateDeletionSelection();
	void BlueprintModePressed();
	void BlueprintModeReleased();
	void ToggleBlueprintMode();
	void CreateBlueprint();
	void ClearSelection();
	void CancelBlueprintPlacement();
	
	// Deletion functions
	void DeletionModePressed();
	void DeletionModeReleased();
	void DeleteTileAndOrbs(const FHex& Hex);
	void DeleteSelectedTilesAndOrbs();
	void RemoveTileAndOrbsAtCursor();
	
	// Blueprint placement functions
	void PlaceBlueprint();
	void RotateBlueprintCW();
	void RotateBlueprintCCW();
	void UpdateBlueprintPreview();
	
	// Blueprint preview mesh management
	void EnsureBlueprintPreviewComponents();
	void UpdateBlueprintPreviewMeshes(float DeltaSeconds);
	void ClearBlueprintPreviewComponents();

	// Helper function to convert simulation ops to arm instructions
	TArray<FArmInstr> ConvertOpsToProgram(const TArray<FOp>& Ops);
	
	// Helper function to get hexes within a selection area
	void GetHexesInSelectionArea(const FHex& HexStart, const FHex& HexEnd, const FVector& WorldStart, const FVector& WorldEnd, TArray<FHex>& OutHexes);

	UFUNCTION(BlueprintCallable, Category = "Input")
	void SetInputMode1();
	UFUNCTION(BlueprintCallable, Category = "Input")
	void SetInputMode2();
	UFUNCTION(BlueprintCallable, Category = "Input")
	void SetInputMode3();
	UFUNCTION(BlueprintCallable, Category = "Input")
	void SetInputMode4();
	UFUNCTION(BlueprintCallable, Category = "Input")
	void SetInputMode5();
	UFUNCTION(BlueprintCallable, Category = "Input")
	void SetInputMode6();
	UFUNCTION(BlueprintCallable, Category = "Input")
	void SetInputMode7();
	void SetInputMode8();
	UFUNCTION(BlueprintCallable, Category = "Input")
	void SetInputMode9();
	UFUNCTION(BlueprintCallable, Category = "Input")
	void SetInputMode10(); // TriggerTile
	void RotCW() { 
		if (bBlueprintPlacementMode && BlueprintTiles.Num() > 0) {
			RotateBlueprintCW();
		} else if (Placer) {
			Placer->RotateCW(); 
		}
	}
	void RotCCW() { 
		if (bBlueprintPlacementMode && BlueprintTiles.Num() > 0) {
			RotateBlueprintCCW();
		} else if (Placer) {
			Placer->RotateCCW(); 
		}
	}
	
	void RequestMoveToTile(); // Request nearest walker to move to cursor position
	
	// Type cycling (works for both Input and Output based on current placement mode)
	// For Kind8 blueprint mode with TypeSelectionActive, cycles DifficultyValue
	void CycleInputTypeNext();
	void CycleInputTypePrevious();
	
	// Glyph cycling (Belt, Bond, Unbond)
	void CycleGlyphType() { if (Placer) Placer->CycleGlyphType(); }
	
	// Type selection modifier key (for cycling orb types)
	void SelectionActivePressed();
	void SelectionActiveReleased();
	
	// Continue placement modifier key (for keeping blueprint after placement)
	void ContinuePlacementActivePressed();
	void ContinuePlacementActiveReleased();

	// Simulation control - advance one tick
	void NextTick();
	
	// Simulation control - tick speed adjustment
	void IncreaseTickSpeed();
	void DecreaseTickSpeed();
	
	// Timeline control - toggle all arm timelines
	void ToggleArmTimelines();
	
	// Timeline cursor control
	void NextCommand();
	void PrevCommand();
	
	// Helper functions for arm timeline toggling
	ABasicArm* FindArmAtHex(const FHex& Hex);
	void ToggleArmTimeline(ABasicArm* Arm);
	
	// Handler for pausing after next tick
	UFUNCTION()
	void HandleNextTickStop(float StepSeconds);
	
	// Timeline editor widget for command editing
	UFUNCTION(BlueprintCallable, Category = "Timeline Editor")
	void ShowTimelineEditor(bool bShow);

	// Toggle the editor UI independently of arm timelines
	UFUNCTION(BlueprintCallable, Category = "Timeline Editor")
	void ToggleEditorUI();
	
	// Get the timeline editor widget
	UFUNCTION(BlueprintPure, Category = "Timeline Editor")
	class UArmTimelineEditorWidget* GetTimelineEditorWidget() const { return TimelineEditorWidget; }
	
	// Set active timeline for editing (call when clicking on an arm)
	UFUNCTION(BlueprintCallable, Category = "Timeline Editor")
	void SetActiveTimelineForEditing(class ABasicArm* Arm);
	
	// Update editor visibility based on timeline visibility (call this tick or when timeline visibility changes)
	UFUNCTION(BlueprintCallable, Category = "Timeline Editor")
	void UpdateTimelineEditorVisibility();

	// Flag to pause on next GTM tick
	bool bPauseOnNextTick = false;
	
	// Blueprint library (persistent) - using TArray directly for runtime, converted for save
	TArray<TArray<FBlueprintTile>> SavedBlueprints;
	
	// Save game slot name for blueprints
	static const FString BlueprintLibrarySaveSlotName;

	// Moved here to preserve memory layout for SavedBlueprints (Live Coding safety)
	// Blueprint smoothing
	bool bBlueprintSmoothingInitialized = false;
	UPROPERTY(EditAnywhere, Category = "Blueprint Preview")
	float BlueprintSmoothingSpeed = 25.0f;

	// Smoothing for tile highlight
	FVector HighlightSmoothedLocation;
	FRotator HighlightSmoothedRotation;
	bool bHighlightSmoothingInitialized = false;
	UPROPERTY(EditAnywhere, Category = "Camera")
	float HighlightSmoothingSpeed = 25.0f;
};
