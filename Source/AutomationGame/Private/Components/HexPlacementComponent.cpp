#include "Components/HexPlacementComponent.h"
#include "Actors/OrbSimDriver.h"
#include "Sim/SimWorldHex.h"
#include "Sim/Tiles.h"
#include "Sim/HexCoord.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Actor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Framework/GameManager.h"
#include "Actors/BasicArm.h"
#include "DrawDebugHelpers.h"
#include "Components/StaticMeshComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Framework/TestLevelDirector.h"
#include "Actors/HexFactoryActor.h"
#include "Data/OrbMaterialManager.h"

UHexPlacementComponent::UHexPlacementComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
}

// Added: helper to locate ATestLevelDirector
static ATestLevelDirector* FindTLD(UWorld* W)
{
	if (!W) return nullptr;
	for (TActorIterator<ATestLevelDirector> It(W); It; ++It)
	{
		return *It;
	}
	return nullptr;
}

void UHexPlacementComponent::BeginPlay()
{
	Super::BeginPlay();
	if (UWorld* W = GetWorld())
	{
		Driver = AOrbSimDriver::Find(W);
		if (Driver) Sim = Driver->GetSim();
		PC = W->GetFirstPlayerController();
	}

	CachedTLD = FindTLD(GetWorld());
}

static void ReacquireSimIfNeeded(UWorld* W, AOrbSimDriver*& Driver, USimWorldHex*& Sim)
{
	if (!Sim || !Driver)
	{
		Driver = AOrbSimDriver::Find(W);
		if (Driver) Sim = Driver->GetSim();
	}
}
static float ComputeDirYawFromNeighbor(const UHexPlacementComponent* Self, const FHex& Base, uint8 Dir)
{
	if (!Self) return 0.f;
	const FHex Neighbor = HexUtil::Neighbor(Base, (EHexDir)(Dir % 6));
	const FVector A = Self->HexToWorld(Base);
	const FVector B = Self->HexToWorld(Neighbor);
	const FVector2D D(B.X - A.X, B.Y - A.Y);
	if (D.IsNearlyZero()) return 0.f;
	return FMath::RadiansToDegrees(FMath::Atan2(D.Y, D.X));
}
void UHexPlacementComponent::TickComponent(float DeltaTime, enum ELevelTick, FActorComponentTickFunction*)
{
	if ((!Driver || !Sim) && GetWorld()) ReacquireSimIfNeeded(GetWorld(), Driver, Sim);

	if (PC)
	{
		UpdateHoverHex();
		UpdatePreviewVisual();
	}

	if (!bUseInputPolling) return;
	if (!Sim || !PC) return;

	const bool bClickL = PC->IsInputKeyDown(EKeys::LeftMouseButton);
	const bool bClickR = PC->IsInputKeyDown(EKeys::RightMouseButton);

	FVector World;
	if (bClickL && !bWasClickingL && HitPlane(World)) DoPlaceAt(World);
	if (bClickR && !bWasClickingR && HitPlane(World)) DoRemoveAt(World);

	bWasClickingL = bClickL;
	bWasClickingR = bClickR;
}

AActor* UHexPlacementComponent::GetAnchor() const
{
	if (bOverrideAnchor && AnchorOverride) return AnchorOverride;
	return Driver ? Driver->GetGridAnchor() : nullptr;
}

bool UHexPlacementComponent::HitPlane(FVector& OutWorld) const
{
	if (!PC) return false;
	float X, Y;
	if (!PC->GetMousePosition(X, Y)) return false;
	FVector Origin, Dir;
	if (!PC->DeprojectScreenPositionToWorld(X, Y, Origin, Dir)) return false;

	const float Z = Driver ? Driver->GetGridPlaneZ(GetOwner()) :
		(GetOwner() ? GetOwner()->GetActorLocation().Z : 0.f);

	if (FMath::IsNearlyZero(Dir.Z)) return false;
	const float T = (Z - Origin.Z) / Dir.Z;
	if (T <= 0) return false;
	OutWorld = Origin + T * Dir;

	if (bDebug)
	{
		DrawDebugSphere(GetWorld(), OutWorld, 6.f, 12, FColor::Green, false, 0.2f);
	}
	return true;
}

void UHexPlacementComponent::DoPlaceAt(const FVector& World)
{
	UWorld* W = GetWorld();
	if ((!Driver || !Sim) && W) ReacquireSimIfNeeded(W, Driver, Sim);
	if (!Sim) {
		UE_LOG(LogTemp, Warning, TEXT("HexPlacementComponent: Sim missing; cannot place."));
		return;
	}

	const float Tile = Driver ? Driver->GetTileSize() : Sim->TileSizeUU;
	const AActor* Anchor = GetAnchor();
	const FVector2D OriginXY = Anchor
		? FVector2D(Anchor->GetActorLocation().X, Anchor->GetActorLocation().Y)
		: FVector2D::ZeroVector;

	const float Yaw = Driver ? Driver->GetGridYaw() : 0.f;
	const EHexOrientation Ori = Driver ? Driver->GetGridOrientation() : EHexOrientation::PointyTop;

	FVector2D P(World.X - OriginXY.X, World.Y - OriginXY.Y);
	const FVector2D Local = HexCoord::Rotate2D(P, -Yaw);
	const FHexAxial A = HexCoord::XYToAxialRound(Local, Tile, Ori);
	const FHex Root{ A.Q, A.R };

	FTile T{};
	switch (PlaceKind)
	{
	case EPlaceKind::Input:  T = FTile{ ETileKind::Input,  (uint8)GetCurrentInputOrbType() }; break;
	case EPlaceKind::Output: T = FTile{ ETileKind::Output, (uint8)GetCurrentOutputOrbType() }; break;
	case EPlaceKind::Belt:   T = FTile{ ETileKind::Belt,   (uint8)BeltDir }; break;
	case EPlaceKind::Bond:   T = FTile{ ETileKind::Bond,   (uint8)BeltDir }; break;
	case EPlaceKind::Unbond: T = FTile{ ETileKind::Unbond, (uint8)BeltDir }; break;
	case EPlaceKind::Arm:
	{
		AActor* GMActor = nullptr;
		for (TActorIterator<AActor> It(GetWorld()); It; ++It)
			if (It->IsA<AGameManager>()) { GMActor = *It; break; }

		if (AGameManager* GM = Cast<AGameManager>(GMActor))
		{
			GM->SpawnBasicArmAt(FHex{ A.Q, A.R }, BeltDir % 6, TArray<FArmInstr>(), true);
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("HexPlacementComponent: cannot spawn Arm (no GameManager)."));
		}

		// Scenario capture for arm
		if (bAutoScenarioCapture)
		{
			if (!CachedTLD) CachedTLD = FindTLD(GetWorld());
			if (CachedTLD)
			{
				CachedTLD->UpsertArmSpec(A.Q, A.R, BeltDir % 6, TArray<FArmInstr>(), true);
			}
		}
		return;
	}
	default:
		UE_LOG(LogTemp, Warning, TEXT("HexPlacementComponent: PlaceKind unset; skip."));
		return;
	}

	// For outputs, remove any existing tile first to avoid overlap
	if (T.Kind == ETileKind::Output)
	{
		// Check if there's already a tile at this location
		if (Sim->TileMap.Contains(Root))
		{
			// Remove the existing tile first
			Sim->PlaceTile(Root, FTile{ ETileKind::Empty, 0 });
		}
	}

	// Attempt placement
	const bool bPlaced = Sim->PlaceTile(Root, T);
	if (!bPlaced)
	{
		if (T.Kind == ETileKind::Bond)
		{
			const FHex Partner = HexUtil::Neighbor(Root, (EHexDir)(BeltDir % 6));
			UE_LOG(LogTemp, Warning,
				TEXT("HexPlacementComponent: Bond placement FAILED at Root(%d,%d) Partner(%d,%d). Partner occupied by blocking tile."),
				Root.Q, Root.R, Partner.Q, Partner.R);
			if (GEngine)
				GEngine->AddOnScreenDebugMessage(-1, 1.5f, FColor::Red,
					FString::Printf(TEXT("Bond blocked (%d,%d)->(%d,%d)"), Root.Q, Root.R, Partner.Q, Partner.R));
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("HexPlacementComponent: Placement failed at (%d,%d)."), Root.Q, Root.R);
		}
		return;
	}

	// Scenario capture (tiles only)
	if (bPlaced && bAutoScenarioCapture)
	{
		if (!CachedTLD) CachedTLD = FindTLD(GetWorld());
		if (CachedTLD)
		{
			CachedTLD->UpsertTileSpec(Root, T.Kind, T.Arg);
		}
	}

	// Force GameManager to rebuild grid to reflect tile changes
	if (bPlaced)
	{
		if (UWorld* GameWorld = GetWorld())
		{
			for (TActorIterator<AGameManager> It(GameWorld); It; ++It)
			{
				AGameManager* GameManager = *It;
				if (GameManager)
				{
					GameManager->Rebuild();
					break;
				}
			}
		}
	}

	// Success logging
	if (bDebug)
	{
		if (T.Kind == ETileKind::Belt)
		{
			UE_LOG(LogTemp, Log, TEXT("Placed Belt at Hex(%d,%d) Dir=%d"), Root.Q, Root.R, (int32)BeltDir);
		}
		else if (T.Kind == ETileKind::Bond)
		{
			const FHex Partner = HexUtil::Neighbor(Root, (EHexDir)(BeltDir % 6));
			UE_LOG(LogTemp, Log, TEXT("Placed Bond at Root(%d,%d) -> Partner(%d,%d) Dir=%d"),
				Root.Q, Root.R, Partner.Q, Partner.R, (int32)BeltDir);
		}
		else
		{
			const FString KindName = StaticEnum<ETileKind>()->GetNameStringByValue((int64)T.Kind);
			UE_LOG(LogTemp, Log, TEXT("Placed %s at Hex(%d,%d)"), *KindName, Root.Q, Root.R);
		}
	}
}

void UHexPlacementComponent::DoRemoveAt(const FVector& World)
{
	UWorld* W = GetWorld();
	if ((!Driver || !Sim) && W) ReacquireSimIfNeeded(W, Driver, Sim);
	if (!Sim) return;

	const float Tile = Driver ? Driver->GetTileSize() : Sim->TileSizeUU;
	const AActor* Anchor = GetAnchor();
	const FVector2D OriginXY = Anchor
		? FVector2D(Anchor->GetActorLocation().X, Anchor->GetActorLocation().Y)
		: FVector2D::ZeroVector;

	const float Yaw = Driver ? Driver->GetGridYaw() : 0.f;
	const EHexOrientation Ori = Driver ? Driver->GetGridOrientation() : EHexOrientation::PointyTop;

	FVector2D P(World.X - OriginXY.X, World.Y - OriginXY.Y);
	const FVector2D Local = HexCoord::Rotate2D(P, -Yaw);
	const FHexAxial A = HexCoord::XYToAxialRound(Local, Tile, Ori);

	Sim->PlaceTile(FHex{ A.Q, A.R }, FTile{ ETileKind::Empty, 0 });
	if (bDebug)
	{
		UE_LOG(LogTemp, Log, TEXT("HexPlacementComponent: Removed tile at Hex(%d,%d)"), A.Q, A.R);
	}

	if (bDebug && GEngine)
	{
		GEngine->AddOnScreenDebugMessage(-1, 1.5f, FColor::Yellow,
			FString::Printf(TEXT("Removed tile at Hex(%d,%d)"), A.Q, A.R));
	}

	// Scenario capture removal
	if (bAutoScenarioCapture)
	{
		if (!CachedTLD) CachedTLD = FindTLD(GetWorld());
		if (CachedTLD)
		{
			CachedTLD->RemoveTileSpec(FHex{ A.Q, A.R });
			CachedTLD->RemoveArmSpec(A.Q, A.R); // in case an arm spec was there
		}
	}
}

void UHexPlacementComponent::HandlePlace(bool bPressed)
{
	if (!bPressed) return;
	FVector World;
	if (HitPlane(World)) DoPlaceAt(World);
}

void UHexPlacementComponent::HandleRemove(bool bPressed)
{
	if (!bPressed) return;
	FVector World;
	if (HitPlane(World)) DoRemoveAt(World);
}

void UHexPlacementComponent::RotateCW()
{
	BeltDir = (BeltDir + 5) % 6;
}

void UHexPlacementComponent::RotateCCW()
{
	BeltDir = (BeltDir + 1) % 6;
}

void UHexPlacementComponent::SetPlaceKind(EPlaceKind NewKind) { PlaceKind = NewKind; }

void UHexPlacementComponent::CycleNextType()
{
	// Get the list of valid orb types (those with assigned materials)
	TArray<EOrbType> ValidTypes = GetValidOrbTypes();
	if (ValidTypes.Num() == 0) return; // No valid types to cycle through
	
	// Cycle the appropriate type based on current placement mode
	if (PlaceKind == EPlaceKind::Input)
	{
		CurrentInputOrbTypeIndex = (CurrentInputOrbTypeIndex + 1) % ValidTypes.Num();
	}
	else if (PlaceKind == EPlaceKind::Output)
	{
		CurrentOutputOrbTypeIndex = (CurrentOutputOrbTypeIndex + 1) % ValidTypes.Num();
	}
}

void UHexPlacementComponent::CyclePreviousType()
{
	// Get the list of valid orb types (those with assigned materials)
	TArray<EOrbType> ValidTypes = GetValidOrbTypes();
	if (ValidTypes.Num() == 0) return; // No valid types to cycle through
	
	// Cycle the appropriate type based on current placement mode
	if (PlaceKind == EPlaceKind::Input)
	{
		CurrentInputOrbTypeIndex = (CurrentInputOrbTypeIndex - 1 + ValidTypes.Num()) % ValidTypes.Num();
	}
	else if (PlaceKind == EPlaceKind::Output)
	{
		CurrentOutputOrbTypeIndex = (CurrentOutputOrbTypeIndex - 1 + ValidTypes.Num()) % ValidTypes.Num();
	}
}

void UHexPlacementComponent::CycleGlyphType()
{
	// Cycle through Belt, Bond, Unbond
	if (PlaceKind == EPlaceKind::Belt)
	{
		PlaceKind = EPlaceKind::Bond;
	}
	else if (PlaceKind == EPlaceKind::Bond)
	{
		PlaceKind = EPlaceKind::Unbond;
	}
	else if (PlaceKind == EPlaceKind::Unbond)
	{
		PlaceKind = EPlaceKind::Belt;
	}
}

void UHexPlacementComponent::CycleInputOrbType()
{
	CycleNextType();
}

TArray<EOrbType> UHexPlacementComponent::GetValidOrbTypes() const
{
	TArray<EOrbType> ValidTypes;
	
	// Try to find UOrbMaterialManager in the world
	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			if (UOrbMaterialManager* MaterialManager = (*It)->FindComponentByClass<UOrbMaterialManager>())
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
							ValidTypes.Add(OrbType);
						}
					}
				}
				break; // Found the material manager, no need to continue searching
			}
		}
	}
	
	// If no valid types found, fallback to at least BaseA (which will use DefaultOrbMaterial)
	if (ValidTypes.Num() == 0)
	{
		ValidTypes.Add(EOrbType::BaseA);
	}
	
	return ValidTypes;
}

EOrbType UHexPlacementComponent::GetCurrentInputOrbType() const
{
	// Get the list of valid orb types (those with assigned materials)
	TArray<EOrbType> ValidTypes = GetValidOrbTypes();
	if (ValidTypes.Num() == 0)
	{
		return EOrbType::BaseA; // Fallback if no valid types
	}
	
	// Clamp the index to valid range
	int32 ClampedIndex = FMath::Clamp(CurrentInputOrbTypeIndex, 0, ValidTypes.Num() - 1);
	return ValidTypes[ClampedIndex];
}


EOrbType UHexPlacementComponent::GetCurrentOutputOrbType() const
{
	// Get the list of valid orb types (those with assigned materials)
	TArray<EOrbType> ValidTypes = GetValidOrbTypes();
	if (ValidTypes.Num() == 0)
	{
		return EOrbType::BaseA; // Fallback if no valid types
	}
	
	// Clamp the index to valid range
	int32 ClampedIndex = FMath::Clamp(CurrentOutputOrbTypeIndex, 0, ValidTypes.Num() - 1);
	return ValidTypes[ClampedIndex];
}

void UHexPlacementComponent::EnsurePreviewComponent()
{
	if (PreviewComp) return;

	AActor* Owner = GetOwner();
	if (!Owner) return;

	PreviewComp = NewObject<UStaticMeshComponent>(Owner);
	PreviewComp->SetupAttachment(Owner->GetRootComponent());
	PreviewComp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	PreviewComp->SetMobility(EComponentMobility::Movable);
	PreviewComp->RegisterComponent();

	if (PreviewBaseMaterial)
	{
		PreviewMID = UMaterialInstanceDynamic::Create(PreviewBaseMaterial, this);
		PreviewComp->SetMaterial(0, PreviewMID);
	}
}

UStaticMesh* UHexPlacementComponent::SelectPreviewMesh() const
{
	switch (PlaceKind)
	{
	case EPlaceKind::Input:
		return GetActualOrbMesh();
	case EPlaceKind::Output:
		return GetActualOutputOrbMesh();
	case EPlaceKind::Belt:
		return GetActualBeltMesh();
	case EPlaceKind::Arm:
		return ArmBasePreviewMesh ? ArmBasePreviewMesh : GenericTilePreviewMesh;
	case EPlaceKind::Bond:
		return BondPreviewMesh ? BondPreviewMesh : GenericTilePreviewMesh;
	case EPlaceKind::Unbond:
		return UnbondPreviewMesh ? UnbondPreviewMesh : GenericTilePreviewMesh;
	default:
		return GenericTilePreviewMesh;
	}
}

UStaticMesh* UHexPlacementComponent::GetActualBeltMesh() const
{
	// First try the local preview mesh
	if (BeltMeshPreview)
	{
		return BeltMeshPreview;
	}
	
	// Fallback: try to find AHexFactoryActor in the world
	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<AHexFactoryActor> It(World); It; ++It)
		{
			AHexFactoryActor* FactoryActor = *It;
			if (FactoryActor && FactoryActor->BeltMesh)
			{
				// Cache the reference for future use
				const_cast<UHexPlacementComponent*>(this)->BeltMeshPreview = FactoryActor->BeltMesh;
				return FactoryActor->BeltMesh;
			}
		}
	}
	
	// Final fallback
	return GenericTilePreviewMesh;
}

UMaterialInterface* UHexPlacementComponent::GetActualBeltMaterial() const
{
	// First try the local preview material
	if (BeltMaterialPreview)
	{
		return BeltMaterialPreview;
	}
	
	// Fallback: try to find AHexFactoryActor in the world
	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<AHexFactoryActor> It(World); It; ++It)
		{
			AHexFactoryActor* FactoryActor = *It;
			if (FactoryActor && FactoryActor->BeltMaterial)
			{
				// Cache the reference for future use
				const_cast<UHexPlacementComponent*>(this)->BeltMaterialPreview = FactoryActor->BeltMaterial;
				return FactoryActor->BeltMaterial;
			}
		}
	}
	
	// Final fallback
	return nullptr;
}

UStaticMesh* UHexPlacementComponent::GetActualOrbMesh() const
{
	// Try to find UOrbMaterialManager in the world
	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			if (UOrbMaterialManager* MaterialManager = (*It)->FindComponentByClass<UOrbMaterialManager>())
			{
				UStaticMesh* OrbMesh = MaterialManager->GetOrbMesh();
				if (OrbMesh)
				{
					return OrbMesh;
				}
			}
		}
	}
	
	// Final fallback
	return GenericTilePreviewMesh;
}

UMaterialInterface* UHexPlacementComponent::GetActualOrbMaterial() const
{
	// Try to find UOrbMaterialManager in the world
	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			if (UOrbMaterialManager* MaterialManager = (*It)->FindComponentByClass<UOrbMaterialManager>())
			{
				EOrbType CurrentType = GetCurrentInputOrbType();
				UMaterialInterface* OrbMaterial = MaterialManager->GetMaterialForOrbType(CurrentType);
				if (OrbMaterial)
				{
					return OrbMaterial;
				}
			}
		}
	}
	
	// Final fallback
	return nullptr;
}

bool UHexPlacementComponent::CanPlaceAt(const FHex& Hex) const
{
	if (!Sim) return true;
	return true;
}

FVector UHexPlacementComponent::HexToWorld(const FHex& H) const
{
	if (!Driver && !Sim)
	{
		return LastHoverWorld;
	}

	const float Tile = Driver ? Driver->GetTileSize() : (Sim ? Sim->TileSizeUU : 100.f);
	const float PlaneZ = Driver ? Driver->GetGridPlaneZ(GetOwner()) :
		(GetOwner() ? GetOwner()->GetActorLocation().Z : 0.f);
	const float YawDeg = Driver ? Driver->GetGridYaw() : 0.f;
	const EHexOrientation Ori = Driver ? Driver->GetGridOrientation() : EHexOrientation::PointyTop;

	const float q = (float)H.Q;
	const float r = (float)H.R;

	FVector2D Local(0, 0);
	const float Sqrt3 = 1.7320508075688772f;

	if (Ori == EHexOrientation::PointyTop)
	{
		Local.X = Tile * (Sqrt3 * q + (Sqrt3 * 0.5f) * r);
		Local.Y = Tile * (1.5f * r);
	}
	else
	{
		Local.X = Tile * (1.5f * q);
		Local.Y = Tile * (Sqrt3 * r + (Sqrt3 * 0.5f) * q);
	}

	const float YawRad = FMath::DegreesToRadians(YawDeg);
	const float CosA = FMath::Cos(YawRad);
	const float SinA = FMath::Sin(YawRad);
	const float RX = Local.X * CosA - Local.Y * SinA;
	const float RY = Local.X * SinA + Local.Y * CosA;

	const AActor* Anchor = GetAnchor();
	const FVector2D OriginXY = Anchor
		? FVector2D(Anchor->GetActorLocation().X, Anchor->GetActorLocation().Y)
		: FVector2D::ZeroVector;

	return FVector(OriginXY.X + RX, OriginXY.Y + RY, PlaneZ);
}

void UHexPlacementComponent::UpdateHoverHex()
{
	FVector World;
	if (!HitPlane(World))
	{
		bHoverValid = false;
		return;
	}

	LastHoverWorld = World;

	if (!Driver && !Sim)
	{
		bHoverValid = false;
		return;
	}

	if (Driver && Driver->IsSphereEnabled())
	{
		FHex H;
		if (Driver->WorldToHex(World, H))
		{
			HoverHex = H;
			bHoverValid = true;
			return;
		}
	}

	const float Tile = Driver ? Driver->GetTileSize() : (Sim ? Sim->TileSizeUU : 100.f);
	const AActor* Anchor = GetAnchor();
	const FVector2D OriginXY = Anchor
		? FVector2D(Anchor->GetActorLocation().X, Anchor->GetActorLocation().Y)
		: FVector2D::ZeroVector;

	const float Yaw = Driver ? Driver->GetGridYaw() : 0.f;
	const EHexOrientation Ori = Driver ? Driver->GetGridOrientation() : EHexOrientation::PointyTop;

	FVector2D P(World.X - OriginXY.X, World.Y - OriginXY.Y);
	const FVector2D Local = HexCoord::Rotate2D(P, -Yaw);
	const FHexAxial A = HexCoord::XYToAxialRound(Local, Tile, Ori);

	HoverHex = FHex{ A.Q, A.R };
	bHoverValid = true;
}

void UHexPlacementComponent::UpdatePreviewVisual()
{
	// Disable preview if in blueprint mode
	if (bDisablePreview)
	{
		if (PreviewComp)
		{
			PreviewComp->SetVisibility(false);
		}
		return;
	}
	
	if (!bHoverValid || !GetWorld()) return;

	const bool bPlaceOK = CanPlaceAt(HoverHex);
	const float Tile = Driver ? Driver->GetTileSize() : (Sim ? Sim->TileSizeUU : 100.f);

	const FVector CenterA = HexToWorld(HoverHex) + FVector(0, 0, PreviewZOffset);

	// Outline (keep single hex for non-bond, two for bond/unbond)
	if (PlaceKind != EPlaceKind::Bond && PlaceKind != EPlaceKind::Unbond)
	{
		DrawDebugCircle(GetWorld(), CenterA, Tile * 0.95f, 6,
			bPlaceOK ? FColor(0, 200, 40) : FColor(200, 20, 20),
			false, 0.f, 0, 2.f, FVector(1, 0, 0), FVector(0, 1, 0), false);
	}
	else
	{
		const FHex NeighborHex = HexUtil::Neighbor(HoverHex, (EHexDir)(BeltDir % 6));
		const FVector CenterB = HexToWorld(NeighborHex) + FVector(0, 0, PreviewZOffset);
		const FColor Col = bPlaceOK ? FColor(0, 200, 40) : FColor(200, 20, 20);
		DrawDebugCircle(GetWorld(), CenterA, Tile * 0.95f, 6, Col, false, 0.f, 0, 2.f, FVector(1, 0, 0), FVector(0, 1, 0), false);
		DrawDebugCircle(GetWorld(), CenterB, Tile * 0.95f, 6, Col, false, 0.f, 0, 2.f, FVector(1, 0, 0), FVector(0, 1, 0), false);
	}

	// Direction arrow only for Belt/Arm
	if (PlaceKind == EPlaceKind::Belt || PlaceKind == EPlaceKind::Arm)
	{
		const FHex NeighborHex = HexUtil::Neighbor(HoverHex, (EHexDir)(BeltDir % 6));
		const FVector CenterB = HexToWorld(NeighborHex) + FVector(0, 0, PreviewZOffset);
		const FVector DirVec = (CenterB - CenterA).GetSafeNormal();
		if (!DirVec.IsNearlyZero())
		{
			const float ArrowLen = (CenterB - CenterA).Size() * 0.55f;
			const FVector Tip = CenterA + DirVec * ArrowLen;
			const FColor Col = bPlaceOK ? FColor(0, 200, 40) : FColor(200, 20, 20);
			DrawDebugLine(GetWorld(), CenterA, Tip, Col, false, 0.f, 0, 3.f);
			const FVector Side(-DirVec.Y, DirVec.X, 0.f);
			DrawDebugLine(GetWorld(), Tip, Tip - DirVec * (ArrowLen * 0.25f) + Side * (ArrowLen * 0.15f), Col, false, 0.f, 0, 2.f);
			DrawDebugLine(GetWorld(), Tip, Tip - DirVec * (ArrowLen * 0.25f) - Side * (ArrowLen * 0.15f), Col, false, 0.f, 0, 2.f);
		}
	}

	// Mesh preview (now also for Input, Output, Belt, Arm, Bond, and Unbond)
	if (PlaceKind == EPlaceKind::Input || PlaceKind == EPlaceKind::Output || PlaceKind == EPlaceKind::Belt || PlaceKind == EPlaceKind::Arm || PlaceKind == EPlaceKind::Bond || PlaceKind == EPlaceKind::Unbond)
	{
		EnsurePreviewComponent();
		if (!PreviewComp) return;

		UStaticMesh* Mesh = SelectPreviewMesh();
		if (!Mesh)
		{
			PreviewComp->SetVisibility(false);
			return;
		}
		if (PreviewComp->GetStaticMesh() != Mesh)
			PreviewComp->SetStaticMesh(Mesh);

		const uint8 EffectiveDir = BeltDir % 6;
		const float Yaw = ComputeDirYawFromNeighbor(this, HoverHex, EffectiveDir);

		float FixedYaw = Yaw;
		if (PlaceKind == EPlaceKind::Arm)
		{
			FixedYaw = FMath::UnwindDegrees(FixedYaw + ArmPreviewFacingFixDeg);
		}
		else if (PlaceKind == EPlaceKind::Output)
		{
			// For output tiles, we need to match the grid orientation
			// If the grid is using FlatTop orientation, rotate the tile mesh 30 degrees
			const EHexOrientation Ori = Driver ? Driver->GetGridOrientation() : EHexOrientation::PointyTop;
			if (Ori == EHexOrientation::FlatTop)
			{
				FixedYaw = FMath::UnwindDegrees(FixedYaw + 30.0f);
			}
		}

		FVector UseLoc = CenterA;
		FVector UseScale(PreviewScale);

		if (PlaceKind == EPlaceKind::Bond || PlaceKind == EPlaceKind::Unbond)
		{
			// Midpoint between the two hex centers
			const FHex NeighborHex = HexUtil::Neighbor(HoverHex, (EHexDir)EffectiveDir);
			const FVector CenterB = HexToWorld(NeighborHex) + FVector(0, 0, PreviewZOffset);
			UseLoc = (CenterA + CenterB) * 0.5f;
			// Assume authored size fits two hexes; let user adjust in editor via mesh or PreviewScale
			UseScale = FVector(PreviewScale);
		}

		PreviewComp->SetWorldLocationAndRotation(UseLoc, FRotator(0.f, FixedYaw, 0.f));
		PreviewComp->SetWorldScale3D(UseScale);
		PreviewComp->SetVisibility(true);

		// Set material based on placement type
		if (PlaceKind == EPlaceKind::Input)
		{
			// For input tiles, use the actual orb material
			UMaterialInterface* OrbMaterial = GetActualOrbMaterial();
			if (OrbMaterial)
			{
				PreviewComp->SetMaterial(0, OrbMaterial);
			}
		}
		else if (PlaceKind == EPlaceKind::Output)
		{
			// For output tiles, use the actual orb material with half opacity
			UMaterialInterface* OrbMaterial = GetActualOutputOrbMaterial();
			if (OrbMaterial)
			{
				// Create a dynamic material instance for opacity control
				UMaterialInstanceDynamic* OutputMID = UMaterialInstanceDynamic::Create(OrbMaterial, this);
				if (OutputMID)
				{
					// Set half opacity (0.5 alpha)
					OutputMID->SetScalarParameterValue(FName("Opacity"), 0.5f);
					PreviewComp->SetMaterial(0, OutputMID);
				}
				else
				{
					PreviewComp->SetMaterial(0, OrbMaterial);
				}
			}
		}
		else if (PreviewMID)
		{
			// For other types, use the tinted material
			const FLinearColor Col = bPlaceOK
				? FLinearColor(0.f, 0.9f, 0.f, 0.5f)
				: FLinearColor(0.9f, 0.f, 0.f, 0.6f);
			PreviewMID->SetVectorParameterValue(FName("Tint"), Col);
		}
	}
	else if (PreviewComp)
	{
		PreviewComp->SetVisibility(false);
	}
}

UStaticMesh* UHexPlacementComponent::GetActualOutputOrbMesh() const
{
	// Try to find GameManager for tile mesh
	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<AGameManager> It(World); It; ++It)
		{
			AGameManager* GameManager = *It;
			if (GameManager && GameManager->TileMesh)
			{
				return GameManager->TileMesh;
			}
		}
	}
	
	// Fallback to local tile mesh if available
	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<AHexFactoryActor> It(World); It; ++It)
		{
			AHexFactoryActor* FactoryActor = *It;
			if (FactoryActor && FactoryActor->TileMesh)
			{
				return FactoryActor->TileMesh;
			}
		}
	}
	
	// Final fallback
	return GenericTilePreviewMesh;
}

UMaterialInterface* UHexPlacementComponent::GetActualOutputOrbMaterial() const
{
	// Try to find UOrbMaterialManager in the world
	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			if (UOrbMaterialManager* MaterialManager = (*It)->FindComponentByClass<UOrbMaterialManager>())
			{
				EOrbType CurrentType = GetCurrentOutputOrbType();
				UMaterialInterface* OrbMaterial = MaterialManager->GetMaterialForOrbType(CurrentType);
				if (OrbMaterial)
				{
					return OrbMaterial;
				}
			}
		}
	}
	
	// Final fallback
	return nullptr;
}
