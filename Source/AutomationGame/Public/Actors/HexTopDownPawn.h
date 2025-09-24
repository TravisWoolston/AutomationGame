#pragma once
#include "AutomationGameMinimal.h"
#include "GameFramework/Pawn.h"
#include "Components/HexPlacementComponent.h"
#include "Sim/HexTypes.h"
#include "Sim/Tiles.h"
#include "Sim/ArmTypes.h"
#include "Actors/BasicArm.h"
#include "HexTopDownPawn.generated.h"

class USpringArmComponent;
class UCameraComponent;
class UHexPlacementComponent;

USTRUCT(BlueprintType)
struct AUTOMATIONGAME_API FBlueprintTile
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FHex LocalHex;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	ETileKind TileType;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	uint8 TileDirection;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	bool bIsArm = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	uint8 ArmFacing = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	TArray<FArmInstr> ArmProgram;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	bool bArmLoopProgram = true;

	FBlueprintTile()
		: LocalHex(0, 0), TileType(ETileKind::Input), TileDirection(0), bIsArm(false), ArmFacing(0), bArmLoopProgram(true) {}

	FBlueprintTile(FHex InLocalHex, ETileKind InTileType, uint8 InDirection)
		: LocalHex(InLocalHex), TileType(InTileType), TileDirection(InDirection), bIsArm(false), ArmFacing(0), bArmLoopProgram(true) {}

	FBlueprintTile(FHex InLocalHex, uint8 InArmFacing, const TArray<FArmInstr>& InArmProgram, bool bInLoopProgram)
		: LocalHex(InLocalHex), TileType(ETileKind::Empty), TileDirection(0), bIsArm(true), ArmFacing(InArmFacing), ArmProgram(InArmProgram), bArmLoopProgram(bInLoopProgram) {}
};

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

	UPROPERTY(VisibleAnywhere, Category = "Camera") USpringArmComponent* SpringArm;
	UPROPERTY(VisibleAnywhere, Category = "Camera") UCameraComponent* Camera;
	UPROPERTY(VisibleAnywhere, Category = "Place")  UHexPlacementComponent* Placer;

private:
	float AxisX = 0.f, AxisY = 0.f, AxisZoom = 0.f;
	bool bTypeSelectionActive = false;  // For cycling orb types (Alt key)
	bool bBlueprintSelectionActive = false;  // For blueprint drag selection
	bool bDeletionSelectionActive = false;  // For deletion drag selection
	bool bBlueprintMode = false;
	bool bBlueprintPlacementMode = false;
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

	void SetInputMode1();
	void SetInputMode2();
	void SetInputMode3();
	void SetInputMode4();
	void SetInputMode5();
	void SetInputMode6();
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

	// Type cycling (works for both Input and Output based on current placement mode)
	void CycleInputTypeNext() { if (Placer) Placer->CycleNextType(); }
	void CycleInputTypePrevious() { if (Placer) Placer->CyclePreviousType(); }
	
	// Glyph cycling (Belt, Bond, Unbond)
	void CycleGlyphType() { if (Placer) Placer->CycleGlyphType(); }
	
	// Type selection modifier key (for cycling orb types)
	void SelectionActivePressed();
	void SelectionActiveReleased();
};
