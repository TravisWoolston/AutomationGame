#include "Actors/HexTopDownPawn.h"
#include "GameFramework/SpringArmComponent.h"
#include "Camera/CameraComponent.h"
#include "Components/HexPlacementComponent.h"
#include "GameFramework/PlayerController.h"

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

void AHexTopDownPawn::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	// Pan
	const FVector Right = GetActorRightVector();
	const FVector Fwd = GetActorForwardVector();
	FVector Delta = (Right * AxisX + Fwd * AxisY) * MoveSpeed * DeltaSeconds;
	AddActorWorldOffset(Delta, true);

	// Zoom (disabled when SelectionActive is held)
	if (AxisZoom != 0.f && !bSelectionActive)
	{
		SpringArm->TargetArmLength = FMath::Clamp(SpringArm->TargetArmLength - AxisZoom * ZoomSpeed * DeltaSeconds, MinArm, MaxArm);
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
	IC->BindAction("Remove", IE_Pressed, this, &AHexTopDownPawn::RemovePressed);

	IC->BindAction("RotateCW", IE_Pressed, this, &AHexTopDownPawn::RotCW);
	IC->BindAction("RotateCCW", IE_Pressed, this, &AHexTopDownPawn::RotCCW);

	IC->BindAction("Kind1", IE_Pressed, this, &AHexTopDownPawn::SetInputMode1);
	IC->BindAction("Kind2", IE_Pressed, this, &AHexTopDownPawn::SetInputMode2);
	IC->BindAction("Kind3", IE_Pressed, this, &AHexTopDownPawn::SetInputMode3);
	IC->BindAction("Kind4", IE_Pressed, this, &AHexTopDownPawn::SetInputMode4);
	IC->BindAction("Kind5", IE_Pressed, this, &AHexTopDownPawn::SetInputMode5);

	// Input type cycling (for when in Input mode)
	IC->BindAction("CycleNextType", IE_Pressed, this, &AHexTopDownPawn::CycleInputTypeNext);
	IC->BindAction("CyclePreviousType", IE_Pressed, this, &AHexTopDownPawn::CycleInputTypePrevious);
	
	// Selection modifier key (Left Alt)
	IC->BindAction("SelectionActive", IE_Pressed, this, &AHexTopDownPawn::SelectionActivePressed);
	IC->BindAction("SelectionActive", IE_Released, this, &AHexTopDownPawn::SelectionActiveReleased);
}

void AHexTopDownPawn::SetInputMode1() { if (Placer) Placer->SetPlaceKind(EPlaceKind::Input); }
void AHexTopDownPawn::SetInputMode2() { if (Placer) Placer->SetPlaceKind(EPlaceKind::Output); }
void AHexTopDownPawn::SetInputMode3() { if (Placer) Placer->SetPlaceKind(EPlaceKind::Belt); }
void AHexTopDownPawn::SetInputMode4() { if (Placer) Placer->SetPlaceKind(EPlaceKind::Arm); }
void AHexTopDownPawn::SetInputMode5() { if (Placer) Placer->SetPlaceKind(EPlaceKind::Bond); }

void AHexTopDownPawn::SelectionActivePressed() { bSelectionActive = true; }
void AHexTopDownPawn::SelectionActiveReleased() { bSelectionActive = false; }
