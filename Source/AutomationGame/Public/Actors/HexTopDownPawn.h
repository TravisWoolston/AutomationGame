#pragma once
#include "Actors/BasicArm.h"
#include "AutomationGameMinimal.h"
#include "Blueprints/BlueprintTile.h"
#include "Components/HexPlacementComponent.h"
#include "GameFramework/Pawn.h"

#include "Sim/ArmTypes.h"
#include "Sim/HexTypes.h"
#include "Sim/Tiles.h"

#include "HexTopDownPawn.generated.h"
class USpringArmComponent;
class UCameraComponent;
class UHexPlacementComponent;
class UStaticMeshComponent;

// Forward declaration
struct FBlueprintData;

UCLASS()
class AUTOMATIONGAME_API AHexTopDownPawn : public APawn {
  GENERATED_BODY()
public:
  AHexTopDownPawn();

  virtual void BeginPlay() override;
  virtual void Tick(float DeltaSeconds) override;
  virtual void
  SetupPlayerInputComponent(UInputComponent *PlayerInputComponent) override;

  UPROPERTY(EditAnywhere, Category = "Camera") float MoveSpeed = 1500.f;
  UPROPERTY(EditAnywhere, Category = "Camera") float ZoomSpeed = 1500.f;
  UPROPERTY(EditAnywhere, Category = "Camera") float MinArm = 600.f;
  UPROPERTY(EditAnywhere, Category = "Camera") float MaxArm = 3000.f;

  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
  bool LogSelectedTile = false;

  /** Distance from grid center as multiple of Goldberg radius (for orbital
   * camera). */
  UPROPERTY(EditAnywhere, Category = "Camera|Goldberg")
  float OrbitalDistanceMultiplier = 2.5f;

  UPROPERTY(VisibleAnywhere, Category = "Camera")
  USpringArmComponent *SpringArm;
  UPROPERTY(VisibleAnywhere, Category = "Camera") UCameraComponent *Camera;
  UPROPERTY(VisibleAnywhere, Category = "Place") UHexPlacementComponent *Placer;

  // Blueprint preview mesh components
  UPROPERTY(VisibleAnywhere, Category = "Blueprint Preview")
  TArray<UStaticMeshComponent *> BlueprintPreviewComponents;

  // Components for highlighting orbs/clusters in Orb View
  UPROPERTY()
  TArray<UStaticMeshComponent *> OrbHighlightComponents;

  // Components for highlighting multiple tiles (selection box)
  UPROPERTY()
  TArray<UStaticMeshComponent *> SelectionHighlightComponents;

  // Helper to find the anchor hex of an item/cluster at a given position
  FHex FindItemAnchorAt(const FHex &Hex, FItem &OutItem) const;

  // Timeline editor widget class
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Timeline Editor")
  TSubclassOf<class UUserWidget> TimelineEditorWidgetClass = nullptr;

  // Timeline editor widget instance (read-only)
  UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Timeline Editor")
  class UArmTimelineEditorWidget *TimelineEditorWidget = nullptr;

  // Blueprint library management (public so widgets can access)
  void SaveSelectionAsBlueprint();
  void SaveBlueprintToLibrary(const TArray<FBlueprintTile> &Blueprint);

  UFUNCTION(BlueprintCallable, Category = "Blueprint Library")
  void LoadBlueprintFromLibrary(int32 Index);

  UFUNCTION(BlueprintCallable, Category = "Blueprint Library")
  TArray<FBlueprintData> GetSavedBlueprints() const;

  UFUNCTION(BlueprintCallable, Category = "Blueprint Library")
  void RemoveBlueprintFromLibrary(int32 Index);

  void SetBlueprintForPlacement(const TArray<FBlueprintTile> &Blueprint);

  // Toggle grid visibility
  void ToggleGridVisibility();

  // Toggle walker debug visualization
  void ToggleDebugWalkers();

  // Cycle to the next view mode (Standard, Advanced Filter, Orb View)
  UFUNCTION(BlueprintCallable, Category = "View")
  void NextViewMode();

  UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "View")
  EViewMode CurrentViewMode = EViewMode::Standard;

  // Highlight components
  // Helper to get the correct highlight mesh, rotation and scale for a given
  // hex
  UStaticMesh *GetHighlightMeshForHex(const FHex &Hex, float &OutYaw,
                                      FVector &OutScale, FVector &OutWorldPos,
                                      bool &bOutWorldPosSet);

  UPROPERTY()
  UStaticMeshComponent *TileHighlightMesh;

  UPROPERTY()
  AActor *LastHighlightedActor;

  UPROPERTY()
  class AHexFactoryActor *CachedHexFactory;

  UPROPERTY()
  class AGameManager *CachedGameManager;

  UPROPERTY()
  class AOrbSimDriver *CachedOrbSimDriver;

  int32 ActiveGenerationMode = 0;
  bool bAncestralInputMode = false;

  void UpdateHoverHighlight(float DeltaSeconds);

private:
  float AxisX = 0.f, AxisY = 0.f, AxisZoom = 0.f;
  bool bTypeSelectionActive = false;      // For cycling orb types (Alt key)
  bool bBlueprintSelectionActive = false; // For blueprint drag selection
  bool bDeletionSelectionActive = false;  // For deletion drag selection
  bool bBlueprintMode = false;
  bool bBlueprintPlacementMode = false;
  bool bContinuePlacementActive = false;
  bool bPlaceHeld = false; // Track if the Place action button is currently held
  bool bDeletionMode = false;
  FVector2D SelectionStart;
  FVector2D SelectionEnd;
  TArray<FHex> SelectedTiles;
  TArray<FBlueprintTile> BlueprintTiles;
  FHex BlueprintPlacementOffset;
  uint8 BlueprintRotation = 0;
  int32 CurrentLevel = 0;
  EPlaceKind LastGlobalHighlightKind = (EPlaceKind)255;
  EOrbType LastGlobalHighlightOrbType = EOrbType::None;
  TArray<TWeakObjectPtr<AActor>> GlobalHighlightedActors;
  FHex LastLoggedHex = FHex(-1, -1, -1);
  void SetGlobalHighlightForKind(EPlaceKind Kind, bool bEnable);

  void MoveX(float V) { AxisX = V; }
  void MoveY(float V) { AxisY = V; }
  void Zoom(float V) { AxisZoom = V; }

  void PlacePressed();
  void PlaceReleased();
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
  void UpdateSelectionHighlights();
  void BlueprintModePressed();
  void BlueprintModeReleased();
  void ToggleBlueprintMode();
  void CreateBlueprint();
  void ClearSelection();
  void CancelBlueprintPlacement();

  // Deletion functions
  void DeletionModePressed();
  void DeletionModeReleased();
  void DeleteTileAndOrbs(const FHex &Hex);
  void DeleteSelectedTilesAndOrbs();
  void RemoveTileAndOrbsAtCursor();
  TArray<FHex> FindItemConnectedComponent(const FHex &StartHex,
                                          TArray<FHex> &OutAnchors,
                                          TArray<int32> &OutBondIndices,
                                          TArray<int32> &OutArmIndices) const;

  // Blueprint placement functions
  void PlaceBlueprint();
  void RotateBlueprintCW();
  void RotateBlueprintCCW();
  void UpdateBlueprintPreview();

  // Blueprint preview mesh management
  void EnsureBlueprintPreviewComponents();
  void UpdateBlueprintPreviewMeshes(float DeltaSeconds);
  void ClearBlueprintPreviewComponents();
  void ClearOrbHighlightComponents();

  // Helper function to convert simulation ops to arm instructions
  TArray<FArmInstr> ConvertOpsToProgram(const TArray<FOp> &Ops);

  // Helper function to get hexes within a selection area
  void GetHexesInSelectionArea(const FHex &HexStart, const FHex &HexEnd,
                               const FVector &WorldStart,
                               const FVector &WorldEnd, int32 Level,
                               TArray<FHex> &OutHexes);

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
  UFUNCTION(BlueprintCallable, Category = "Input")
  void SetInputMode11(); // ClusterSend
  UFUNCTION(BlueprintCallable, Category = "Input")
  void
  SetInputMode12(bool bForceRegenerate = false); // Ancestral/Discovery Mode
  void SetInputMode12Pressed() {
    SetInputMode12(false);
  } // Wrapper for input binding
  UFUNCTION(BlueprintCallable, Category = "Input")
  void SetInputMode13(); // HelixOutput placement
  UFUNCTION(BlueprintCallable, Category = "Input")
  void SwitchGrid();
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

  // Type cycling (works for both Input and Output based on current placement
  // mode) For Kind8 blueprint mode with TypeSelectionActive, cycles
  // DifficultyValue
  void CycleInputTypeNext();
  void CycleInputTypePrevious();

  // Glyph cycling (Belt, Bond, Unbond)
  void CycleGlyphType() {
    if (Placer)
      Placer->CycleGlyphType();
  }

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
  ABasicArm *FindArmAtHex(const FHex &Hex);
  void ToggleArmTimeline(ABasicArm *Arm);

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
  class UArmTimelineEditorWidget *GetTimelineEditorWidget() const {
    return TimelineEditorWidget;
  }

  // Set active timeline for editing (call when clicking on an arm)
  UFUNCTION(BlueprintCallable, Category = "Timeline Editor")
  void SetActiveTimelineForEditing(class ABasicArm *Arm);

  // Update editor visibility based on timeline visibility (call this tick or
  // when timeline visibility changes)
  UFUNCTION(BlueprintCallable, Category = "Timeline Editor")
  void UpdateTimelineEditorVisibility();

  // Flag to pause on next GTM tick
  bool bPauseOnNextTick = false;

  // Blueprint library (persistent) - using TArray directly for runtime,
  // converted for save
  TArray<TArray<FBlueprintTile>> SavedBlueprints;

  // Save game slot name for blueprints
  static const FString BlueprintLibrarySaveSlotName;

  // Moved here to preserve memory layout for SavedBlueprints (Live Coding
  // safety) Blueprint smoothing
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
