#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Components/HexPlacementComponent.h"
#include "HotbarWidget.generated.h"

class UTexture2D;
class UButton;
class UHorizontalBox;
class UVerticalBox;
class UOverlay;
class UImage;
class AHexTopDownPawn;

/**
 * Icons assigned to each placeable tile type
 */
USTRUCT(BlueprintType)
struct FTileIconEntry
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icon") UTexture2D* Texture = nullptr;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icon") float Scale = 1.0f;
};

/**
 * Icons assigned to each placeable tile type
 */
USTRUCT(BlueprintType)
struct FTileIconAssignments
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icons") FTileIconEntry Input;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icons") FTileIconEntry Output;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icons") FTileIconEntry Belt;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icons") FTileIconEntry Arm;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icons") FTileIconEntry Walker;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icons") FTileIconEntry Bond;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icons") FTileIconEntry Unbond;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icons") FTileIconEntry ClusterOutput;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icons") FTileIconEntry ClusterInput;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icons") FTileIconEntry RequestBeacon;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icons") FTileIconEntry TriggerTile;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icons") FTileIconEntry ClusterSend;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icons") FTileIconEntry HelixOutput;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icons") FTileIconEntry HelixInput;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icons") FTileIconEntry GatherScatter;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icons") FTileIconEntry Source;

	/** Special icons for the multi-step/random modes */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icons") FTileIconEntry RandomCluster;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Icons") FTileIconEntry AncestralMode;
};

/**
 * Player hotbar widget that displays icons for placeable items.
 * Handles groups of items sharing the same hotkey with a popup drawer.
 */
UCLASS(BlueprintType, Blueprintable)
class AUTOMATIONGAME_API UHotbarWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	UHotbarWidget(const FObjectInitializer& ObjectInitializer);

	virtual void NativeConstruct() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

	/** 
	 * Generate hotbar buttons. 
	 * Groups actions sharing the same key and populates the UI.
	 */
	UFUNCTION(BlueprintCallable, Category = "Hotbar")
	void GenerateHotbar();

	/** Trigger the placement action */
	UFUNCTION(BlueprintCallable, Category = "Hotbar")
	void OnSlotClicked(int32 ActionIndex);

	/** Delegate wrappers for visual positions (0-14 horizontal slots) */
	UFUNCTION() void OnSlot0Clicked();
	UFUNCTION() void OnSlot1Clicked();
	UFUNCTION() void OnSlot2Clicked();
	UFUNCTION() void OnSlot3Clicked();
	UFUNCTION() void OnSlot4Clicked();
	UFUNCTION() void OnSlot5Clicked();
	UFUNCTION() void OnSlot6Clicked();
	UFUNCTION() void OnSlot7Clicked();
	UFUNCTION() void OnSlot8Clicked();
	UFUNCTION() void OnSlot9Clicked();
	UFUNCTION() void OnSlot10Clicked();
	UFUNCTION() void OnSlot11Clicked();
	UFUNCTION() void OnSlot12Clicked();
	UFUNCTION() void OnSlot13Clicked();
	UFUNCTION() void OnSlot14Clicked();

	/** Drawer visibility toggle functions */
	UFUNCTION() void OnSlotHovered(int32 VisualIndex);
	UFUNCTION() void OnSlotUnhovered(int32 VisualIndex);

	UFUNCTION() void OnSlot0Hovered(); UFUNCTION() void OnSlot0Unhovered();
	UFUNCTION() void OnSlot1Hovered(); UFUNCTION() void OnSlot1Unhovered();
	UFUNCTION() void OnSlot2Hovered(); UFUNCTION() void OnSlot2Unhovered();
	UFUNCTION() void OnSlot3Hovered(); UFUNCTION() void OnSlot3Unhovered();
	UFUNCTION() void OnSlot4Hovered(); UFUNCTION() void OnSlot4Unhovered();
	UFUNCTION() void OnSlot5Hovered(); UFUNCTION() void OnSlot5Unhovered();
	UFUNCTION() void OnSlot6Hovered(); UFUNCTION() void OnSlot6Unhovered();
	UFUNCTION() void OnSlot7Hovered(); UFUNCTION() void OnSlot7Unhovered();
	UFUNCTION() void OnSlot8Hovered(); UFUNCTION() void OnSlot8Unhovered();
	UFUNCTION() void OnSlot9Hovered(); UFUNCTION() void OnSlot9Unhovered();
	UFUNCTION() void OnSlot10Hovered(); UFUNCTION() void OnSlot10Unhovered();
	UFUNCTION() void OnSlot11Hovered(); UFUNCTION() void OnSlot11Unhovered();
	UFUNCTION() void OnSlot12Hovered(); UFUNCTION() void OnSlot12Unhovered();
	UFUNCTION() void OnSlot13Hovered(); UFUNCTION() void OnSlot13Unhovered();
	UFUNCTION() void OnSlot14Hovered(); UFUNCTION() void OnSlot14Unhovered();

public:
	/** Assign icons for each tile type in the Blueprint editor */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hotbar Icons")
	FTileIconAssignments TileIcons;

	/** HorizontalBox that will contain the generated buttons */
	UPROPERTY(BlueprintReadOnly, meta = (BindWidget))
	UHorizontalBox* HotbarContainer = nullptr;

	/** Slide animation speed */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hotbar Config")
	float SlideSpeed = 15.0f;

protected:
	/** Cached reference to the player pawn */
	UPROPERTY()
	AHexTopDownPawn* CachedPawn = nullptr;

private:
	/** Get the primary key assigned to an action name in Input Settings */
	FKey GetKeyForAction(FName ActionName) const;

	/** Get the appropriate icon for a specific action index */
	FTileIconEntry GetIconForAction(int32 ActionIndex) const;
	
	/** Get a displayable label for an action index */
	FString GetLabelForAction(int32 ActionIndex) const;

	/** Internal state for tracking active groups and their drawers */
	struct FSlotGroup
	{
		TArray<int32> Actions;
		FKey Key;
		UOverlay* SlotOverlay = nullptr;
		UVerticalBox* Drawer = nullptr;
		UImage* MainIcon = nullptr;
		UButton* MainButton = nullptr;
		UImage* SelectionBorder = nullptr;
		TArray<UImage*> SubIcons;
		TArray<EPlaceKind> GroupKinds;
		float CurrentSlide = 0.0f; // 0=Closed, 1=Open
		float TargetSlide = 0.0f;
		bool bIsHovered = false;
		bool bIsAltGroup = false;
	};

	TArray<FSlotGroup> StandardGroups;
	TArray<FSlotGroup> AltGroups;

	/** Was alt held in previous frame? */
	bool bWasAltHeld = false;

	/** Helper to implementation of visual slot delegates */
	void HandleSlotClick(int32 VisualIndex);

	/** Helper to map action index to EPlaceKind for cycling */
	EPlaceKind GetPlaceKindForAction(int32 ActionId) const;
};
