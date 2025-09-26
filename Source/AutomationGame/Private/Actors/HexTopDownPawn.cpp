#include "Actors/HexTopDownPawn.h"
#include "GameFramework/SpringArmComponent.h"
#include "Camera/CameraComponent.h"
#include "Components/HexPlacementComponent.h"
#include "GameFramework/PlayerController.h"
#include "Actors/OrbSimDriver.h"
#include "Actors/BasicArm.h"
#include "Sim/SimWorldHex.h"
#include "Sim/HexCoord.h"
#include "Sim/HexTypes.h"
#include "Sim/ArmTypes.h"
#include "DrawDebugHelpers.h"
#include "UI/AutomationGameHUD.h"
#include "Framework/GameManager.h"
#include "EngineUtils.h"

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

void AHexTopDownPawn::BeginPlay()
{
	Super::BeginPlay();
	
	// Ensure placement preview is enabled by default
	if (Placer)
	{
		Placer->SetBlueprintMode(false);
	}
}

void AHexTopDownPawn::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	// Pan
	const FVector Right = GetActorRightVector();
	const FVector Fwd = GetActorForwardVector();
	FVector Delta = (Right * AxisX + Fwd * AxisY) * MoveSpeed * DeltaSeconds;
	AddActorWorldOffset(Delta, true);

	// Zoom (disabled when TypeSelectionActive is held)
	if (AxisZoom != 0.f && !bTypeSelectionActive)
	{
		SpringArm->TargetArmLength = FMath::Clamp(SpringArm->TargetArmLength - AxisZoom * ZoomSpeed * DeltaSeconds, MinArm, MaxArm);
	}
	
	// Update selection if active
	UpdateSelection();
	UpdateDeletionSelection();
	
	// Update HUD selection box if active (both blueprint and deletion use the same box)
	if ((bBlueprintSelectionActive || bDeletionSelectionActive) && GetWorld())
	{
		APlayerController* PC = GetWorld()->GetFirstPlayerController();
		if (PC)
		{
			AAutomationGameHUD* GameHUD = Cast<AAutomationGameHUD>(PC->GetHUD());
			if (GameHUD)
			{
				FVector2D MousePosition;
				if (PC->GetMousePosition(MousePosition.X, MousePosition.Y))
				{
					GameHUD->UpdateSelectionBox(MousePosition);
				}
			}
		}
	}
	
	// Draw blueprint preview if in placement mode
	if (bBlueprintPlacementMode && BlueprintTiles.Num() > 0 && GetWorld())
	{
		FVector2D MousePosition;
		if (GetWorld()->GetFirstPlayerController() && 
			GetWorld()->GetFirstPlayerController()->GetMousePosition(MousePosition.X, MousePosition.Y))
		{
			// Get tile size and grid info from driver
			AOrbSimDriver* Driver = AOrbSimDriver::Find(GetWorld());
			float TileSize = Driver ? Driver->GetTileSize() : 100.f;
			EHexOrientation Orientation = Driver ? Driver->GetGridOrientation() : EHexOrientation::PointyTop;
			float GridPlaneZ = Driver ? Driver->GetGridPlaneZ(this) : 0.f;
			
			// Convert mouse position to world coordinates by projecting ray to the grid plane
			FVector WorldPosition, WorldDirection;
			GetWorld()->GetFirstPlayerController()->DeprojectScreenPositionToWorld(
				MousePosition.X, MousePosition.Y, WorldPosition, WorldDirection);
			
			// Project the ray down to the grid plane (Z = GridPlaneZ)
			if (FMath::Abs(WorldDirection.Z) > KINDA_SMALL_NUMBER)
			{
				float T = (GridPlaneZ - WorldPosition.Z) / WorldDirection.Z;
				WorldPosition = WorldPosition + WorldDirection * T;
			}
			
			// Convert world coordinates to hex coordinates
			FHexAxial TargetHexAxial = HexCoord::XYToAxialRound(FVector2D(WorldPosition.X, WorldPosition.Y), TileSize, Orientation);
			FHex TargetHex(TargetHexAxial.Q, TargetHexAxial.R);
			
			// Draw preview of each tile in the blueprint
			for (const FBlueprintTile& BlueprintTile : BlueprintTiles)
			{
				// Calculate the preview hex position
				FHex PreviewHex = TargetHex;
				PreviewHex.Q += BlueprintTile.LocalHex.Q;
				PreviewHex.R += BlueprintTile.LocalHex.R;
				
				// Convert hex to world position for drawing
				FVector2D PreviewWorldPos2D = HexCoord::AxialToXY(FHexAxial(PreviewHex.Q, PreviewHex.R), TileSize, Orientation);
				FVector PreviewWorldPos = FVector(PreviewWorldPos2D.X, PreviewWorldPos2D.Y, 0);
				
				// Choose color and size based on tile type
				FColor PreviewColor;
				float SphereSize = 40.f;
				FString TileTypeText;
				
				if (BlueprintTile.bIsArm)
				{
					PreviewColor = FColor::Green;
					SphereSize = 60.f;
					TileTypeText = FString::Printf(TEXT("ARM (F:%d R:%d)"), BlueprintTile.ArmFacing, BlueprintTile.ArmReach);
				}
				else
				{
					switch (BlueprintTile.TileType)
					{
					case ETileKind::Input:
						PreviewColor = FColor::Cyan;
						TileTypeText = TEXT("INPUT");
						break;
					case ETileKind::Output:
						PreviewColor = FColor::Magenta;
						TileTypeText = TEXT("OUTPUT");
						break;
					case ETileKind::Belt:
						PreviewColor = FColor::Yellow;
						TileTypeText = FString::Printf(TEXT("BELT (D:%d)"), BlueprintTile.TileDirection);
						break;
					case ETileKind::Bond:
						PreviewColor = FColor::Orange;
						TileTypeText = FString::Printf(TEXT("BOND (D:%d)"), BlueprintTile.TileDirection);
						break;
					case ETileKind::Unbond:
						PreviewColor = FColor::Red;
						TileTypeText = FString::Printf(TEXT("UNBOND (D:%d)"), BlueprintTile.TileDirection);
						break;
					default:
						PreviewColor = FColor::White;
						TileTypeText = TEXT("EMPTY");
						break;
					}
				}
				
				// Draw the preview sphere
				DrawDebugSphere(GetWorld(), PreviewWorldPos, SphereSize, 8, PreviewColor, false, 0.f, 0, 2.f);
				
				// Draw tile type text above the sphere
				DrawDebugString(GetWorld(), PreviewWorldPos + FVector(0, 0, SphereSize + 20.f), 
					TileTypeText, nullptr, PreviewColor, 0.f, true, 1.0f);
				
				// Draw hex coordinates for debugging
				DrawDebugString(GetWorld(), PreviewWorldPos + FVector(0, 0, -20.f), 
					FString::Printf(TEXT("(%d,%d)"), PreviewHex.Q, PreviewHex.R), 
					nullptr, FColor::White, 0.f, true, 0.8f);
			}
		}
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
	IC->BindAction("Place", IE_Released, this, &AHexTopDownPawn::PlaceReleased);
	IC->BindAction("Remove", IE_Pressed, this, &AHexTopDownPawn::RemovePressed);
	
	// Blueprint mode (hold to select, toggle for placement)
	IC->BindAction("BlueprintMode", IE_Pressed, this, &AHexTopDownPawn::BlueprintModePressed);
	IC->BindAction("BlueprintMode", IE_Released, this, &AHexTopDownPawn::BlueprintModeReleased);
	
	// Deletion mode (hold to select, then delete on release)
	IC->BindAction("DeletionMode", IE_Pressed, this, &AHexTopDownPawn::DeletionModePressed);
	IC->BindAction("DeletionMode", IE_Released, this, &AHexTopDownPawn::DeletionModeReleased);
	
	// Blueprint placement actions (use same keys as regular rotation)
	// IC->BindAction("BlueprintRotateCW", IE_Pressed, this, &AHexTopDownPawn::RotateBlueprintCW);
	// IC->BindAction("BlueprintRotateCCW", IE_Pressed, this, &AHexTopDownPawn::RotateBlueprintCCW);

	IC->BindAction("RotateCW", IE_Pressed, this, &AHexTopDownPawn::RotCW);
	IC->BindAction("RotateCCW", IE_Pressed, this, &AHexTopDownPawn::RotCCW);

	IC->BindAction("Kind1", IE_Pressed, this, &AHexTopDownPawn::SetInputMode1);
	IC->BindAction("Kind2", IE_Pressed, this, &AHexTopDownPawn::SetInputMode2);
	IC->BindAction("Kind3", IE_Pressed, this, &AHexTopDownPawn::SetInputMode3);
	IC->BindAction("Kind4", IE_Pressed, this, &AHexTopDownPawn::SetInputMode4);
	IC->BindAction("Kind5", IE_Pressed, this, &AHexTopDownPawn::SetInputMode5);
	IC->BindAction("Kind6", IE_Pressed, this, &AHexTopDownPawn::SetInputMode6);

	// Type cycling (works for both Input and Output based on current placement mode)
	IC->BindAction("CycleNextType", IE_Pressed, this, &AHexTopDownPawn::CycleInputTypeNext);
	IC->BindAction("CyclePreviousType", IE_Pressed, this, &AHexTopDownPawn::CycleInputTypePrevious);
	
	// Glyph cycling (Belt, Bond, Unbond)
	IC->BindAction("CycleGlyphType", IE_Pressed, this, &AHexTopDownPawn::CycleGlyphType);
	
	// Selection modifier key (Left Alt)
	IC->BindAction("TypeSelectionActive", IE_Pressed, this, &AHexTopDownPawn::SelectionActivePressed);
	IC->BindAction("TypeSelectionActive", IE_Released, this, &AHexTopDownPawn::SelectionActiveReleased);
}

void AHexTopDownPawn::SetInputMode1() { if (Placer) Placer->SetPlaceKind(EPlaceKind::Input); }
void AHexTopDownPawn::SetInputMode2() { if (Placer) Placer->SetPlaceKind(EPlaceKind::Output); }
void AHexTopDownPawn::SetInputMode3() { if (Placer) Placer->SetPlaceKind(EPlaceKind::Belt); }
void AHexTopDownPawn::SetInputMode4() { if (Placer) Placer->SetPlaceKind(EPlaceKind::Arm); }
void AHexTopDownPawn::SetInputMode5() { if (Placer) Placer->SetPlaceKind(EPlaceKind::Bond); }
void AHexTopDownPawn::SetInputMode6() { if (Placer) Placer->SetPlaceKind(EPlaceKind::Unbond); }

void AHexTopDownPawn::SelectionActivePressed() { bTypeSelectionActive = true; }
void AHexTopDownPawn::SelectionActiveReleased() { bTypeSelectionActive = false; }

void AHexTopDownPawn::StartSelection()
{
	UE_LOG(LogTemp, Warning, TEXT("StartSelection called, blueprint mode: %s"), bBlueprintMode ? TEXT("ON") : TEXT("OFF"));
	
	if (!bBlueprintMode) return;
	
	APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	if (!PC) return;
	
	FVector2D MousePosition;
	if (PC->GetMousePosition(MousePosition.X, MousePosition.Y))
	{
		SelectionStart = MousePosition;
		SelectionEnd = MousePosition;
		bBlueprintSelectionActive = true;
		
		// Start HUD selection box
		AAutomationGameHUD* GameHUD = Cast<AAutomationGameHUD>(PC->GetHUD());
		if (GameHUD)
		{
			GameHUD->StartSelectionBox(MousePosition);
		}
		
		UE_LOG(LogTemp, Warning, TEXT("Selection started at (%f, %f)"), MousePosition.X, MousePosition.Y);
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("Failed to get mouse position for selection"));
	}
}

void AHexTopDownPawn::EndSelection()
{
	UE_LOG(LogTemp, Warning, TEXT("EndSelection called, blueprint mode: %s, selection active: %s"), 
		bBlueprintMode ? TEXT("ON") : TEXT("OFF"), bBlueprintSelectionActive ? TEXT("YES") : TEXT("NO"));
	
	if (!bBlueprintMode || !bBlueprintSelectionActive) return;
	
	bBlueprintSelectionActive = false;
	
	// End HUD selection box
	APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	if (PC)
	{
		AAutomationGameHUD* GameHUD = Cast<AAutomationGameHUD>(PC->GetHUD());
		if (GameHUD)
		{
			GameHUD->EndSelectionBox();
		}
	}
	
	CreateBlueprint();
}

void AHexTopDownPawn::UpdateSelection()
{
	if (!bBlueprintSelectionActive || !GetWorld()) return;
	
	FVector2D MousePosition;
	if (GetWorld()->GetFirstPlayerController() && 
		GetWorld()->GetFirstPlayerController()->GetMousePosition(MousePosition.X, MousePosition.Y))
	{
		SelectionEnd = MousePosition;
		
		// Update selected tiles based on current selection box
		SelectedTiles.Empty();
		
		// Get tile size and grid info from driver
		AOrbSimDriver* Driver = AOrbSimDriver::Find(GetWorld());
		float TileSize = Driver ? Driver->GetTileSize() : 100.f;
		EHexOrientation Orientation = Driver ? Driver->GetGridOrientation() : EHexOrientation::PointyTop;
		float GridPlaneZ = Driver ? Driver->GetGridPlaneZ(this) : 0.f;
		
		// Convert screen coordinates to world coordinates by projecting rays to the grid plane
		FVector WorldStart, WorldEnd;
		FVector WorldStartDir, WorldEndDir;
		
		// Deproject both screen points to get rays
		GetWorld()->GetFirstPlayerController()->DeprojectScreenPositionToWorld(
			SelectionStart.X, SelectionStart.Y, WorldStart, WorldStartDir);
		GetWorld()->GetFirstPlayerController()->DeprojectScreenPositionToWorld(
			SelectionEnd.X, SelectionEnd.Y, WorldEnd, WorldEndDir);
		
		// Project the rays down to the grid plane (Z = GridPlaneZ)
		if (FMath::Abs(WorldStartDir.Z) > KINDA_SMALL_NUMBER)
		{
			float T = (GridPlaneZ - WorldStart.Z) / WorldStartDir.Z;
			WorldStart = WorldStart + WorldStartDir * T;
		}
		if (FMath::Abs(WorldEndDir.Z) > KINDA_SMALL_NUMBER)
		{
			float T = (GridPlaneZ - WorldEnd.Z) / WorldEndDir.Z;
			WorldEnd = WorldEnd + WorldEndDir * T;
		}
		
		// Convert world coordinates to hex coordinates
		FHexAxial HexStartAxial = HexCoord::XYToAxialRound(FVector2D(WorldStart.X, WorldStart.Y), TileSize, Orientation);
		FHexAxial HexEndAxial = HexCoord::XYToAxialRound(FVector2D(WorldEnd.X, WorldEnd.Y), TileSize, Orientation);
		FHex HexStart(HexStartAxial.Q, HexStartAxial.R);
		FHex HexEnd(HexEndAxial.Q, HexEndAxial.R);
		
		// Debug: Log the selection area with grid plane info
		UE_LOG(LogTemp, Warning, TEXT("Selection area: Screen(%f,%f) to (%f,%f) -> World(%f,%f,%.1f) to (%f,%f,%.1f) -> Hex(%d,%d) to (%d,%d) [GridZ=%.1f]"), 
			SelectionStart.X, SelectionStart.Y, SelectionEnd.X, SelectionEnd.Y,
			WorldStart.X, WorldStart.Y, WorldStart.Z, WorldEnd.X, WorldEnd.Y, WorldEnd.Z,
			HexStart.Q, HexStart.R, HexEnd.Q, HexEnd.R, GridPlaneZ);
		
		// Create selection box bounds
		int32 MinQ = FMath::Min(HexStart.Q, HexEnd.Q);
		int32 MaxQ = FMath::Max(HexStart.Q, HexEnd.Q);
		int32 MinR = FMath::Min(HexStart.R, HexEnd.R);
		int32 MaxR = FMath::Max(HexStart.R, HexEnd.R);
		
		// Add all hexes in the selection box
		for (int32 q = MinQ; q <= MaxQ; q++)
		{
			for (int32 r = MinR; r <= MaxR; r++)
			{
				SelectedTiles.Add(FHex(q, r));
			}
		}
		
		UE_LOG(LogTemp, Warning, TEXT("Selected %d tiles in area Q(%d-%d) R(%d-%d)"), 
			SelectedTiles.Num(), MinQ, MaxQ, MinR, MaxR);
	}
}

void AHexTopDownPawn::BlueprintModePressed()
{
	UE_LOG(LogTemp, Warning, TEXT("BlueprintMode pressed"));
	if (bBlueprintPlacementMode)
	{
		// If we're in placement mode, toggle to exit
		bBlueprintPlacementMode = false;
		bBlueprintMode = false;
		ClearSelection();
	}
	else
	{
		// Enter blueprint selection mode temporarily
		bBlueprintMode = true;
	}
}

void AHexTopDownPawn::BlueprintModeReleased()
{
	UE_LOG(LogTemp, Warning, TEXT("BlueprintMode released"));
	if (bBlueprintSelectionActive)
	{
		// End selection and create blueprint
		EndSelection();
	}
	else if (!bBlueprintPlacementMode)
	{
		// Exit blueprint mode if not in placement mode
		bBlueprintMode = false;
	}
}

void AHexTopDownPawn::ToggleBlueprintMode()
{
	bBlueprintMode = !bBlueprintMode;
	UE_LOG(LogTemp, Warning, TEXT("Blueprint mode toggled: %s"), bBlueprintMode ? TEXT("ON") : TEXT("OFF"));
	
	if (!bBlueprintMode)
	{
		ClearSelection();
		bBlueprintPlacementMode = false;
	}
	else
	{
		// Enter blueprint mode - clear any existing blueprint
		ClearSelection();
		bBlueprintPlacementMode = false;
	}
}

void AHexTopDownPawn::CreateBlueprint()
{
	UE_LOG(LogTemp, Warning, TEXT("CreateBlueprint called with %d selected tiles"), SelectedTiles.Num());
	
	if (SelectedTiles.Num() == 0) 
	{
		UE_LOG(LogTemp, Warning, TEXT("No tiles selected, exiting CreateBlueprint"));
		return;
	}
	
	BlueprintTiles.Empty();
	
	// Find the simulation world
	AOrbSimDriver* Driver = AOrbSimDriver::Find(GetWorld());
	if (!Driver) return;
	
	USimWorldHex* Sim = Driver->GetSim();
	if (!Sim) return;
	
	// Find the minimum hex coordinates to use as the blueprint origin
	FHex MinHex = SelectedTiles[0];
	for (const FHex& Hex : SelectedTiles)
	{
		MinHex.Q = FMath::Min(MinHex.Q, Hex.Q);
		MinHex.R = FMath::Min(MinHex.R, Hex.R);
	}
	
	// Debug: Log what we're checking
	UE_LOG(LogTemp, Warning, TEXT("Checking %d arms and %d tiles in simulation"), Sim->Arms.Num(), Sim->TileMap.Num());
	
	// Convert selected tiles to blueprint tiles
	for (const FHex& Hex : SelectedTiles)
	{
		UE_LOG(LogTemp, Warning, TEXT("Checking hex (%d,%d) for content"), Hex.Q, Hex.R);
		
		// Calculate local hex coordinates relative to blueprint origin
		FHex LocalHex(Hex.Q - MinHex.Q, Hex.R - MinHex.R);
		
		// Check if there's an arm at this position first
		bool bFoundArm = false;
		for (int32 ArmIndex = 0; ArmIndex < Sim->Arms.Num(); ArmIndex++)
		{
			const FArmState& ArmState = Sim->Arms[ArmIndex];
			if (ArmState.BaseHex.Q == Hex.Q && ArmState.BaseHex.R == Hex.R)
			{
				// Found the arm - capture its program and HOME facing/reach (reset position)
				// Convert simulation ops (FOp) back to arm instructions (FArmInstr)
				TArray<FArmInstr> CapturedProgram = ConvertOpsToProgram(ArmState.Tape);
				
				// Create blueprint tile with captured program and starting facing/reach
				// Get the starting values from the arm actor
				uint8 StartingRot = (uint8)ArmState.HomeFacing; // Default to HomeFacing
				int32 StartingRch = ArmState.HomeArmLen;        // Default to HomeArmLen

				// Find the arm actor to get its StartingRotation and StartingReach
				ABasicArm* ArmActor = nullptr;
				if (UWorld* World = GetWorld())
				{
					for (TActorIterator<ABasicArm> It(World); It; ++It)
					{
						ABasicArm* TestArm = *It;
						if (TestArm && TestArm->SimArmIndex == ArmIndex)
						{
							ArmActor = TestArm;
							break;
						}
					}
				}

				if (ArmActor)
				{
					StartingRot = ArmActor->StartingRotation;
					StartingRch = ArmActor->StartingReach;
				}

				FBlueprintTile BlueprintTile(LocalHex, (uint8)ArmState.HomeFacing, ArmState.HomeArmLen, StartingRot, StartingRch, CapturedProgram, true); // Assume looping for now
				BlueprintTiles.Add(BlueprintTile);
				
				UE_LOG(LogTemp, Warning, TEXT("Blueprint: Captured arm at (%d,%d) - CurrentFacing=%d HomeFacing=%d (using HomeFacing for blueprint)"), 
					Hex.Q, Hex.R, (int32)ArmState.Facing, (int32)ArmState.HomeFacing);
				bFoundArm = true;
				break;
			}
		}
		
		// If no arm found, check for regular tiles
		if (!bFoundArm)
		{
			FTile* Tile = Sim->TileMap.Find(Hex);
			if (Tile)
			{
				// Get tile direction from the tile's Arg field
				uint8 TileDirection = Tile->Arg;
				
				// Regular tile (not an arm)
				BlueprintTiles.Add(FBlueprintTile(LocalHex, Tile->Kind, TileDirection));
				UE_LOG(LogTemp, Warning, TEXT("Found and captured tile at (%d,%d) of type %d"), 
					Hex.Q, Hex.R, (int32)Tile->Kind);
			}
			else
			{
				UE_LOG(LogTemp, Warning, TEXT("No tile or arm found at (%d,%d)"), Hex.Q, Hex.R);
			}
		}
	}
	
	UE_LOG(LogTemp, Log, TEXT("Created blueprint with %d tiles"), BlueprintTiles.Num());
	
	// Enable blueprint placement mode after creating a blueprint
	if (BlueprintTiles.Num() > 0)
	{
		bBlueprintPlacementMode = true;
		BlueprintRotation = 0;
		BlueprintPlacementOffset = FHex(0, 0);
		
		// Disable HexPlacementComponent preview when in blueprint mode
		if (Placer)
		{
			Placer->SetBlueprintMode(true);
		}
	}
}

void AHexTopDownPawn::ClearSelection()
{
	SelectedTiles.Empty();
	BlueprintTiles.Empty();
	bBlueprintSelectionActive = false;
}

void AHexTopDownPawn::CancelBlueprintPlacement()
{
	UE_LOG(LogTemp, Warning, TEXT("Canceling blueprint placement"));
	
	// Exit blueprint placement mode and return to normal kind placement
	bBlueprintPlacementMode = false;
	bBlueprintMode = false;
	ClearSelection();
	
	// Re-enable HexPlacementComponent preview when exiting blueprint mode
	if (Placer)
	{
		Placer->SetBlueprintMode(false);
	}
	
	// Clear any HUD selection box that might still be active
	APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	if (PC)
	{
		AAutomationGameHUD* GameHUD = Cast<AAutomationGameHUD>(PC->GetHUD());
		if (GameHUD)
		{
			GameHUD->EndSelectionBox();
		}
	}
}

void AHexTopDownPawn::PlaceBlueprint()
{
	if (BlueprintTiles.Num() == 0) return;
	
	// Find the simulation world
	AOrbSimDriver* Driver = AOrbSimDriver::Find(GetWorld());
	if (!Driver) return;
	
	USimWorldHex* Sim = Driver->GetSim();
	if (!Sim) return;
	
	// Get the current mouse position in world coordinates
	FVector2D MousePosition;
	if (!GetWorld()->GetFirstPlayerController() || 
		!GetWorld()->GetFirstPlayerController()->GetMousePosition(MousePosition.X, MousePosition.Y))
	{
		return;
	}
	
	// Get tile size and grid info from driver
	float TileSize = Driver ? Driver->GetTileSize() : 100.f;
	EHexOrientation Orientation = Driver ? Driver->GetGridOrientation() : EHexOrientation::PointyTop;
	float GridPlaneZ = Driver ? Driver->GetGridPlaneZ(this) : 0.f;
	
	// Convert mouse position to world coordinates by projecting ray to the grid plane
	FVector WorldPosition, WorldDirection;
	GetWorld()->GetFirstPlayerController()->DeprojectScreenPositionToWorld(
		MousePosition.X, MousePosition.Y, WorldPosition, WorldDirection);
	
	// Project the ray down to the grid plane (Z = GridPlaneZ)
	if (FMath::Abs(WorldDirection.Z) > KINDA_SMALL_NUMBER)
	{
		float T = (GridPlaneZ - WorldPosition.Z) / WorldDirection.Z;
		WorldPosition = WorldPosition + WorldDirection * T;
	}
	
	// Convert world coordinates to hex coordinates
	FHexAxial TargetHexAxial = HexCoord::XYToAxialRound(FVector2D(WorldPosition.X, WorldPosition.Y), TileSize, Orientation);
	FHex TargetHex(TargetHexAxial.Q, TargetHexAxial.R);
	
	// Apply blueprint rotation to the placement offset
	FHex RotatedOffset = BlueprintPlacementOffset;
	if (BlueprintRotation > 0)
	{
		// Rotate the offset by the blueprint rotation
		for (uint8 i = 0; i < BlueprintRotation; i++)
		{
			RotatedOffset = HexUtil::Neighbor(RotatedOffset, EHexDir::D1); // 60 degree rotation
		}
	}
	
	// Place each tile in the blueprint
	for (const FBlueprintTile& BlueprintTile : BlueprintTiles)
	{
		// Calculate the final hex position
		FHex FinalHex = TargetHex;
		FinalHex.Q += BlueprintTile.LocalHex.Q + RotatedOffset.Q;
		FinalHex.R += BlueprintTile.LocalHex.R + RotatedOffset.R;
		
		if (BlueprintTile.bIsArm)
		{
			// Use GameManager to spawn arm (same as HexPlacementComponent does)
			// Find GameManager and let it handle both spawning and simulation
			AGameManager* GameManager = nullptr;
			if (UWorld* World = GetWorld())
			{
				for (TActorIterator<AGameManager> It(World); It; ++It)
				{
					GameManager = *It;
					break;
				}
			}
			
			if (GameManager)
			{
				UE_LOG(LogTemp, Warning, TEXT("Blueprint: Placing arm at (%d,%d) with facing=%d reach=%d"), 
					FinalHex.Q, FinalHex.R, BlueprintTile.ArmFacing, BlueprintTile.ArmReach);
				
				// Use GameManager to spawn the arm with the captured program
				ABasicArm* SpawnedArm = GameManager->SpawnBasicArmAt(
					FinalHex, BlueprintTile.ArmFacing, BlueprintTile.ArmProgram, BlueprintTile.bArmLoopProgram);
				
				if (SpawnedArm)
				{
					// Set the captured reach distance (reset position)
					SpawnedArm->ReachTiles = BlueprintTile.StartingReach;
					SpawnedArm->StartingReach = BlueprintTile.StartingReach;

					// Set the captured rotation (reset position)
					SpawnedArm->FacingDir = BlueprintTile.StartingRotation % 6;
					SpawnedArm->StartingRotation = BlueprintTile.StartingRotation % 6;

					// Also update the simulation to match the captured reset reach and rotation
					if (SpawnedArm->SimArmIndex >= 0 && SpawnedArm->SimArmIndex < Sim->Arms.Num())
					{
						FArmState& ArmState = Sim->Arms[SpawnedArm->SimArmIndex];
						ArmState.Facing = (EHexDir)(BlueprintTile.StartingRotation % 6);
						ArmState.HomeFacing = (EHexDir)(BlueprintTile.StartingRotation % 6);
						ArmState.ArmLen = BlueprintTile.StartingReach;
						ArmState.HomeArmLen = BlueprintTile.StartingReach;

						UE_LOG(LogTemp, Warning, TEXT("Blueprint: Sim state for spawned arm - Facing=%d HomeFacing=%d"), 
							(int32)ArmState.Facing, (int32)ArmState.HomeFacing);
					}
					
					UE_LOG(LogTemp, Warning, TEXT("Blueprint: Spawned arm at (%d,%d) - FacingDir=%d StartFacingDir=%d SimIndex=%d"), 
						FinalHex.Q, FinalHex.R, SpawnedArm->FacingDir, SpawnedArm->GetStartFacingDir(), SpawnedArm->SimArmIndex);
				}
				else
				{
					UE_LOG(LogTemp, Warning, TEXT("GameManager failed to spawn arm at (%d,%d)"), FinalHex.Q, FinalHex.R);
				}
			}
			else
			{
				UE_LOG(LogTemp, Warning, TEXT("No GameManager found - cannot spawn arm at (%d,%d)"), FinalHex.Q, FinalHex.R);
			}
		}
		else
		{
			// Create a regular tile
			FTile NewTile;
			NewTile.Kind = BlueprintTile.TileType;
			NewTile.Arg = BlueprintTile.TileDirection;
			
			// Place the tile in the simulation
			Sim->PlaceTile(FinalHex, NewTile);
			
			UE_LOG(LogTemp, Log, TEXT("Placed tile at (%d,%d) type %d"), 
				FinalHex.Q, FinalHex.R, (int32)BlueprintTile.TileType);
		}
	}
	
	UE_LOG(LogTemp, Log, TEXT("Placed blueprint with %d tiles"), BlueprintTiles.Num());
	
	// Exit blueprint placement mode after successful placement
	bBlueprintPlacementMode = false;
	bBlueprintMode = false;
	ClearSelection();
	
	// Re-enable HexPlacementComponent preview
	if (Placer)
	{
		Placer->SetBlueprintMode(false);
	}
}

void AHexTopDownPawn::RotateBlueprintCW()
{
	if (BlueprintTiles.Num() == 0) return;
	
	BlueprintRotation = (BlueprintRotation + 1) % 6;
	UE_LOG(LogTemp, Log, TEXT("Blueprint rotation: %d"), BlueprintRotation);
}

void AHexTopDownPawn::RotateBlueprintCCW()
{
	if (BlueprintTiles.Num() == 0) return;
	
	BlueprintRotation = (BlueprintRotation + 5) % 6; // +5 is equivalent to -1 mod 6
	UE_LOG(LogTemp, Log, TEXT("Blueprint rotation: %d"), BlueprintRotation);
}

void AHexTopDownPawn::UpdateBlueprintPreview()
{
	// TODO: Implement blueprint preview visualization
	// This would show a ghost/preview of where the blueprint would be placed
}

TArray<FArmInstr> AHexTopDownPawn::ConvertOpsToProgram(const TArray<FOp>& Ops)
{
	TArray<FArmInstr> Out;
	for (const FOp& Op : Ops)
	{
		FArmInstr Instr;
		switch (Op.Cmd)
		{
		case ECmd::Wait:     
			Instr.Cmd = EArmCmd::Wait; 
			Instr.Arg = FMath::Max(1, Op.Arg); 
			break;
		case ECmd::Grab:     
			Instr.Cmd = EArmCmd::Grab; 
			Instr.Arg = 0; 
			break;
		case ECmd::Drop:     
			Instr.Cmd = EArmCmd::Drop; 
			Instr.Arg = 0; 
			break;
		case ECmd::RotateR:  
			Instr.Cmd = EArmCmd::RotateCW; 
			Instr.Arg = FMath::Max(1, Op.Arg); 
			break;
		case ECmd::RotateL:  
			Instr.Cmd = EArmCmd::RotateCCW; 
			Instr.Arg = FMath::Max(1, Op.Arg); 
			break;
		case ECmd::Extend:   
			Instr.Cmd = EArmCmd::Extend; 
			Instr.Arg = FMath::Max(1, Op.Arg); 
			break;
		case ECmd::Retract:  
			Instr.Cmd = EArmCmd::Retract; 
			Instr.Arg = FMath::Max(1, Op.Arg); 
			break;
		case ECmd::Reset:    
			Instr.Cmd = EArmCmd::Reset; 
			Instr.Arg = 0; 
			break;
		default:             
			Instr.Cmd = EArmCmd::Wait; 
			Instr.Arg = 1; 
			break;
		}
		Out.Add(Instr);
	}
	return Out;
}

void AHexTopDownPawn::DeletionModePressed()
{
	UE_LOG(LogTemp, Warning, TEXT("DeletionMode pressed - entering deletion selection mode"));
	bDeletionMode = true;
}

void AHexTopDownPawn::DeletionModeReleased()
{
	UE_LOG(LogTemp, Warning, TEXT("DeletionMode released"));
	if (bDeletionSelectionActive)
	{
		// End deletion selection and delete selected tiles
		EndDeletionSelection();
	}
	else
	{
		// Exit deletion mode
		bDeletionMode = false;
	}
}

void AHexTopDownPawn::DeleteTileAndOrbs(const FHex& Hex)
{
	// Find the simulation world
	AOrbSimDriver* Driver = AOrbSimDriver::Find(GetWorld());
	if (!Driver) return;
	
	USimWorldHex* Sim = Driver->GetSim();
	if (!Sim) return;
	
	UE_LOG(LogTemp, Log, TEXT("Deleting tile and orbs at (%d,%d)"), Hex.Q, Hex.R);
	
	// 1. Remove any orbs/items at this hex
	FItem RemovedItem;
	if (Sim->TryTakeItem(Hex, RemovedItem))
	{
		UE_LOG(LogTemp, Log, TEXT("Removed orb/item ID %d at (%d,%d)"), RemovedItem.Id, Hex.Q, Hex.R);
	}
	
	// 2. Remove any arms at this hex (check both simulation and world actors)
	// First remove from simulation
	int32 RemovedArmIndex = -1;
	for (int32 ArmIndex = Sim->Arms.Num() - 1; ArmIndex >= 0; ArmIndex--)
	{
		const FArmState& ArmState = Sim->Arms[ArmIndex];
		if (ArmState.BaseHex.Q == Hex.Q && ArmState.BaseHex.R == Hex.R)
		{
			UE_LOG(LogTemp, Warning, TEXT("HexTopDownPawn::DeleteTileAndOrbs - Removing arm from simulation at (%d,%d) SimIndex=%d (total arms before: %d)"), 
				Hex.Q, Hex.R, ArmIndex, Sim->Arms.Num());
			RemovedArmIndex = ArmIndex;
			Sim->Arms.RemoveAt(ArmIndex);
			UE_LOG(LogTemp, Warning, TEXT("HexTopDownPawn::DeleteTileAndOrbs - After removal, total arms: %d"), Sim->Arms.Num());
			break;
		}
	}
	
	// CRITICAL: Update SimArmIndex for all arms that were shifted down by the removal
	if (RemovedArmIndex >= 0)
	{
		if (UWorld* World = GetWorld())
		{
			for (TActorIterator<ABasicArm> It(World); It; ++It)
			{
				ABasicArm* Arm = *It;
				if (Arm && Arm->SimArmIndex > RemovedArmIndex)
				{
					Arm->SimArmIndex--; // Shift down by 1
					UE_LOG(LogTemp, Warning, TEXT("HexTopDownPawn::DeleteTileAndOrbs - Updated arm at (%d,%d) SimIndex: %d -> %d"), 
						Arm->Q, Arm->R, Arm->SimArmIndex + 1, Arm->SimArmIndex);
				}
			}
		}
	}
	
	// Remove runtime spawned arm actors at this hex
	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<ABasicArm> It(World); It; ++It)
		{
			ABasicArm* Arm = *It;
			if (Arm && Arm->Q == Hex.Q && Arm->R == Hex.R)
			{
				UE_LOG(LogTemp, Log, TEXT("Destroying arm actor at (%d,%d)"), Hex.Q, Hex.R);
				Arm->Destroy();
				break;
			}
		}
	}
	
	// 3. Remove the tile itself
	if (Sim->TileMap.Remove(Hex) > 0)
	{
		UE_LOG(LogTemp, Log, TEXT("Removed tile at (%d,%d)"), Hex.Q, Hex.R);
		Sim->TileRevision++; // Increment revision to trigger visual updates
	}
}

void AHexTopDownPawn::DeleteSelectedTilesAndOrbs()
{
	UE_LOG(LogTemp, Log, TEXT("Deleting %d selected tiles and their orbs"), SelectedTiles.Num());
	
	for (const FHex& Hex : SelectedTiles)
	{
		DeleteTileAndOrbs(Hex);
	}
}

void AHexTopDownPawn::RemoveTileAndOrbsAtCursor()
{
	if (!Placer) return;
	
	// Use the placement component's hover hex detection
	if (Placer->IsHoverValid())
	{
		FHex CursorHex = Placer->GetHoverHex();
		UE_LOG(LogTemp, Log, TEXT("Removing tile and orbs at cursor position (%d,%d)"), CursorHex.Q, CursorHex.R);
		DeleteTileAndOrbs(CursorHex);
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("No valid hex under cursor for removal"));
	}
}

void AHexTopDownPawn::StartDeletionSelection()
{
	UE_LOG(LogTemp, Warning, TEXT("StartDeletionSelection called"));
	
	if (!bDeletionMode) return;
	
	APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	if (!PC) return;
	
	FVector2D MousePosition;
	if (PC->GetMousePosition(MousePosition.X, MousePosition.Y))
	{
		SelectionStart = MousePosition;
		SelectionEnd = MousePosition;
		bDeletionSelectionActive = true;
		
		// Start HUD selection box (reuse the same box but with different purpose)
		AAutomationGameHUD* GameHUD = Cast<AAutomationGameHUD>(PC->GetHUD());
		if (GameHUD)
		{
			GameHUD->StartSelectionBox(MousePosition);
		}
		
		UE_LOG(LogTemp, Warning, TEXT("Deletion selection started at (%f, %f)"), MousePosition.X, MousePosition.Y);
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("Failed to get mouse position for deletion selection"));
	}
}

void AHexTopDownPawn::EndDeletionSelection()
{
	UE_LOG(LogTemp, Warning, TEXT("EndDeletionSelection called"));
	
	if (!bDeletionMode || !bDeletionSelectionActive) return;
	
	bDeletionSelectionActive = false;
	bDeletionMode = false; // Exit deletion mode after selection
	
	// End HUD selection box
	APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	if (PC)
	{
		AAutomationGameHUD* GameHUD = Cast<AAutomationGameHUD>(PC->GetHUD());
		if (GameHUD)
		{
			GameHUD->EndSelectionBox();
		}
	}
	
	// Delete selected tiles immediately
	if (SelectedTiles.Num() > 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("Deleting %d selected tiles"), SelectedTiles.Num());
		DeleteSelectedTilesAndOrbs();
		ClearSelection();
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("No tiles selected for deletion"));
	}
}

void AHexTopDownPawn::UpdateDeletionSelection()
{
	if (!bDeletionSelectionActive || !GetWorld()) return;
	
	// Use the same logic as UpdateSelection but for deletion
	FVector2D MousePosition;
	if (GetWorld()->GetFirstPlayerController() && 
		GetWorld()->GetFirstPlayerController()->GetMousePosition(MousePosition.X, MousePosition.Y))
	{
		SelectionEnd = MousePosition;
		
		// Update selected tiles based on current selection box
		SelectedTiles.Empty();
		
		// Get tile size and grid info from driver
		AOrbSimDriver* Driver = AOrbSimDriver::Find(GetWorld());
		float TileSize = Driver ? Driver->GetTileSize() : 100.f;
		EHexOrientation Orientation = Driver ? Driver->GetGridOrientation() : EHexOrientation::PointyTop;
		float GridPlaneZ = Driver ? Driver->GetGridPlaneZ(this) : 0.f;
		
		// Convert screen coordinates to world coordinates by projecting rays to the grid plane
		FVector WorldStart, WorldEnd;
		FVector WorldStartDir, WorldEndDir;
		
		// Deproject both screen points to get rays
		GetWorld()->GetFirstPlayerController()->DeprojectScreenPositionToWorld(
			SelectionStart.X, SelectionStart.Y, WorldStart, WorldStartDir);
		GetWorld()->GetFirstPlayerController()->DeprojectScreenPositionToWorld(
			SelectionEnd.X, SelectionEnd.Y, WorldEnd, WorldEndDir);
		
		// Project the rays down to the grid plane (Z = GridPlaneZ)
		if (FMath::Abs(WorldStartDir.Z) > KINDA_SMALL_NUMBER)
		{
			float T = (GridPlaneZ - WorldStart.Z) / WorldStartDir.Z;
			WorldStart = WorldStart + WorldStartDir * T;
		}
		if (FMath::Abs(WorldEndDir.Z) > KINDA_SMALL_NUMBER)
		{
			float T = (GridPlaneZ - WorldEnd.Z) / WorldEndDir.Z;
			WorldEnd = WorldEnd + WorldEndDir * T;
		}
		
		// Convert world coordinates to hex coordinates
		FHexAxial HexStartAxial = HexCoord::XYToAxialRound(FVector2D(WorldStart.X, WorldStart.Y), TileSize, Orientation);
		FHexAxial HexEndAxial = HexCoord::XYToAxialRound(FVector2D(WorldEnd.X, WorldEnd.Y), TileSize, Orientation);
		FHex HexStart(HexStartAxial.Q, HexStartAxial.R);
		FHex HexEnd(HexEndAxial.Q, HexEndAxial.R);
		
		// Create selection box bounds
		int32 MinQ = FMath::Min(HexStart.Q, HexEnd.Q);
		int32 MaxQ = FMath::Max(HexStart.Q, HexEnd.Q);
		int32 MinR = FMath::Min(HexStart.R, HexEnd.R);
		int32 MaxR = FMath::Max(HexStart.R, HexEnd.R);
		
		// Add all hexes in the selection box
		for (int32 q = MinQ; q <= MaxQ; q++)
		{
			for (int32 r = MinR; r <= MaxR; r++)
			{
				SelectedTiles.Add(FHex(q, r));
			}
		}
		
		UE_LOG(LogTemp, Warning, TEXT("Deletion selection: %d tiles in area Q(%d-%d) R(%d-%d)"), 
			SelectedTiles.Num(), MinQ, MaxQ, MinR, MaxR);
	}
}
