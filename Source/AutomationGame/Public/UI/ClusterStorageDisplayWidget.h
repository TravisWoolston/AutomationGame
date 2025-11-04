#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Components/VerticalBox.h"
#include "Framework/GameManager.h"
#include "ClusterStorageDisplayWidget.generated.h"

/**
 * Widget to display stored cluster types and their counts
 * Shows icons for each unique cluster structure with the number stored
 */
UCLASS(BlueprintType)
class AUTOMATIONGAME_API UClusterStorageDisplayWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	UClusterStorageDisplayWidget(const FObjectInitializer& ObjectInitializer);

	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

	/** Refresh the display with current stored clusters from GameManager */
	UFUNCTION(BlueprintCallable, Category = "Cluster Storage")
	void RefreshDisplay();

	/** Set the GameManager reference */
	UFUNCTION(BlueprintCallable, Category = "Cluster Storage")
	void SetGameManager(AGameManager* InGameManager);

	/** Check if we need to refresh (clusters added) */
	UFUNCTION(BlueprintPure, Category = "Cluster Storage")
	bool NeedsRefresh() const;

	/** Handle clicking on a cluster icon to generate blueprint */
	UFUNCTION()
	void HandleClusterIconClicked();

protected:
	/** Vertical box containing cluster entries */
	UPROPERTY(BlueprintReadOnly, meta = (BindWidget))
	class UVerticalBox* ClusterEntriesBox = nullptr;

	/** GameManager that owns the cluster storage */
	UPROPERTY(BlueprintReadWrite, Category = "Cluster Storage")
	AGameManager* GameManager = nullptr;
	
	/** HexTopDownPawn reference for setting blueprint placement */
	UPROPERTY(BlueprintReadWrite, Category = "Cluster Storage")
	class AHexTopDownPawn* Pawn = nullptr;
	
	/** Map to associate cluster icon buttons with their template indices */
	UPROPERTY()
	TMap<class UButton*, int32> ClusterButtonToIndex;
	
	/** Array of cluster icon buttons */
	UPROPERTY()
	TArray<class UButton*> ClusterIconButtons;

	/** Last known cluster count to detect changes */
	UPROPERTY()
	int32 LastKnownClusterCount = 0;

	/** Last known icon scale to detect changes */
	UPROPERTY()
	float LastKnownIconScale = 1.0f;

	/** Refresh rate in seconds */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cluster Storage", meta = (ClampMin = "0.1", ClampMax = "10.0"))
	float RefreshInterval = 0.5f;

	/** Time since last refresh */
	float TimeSinceLastRefresh = 0.0f;
};

