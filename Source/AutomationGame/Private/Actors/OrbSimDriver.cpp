#include "Actors/OrbSimDriver.h"
#include "Framework/GameTickManager.h"
#include "Framework/GameManager.h"
#include "Sim/SimWorldHex.h"
#include "Engine/World.h"
#include "EngineUtils.h"

AOrbSimDriver::AOrbSimDriver()
{
	// We still Tick() for StepAlpha interpolation, but we DO NOT advance the sim here.
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;
}

void AOrbSimDriver::BeginPlay()
{
	Super::BeginPlay();

	// Ensure we have a sim object
	if (!Sim)
	{
		Sim = NewObject<USimWorldHex>(this);
		if (Sim)
		{
			// Default init radius; change to whatever your game needs.
			Sim->Initialize(20);
			// Keep sim's spacing aligned with our current tile size
			Sim->TileSizeUU = TileSizeUU;
		}
	}

	// Try to auto-find a GameManager if you want to sync knobs (optional)
	if (!GM)
	{
		if (UWorld* W = GetWorld())
		{
			for (TActorIterator<AGameManager> It(W); It; ++It) { GM = *It; break; }
		}
	}
	
	// If GameManager already set our parameters before Sim was created, sync them now
	if (Sim && (GridOrientation != EHexOrientation::PointyTop || GridYawDeg != 0.f || TileSizeUU != 120.f))
	{
		Sim->TileSizeUU = TileSizeUU;
		const FVector AnchorLoc = GridAnchor ? GridAnchor->GetActorLocation() : FVector::ZeroVector;
		Sim->SetWorldTransform(AnchorLoc, GridYawDeg, GridOrientation);
		UE_LOG(LogTemp, Log, TEXT("OrbSimDriver: Synced Sim with pre-set parameters - TileSize=%.1f Orientation=%d Yaw=%.1f"), 
			TileSizeUU, (int32)GridOrientation, GridYawDeg);
	}
	
	// Always sync debug settings when Sim is created
	if (Sim)
	{
		Sim->bDebugLogs = bDebugLogs;
		Sim->bDebugSpheres = bDebugSpheres;
		Sim->bDebugLines = bDebugLines;
		Sim->bDebugBonds = bDebugBonds;
	}
	
	// Sync debug settings to all arms
	SyncDebugSettingsToArms();

	// Subscribe to fixed tick
	if (AGameTickManager* M = AGameTickManager::Get(this))
	{
		if (!bSubscribed)
		{
			M->OnFixedTick.AddDynamic(this, &AOrbSimDriver::HandleFixedTickSeconds);
			bSubscribed = true;
			UE_LOG(LogTemp, Log, TEXT("OrbSimDriver: Bound to GameTickManager::OnFixedTick."));
		}
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("OrbSimDriver: No GameTickManager found. Simulation will not advance."));
	}
}

void AOrbSimDriver::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!Sim) return;

	const float Fixed = FMath::Max(KINDA_SMALL_NUMBER, Sim->FixedStepSec);
	StepAlpha += DeltaSeconds / Fixed;
	StepAlpha = FMath::Clamp(StepAlpha, 0.f, 1.f);

	Sim->SetFrameAlpha(StepAlpha);   // was: Sim->FrameAlpha = StepAlpha;
}

void AOrbSimDriver::HandleFixedTickSeconds(float StepSeconds)
{
	if (!Sim) return;

	UE_LOG(LogTemp, VeryVerbose, TEXT("OrbSimDriver: StepSim dt=%.3f"), StepSeconds);
	Sim->StepSim();

	StepAlpha = 0.f;
	Sim->SetFrameAlpha(0.f);         // was: Sim->FrameAlpha = 0.f;
#if !(UE_BUILD_SHIPPING)
	FItem Tmp;
	const bool L = Sim->PeekItemAt(FHex(-1, 0), Tmp);
	const bool R = Sim->PeekItemAt(FHex(1, 0), Tmp);
	UE_LOG(LogTemp, VeryVerbose, TEXT("OrbSimDriver: Inputs present? L=%d R=%d  (ItemMap=%d)"),
		L ? 1 : 0, R ? 1 : 0, Sim->ItemMap.Num());
#endif
}


float AOrbSimDriver::GetGridPlaneZ(const AActor* FallbackOwner) const
{
	if (bUseAnchorZForPicking && GridAnchor)
	{
		return GridAnchor->GetActorLocation().Z;
	}
	return FallbackOwner ? FallbackOwner->GetActorLocation().Z : 0.f;
}

AOrbSimDriver* AOrbSimDriver::Find(UWorld* World)
{
	if (!World) return nullptr;
	for (TActorIterator<AOrbSimDriver> It(World); It; ++It)
	{
		return *It;
	}
	return nullptr;
}

void AOrbSimDriver::SetGridParameters(EHexOrientation InOrientation, float InYawDeg, float InTileSizeUU, AActor* InAnchor)
{
	GridOrientation = InOrientation;
	GridYawDeg = InYawDeg;
	TileSizeUU = InTileSizeUU;
	GridAnchor = InAnchor;
	
	// Update the sim's parameters to match (if sim exists)
	if (Sim)
	{
		Sim->TileSizeUU = TileSizeUU;
		const FVector AnchorLoc = InAnchor ? InAnchor->GetActorLocation() : FVector::ZeroVector;
		Sim->SetWorldTransform(AnchorLoc, GridYawDeg, GridOrientation);
		
		// Sync debug settings
		Sim->bDebugLogs = bDebugLogs;
		Sim->bDebugSpheres = bDebugSpheres;
		Sim->bDebugLines = bDebugLines;
		Sim->bDebugBonds = bDebugBonds;
	}
	
	UE_LOG(LogTemp, Log, TEXT("OrbSimDriver: Grid parameters updated - Orientation=%d Yaw=%.1f TileSize=%.1f Sim=%s"),
		(int32)GridOrientation, GridYawDeg, TileSizeUU, Sim ? TEXT("exists") : TEXT("null"));
}

void AOrbSimDriver::SyncDebugSettingsToArms()
{
	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<ABasicArm> It(World); It; ++It)
		{
			ABasicArm* Arm = *It;
			if (Arm)
			{
				Arm->bDebugLogs = bDebugLogs;
				Arm->bDebugSpheres = bDebugSpheres;
				Arm->bDebugLines = bDebugLines;
			}
		}
	}
}

#if WITH_EDITOR
void AOrbSimDriver::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	
	if (HasAnyFlags(RF_ClassDefaultObject)) return;
	if (!GIsPlayInEditorWorld) return; // only care while PIE is running
	
	const FName PropName = PropertyChangedEvent.Property ? PropertyChangedEvent.Property->GetFName() : NAME_None;
	
	// Check if any debug settings changed
	if (PropName == GET_MEMBER_NAME_CHECKED(AOrbSimDriver, bDebugLogs) ||
		PropName == GET_MEMBER_NAME_CHECKED(AOrbSimDriver, bDebugSpheres) ||
		PropName == GET_MEMBER_NAME_CHECKED(AOrbSimDriver, bDebugLines) ||
		PropName == GET_MEMBER_NAME_CHECKED(AOrbSimDriver, bDebugBonds))
	{
		// Sync to sim
		if (Sim)
		{
			Sim->bDebugLogs = bDebugLogs;
			Sim->bDebugSpheres = bDebugSpheres;
			Sim->bDebugLines = bDebugLines;
			Sim->bDebugBonds = bDebugBonds;
		}
		
		// Sync to all arms
		SyncDebugSettingsToArms();
		
		UE_LOG(LogTemp, Log, TEXT("OrbSimDriver: Debug settings changed - Logs=%d Spheres=%d Lines=%d Bonds=%d"), 
			bDebugLogs ? 1 : 0, bDebugSpheres ? 1 : 0, bDebugLines ? 1 : 0, bDebugBonds ? 1 : 0);
	}
}
#endif

void AOrbSimDriver::ForceSyncDebugSettings()
{
	// Sync to sim
	if (Sim)
	{
		Sim->bDebugLogs = bDebugLogs;
		Sim->bDebugSpheres = bDebugSpheres;
		Sim->bDebugLines = bDebugLines;
		Sim->bDebugBonds = bDebugBonds;
	}
	
	// Sync to all arms
	SyncDebugSettingsToArms();
	
	UE_LOG(LogTemp, Log, TEXT("OrbSimDriver: Force synced debug settings - Logs=%d Spheres=%d Lines=%d Bonds=%d"), 
		bDebugLogs ? 1 : 0, bDebugSpheres ? 1 : 0, bDebugLines ? 1 : 0, bDebugBonds ? 1 : 0);
}

void AOrbSimDriver::CreateTestBonds()
{
	if (Sim)
	{
		Sim->CreateTestBonds();
		UE_LOG(LogTemp, Log, TEXT("OrbSimDriver: Created test bonds"));
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("OrbSimDriver: No Sim found, cannot create test bonds"));
	}
}





