#include "Actors/OrbSimDriver.h"
#include "Framework/GameTickManager.h"
#include "Framework/GameManager.h"
#include "Sim/SimWorldHex.h"
#include "Sim/HexCoord.h"
#include "Sim/HexTypes.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "DrawDebugHelpers.h"

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
	
	// Draw debug spheres for all orbs in the simulation
	if (bDebugSpheres && GetWorld())
	{
		DrawAllOrbDebugSpheres();
	}
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

void AOrbSimDriver::DrawAllOrbDebugSpheres()
{
	if (!Sim || !GetWorld()) return;
	
	const AActor* Anchor = GetGridAnchor();
	const FVector AnchorV = Anchor ? Anchor->GetActorLocation() : FVector::ZeroVector;
	const float Tile = GetTileSize();
	const float GridYaw = GetGridYaw();
	const EHexOrientation Ori = GetGridOrientation();
	
	// Draw spheres for all board items (not held by arms)
	for (const auto& KV : Sim->ItemMap)
	{
		const FHex& Hex = KV.Key;
		const FItem& Item = KV.Value;
		
		if (Item.Id == 0) continue; // Skip invalid items
		
		// Calculate world position for this hex
		const FHexAxial A(Hex.Q, Hex.R);
		const FVector2D LocalXY = HexCoord::AxialToXY(A, Tile, Ori);
		const FVector2D RotXY = HexCoord::Rotate2D(LocalXY, GridYaw);
		const FVector HexWorld = AnchorV + FVector(RotXY.X, RotXY.Y, 0.f);
		
		if (!Item.bIsCluster)
		{
			// Single orb
			const FColor DebugColor = GetDebugColorForOrbType(static_cast<uint8>(Item.Single.Type));
			const float SphereSize = GetDebugSizeForOrbType(static_cast<uint8>(Item.Single.Type), false);
			DrawDebugSphere(GetWorld(), HexWorld, SphereSize, 12, DebugColor, false, 0.f, 0, 2.f);
			
			// Draw item ID text
			DrawDebugString(GetWorld(), HexWorld + FVector(0, 0, SphereSize + 5.f), 
				FString::Printf(TEXT("ID:%d"), Item.Id), 
				nullptr, DebugColor, 0.f, true, 1.0f);
		}
		else
		{
			// Cluster - draw spheres for each orb in the cluster
			for (int32 SubIdx = 0; SubIdx < Item.Orbs.Num(); ++SubIdx)
			{
				const FOrbData& Sub = Item.Orbs[SubIdx];
				
				// Calculate offset from cluster center using hex coordinates
				const FHex SubHex(Hex.Q + Sub.dQ, Hex.R + Sub.dR);
				const FHexAxial SubA(SubHex.Q, SubHex.R);
				const FVector2D SubLocalXY = HexCoord::AxialToXY(SubA, Tile, Ori);
				const FVector2D SubRotXY = HexCoord::Rotate2D(SubLocalXY, GridYaw);
				const FVector SubWorld = AnchorV + FVector(SubRotXY.X, SubRotXY.Y, 0.f);
				
				// Color code: grabbed orb (dQ=0, dR=0) in green, others based on type
				FColor DebugColor;
				if (Sub.dQ == 0 && Sub.dR == 0)
				{
					DebugColor = FColor::Green; // Grabbed orb
				}
				else
				{
					DebugColor = GetDebugColorForOrbType(static_cast<uint8>(Sub.Type));
				}
				
				const float SphereSize = GetDebugSizeForOrbType(static_cast<uint8>(Sub.Type), true);
				DrawDebugSphere(GetWorld(), SubWorld, SphereSize, 12, DebugColor, false, 0.f, 0, 2.f);
				
				// Draw orb info
				DrawDebugString(GetWorld(), SubWorld + FVector(0, 0, SphereSize + 5.f), 
					FString::Printf(TEXT("ID:%d[%d] dQ:%d dR:%d"), Item.Id, SubIdx, Sub.dQ, Sub.dR), 
					nullptr, DebugColor, 0.f, true, 0.8f);
			}
		}
	}
	
	// Draw spheres for all held items by arms
	for (const FArmState& ArmState : Sim->Arms)
	{
		if (!ArmState.bHolding || ArmState.Held.Id == 0) continue;
		
		const FItem& HeldItem = ArmState.Held;
		
		// Calculate arm grip position
		FHex GripHex = ArmState.BaseHex;
		for (int32 i = 0; i < ArmState.ArmLen; ++i)
		{
			GripHex = HexUtil::Neighbor(GripHex, ArmState.Facing);
		}
		
		const FHexAxial GA(GripHex.Q, GripHex.R);
		const FVector2D LocalXY = HexCoord::AxialToXY(GA, Tile, Ori);
		const FVector2D RotXY = HexCoord::Rotate2D(LocalXY, GridYaw);
		const FVector GripWorld = AnchorV + FVector(RotXY.X, RotXY.Y, 0.f);
		
		// Apply arm rotation for held items
		const float ArmYawDeg = GridYaw - 60.f * static_cast<uint8>(ArmState.Facing);
		
		if (!HeldItem.bIsCluster)
		{
			// Single held orb
			const FColor DebugColor = FColor::Yellow; // Held items in yellow
			const float SphereSize = GetDebugSizeForOrbType(static_cast<uint8>(HeldItem.Single.Type), false);
			DrawDebugSphere(GetWorld(), GripWorld, SphereSize, 12, DebugColor, false, 0.f, 0, 2.f);
			
			// Draw held item info
			DrawDebugString(GetWorld(), GripWorld + FVector(0, 0, SphereSize + 5.f), 
				FString::Printf(TEXT("HELD ID:%d"), HeldItem.Id), 
				nullptr, DebugColor, 0.f, true, 1.0f);
		}
		else
		{
			// Held cluster - draw spheres for each orb in the cluster with arm rotation applied
			for (int32 SubIdx = 0; SubIdx < HeldItem.Orbs.Num(); ++SubIdx)
			{
				const FOrbData& Sub = HeldItem.Orbs[SubIdx];
				
				// Calculate orb position relative to grip with arm rotation
				const FHex SubHex(GripHex.Q + Sub.dQ, GripHex.R + Sub.dR);
				const FHexAxial SubA(SubHex.Q, SubHex.R);
				const FVector2D SubLocalXY = HexCoord::AxialToXY(SubA, Tile, Ori);
				
				// Apply arm rotation to the offset
				FVector2D RotatedOffset = SubLocalXY;
				if (Sub.dQ != 0 || Sub.dR != 0) // Don't rotate the grabbed orb
				{
					const FVector2D HexOffset = HexCoord::AxialToXY(FHexAxial(Sub.dQ, Sub.dR), Tile, Ori);
					const float ArmYawRad = FMath::DegreesToRadians(ArmYawDeg);
					const float CosA = FMath::Cos(ArmYawRad);
					const float SinA = FMath::Sin(ArmYawRad);
					const float RotX = HexOffset.X * CosA - HexOffset.Y * SinA;
					const float RotY = HexOffset.X * SinA + HexOffset.Y * CosA;
					RotatedOffset = FVector2D(RotX, RotY);
				}
				
				const FVector SubWorld = GripWorld + FVector(RotatedOffset.X, RotatedOffset.Y, 0.f);
				
				// Color code: grabbed orb in bright yellow, others in orange
				FColor DebugColor;
				if (Sub.dQ == 0 && Sub.dR == 0)
				{
					DebugColor = FColor::Yellow; // Grabbed orb
				}
				else
				{
					DebugColor = FColor::Orange; // Other held orbs
				}
				
				const float SphereSize = GetDebugSizeForOrbType(static_cast<uint8>(Sub.Type), true);
				DrawDebugSphere(GetWorld(), SubWorld, SphereSize, 12, DebugColor, false, 0.f, 0, 2.f);
				
				// Draw held cluster orb info
				DrawDebugString(GetWorld(), SubWorld + FVector(0, 0, SphereSize + 5.f), 
					FString::Printf(TEXT("HELD ID:%d[%d]"), HeldItem.Id, SubIdx), 
					nullptr, DebugColor, 0.f, true, 0.8f);
			}
		}
	}
}

FColor AOrbSimDriver::GetDebugColorForOrbType(uint8 OrbType) const
{
	// Color coding for different orb types in debug spheres
	switch (OrbType)
	{
	case 0: return FColor::White;    // None/Base
	case 1: return FColor::Red;      // BaseA
	case 2: return FColor::Blue;     // BaseB  
	case 3: return FColor::Green;    // BaseC
	case 4: return FColor::Silver;   // MetalA
	case 5: return FColor::Magenta;  // CatalystX
	default: return FColor(128, 128, 128); // Gray
	}
}

float AOrbSimDriver::GetDebugSizeForOrbType(uint8 OrbType, bool bIsSubOrb) const
{
	// All orbs use the same size regardless of whether they're in a cluster or not
	return DebugSphereSize;
}





