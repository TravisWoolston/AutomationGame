#include "Framework/GameManager.h"
#include "Framework/GameTickManager.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Actors/BasicArm.h"
#include "Actors/OrbSimDriver.h"
#include "Sim/HexCoord.h"
#include "Sim/SimWorldHex.h"          // <-- ADDED for USimWorldHex / FArmState
#include "DrawDebugHelpers.h"
#include "Sim/HexTypes.h" // <-- ADDED for HexUtil
#include "Geometry/GoldbergGrid.h" // NEW
#include "UI/ArmTimelineWidget.h"
#include "Engine/Texture2D.h"
#include "Sim/OrbTypes.h"
#include "Data/OrbMaterialManager.h"

// Local helper to build orthonormal basis from Up + arbitrary forward (similar to HexFactoryActor)
namespace {
	inline void BuildSafeBasisGM(const FVector& InUpRaw, const FVector& InForwardRaw, FVector& OutF, FVector& OutR, FVector& OutU)
	{
		OutU = InUpRaw.GetSafeNormal(); if (OutU.IsNearlyZero()) OutU = FVector::UpVector;
		FVector F = InForwardRaw - OutU * FVector::DotProduct(InForwardRaw, OutU);
		if (F.SizeSquared() < KINDA_SMALL_NUMBER)
		{
			F = FVector::CrossProduct(OutU, FVector::RightVector);
			if (F.SizeSquared() < KINDA_SMALL_NUMBER) F = FVector::CrossProduct(OutU, FVector::ForwardVector);
		}
		OutF = F.GetSafeNormal();
		OutR = FVector::CrossProduct(OutF, OutU).GetSafeNormal();
		if (OutR.IsNearlyZero())
		{
			FVector Aux = FVector::CrossProduct(OutU, FVector::ForwardVector); if (Aux.IsNearlyZero()) Aux = FVector::CrossProduct(OutU, FVector::RightVector);
			OutF = Aux.GetSafeNormal();
			OutR = FVector::CrossProduct(OutF, OutU).GetSafeNormal();
		}
		OutF = FVector::CrossProduct(OutU, OutR).GetSafeNormal();
	}
}

AGameManager::AGameManager()
{
	PrimaryActorTick.bCanEverTick = true; // needed for auto-recenter

	// Initialize timeline system defaults
	bShowArmTimelines = false;
	TimelineWidgetScale = 1.0f;
	TimelineWidgetOffset = FVector(0.0f, 0.0f, 100.0f);
}

void AGameManager::BeginPlay()
{
	Super::BeginPlay();
	AutoFindRefs();
	TryLoadInitial();
	EnsureISM();
	Rebuild();

	const FVector AnchorLoc = GridAnchor ? GridAnchor->GetActorLocation() : GetActorLocation();
	LastGridCenter = AnchorLoc;

	UE_LOG(LogTemp, Log, TEXT("GameManager::BeginPlay - TickManager=%s, OrbSimDriver=%s, Anchor=(%.1f,%.1f,%.1f), TileSize=%.1f, Radius=%d"),
		TickManager ? *TickManager->GetName() : TEXT("null"),
		OrbSimDriver ? *OrbSimDriver->GetName() : TEXT("null"),
		AnchorLoc.X, AnchorLoc.Y, AnchorLoc.Z, TileSizeUU, Radius);

	// Apply timeline visibility settings to all existing arms
	UpdateAllArmTimelineWidgets();
}

void AGameManager::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!bAutoExpandAtRuntime) return;

	const FVector AnchorLoc = GridAnchor ? GridAnchor->GetActorLocation() : GetActorLocation();
	const float Dist = FVector::Dist2D(LastGridCenter, AnchorLoc);
	if (Dist > RecenterThresholdUU)
	{
		UE_LOG(LogTemp, Verbose, TEXT("GameManager::Tick - recentering grid (%.1f uu)"), Dist);
		Rebuild();
		LastGridCenter = AnchorLoc;
	}
}

#if WITH_EDITOR
void AGameManager::OnConstruction(const FTransform&)
{
	EnsureISM();
	Rebuild();
}

void AGameManager::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	// Clear icon cache if ClusterIconScale changed
	if (PropertyChangedEvent.Property && PropertyChangedEvent.Property->GetFName() == GET_MEMBER_NAME_CHECKED(AGameManager, ClusterIconScale))
	{
		ClusterIcons.Empty();
		UE_LOG(LogTemp, Log, TEXT("GameManager: ClusterIconScale changed - cleared icon cache"));
	}
}
#endif

void AGameManager::AutoFindRefs()
{
	UWorld* W = GetWorld(); if (!W) return;

	if (!TickManager)
		for (TActorIterator<AGameTickManager> It(W); It; ++It) { TickManager = *It; break; }

	if (!OrbSimDriver)
		for (TActorIterator<AOrbSimDriver> It(W); It; ++It) { OrbSimDriver = *It; break; }  // <-- typed

	if (!OrbSimDriver && OrbSimDriverClass)
	{
		FActorSpawnParameters S; S.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		OrbSimDriver = W->SpawnActor<AOrbSimDriver>(OrbSimDriverClass, GetActorTransform(), S); // <-- typed
	}

	// Sync OrbSimDriver parameters with GameManager (GameManager is the source of truth)
	SyncOrbSimDriverParameters();
}

void AGameManager::SetGridRadius(int32 NewRadius)
{
	Radius = FMath::Max(1, NewRadius);
	UE_LOG(LogTemp, Log, TEXT("GameManager::SetGridRadius - Radius=%d"), Radius);
	Rebuild();
}

void AGameManager::SetTileMeshAndMaterial(UStaticMesh* Mesh, UMaterialInterface* Mat)
{
	TileMesh = Mesh;
	TileMaterial = Mat;
	UE_LOG(LogTemp, Log, TEXT("GameManager::SetTileMeshAndMaterial - Mesh=%s, Mat=%s"),
		TileMesh ? *TileMesh->GetName() : TEXT("null"),
		TileMaterial ? *TileMaterial->GetName() : TEXT("null"));
	Rebuild();
}

void AGameManager::TryLoadInitial()
{
	if (InitialMapToLoad.IsEmpty() || !OrbSimDriver) return;
	const FString Cmd = FString::Printf(TEXT("LoadMap %s"), *InitialMapToLoad);
	if (!OrbSimDriver->CallFunctionByNameWithArguments(*Cmd, *GLog, nullptr, true))
	{
		UE_LOG(LogTemp, Warning, TEXT("GameManager: OrbSimDriver didn't accept Exec 'LoadMap'."));
	}
	else
	{
		UE_LOG(LogTemp, Log, TEXT("GameManager::TryLoadInitial - Requested map '%s'"), *InitialMapToLoad);
	}
}

void AGameManager::EnsureISM()
{
	if (!ISM)
	{
		ISM = FindComponentByClass<UInstancedStaticMeshComponent>();
		if (!ISM)
		{
			ISM = NewObject<UInstancedStaticMeshComponent>(this, TEXT("GridISM"));
			ISM->SetupAttachment(GetRootComponent());
			ISM->RegisterComponent();
		}
		ISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	}
}

void AGameManager::BuildGridInstances()
{
	if (!ISM) return;
	ISM->ClearInstances();
	if (TileMesh) ISM->SetStaticMesh(TileMesh);
	if (TileMaterial) ISM->SetMaterial(0, TileMaterial);

	const float TileSpacing = TileSizeUU; // alias for clarity
	const float S = FMath::Max(1.f, TileSpacing);
	const FVector Origin = GridAnchor ? GridAnchor->GetActorLocation() : GetActorLocation();
	const float OriYawOffsetDegPlanar = (GridOrientation == EHexOrientation::PointyTop) ? 30.f : 0.f;
	USimWorldHex* Sim = OrbSimDriver ? OrbSimDriver->GetSim() : nullptr;

	if (bSphereGrid && bUseGoldbergSphere)
	{
		if (!GoldbergGrid) GoldbergGrid = NewObject<UGoldbergGrid>(this, TEXT("GoldbergGrid"));
		GoldbergGrid->Build(GoldbergFrequency, GetEffectiveSphereRadius(), Origin);
		const TArray<FGoldbergCell>& Cells = GoldbergGrid->GetCells();
		const bool bPointy = (GridOrientation == EHexOrientation::PointyTop);
		for (int32 i = 0;i < Cells.Num();++i)
		{
			const FGoldbergCell& C = Cells[i];
			if (C.bPentagon) continue; // leave pentagon slots empty for now
			FVector Up = (C.Position - Origin).GetSafeNormal(); if (Up.IsNearlyZero()) Up = FVector::UpVector;
			FVector East = FVector::CrossProduct(FVector::UpVector, Up); if (East.IsNearlyZero()) East = FVector::RightVector; East.Normalize();
			FVector North = FVector::CrossProduct(Up, East).GetSafeNormal();
			// Unreal forward = X (use East), right = Y (use North), up = Z
			FMatrix M(East, North, Up, FVector::ZeroVector);
			FTransform XForm(M.Rotator(), C.Position, FVector(1));
			if (bPointy)
			{
				// Apply same 30 deg yaw offset used in planar grid so hex mesh aligns consistently
				FQuat OffsetRot(Up, FMath::DegreesToRadians(30.f));
				XForm.ConcatenateRotation(OffsetRot);
			}
			ISM->AddInstance(XForm);
		}
		return; // done
	}

	if (bSphereGrid && OrbSimDriver)
	{
		TArray<FHex> Hexes; Hexes.Reserve((Radius * 2 + 1) * (Radius * 2 + 1));
		float MinX = FLT_MAX, MaxX = -FLT_MAX, MinY = FLT_MAX, MaxY = -FLT_MAX;
		for (int r = -Radius;r <= Radius;++r) { for (int q = -Radius;q <= Radius;++q) { int s = -q - r; if (FMath::Max3(FMath::Abs(q), FMath::Abs(r), FMath::Abs(s)) > Radius) continue; Hexes.Add(FHex(q, r)); FVector2D XY = HexCoord::AxialToXY(FHexAxial(q, r), S, GridOrientation); MinX = FMath::Min(MinX, XY.X); MaxX = FMath::Max(MaxX, XY.X); MinY = FMath::Min(MinY, XY.Y); MaxY = FMath::Max(MaxY, XY.Y); } }
		const float SphereR = GetEffectiveSphereRadius();
		OrbSimDriver->SetSphereMappingExtents(MinX, MaxX, MinY, MaxY, SphereR);
		for (const FHex& H : Hexes) { if (Sim && Sim->TileMap.Contains(H)) { const FTile& PT = Sim->TileMap[H]; if (PT.Kind == ETileKind::Output) continue; } FVector P = OrbSimDriver->MapHexToWorld(H); FVector Radial = (P - Origin).GetSafeNormal(); if (Radial.IsNearlyZero()) Radial = FVector::UpVector; FVector East = FVector::CrossProduct(FVector::UpVector, Radial); if (East.IsNearlyZero()) East = FVector::CrossProduct(FVector::RightVector, Radial); East.Normalize(); FVector North = FVector::CrossProduct(Radial, East).GetSafeNormal(); FMatrix M(East, North, Radial, FVector::ZeroVector); FTransform X(M.Rotator(), P, FVector(1)); if (GridOrientation == EHexOrientation::PointyTop) { FQuat Off(Radial, FMath::DegreesToRadians(30.f)); X.ConcatenateRotation(Off); } ISM->AddInstance(X); }
		return;
	}
	// planar path
	const auto ToWorld = [S, Origin, this](int32 Q, int32 R) { FVector2D XY = HexCoord::AxialToXY(FHexAxial(Q, R), S, GridOrientation); FVector2D RotXY = HexCoord::Rotate2D(XY, GridYawDeg); return Origin + FVector(RotXY.X, RotXY.Y, 0.f); };
	for (int r = -Radius;r <= Radius;++r) { for (int q = -Radius;q <= Radius;++q) { int s = -q - r; if (FMath::Max3(FMath::Abs(q), FMath::Abs(r), FMath::Abs(s)) > Radius) continue; if (Sim && Sim->TileMap.Contains(FHex(q, r))) { const FTile& PT = Sim->TileMap[FHex(q, r)]; if (PT.Kind == ETileKind::Output) continue; } FVector P = ToWorld(q, r); FRotator R(0.f, GridYawDeg + OriYawOffsetDegPlanar, 0.f); ISM->AddInstance(FTransform(R, P, FVector(1))); } }
}

void AGameManager::Rebuild()
{
	EnsureISM();
	BuildGridInstances();
	// Sync OrbSimDriver whenever we rebuild the grid
	SyncOrbSimDriverParameters();
}

// Local helper to mirror TestLevelDirector conversion so runtime-placed arms work too.
static TArray<FOp> ConvertProgramToOps(const TArray<FArmInstr>& Program)
{
	TArray<FOp> Out;
	if (Program.Num() == 0)
	{
		Out.Add({ ECmd::Wait, 1 });
		return Out;
	}
	for (const FArmInstr& I : Program)
	{
		switch (I.Cmd)
		{
		case EArmCmd::NoOp:      Out.Add({ ECmd::Wait, 1 }); break;
		case EArmCmd::Grab:      Out.Add({ ECmd::Grab, 0 }); break;
		case EArmCmd::Drop:      Out.Add({ ECmd::Drop, 0 }); break;
		case EArmCmd::RotateCW:  Out.Add({ ECmd::RotateR, (uint8)FMath::Max(1, I.Arg) }); break;
		case EArmCmd::RotateCCW: Out.Add({ ECmd::RotateL, (uint8)FMath::Max(1, I.Arg) }); break;
		case EArmCmd::Wait:      Out.Add({ ECmd::Wait, (uint8)FMath::Clamp(I.Arg, 0, 255) }); break;
		case EArmCmd::Reset:     Out.Add({ ECmd::Reset, 0 }); break;
		case EArmCmd::Extend:    Out.Add({ ECmd::Extend,  (uint8)FMath::Clamp(I.Arg == 0 ? 1 : I.Arg, 0, 255) }); break;  // NEW
		case EArmCmd::Retract:   Out.Add({ ECmd::Retract, (uint8)FMath::Clamp(I.Arg == 0 ? 1 : I.Arg, 0, 255) }); break;  // NEW
		default:                 Out.Add({ ECmd::Wait, 1 }); break;
		}
	}
	return Out;
}

ABasicArm* AGameManager::SpawnBasicArmAt(const FHex& Hex, uint8 Facing, const TArray<FArmInstr>& Program, bool bLoop)
{
	if (!BasicArmClass)
	{
		UE_LOG(LogTemp, Warning, TEXT("GameManager: BasicArmClass not set; cannot spawn."));
		return nullptr;
	}
	if (!OrbSimDriver)
	{
		UE_LOG(LogTemp, Warning, TEXT("GameManager: OrbSimDriver not set; cannot compute grid transform for arm."));
		return nullptr;
	}

	FVector Loc = OrbSimDriver->MapHexToWorld(Hex);
	float Yaw = OrbSimDriver->GetGridYaw() + 60.f * float(Facing % 6);

	FActorSpawnParameters S; S.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	// Spawn at neutral rotation - bone rotation system will handle facing direction
	ABasicArm* Arm = GetWorld()->SpawnActor<ABasicArm>(BasicArmClass, FTransform(FRotator::ZeroRotator, Loc), S);
	if (!Arm) return nullptr;

	// Initialize actor-side data
	Arm->Driver = OrbSimDriver;
	Arm->Q = Hex.Q;
	Arm->R = Hex.R;
	Arm->FacingDir = Facing % 6;
	Arm->StartingRotation = Facing % 6;
	Arm->StartingReach = Arm->ReachTiles; // Use current ReachTiles as starting reach
	// Don't call SetProgram yet - need SimArmIndex first

	// --- Add to simulation so it actually runs ---
	if (USimWorldHex* Sim = OrbSimDriver->GetSim())
	{
		FArmState ArmState;
		ArmState.BaseHex = Hex;
		ArmState.Facing = static_cast<EHexDir>(Facing % 6);
		ArmState.HomeFacing = static_cast<EHexDir>(Arm->StartingRotation % 6);
		ArmState.HomeArmLen = FMath::Clamp(Arm->StartingReach, 1, 6);
		ArmState.HomeTrackIdx = 0;
		ArmState.ArmLen = FMath::Clamp(Arm->ReachTiles, 1, 6);
		ArmState.TrackIdx = 0;
		ArmState.PC = 0;
		ArmState.bHolding = false;
		ArmState.WaitCounter = 0;                                         // (if field exists) NEW
		// Don't convert program here - let SetProgram handle it after SimArmIndex is assigned
		ArmState.Tape.Add({ ECmd::Wait, 1 }); // Just add a default wait

		const int32 ArmIndex = Sim->AddArm(ArmState);
		Arm->SimArmIndex = ArmIndex;

		// Now that SimArmIndex is set, we can call SetProgram which will properly expand Reset commands
		Arm->SetProgram(Program, bLoop);
		Arm->Start();

		UE_LOG(LogTemp, Warning, TEXT("GameManager: Spawned + registered arm at Hex(%d,%d) Facing=%d SimIndex=%d TapeLen=%d"),
			Hex.Q, Hex.R, (int32)ArmState.Facing, ArmIndex, ArmState.Tape.Num());
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("GameManager: Arm spawned visually but Sim is null; program will not run."));
	}

	// Apply timeline settings to the newly spawned arm
	if (UArmTimelineWidget* TimelineWidget = Arm->FindComponentByClass<UArmTimelineWidget>())
	{
		TimelineWidget->SetCommandTextures(CommandTextures);
		TimelineWidget->SetDefaultCommandTexture(DefaultCommandTexture);
		TimelineWidget->SetIconSize(TimelineIconSize);
		TimelineWidget->SetIconBackgroundColor(TimelineIconBackgroundColor);
		TimelineWidget->SetNormalCommandColor(TimelineNormalCommandColor);
		TimelineWidget->SetCurrentCommandColor(TimelineCurrentCommandColor);
		TimelineWidget->SetCompletedCommandColor(TimelineCompletedCommandColor);
		TimelineWidget->SetTimelineWidgetClass(ArmTimelineWidgetClass);
		TimelineWidget->SetTimelineWidgetVisible(bShowArmTimelines);
		TimelineWidget->SetTimelineScale(TimelineWidgetScale);
		TimelineWidget->SetTimelineOffset(TimelineWidgetOffset);
		TimelineWidget->SetMaxCommandIcons(MaxTimelineIcons);
	}

	return Arm;
}

void AGameManager::SyncOrbSimDriverParameters()
{
	if (!OrbSimDriver) return;
	OrbSimDriver->SetGridParameters(GridOrientation, GridYawDeg, TileSizeUU, GridAnchor);
	OrbSimDriver->ApplySphereSettings(bSphereGrid, GetEffectiveSphereRadius());
	UE_LOG(LogTemp, Log, TEXT("GameManager: Synced OrbSimDriver params Orientation=%d Yaw=%.1f TileSpacing=%.1f SphereR=%.1f Auto=%d"), (int32)GridOrientation, GridYawDeg, TileSizeUU, GetEffectiveSphereRadius(), bAutoSphereRadius ? 1 : 0);
}

FVector AGameManager::GetWorldForHex(const FHex& H) const
{
	if (OrbSimDriver)
	{
		return OrbSimDriver->MapHexToWorld(H);
	}
	// Fallback to previous planar conversion
	const float S = FMath::Max(1.f, TileSizeUU);
	const FVector Origin = GridAnchor ? GridAnchor->GetActorLocation() : GetActorLocation();
	const FVector2D XY = HexCoord::AxialToXY(FHexAxial(H.Q, H.R), S, GridOrientation);
	const FVector2D RotXY = HexCoord::Rotate2D(XY, GridYawDeg);
	return Origin + FVector(RotXY.X, RotXY.Y, 0.f);
}

// ------------ Arm Timeline System Implementation ------------

void AGameManager::SetCommandTextures(const TMap<EArmCmd, UTexture2D*>& Textures)
{
	CommandTextures = Textures;

	UE_LOG(LogTemp, Log, TEXT("GameManager: SetCommandTextures - %d textures set"), Textures.Num());

	// Update all existing arm timeline widgets
	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<ABasicArm> It(World); It; ++It)
		{
			ABasicArm* Arm = *It;
			if (Arm && Arm->IsValidLowLevel())
			{
				UArmTimelineWidget* TimelineWidget = Arm->FindComponentByClass<UArmTimelineWidget>();
				if (TimelineWidget)
				{
					TimelineWidget->SetCommandTextures(CommandTextures);
				}
			}
		}
	}
}

UTexture2D* AGameManager::GetTextureForCommand(EArmCmd Command) const
{
	if (const UTexture2D* const* FoundTexture = CommandTextures.Find(Command))
	{
		return const_cast<UTexture2D*>(*FoundTexture);
	}

	return DefaultCommandTexture;
}

void AGameManager::SetTimelineVisibility(bool bVisible)
{
	bShowArmTimelines = bVisible;


	// Update all existing arm timeline widgets
	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<ABasicArm> It(World); It; ++It)
		{
			ABasicArm* Arm = *It;
			if (Arm && Arm->IsValidLowLevel())
			{
				UArmTimelineWidget* TimelineWidget = Arm->FindComponentByClass<UArmTimelineWidget>();
				if (TimelineWidget)
				{
					TimelineWidget->SetTimelineWidgetVisible(bVisible);
					TimelineWidget->SetTimelineWidgetClass(ArmTimelineWidgetClass);
				}
			}
		}
	}
}

void AGameManager::UpdateArmTimeline(ABasicArm* Arm, int32 CommandIndex, float Progress)
{
	if (!Arm || !bShowArmTimelines)
	{
		return;
	}

	UArmTimelineWidget* TimelineWidget = Arm->FindComponentByClass<UArmTimelineWidget>();
	if (TimelineWidget)
	{
		TimelineWidget->SetCurrentCommand(CommandIndex, Progress);
	}
}

void AGameManager::UpdateAllArmTimelineWidgets()
{

	// Update all existing arm timeline widgets with current settings
	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<ABasicArm> It(World); It; ++It)
		{
			ABasicArm* Arm = *It;
			if (Arm && Arm->IsValidLowLevel())
			{
				UArmTimelineWidget* TimelineWidget = Arm->FindComponentByClass<UArmTimelineWidget>();
				if (TimelineWidget)
				{
					TimelineWidget->SetCommandTextures(CommandTextures);
					TimelineWidget->SetDefaultCommandTexture(DefaultCommandTexture);
					TimelineWidget->SetIconSize(TimelineIconSize);
					TimelineWidget->SetIconBackgroundColor(TimelineIconBackgroundColor);
					TimelineWidget->SetNormalCommandColor(TimelineNormalCommandColor);
					TimelineWidget->SetCurrentCommandColor(TimelineCurrentCommandColor);
					TimelineWidget->SetCompletedCommandColor(TimelineCompletedCommandColor);
					TimelineWidget->SetTimelineWidgetClass(ArmTimelineWidgetClass);
					TimelineWidget->SetTimelineWidgetVisible(bShowArmTimelines);
					TimelineWidget->SetMaxCommandIcons(MaxTimelineIcons);
				}
			}
		}
	}
}

void AGameManager::SetActiveTimeline(UArmTimelineWidget* Timeline)
{
	ActiveTimelineWidget = Timeline;
}

UArmTimelineWidget* AGameManager::GetActiveTimeline() const
{
	return ActiveTimelineWidget;
}

ABasicArm* AGameManager::GetActiveArm() const
{
	if (!ActiveTimelineWidget)
	{
		return nullptr;
	}

	// Find the arm that owns this timeline widget
	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<ABasicArm> It(World); It; ++It)
		{
			ABasicArm* Arm = *It;
			if (Arm && Arm->IsValidLowLevel())
			{
				UArmTimelineWidget* TimelineWidget = Arm->FindComponentByClass<UArmTimelineWidget>();
				if (TimelineWidget == ActiveTimelineWidget)
				{
					return Arm;
				}
			}
		}
	}

	return nullptr;
}

int32 AGameManager::GenerateRandomCluster()
{
	// Get the simulation
	if (!OrbSimDriver)
	{
		UE_LOG(LogTemp, Warning, TEXT("GenerateRandomCluster: No OrbSimDriver found"));
		return -1;
	}

	USimWorldHex* Sim = OrbSimDriver->GetSim();
	if (!Sim)
	{
		UE_LOG(LogTemp, Warning, TEXT("GenerateRandomCluster: No Sim found"));
		return -1;
	}

	// Determine number of unique orb types and total orbs based on difficulty
	// DifficultyValue = TotalOrbs + NumOfUniqueOrbTypes
	// Examples: 4=1+3, 6=2+4 or 4+2, 9=3+6 or 5+4 or 7+2
	int32 NumUniqueTypes = 0;
	int32 TotalOrbs = 0;

	// Generate random valid combinations
	// For difficulty 4: only option is 1 unique type + 3 orbs
	// For difficulty 6: can be 2 types + 4 orbs OR 4 orbs + 2 types (same as 2+4)
	// For difficulty 9: can be 3+6, 4+5, 5+4, 6+3, 7+2, 2+7

	// Start with at least 1 unique type and at least 1 orb
	int32 MinTypes = 1;
	int32 MaxOrbs = DifficultyValue - 1;

	// Randomly choose number of unique types (between 1 and approximately half the difficulty for variety)
	int32 MaxTypes = FMath::Clamp(DifficultyValue / 2, 1, 6); // Cap at 6 orb types max
	NumUniqueTypes = FMath::RandRange(MinTypes, MaxTypes);
	TotalOrbs = DifficultyValue - NumUniqueTypes;

	// Ensure we have at least 1 orb per type minimum
	if (TotalOrbs < NumUniqueTypes)
	{
		// Adjust: set orbs to at least equal types, reduce types if needed
		TotalOrbs = NumUniqueTypes;
		NumUniqueTypes = DifficultyValue - TotalOrbs;
		if (NumUniqueTypes < 1)
		{
			NumUniqueTypes = 1;
			TotalOrbs = DifficultyValue - 1;
		}
	}

	UE_LOG(LogTemp, Log, TEXT("GenerateRandomCluster: Difficulty=%d, UniqueTypes=%d, TotalOrbs=%d"),
		DifficultyValue, NumUniqueTypes, TotalOrbs);

	// Generate list of unique orb types (randomly selected from available types)
	// First, get all orb types that have valid materials from OrbMaterialManager
	TArray<EOrbType> AllValidOrbTypes;
	UOrbMaterialManager* MaterialManager = UOrbMaterialManager::FindInWorld(this);
	if (MaterialManager)
	{
		// Check all possible orb types to see which ones have materials assigned
		TArray<EOrbType> AllOrbTypes = {
			EOrbType::None,
			EOrbType::BaseA,
			EOrbType::BaseB,
			EOrbType::BaseC,
			EOrbType::MetalA,
			EOrbType::CatalystX
		};

		for (EOrbType OrbType : AllOrbTypes)
		{
			// Check if this orb type has a material assigned in the map
			if (const UMaterialInterface* const* FoundMaterial = MaterialManager->OrbTypeMaterials.Find(OrbType))
			{
				// Only add if the material is not null and not the default material
				if (*FoundMaterial != nullptr && *FoundMaterial != MaterialManager->DefaultOrbMaterial)
				{
					AllValidOrbTypes.Add(OrbType);
				}
			}
		}

		UE_LOG(LogTemp, Log, TEXT("GenerateRandomCluster: Found %d valid orb types with materials"), AllValidOrbTypes.Num());
	}

	// If no valid types found from material manager, fallback to hardcoded difficulty-based types
	TArray<EOrbType> AvailableOrbTypes;
	if (AllValidOrbTypes.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("GenerateRandomCluster: No valid orb types from MaterialManager, using fallback"));
		if (DifficultyValue <= 4)
		{
			AvailableOrbTypes = { EOrbType::BaseA };
		}
		else if (DifficultyValue <= 6)
		{
			AvailableOrbTypes = { EOrbType::BaseA, EOrbType::BaseB };
		}
		else if (DifficultyValue <= 9)
		{
			AvailableOrbTypes = { EOrbType::BaseA, EOrbType::BaseB, EOrbType::BaseC };
		}
		else if (DifficultyValue <= 12)
		{
			AvailableOrbTypes = { EOrbType::BaseA, EOrbType::BaseB, EOrbType::BaseC, EOrbType::MetalA };
		}
		else
		{
			AvailableOrbTypes = { EOrbType::BaseA, EOrbType::BaseB, EOrbType::BaseC,
								  EOrbType::MetalA, EOrbType::CatalystX };
		}
	}
	else
	{
		// Sort valid types by enum order to ensure consistent difficulty progression
		AllValidOrbTypes.Sort();

		// Use exponential scaling to determine which types are unlocked based on difficulty
		// For each type beyond BaseA (index 1), calculate its unlock threshold
		for (EOrbType OrbType : AllValidOrbTypes)
		{
			int32 TypeIndex = (int32)OrbType;

			// BaseA (index 1) is always available starting at difficulty 4
			if (TypeIndex == 1) // EOrbType::BaseA
			{
				AvailableOrbTypes.Add(OrbType);
			}
			else if (TypeIndex > 1 && TypeIndex <= 5) // BaseB through CatalystX
			{
				// Calculate unlock threshold for this type using exponential scaling
				// Type 2 (BaseB) unlocks at BaseUnlockDifficulty
				// Type 3 (BaseC) unlocks at BaseUnlockDifficulty * DifficultyScaleMult^1
				// Type 4 (MetalA) unlocks at BaseUnlockDifficulty * DifficultyScaleMult^2
				// Type 5 (CatalystX) unlocks at BaseUnlockDifficulty * DifficultyScaleMult^3
				int32 UnlockTypeIndex = TypeIndex - 2; // 0 for BaseB, 1 for BaseC, etc.
				int32 UnlockThreshold = FMath::RoundToInt(BaseUnlockDifficulty * FMath::Pow(DifficultyScaleMult, UnlockTypeIndex));

				if (DifficultyValue >= UnlockThreshold)
				{
					AvailableOrbTypes.Add(OrbType);
				}
			}
		}

		// Fallback if filtering removed all types
		if (AvailableOrbTypes.Num() == 0)
		{
			AvailableOrbTypes = { EOrbType::BaseA };
		}

		UE_LOG(LogTemp, Log, TEXT("GenerateRandomCluster: Filtered to %d types based on difficulty (BaseUnlock=%d, ScaleMult=%.2f)"),
			AvailableOrbTypes.Num(), BaseUnlockDifficulty, DifficultyScaleMult);
	}

	TArray<EOrbType> SelectedTypes;

	// Shuffle and take NumUniqueTypes
	TArray<EOrbType> Shuffled = AvailableOrbTypes;
	for (int32 i = Shuffled.Num() - 1; i > 0; i--)
	{
		int32 j = FMath::RandRange(0, i);
		Shuffled.Swap(i, j);
	}
	for (int32 i = 0; i < NumUniqueTypes && i < Shuffled.Num(); i++)
	{
		SelectedTypes.Add(Shuffled[i]);
	}

	// Generate random orb positions in a connected cluster
	TSet<FHex> UsedPositions;
	TArray<FOrbData> GeneratedOrbs;
	FHex CenterPos(0, 0);

	// Start with center orb
	FOrbData CenterOrb;
	CenterOrb.Type = (uint8)SelectedTypes[0];
	CenterOrb.dQ = 0;
	CenterOrb.dR = 0;
	GeneratedOrbs.Add(CenterOrb);
	UsedPositions.Add(CenterPos);

	// Define hex neighbor offsets (6 directions)
	TArray<FHex> HexNeighbors = {
		FHex(1, 0), FHex(1, -1), FHex(0, -1),
		FHex(-1, 0), FHex(-1, 1), FHex(0, 1)
	};

	// Add remaining orbs one by one, always adjacent to at least one existing orb
	for (int32 i = 1; i < TotalOrbs; i++)
	{
		// Collect all candidate positions (neighbors of existing orbs that aren't used)
		TArray<FHex> CandidatePositions;
		for (const FHex& ExistingPos : UsedPositions)
		{
			for (const FHex& Neighbor : HexNeighbors)
			{
				FHex Candidate = FHex(ExistingPos.Q + Neighbor.Q, ExistingPos.R + Neighbor.R);
				if (!UsedPositions.Contains(Candidate))
				{
					CandidatePositions.AddUnique(Candidate);
				}
			}
		}

		if (CandidatePositions.Num() == 0)
		{
			UE_LOG(LogTemp, Warning, TEXT("GenerateRandomCluster: Ran out of valid positions at orb %d of %d"), i, TotalOrbs);
			break;
		}

		// Randomly select a candidate position
		int32 RandomIndex = FMath::RandRange(0, CandidatePositions.Num() - 1);
		FHex SelectedPos = CandidatePositions[RandomIndex];

		// Assign a random type from SelectedTypes
		EOrbType RandomType = SelectedTypes[FMath::RandRange(0, SelectedTypes.Num() - 1)];

		FOrbData NewOrb;
		NewOrb.Type = (uint8)RandomType;
		NewOrb.dQ = SelectedPos.Q;
		NewOrb.dR = SelectedPos.R;
		GeneratedOrbs.Add(NewOrb);
		UsedPositions.Add(SelectedPos);
	}

	// Generate bonds: bond each orb to its neighbors in the structure
	TArray<TPair<int32, int32>> Bonds;
	for (int32 i = 0; i < GeneratedOrbs.Num(); i++)
	{
		const FOrbData& Orb1 = GeneratedOrbs[i];
		for (int32 j = i + 1; j < GeneratedOrbs.Num(); j++)
		{
			const FOrbData& Orb2 = GeneratedOrbs[j];
			// Check if they're neighbors (distance = 1 in hex grid)
			// Hex distance in axial coordinates: (|dq1-dq2| + |dq1+dr1-dq2-dr2| + |dr1-dr2|) / 2
			// For adjacent hexes this equals 1
			int32 dQ = Orb1.dQ - Orb2.dQ;
			int32 dR = Orb1.dR - Orb2.dR;
			int32 Dist = (FMath::Abs(dQ) + FMath::Abs(dQ + dR) + FMath::Abs(dR)) / 2;
			if (Dist == 1)
			{
				Bonds.Add(TPair<int32, int32>(i, j));
			}
		}
	}

	// Generate a unique template ID
	uint8 TemplateId = Sim->GenerateNewTemplateId();

	// Register the template
	FString TemplateName = FString::Printf(TEXT("Random_Difficulty%d_Types%d_Orbs%d"),
		DifficultyValue, NumUniqueTypes, GeneratedOrbs.Num());
	Sim->RegisterClusterTemplate(TemplateId, GeneratedOrbs, Bonds, TemplateName);

	UE_LOG(LogTemp, Log, TEXT("GenerateRandomCluster: Registered template ID %d with %d orbs and %d bonds"),
		TemplateId, GeneratedOrbs.Num(), Bonds.Num());

	return (int32)TemplateId;
}

// ------------ Cluster Storage Implementation ------------

// Comparison operator for FClusterCanonicalKey
bool FClusterCanonicalKey::operator==(const FClusterCanonicalKey& Other) const
{
	if (CanonicalOrbs.Num() != Other.CanonicalOrbs.Num()) return false;
	if (CanonicalBonds.Num() != Other.CanonicalBonds.Num()) return false;

	// Compare orbs
	for (int32 i = 0; i < CanonicalOrbs.Num(); i++)
	{
		if (CanonicalOrbs[i].dQ != Other.CanonicalOrbs[i].dQ) return false;
		if (CanonicalOrbs[i].dR != Other.CanonicalOrbs[i].dR) return false;
		if (CanonicalOrbs[i].Type != Other.CanonicalOrbs[i].Type) return false;
	}

	// Compare bonds
	for (int32 i = 0; i < CanonicalBonds.Num(); i++)
	{
		if (CanonicalBonds[i].Key != Other.CanonicalBonds[i].Key) return false;
		if (CanonicalBonds[i].Value != Other.CanonicalBonds[i].Value) return false;
	}

	return true;
}

// Hash function for FClusterCanonicalKey
uint32 GetTypeHash(const FClusterCanonicalKey& Key)
{
	uint32 Hash = 0;

	// Hash all orb data
	for (const FOrbData& Orb : Key.CanonicalOrbs)
	{
		Hash = HashCombine(Hash, ::GetTypeHash(Orb.dQ));
		Hash = HashCombine(Hash, ::GetTypeHash(Orb.dR));
		Hash = HashCombine(Hash, ::GetTypeHash(Orb.Type));
	}

	// Hash all bonds
	for (const TPair<int32, int32>& Bond : Key.CanonicalBonds)
	{
		Hash = HashCombine(Hash, ::GetTypeHash(Bond.Key));
		Hash = HashCombine(Hash, ::GetTypeHash(Bond.Value));
	}

	return Hash;
}

FClusterCanonicalKey AGameManager::GenerateCanonicalKey(const USimWorldHex::FClusterTemplate& ClusterTemplate) const
{
	auto RotateHex = [](int32 Q, int32 R) -> TPair<int32, int32> {
		return TPair<int32, int32>(-R, Q + R);
		};

	auto CanonicalizeConfig = [](const TArray<FOrbData>& Orbs, const TArray<TPair<int32, int32>>& Bonds) -> FClusterCanonicalKey {
		FClusterCanonicalKey Key;

		if (Orbs.Num() == 0)
		{
			return Key;
		}

		// Find the minimum dQ and dR values
		int32 MinQ = Orbs[0].dQ;
		int32 MinR = Orbs[0].dR;
		for (const FOrbData& Orb : Orbs)
		{
			MinQ = FMath::Min(MinQ, Orb.dQ);
			MinR = FMath::Min(MinR, Orb.dR);
		}

		// Normalize all orbs by subtracting the minimum
		for (const FOrbData& Orb : Orbs)
		{
			FOrbData NormalizedOrb = Orb;
			NormalizedOrb.dQ -= MinQ;
			NormalizedOrb.dR -= MinR;
			NormalizedOrb.ItemId = 0; // Exclude ItemId from canonical representation
			Key.CanonicalOrbs.Add(NormalizedOrb);
		}

		// Sort orbs by dQ, then dR, then Type for consistent ordering
		Key.CanonicalOrbs.Sort([](const FOrbData& A, const FOrbData& B) {
			if (A.dQ != B.dQ) return A.dQ < B.dQ;
			if (A.dR != B.dR) return A.dR < B.dR;
			return A.Type < B.Type;
			});

		// Create mapping from old indices to new sorted indices
		TMap<int32, int32> OldToNewIndex;
		for (int32 OldIdx = 0; OldIdx < Orbs.Num(); OldIdx++)
		{
			const FOrbData& OriginalOrb = Orbs[OldIdx];
			for (int32 NewIdx = 0; NewIdx < Key.CanonicalOrbs.Num(); NewIdx++)
			{
				const FOrbData& CanonicalOrb = Key.CanonicalOrbs[NewIdx];
				// Compare normalized coordinates and type
				if (OriginalOrb.dQ - MinQ == CanonicalOrb.dQ &&
					OriginalOrb.dR - MinR == CanonicalOrb.dR &&
					OriginalOrb.Type == CanonicalOrb.Type)
				{
					OldToNewIndex.Add(OldIdx, NewIdx);
					break;
				}
			}
		}

		// Remap bonds to use new sorted indices
		for (const TPair<int32, int32>& OriginalBond : Bonds)
		{
			if (int32* NewKey = OldToNewIndex.Find(OriginalBond.Key))
			{
				if (int32* NewValue = OldToNewIndex.Find(OriginalBond.Value))
				{
					int32 A = FMath::Min(*NewKey, *NewValue);
					int32 B = FMath::Max(*NewKey, *NewValue);
					Key.CanonicalBonds.Add(TPair<int32, int32>(A, B));
				}
			}
		}

		// Sort bonds for consistent ordering
		Key.CanonicalBonds.Sort([](const TPair<int32, int32>& A, const TPair<int32, int32>& B) {
			if (A.Key != B.Key) return A.Key < B.Key;
			return A.Value < B.Value;
			});

		return Key;
		};

	// Generate all six rotations and pick the lexicographically smallest
	FClusterCanonicalKey BestKey;
	bool bFirst = true;
	int32 BestRotation = 0;

	for (int32 Rotation = 0; Rotation < 6; Rotation++)
	{
		// Create rotated version of the cluster
		TArray<FOrbData> RotatedOrbs = ClusterTemplate.Orbs;

		// Apply rotation to all orbs
		for (FOrbData& Orb : RotatedOrbs)
		{
			int32 CurrentQ = Orb.dQ;
			int32 CurrentR = Orb.dR;

			// Apply rotation N times
			for (int32 r = 0; r < Rotation; r++)
			{
				TPair<int32, int32> Rotated = RotateHex(CurrentQ, CurrentR);
				CurrentQ = Rotated.Key;
				CurrentR = Rotated.Value;
			}

			Orb.dQ = CurrentQ;
			Orb.dR = CurrentR;
		}

		// Bonds don't need rotation - they reference orb indices which don't change
		FClusterCanonicalKey CandidateKey = CanonicalizeConfig(RotatedOrbs, ClusterTemplate.Bonds);

		// Compare keys lexicographically
		if (bFirst)
		{
			BestKey = CandidateKey;
			BestRotation = Rotation;
			bFirst = false;
		}
		else
		{
			// Compare orb arrays lexicographically
			bool bCandidateIsLess = false;

			if (CandidateKey.CanonicalOrbs.Num() != BestKey.CanonicalOrbs.Num())
			{
				bCandidateIsLess = CandidateKey.CanonicalOrbs.Num() < BestKey.CanonicalOrbs.Num();
			}
			else
			{
				// Compare orb by orb
				for (int32 i = 0; i < CandidateKey.CanonicalOrbs.Num(); i++)
				{
					const FOrbData& CandOrb = CandidateKey.CanonicalOrbs[i];
					const FOrbData& BestOrb = BestKey.CanonicalOrbs[i];

					if (CandOrb.dQ != BestOrb.dQ)
					{
						bCandidateIsLess = CandOrb.dQ < BestOrb.dQ;
						break;
					}
					if (CandOrb.dR != BestOrb.dR)
					{
						bCandidateIsLess = CandOrb.dR < BestOrb.dR;
						break;
					}
					if (CandOrb.Type != BestOrb.Type)
					{
						bCandidateIsLess = CandOrb.Type < BestOrb.Type;
						break;
					}
				}
			}

			// If orbs are identical, compare bonds
			if (!bCandidateIsLess && CandidateKey.CanonicalOrbs.Num() == BestKey.CanonicalOrbs.Num())
			{
				// First check if all orbs are identical
				bool bOrbsIdentical = true;
				for (int32 i = 0; i < CandidateKey.CanonicalOrbs.Num(); i++)
				{
					const FOrbData& CandOrb = CandidateKey.CanonicalOrbs[i];
					const FOrbData& BestOrb = BestKey.CanonicalOrbs[i];
					if (CandOrb.dQ != BestOrb.dQ || CandOrb.dR != BestOrb.dR || CandOrb.Type != BestOrb.Type)
					{
						bOrbsIdentical = false;
						break;
					}
				}

				if (bOrbsIdentical)
				{
					// Compare bonds
					for (int32 i = 0; i < FMath::Min(CandidateKey.CanonicalBonds.Num(), BestKey.CanonicalBonds.Num()); i++)
					{
						if (CandidateKey.CanonicalBonds[i].Key != BestKey.CanonicalBonds[i].Key)
						{
							bCandidateIsLess = CandidateKey.CanonicalBonds[i].Key < BestKey.CanonicalBonds[i].Key;
							break;
						}
						if (CandidateKey.CanonicalBonds[i].Value != BestKey.CanonicalBonds[i].Value)
						{
							bCandidateIsLess = CandidateKey.CanonicalBonds[i].Value < BestKey.CanonicalBonds[i].Value;
							break;
						}
					}

					if (!bCandidateIsLess && CandidateKey.CanonicalBonds.Num() < BestKey.CanonicalBonds.Num())
					{
						bCandidateIsLess = true;
					}
				}
			}

			if (bCandidateIsLess)
			{
				BestKey = CandidateKey;
				BestRotation = Rotation;
			}
		}
	}

	UE_LOG(LogTemp, Verbose, TEXT("GenerateCanonicalKey: Selected rotation %d as canonical (%d orbs, %d bonds)"),
		BestRotation, BestKey.CanonicalOrbs.Num(), BestKey.CanonicalBonds.Num());

	return BestKey;
}

void AGameManager::AddClusterToStorage(const USimWorldHex::FClusterTemplate& ClusterTemplate)
{
	FClusterCanonicalKey Key = GenerateCanonicalKey(ClusterTemplate);

	// Log first orb of canonical key for debugging
	if (Key.CanonicalOrbs.Num() > 0)
	{
		const FOrbData& FirstOrb = Key.CanonicalOrbs[0];
		UE_LOG(LogTemp, Warning, TEXT("AddClusterToStorage: Canonical key has first orb at dQ=%d, dR=%d, Type=%d"),
			FirstOrb.dQ, FirstOrb.dR, FirstOrb.Type);
	}

	if (int32* ExistingCount = ClusterCounts.Find(Key))
	{
		// Increment existing count
		(*ExistingCount)++;

	}
	else
	{
		// Add new cluster
		ClusterCounts.Add(Key, 1);
		StoredClusterTemplates.Add(Key, ClusterTemplate);

	}
}

int32 AGameManager::GetClusterCount(const USimWorldHex::FClusterTemplate& ClusterTemplate) const
{
	FClusterCanonicalKey Key = GenerateCanonicalKey(ClusterTemplate);
	if (const int32* Count = ClusterCounts.Find(Key))
	{
		return *Count;
	}
	return 0;
}

bool AGameManager::DoesClusterExistInStorage(const USimWorldHex::FClusterTemplate& ClusterTemplate) const
{
	FClusterCanonicalKey Key = GenerateCanonicalKey(ClusterTemplate);
	return ClusterCounts.Contains(Key);
}

bool AGameManager::DoesClusterExistInStorageByTemplateId(uint8 TemplateId) const
{
	if (const FClusterCanonicalKey* CachedKey = TemplateIdToCanonicalKey.Find(TemplateId))
	{
		return ClusterCounts.Contains(*CachedKey);
	}
	return false;
}

void AGameManager::CacheCanonicalKeyForTemplate(uint8 TemplateId, const USimWorldHex::FClusterTemplate& ClusterTemplate)
{
	// Only cache if not already cached (templates shouldn't change structure)
	if (!TemplateIdToCanonicalKey.Contains(TemplateId))
	{
		FClusterCanonicalKey Key = GenerateCanonicalKey(ClusterTemplate);
		TemplateIdToCanonicalKey.Add(TemplateId, Key);
		//UE_LOG(LogTemp, Warning, TEXT("Cached canonical key for template %d (%d orbs, %d bonds)"),
		//	TemplateId, Key.CanonicalOrbs.Num(), Key.CanonicalBonds.Num());
	}
}

void AGameManager::GetStoredClusters(TArray<USimWorldHex::FClusterTemplate>& OutTemplates, TArray<int32>& OutCounts) const
{
	OutTemplates.Empty();
	OutCounts.Empty();

	for (const auto& KV : ClusterCounts)
	{
		if (const USimWorldHex::FClusterTemplate* Template = StoredClusterTemplates.Find(KV.Key))
		{
			OutTemplates.Add(*Template);
			OutCounts.Add(KV.Value);
		}
	}
}

void AGameManager::ClearClusterStorage()
{
	ClusterCounts.Empty();
	StoredClusterTemplates.Empty();
	ClusterIcons.Empty();
	UE_LOG(LogTemp, Log, TEXT("Cleared all cluster storage"));
}

void AGameManager::ClearClusterIcons()
{
	ClusterIcons.Empty();
	UE_LOG(LogTemp, Log, TEXT("Cleared all cluster icons"));
}

UTexture2D* AGameManager::GenerateClusterIcon(const USimWorldHex::FClusterTemplate& ClusterTemplate)
{
	// Create a larger 256x256 texture for better detail
	const int32 IconSize = 256;
	TArray<FColor> Pixels;
	Pixels.SetNum(IconSize * IconSize);

	// Fill with transparent background
	for (FColor& Pixel : Pixels)
	{
		Pixel = FColor(0, 0, 0, 0);
	}

	// Get OrbMaterialManager for colors
	UOrbMaterialManager* MaterialManager = UOrbMaterialManager::FindInWorld(this);

	// Calculate bounds to fit all orbs
	int32 MinQ = INT_MAX, MaxQ = INT_MIN;
	int32 MinR = INT_MAX, MaxR = INT_MIN;
	for (const FOrbData& Orb : ClusterTemplate.Orbs)
	{
		MinQ = FMath::Min(MinQ, Orb.dQ);
		MaxQ = FMath::Max(MaxQ, Orb.dQ);
		MinR = FMath::Min(MinR, Orb.dR);
		MaxR = FMath::Max(MaxR, Orb.dR);
	}

	// Calculate the center of the cluster for proper centering
	float CenterQ = (MinQ + MaxQ) * 0.5f;
	float CenterR = (MinR + MaxR) * 0.5f;

	// Calculate scale to fit everything
	int32 HexWidth = MaxQ - MinQ + 1;
	int32 HexHeight = MaxR - MinR + 1;

	// Use ClusterIconScale to adjust overall icon size
	const float BaseOrbRadius = 18.0f;
	const float OrbRadius = BaseOrbRadius * ClusterIconScale;
	const float Padding = OrbRadius; // Minimal padding - just enough for orb radius

	// Calculate scale with proper padding to fit everything
	const float ScaleX = (IconSize - Padding * 2) / (HexWidth * 1.732f); // sqrt(3) for hex width
	const float ScaleY = (IconSize - Padding * 2) / (HexHeight * 1.5f);
	const float Scale = FMath::Min(ScaleX, ScaleY); // Removed extra 0.85f scaling
	for (const FOrbData& Orb : ClusterTemplate.Orbs)
	{
		// Convert hex coordinates to pixel coordinates (pointy-top hex), offset by cluster center
		float OrbCenterX = ((Orb.dQ - CenterQ) * 1.732f + (Orb.dR - CenterR) * 0.866f) * Scale;
		float OrbCenterY = (Orb.dR - CenterR) * 1.5f * Scale;

		// Center in the icon
		OrbCenterX += IconSize * 0.5f;
		OrbCenterY += IconSize * 0.5f;

		// Get color for this orb type
		EOrbType OrbType = (EOrbType)Orb.Type;
		FLinearColor OrbColor = MaterialManager ? MaterialManager->GetOrbTypeUIColor(OrbType) : FLinearColor::White;
		FColor PixelColor = OrbColor.ToFColor(true);

		// Draw a filled circle
		for (int32 Y = 0; Y < IconSize; Y++)
		{
			for (int32 X = 0; X < IconSize; X++)
			{
				float Dx = X - OrbCenterX;
				float Dy = Y - OrbCenterY;
				float Distance = FMath::Sqrt(Dx * Dx + Dy * Dy);

				if (Distance <= OrbRadius)
				{
					int32 Index = Y * IconSize + X;
					if (Index >= 0 && Index < Pixels.Num())
					{
						Pixels[Index] = PixelColor;
					}
				}
			}
		}
	}

	// Draw bonds as lines - scale thickness with icon scale
	const float BaseLineThickness = 3.0f;
	const float LineThickness = BaseLineThickness * ClusterIconScale;
	for (const TPair<int32, int32>& Bond : ClusterTemplate.Bonds)
	{
		if (Bond.Key >= 0 && Bond.Key < ClusterTemplate.Orbs.Num() &&
			Bond.Value >= 0 && Bond.Value < ClusterTemplate.Orbs.Num())
		{
			const FOrbData& OrbA = ClusterTemplate.Orbs[Bond.Key];
			const FOrbData& OrbB = ClusterTemplate.Orbs[Bond.Value];

			// Convert hex coordinates to pixel coordinates, offset by cluster center
			float X1 = ((OrbA.dQ - CenterQ) * 1.732f + (OrbA.dR - CenterR) * 0.866f) * Scale + IconSize * 0.5f;
			float Y1 = (OrbA.dR - CenterR) * 1.5f * Scale + IconSize * 0.5f;
			float X2 = ((OrbB.dQ - CenterQ) * 1.732f + (OrbB.dR - CenterR) * 0.866f) * Scale + IconSize * 0.5f;
			float Y2 = (OrbB.dR - CenterR) * 1.5f * Scale + IconSize * 0.5f;

			// Draw a line using Bresenham's algorithm
			int32 X1i = FMath::RoundToInt(X1);
			int32 Y1i = FMath::RoundToInt(Y1);
			int32 X2i = FMath::RoundToInt(X2);
			int32 Y2i = FMath::RoundToInt(Y2);

			int32 Dx = FMath::Abs(X2i - X1i);
			int32 Dy = FMath::Abs(Y2i - Y1i);
			int32 Sx = (X1i < X2i) ? 1 : -1;
			int32 Sy = (Y1i < Y2i) ? 1 : -1;
			int32 Err = Dx - Dy;

			int32 X = X1i, Y = Y1i;
			int32 HalfThickness = FMath::Max(1, (int32)(LineThickness / 2.0f));
			while (true)
			{
				// Draw a thicker line by drawing multiple pixels around each point
				for (int32 OffsetY = -HalfThickness; OffsetY <= HalfThickness; OffsetY++)
				{
					for (int32 OffsetX = -HalfThickness; OffsetX <= HalfThickness; OffsetX++)
					{
						int32 DrawX = X + OffsetX;
						int32 DrawY = Y + OffsetY;
						if (DrawX >= 0 && DrawX < IconSize && DrawY >= 0 && DrawY < IconSize)
						{
							int32 Index = DrawY * IconSize + DrawX;
							if (Index >= 0 && Index < Pixels.Num())
							{
								Pixels[Index] = FColor::White;
							}
						}
					}
				}

				if (X == X2i && Y == Y2i) break;

				int32 Err2 = Err * 2;
				if (Err2 > -Dy) { Err -= Dy; X += Sx; }
				if (Err2 < Dx) { Err += Dx; Y += Sy; }
			}
		}
	}

	// Create texture from pixel data
	UTexture2D* Texture = UTexture2D::CreateTransient(IconSize, IconSize, PF_B8G8R8A8);
	if (Texture)
	{
		FTexture2DMipMap& Mip = Texture->GetPlatformData()->Mips[0];
		void* TextureData = Mip.BulkData.Lock(LOCK_READ_WRITE);
		FMemory::Memcpy(TextureData, Pixels.GetData(), Pixels.Num() * sizeof(FColor));
		Mip.BulkData.Unlock();

		Texture->UpdateResource();
	}

	return Texture;
}

UTexture2D* AGameManager::GetClusterIcon(const USimWorldHex::FClusterTemplate& ClusterTemplate)
{
	FClusterCanonicalKey Key = GenerateCanonicalKey(ClusterTemplate);

	// Check if icon already exists
	if (UTexture2D** ExistingIcon = ClusterIcons.Find(Key))
	{
		return *ExistingIcon;
	}

	// Generate new icon
	UTexture2D* NewIcon = GenerateClusterIcon(ClusterTemplate);
	if (NewIcon)
	{
		ClusterIcons.Add(Key, NewIcon);
	}

	return NewIcon;
}
