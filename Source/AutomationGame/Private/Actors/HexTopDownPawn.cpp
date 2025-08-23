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

	// Zoom
	if (AxisZoom != 0.f)
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
}

void AHexTopDownPawn::SetInputMode1() { if (Placer) Placer->SetPlaceKind(EPlaceKind::Input); }
void AHexTopDownPawn::SetInputMode2() { if (Placer) Placer->SetPlaceKind(EPlaceKind::Output); }
void AHexTopDownPawn::SetInputMode3() { if (Placer) Placer->SetPlaceKind(EPlaceKind::Belt); }
