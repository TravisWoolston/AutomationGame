#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Components/VerticalBox.h"
#include "Components/Button.h"
#include "Actors/HexTopDownPawn.h"
#include "BlueprintLibraryDisplayWidget.generated.h"

class UButton;

/**
 * Widget to display saved blueprints
 * Shows icons for each blueprint with click to place and delete buttons
 */
UCLASS(BlueprintType)
class AUTOMATIONGAME_API UBlueprintLibraryDisplayWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	UBlueprintLibraryDisplayWidget(const FObjectInitializer& ObjectInitializer);

	virtual void NativeConstruct() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

	/** Refresh the display with current saved blueprints */
	UFUNCTION(BlueprintCallable, Category = "Blueprint Library")
	void RefreshDisplay();

	/** Set the HexTopDownPawn reference */
	UFUNCTION(BlueprintCallable, Category = "Blueprint Library")
	void SetPawn(AHexTopDownPawn* InPawn);

	/** Generate an icon texture for a blueprint */
	UFUNCTION(BlueprintCallable, Category = "Blueprint Library")
	UTexture2D* GenerateBlueprintIcon(const TArray<FBlueprintTile>& Blueprint);

protected:
	/** Vertical box containing blueprint entries */
	UPROPERTY(BlueprintReadOnly, meta = (BindWidget))
	class UVerticalBox* BlueprintEntriesBox = nullptr;

	/** HexTopDownPawn that owns the blueprint library */
	UPROPERTY(BlueprintReadWrite, Category = "Blueprint Library")
	AHexTopDownPawn* Pawn = nullptr;

	/** Last known blueprint count to detect changes */
	UPROPERTY()
	int32 LastKnownBlueprintCount = 0;

	/** Refresh rate in seconds */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Blueprint Library", meta = (ClampMin = "0.1", ClampMax = "10.0"))
	float RefreshInterval = 0.5f;

	/** Time since last refresh */
	float TimeSinceLastRefresh = 0.0f;

	/** Icon scale for blueprint icons */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Blueprint Library", meta = (ClampMin = "0.1"))
	float BlueprintIconScale = 1.0f;

	/** Cached blueprint icons */
	UPROPERTY()
	TMap<int32, UTexture2D*> BlueprintIcons;

	/** Arrays of buttons indexed by blueprint index */
	UPROPERTY()
	TArray<UButton*> BlueprintIconButtons;

	UPROPERTY()
	TArray<UButton*> BlueprintDeleteButtons;
	
	/** Map to associate buttons with their blueprint indices */
	UPROPERTY()
	TMap<UButton*, int32> ButtonToBlueprintIndex;

	/** Handle button click - will look up index from button arrays */
	UFUNCTION()
	void HandleBlueprintIconClicked();

	/** Handle delete button click - will look up index from button arrays */
	UFUNCTION()
	void HandleDeleteBlueprintClicked();

	/** Currently clicked button index (set before calling handler) */
	int32 CurrentClickedIndex = -1;
};

