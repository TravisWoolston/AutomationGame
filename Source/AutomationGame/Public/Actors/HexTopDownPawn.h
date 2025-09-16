#pragma once
#include "AutomationGameMinimal.h"
#include "GameFramework/Pawn.h"
#include "Components/HexPlacementComponent.h"
#include "HexTopDownPawn.generated.h"

class USpringArmComponent;
class UCameraComponent;
class UHexPlacementComponent;

UCLASS()
class AUTOMATIONGAME_API AHexTopDownPawn : public APawn
{
	GENERATED_BODY()
public:
	AHexTopDownPawn();

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
	bool bSelectionActive = false;
	void MoveX(float V) { AxisX = V; }
	void MoveY(float V) { AxisY = V; }
	void Zoom(float V) { AxisZoom = V; }

	void PlacePressed() { if (Placer) Placer->HandlePlace(true); }
	void RemovePressed() { if (Placer) Placer->HandleRemove(true); }

	void SetInputMode1();
	void SetInputMode2();
	void SetInputMode3();
	void SetInputMode4();
	void SetInputMode5();
	void RotCW() { if (Placer) Placer->RotateCW(); }
	void RotCCW() { if (Placer) Placer->RotateCCW(); }

	// Type cycling (works for both Input and Output based on current placement mode)
	void CycleInputTypeNext() { if (Placer) Placer->CycleNextType(); }
	void CycleInputTypePrevious() { if (Placer) Placer->CyclePreviousType(); }
	
	// Selection modifier key
	void SelectionActivePressed();
	void SelectionActiveReleased();
};
