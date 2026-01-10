#include "Actors/OrbSimDriver.h"
#include "Framework/GameTickManager.h"
#include "Framework/GameManager.h"
#include "Sim/SimWorldHex.h"
#include "Sim/HexCoord.h"
#include "Sim/HexTypes.h"
#include "Geometry/GoldbergGrid.h"
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
		Sim->SetWorldTransform(AnchorLoc, GridYawDeg, GridOrientation); // set world transform (3 params)
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

	// Read alpha directly from the manager (single source of truth)
	if (AGameTickManager* TickManager = AGameTickManager::Get(this))
	{
		StepAlpha = TickManager->GetStepAlpha();
	}
	else
	{
		StepAlpha = 0.f;
	}

	Sim->SetFrameAlpha(StepAlpha);

	// Draw debug spheres for all orbs in the simulation
	// Moved here from HandleFixedTickSeconds to use proper StepAlpha for interpolation
	if (bDebugSpheres && GetWorld())
	{
		DrawAllOrbDebugSpheres();
	}
	
	// Draw collision debug spheres when enabled
	if (DebugCollision && GetWorld())
	{
		DrawCollisionDebugSpheres();
	}
}

void AOrbSimDriver::HandleFixedTickSeconds(float StepSeconds)
{
	if (!Sim) return;

	UE_LOG(LogTemp, VeryVerbose, TEXT("OrbSimDriver: StepSim dt=%.3f"), StepSeconds);
	Sim->StepSim();

	// Ensure the sim's frame alpha reflects the manager's start-of-step alpha (0 at tick)
	if (AGameTickManager* TickManager = AGameTickManager::Get(this))
	{
		Sim->SetFrameAlpha(TickManager->GetStepAlpha());
	}

#if !(UE_BUILD_SHIPPING)
	FItem Tmp;
	const bool L = Sim->PeekItemAt(FHex(-1, 0), Tmp);
	const bool R = Sim->PeekItemAt(FHex(1, 0), Tmp);
	UE_LOG(LogTemp, VeryVerbose, TEXT("OrbSimDriver: Inputs present? L=%d R=%d  (ItemMap=%d)"),
		L ? 1 : 0, R ? 1 : 0, Sim->ItemMap.Num());
#endif

	// Broadcast that simulation has completed this step
	OnSimStepComplete.Broadcast(StepSeconds);
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
	// Only update and log if values actually changed
	const bool bOrientationChanged = GridOrientation != InOrientation;
	const bool bYawChanged = !FMath::IsNearlyEqual(GridYawDeg, InYawDeg, 0.01f);
	const bool bTileSizeChanged = !FMath::IsNearlyEqual(TileSizeUU, InTileSizeUU, 0.01f);
	const bool bAnchorChanged = GridAnchor != InAnchor;
	
	if (!bOrientationChanged && !bYawChanged && !bTileSizeChanged && !bAnchorChanged)
	{
		// No changes, skip update and logging
		return;
	}
	
	GridOrientation = InOrientation;
	GridYawDeg = InYawDeg;
	TileSizeUU = InTileSizeUU;
	GridAnchor = InAnchor;
	if (Sim)
	{
		Sim->TileSizeUU = TileSizeUU;
		const FVector AnchorLoc = InAnchor ? InAnchor->GetActorLocation() : FVector::ZeroVector;
		Sim->SetWorldTransform(AnchorLoc, GridYawDeg, GridOrientation);
	}
	UE_LOG(LogTemp, Log, TEXT("OrbSimDriver: Grid parameters updated - Orientation=%d Yaw=%.1f TileSize=%.1f"), (int32)GridOrientation, GridYawDeg, TileSizeUU);
}

// Deprecated legacy function kept for BP compatibility (now calls ApplySphereSettings via inline wrapper in header)

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
		PropName == GET_MEMBER_NAME_CHECKED(AOrbSimDriver, bDebugBonds) ||
		PropName == GET_MEMBER_NAME_CHECKED(AOrbSimDriver, DebugCollision))
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

void AOrbSimDriver::DrawAllOrbDebugSpheres()
{
	if (!Sim || !GetWorld()) return;

	const AActor* Anchor = GetGridAnchor();
	const FVector AnchorV = Anchor ? Anchor->GetActorLocation() : FVector::ZeroVector;
	const float Tile = GetTileSize();
	const float GridYaw = GetGridYaw();
	const EHexOrientation Ori = GetGridOrientation();
	
	// Calculate GTM tick duration for debug draw lifetime
	float DebugLifetime = 0.f;
	if (AGameTickManager* TickManager = AGameTickManager::Get(this))
	{
		DebugLifetime = 1.0f / FMath::Max(0.01f, TickManager->TicksPerSecond);
	}

	// Draw spheres for all board items (not held by arms)
	for (const auto& KV : Sim->ItemMap)
	{
		const FHex& Hex = KV.Key;
		const FItem& Item = KV.Value;

		if (Item.Id == 0) continue; // Skip invalid items

		// Calculate world position for this hex
		const FVector HexWorld = MapHexToWorld(Hex);

		if (!Item.bIsCluster)
		{
			// Single orb
			const FColor DebugColor = GetDebugColorForOrbType(static_cast<uint8>(Item.Single.Type));
			const float SphereSize = GetDebugSizeForOrbType(static_cast<uint8>(Item.Single.Type), false);
			DrawDebugSphere(GetWorld(), HexWorld, SphereSize, 12, DebugColor, false, DebugLifetime, 0, 2.f);

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
				const FVector SubWorld = MapHexToWorld(SubHex);

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
				DrawDebugSphere(GetWorld(), SubWorld, SphereSize, 12, DebugColor, false, DebugLifetime, 0, 2.f);

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

		const FVector GripWorld = MapHexToWorld(GripHex);

		// Apply arm rotation for held items
		const float ArmYawDeg = GridYaw - 60.f * static_cast<uint8>(ArmState.Facing);

		if (!HeldItem.bIsCluster)
		{
			// Single held orb
			const FColor DebugColor = FColor::Yellow; // Held items in yellow
			const float SphereSize = GetDebugSizeForOrbType(static_cast<uint8>(HeldItem.Single.Type), false);
			DrawDebugSphere(GetWorld(), GripWorld, SphereSize, 12, DebugColor, false, DebugLifetime, 0, 2.f);

			// Draw held item info
			DrawDebugString(GetWorld(), GripWorld + FVector(0, 0, SphereSize + 5.f),
				FString::Printf(TEXT("HELD ID:%d"), HeldItem.Id),
				nullptr, DebugColor, 0.f, true, 1.0f);
		}
		else
		{
		// Held cluster - draw spheres for each orb in the cluster with proper rotation applied
		// Use the same rotation calculation as the simulation (GetHeldCCW_Steps_Sim)
		const uint8 cur = static_cast<uint8>(ArmState.Facing) % 6;
		const int8 grab = HeldItem.GrabFacing;
		const uint8 facingDelta = (grab >= 0)
			? (uint8)(((int32)cur - (int32)(grab % 6) + 6) % 6)
			: cur;
		const uint8 ccw = (uint8)((6 - facingDelta) % 6);

		// Helper lambda for hex rotation (same as simulation)
		auto Rot60 = [](FHex D, uint8 StepsCCW) -> FHex
		{
			for (uint8 i = 0; i < (StepsCCW % 6); ++i)
			{
				int32 q = D.Q;
				int32 r = D.R;
				D.Q = -r;
				D.R = q + r;
			}
			return D;
		};

		for (int32 SubIdx = 0; SubIdx < HeldItem.Orbs.Num(); ++SubIdx)
		{
			const FOrbData& Sub = HeldItem.Orbs[SubIdx];

			// Apply the same rotation logic as the simulation
			FHex Local(Sub.dQ, Sub.dR);
			FHex Rot = Rot60(Local, ccw);
			FHex WorldPos(GripHex.Q + Rot.Q, GripHex.R + Rot.R);

			// Convert to world coordinates
			const FVector SubWorld = MapHexToWorld(WorldPos);

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
			DrawDebugSphere(GetWorld(), SubWorld, SphereSize, 12, DebugColor, false, DebugLifetime, 0, 2.f);

			// Draw held cluster orb info
			DrawDebugString(GetWorld(), SubWorld + FVector(0, 0, SphereSize + 5.f),
				FString::Printf(TEXT("HELD ID:%d[%d] R:%d"), HeldItem.Id, SubIdx, ccw),
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

void AOrbSimDriver::DrawCollisionDebugSpheres()
{
	if (!Sim || !GetWorld()) return;

	const AActor* Anchor = GetGridAnchor();
	const FVector AnchorV = Anchor ? Anchor->GetActorLocation() : FVector::ZeroVector;
	const float Tile = GetTileSize();
	const float GridYaw = GetGridYaw();
	const EHexOrientation Ori = GetGridOrientation();
	
	// Calculate GTM tick duration for debug draw lifetime
	float DebugLifetime = 0.f;
	if (AGameTickManager* TickManager = AGameTickManager::Get(this))
	{
		DebugLifetime = 1.0f / FMath::Max(0.01f, TickManager->TicksPerSecond);
	}

	// Use the collision data tracked by the simulation
	const TArray<USimWorldHex::FCollisionDebugInfo>& CollisionData = Sim->GetCollisionDebugData();
	
	for (const USimWorldHex::FCollisionDebugInfo& CollisionInfo : CollisionData)
	{
		// Convert blocked hex to world coordinates
		const FVector BlockedWorld = MapHexToWorld(CollisionInfo.BlockedHex);
		
		// Convert arm base hex to world coordinates for line drawing
		const FVector BaseWorld = MapHexToWorld(CollisionInfo.ArmBaseHex);
		
		// Draw the collision sphere and line
		DrawDebugSphere(GetWorld(), BlockedWorld, DebugSphereSize * 1.2f, 12, FColor::Red, false, DebugLifetime, 0, 2.f);
		DrawDebugLine(GetWorld(), BaseWorld, BlockedWorld, FColor::Red, false, DebugLifetime, 0, 3.f);
		DrawDebugString(GetWorld(), BlockedWorld + FVector(0, 0, DebugSphereSize + 5.f),
			FString::Printf(TEXT("Arm %d: %s"), CollisionInfo.ArmIndex, *CollisionInfo.Reason),
			nullptr, FColor::Red, 0.f, true, 1.0f);
	}
	
	// Draw predictive collision debug data
	const TArray<USimWorldHex::FPredictiveCollisionInfo>& PredictiveCollisionData = Sim->GetPredictiveCollisionDebugData();
	
	for (const USimWorldHex::FPredictiveCollisionInfo& PredictiveInfo : PredictiveCollisionData)
	{
		// Convert collision hex to world coordinates
		const FVector CollisionWorld = MapHexToWorld(PredictiveInfo.CollisionHex);
		
		// Draw predictive collision sphere in orange
		DrawDebugSphere(GetWorld(), CollisionWorld, DebugSphereSize * 1.5f, 12, FColor::Orange, false, DebugLifetime, 0, 2.f);
		DrawDebugString(GetWorld(), CollisionWorld + FVector(0, 0, DebugSphereSize + 10.f),
			FString::Printf(TEXT("Predictive: %s"), *PredictiveInfo.Reason),
			nullptr, FColor::Orange, 0.f, true, 1.2f);
	}

	// Draw walker foot positions (potential obstacles)
	for (int32 i = 0; i < Sim->Walkers.Num(); ++i)
	{
		const FWalkerState& W = Sim->Walkers[i];
		
		auto DrawFoot = [&](const FHex& H, const FString& Label)
		{
			FVector Pos = MapHexToWorld(H);
			DrawDebugSphere(GetWorld(), Pos, DebugSphereSize, 8, FColor::Cyan, false, DebugLifetime, 0, 1.5f);
			DrawDebugString(GetWorld(), Pos + FVector(0, 0, DebugSphereSize + 15.f),
				Label, nullptr, FColor::Cyan, 0.f, true, 0.8f);
		};

		DrawFoot(W.DFoot.CurrentTile, FString::Printf(TEXT("W%d-D"), i));
		if (W.DFoot.TargetTile != W.DFoot.CurrentTile) DrawFoot(W.DFoot.TargetTile, FString::Printf(TEXT("W%d-D(T)"), i));
		
		DrawFoot(W.LFoot.CurrentTile, FString::Printf(TEXT("W%d-L"), i));
		if (W.LFoot.TargetTile != W.LFoot.CurrentTile) DrawFoot(W.LFoot.TargetTile, FString::Printf(TEXT("W%d-L(T)"), i));
		
		DrawFoot(W.RFoot.CurrentTile, FString::Printf(TEXT("W%d-R"), i));
		if (W.RFoot.TargetTile != W.RFoot.CurrentTile) DrawFoot(W.RFoot.TargetTile, FString::Printf(TEXT("W%d-R(T)"), i));
		
		DrawFoot(W.UFoot.CurrentTile, FString::Printf(TEXT("W%d-U"), i));
		if (W.UFoot.TargetTile != W.UFoot.CurrentTile) DrawFoot(W.UFoot.TargetTile, FString::Printf(TEXT("W%d-U(T)"), i));
	}
}

FVector AOrbSimDriver::MapHexToWorld(const FHex& H) const
{
	const AActor* Anchor = GetGridAnchor();
	const FVector AnchorLoc = Anchor ? Anchor->GetActorLocation() : FVector::ZeroVector;
	
	// Goldberg mode: lookup cell position from the grid
	if (bGoldbergMode && CachedGoldbergGrid)
	{
		const int32 CellIndex = CachedGoldbergGrid->GetCellIndexForHex(H);
		if (CellIndex != INDEX_NONE)
		{
			return CachedGoldbergGrid->GetCells()[CellIndex].Position;
		}
		// Invalid hex - return zero (or could return anchor location)
		UE_LOG(LogTemp, Warning, TEXT("MapHexToWorld: No Goldberg cell for hex (%d,%d)"), H.Q, H.R);
		return FVector::ZeroVector;
	}
	
	// Planar mode
	const FVector2D XY = HexCoord::AxialToXY(FHexAxial(H.Q, H.R), TileSizeUU, GridOrientation);
	const FVector2D RotXY = HexCoord::Rotate2D(XY, GridYawDeg);
	float Z = 0.f;
	if (Sim)
	{
		Z = (H.Level == 0) ? Sim->OrbZOffset : -Sim->OrbZOffset;
	}
	return AnchorLoc + FVector(RotXY.X, RotXY.Y, Z);
}

bool AOrbSimDriver::WorldToHex(const FVector& World, FHex& OutHex) const
{
	const AActor* Anchor = GetGridAnchor();
	const FVector AnchorLoc = Anchor ? Anchor->GetActorLocation() : FVector::ZeroVector;
	
	// Goldberg mode: find nearest cell
	if (bGoldbergMode && CachedGoldbergGrid)
	{
		const TArray<FGoldbergCell>& Cells = CachedGoldbergGrid->GetCells();
		float BestDistSq = FLT_MAX;
		int32 BestCellIndex = INDEX_NONE;
		
		for (int32 i = 0; i < Cells.Num(); ++i)
		{
			if (Cells[i].bPentagon) continue; // Skip pentagons
			
			float DistSq = FVector::DistSquared(World, Cells[i].Position);
			if (DistSq < BestDistSq)
			{
				BestDistSq = DistSq;
				BestCellIndex = i;
			}
		}
		
		if (BestCellIndex != INDEX_NONE)
		{
			OutHex = CachedGoldbergGrid->GetHexForCellIndex(BestCellIndex);
			return true;
		}
		
		OutHex = FHex(0, 0);
		return false;
	}
	
	// Planar mode
	FVector Local = World - AnchorLoc;
	FVector2D Rot2D = HexCoord::Rotate2D(FVector2D(Local.X, Local.Y), -GridYawDeg);
	FHexAxial A = HexCoord::XYToAxialRound(Rot2D, TileSizeUU, GridOrientation);
	OutHex = FHex(A.Q, A.R);
	return true;
}
