#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "AutomationGameHUD.generated.h"

UCLASS()
class AUTOMATIONGAME_API AAutomationGameHUD : public AHUD
{
	GENERATED_BODY()

public:
	AAutomationGameHUD();

	virtual void DrawHUD() override;

	// Selection box drawing
	UFUNCTION(BlueprintCallable, Category = "Selection")
	void StartSelectionBox(FVector2D StartPosition);

	UFUNCTION(BlueprintCallable, Category = "Selection")
	void UpdateSelectionBox(FVector2D CurrentPosition);

	UFUNCTION(BlueprintCallable, Category = "Selection")
	void EndSelectionBox();

	UFUNCTION(BlueprintCallable, Category = "Selection")
	bool IsSelectionActive() const { return bSelectionBoxActive; }

	UFUNCTION(BlueprintCallable, Category = "Selection")
	FVector2D GetSelectionStart() const { return SelectionStart; }

	UFUNCTION(BlueprintCallable, Category = "Selection")
	FVector2D GetSelectionEnd() const { return SelectionEnd; }

protected:
	// Selection box properties
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Selection")
	FLinearColor SelectionBoxColor = FLinearColor::Yellow;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Selection")
	float SelectionBoxThickness = 2.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Selection")
	FLinearColor SelectionBoxFillColor = FLinearColor(1.0f, 1.0f, 0.0f, 0.1f); // Semi-transparent yellow

private:
	bool bSelectionBoxActive = false;
	FVector2D SelectionStart = FVector2D::ZeroVector;
	FVector2D SelectionEnd = FVector2D::ZeroVector;

	void DrawSelectionBox();
	void DrawTimelineToArmLines();
};
