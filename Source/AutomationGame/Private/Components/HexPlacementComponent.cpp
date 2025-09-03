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

UHexPlacementComponent::UHexPlacementComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
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
}

static void ReacquireSimIfNeeded(UWorld* W, AOrbSimDriver*& Driver, USimWorldHex*& Sim)
{
	if (!Sim || !Driver)
	{
		Driver = AOrbSimDriver::Find(W);
		if (Driver) Sim = Driver->GetSim();
	}
}

void UHexPlacementComponent::TickComponent(float DeltaTime, enum ELevelTick, FActorComponentTickFunction*)
{
	// Keep sim refs fresh
	if ((!Driver || !Sim) && GetWorld()) ReacquireSimIfNeeded(GetWorld(), Driver, Sim);

	// Always update hover + preview if we have a player controller (independent of input polling)
	if (PC)
	{
		UpdateHoverHex();
		UpdatePreviewVisual();
	}

	// Input polling (placing / removing) still optional
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
	if (!Sim)
	{
		if (GEngine) GEngine->AddOnScreenDebugMessage(-1, 2.f, FColor::Red, TEXT("No Sim/Driver found; cannot place."));
		UE_LOG(LogTemp, Warning, TEXT("HexPlacementComponent: missing Sim/Driver; cannot place."));
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

	FTile T{};
	switch (PlaceKind)
	{
	case EPlaceKind::Input:  T = FTile{ ETileKind::Input,  0 }; break;
	case EPlaceKind::Output: T = FTile{ ETileKind::Output, 0 }; break;
	case EPlaceKind::Belt:   T = FTile{ ETileKind::Belt,   (uint8)BeltDir }; break;
	case EPlaceKind::Arm:
	{
		AActor* GMActor = nullptr;
		for (TActorIterator<AActor> It(GetWorld()); It; ++It)
		{
			if (It->IsA<AGameManager>()) { GMActor = *It; break; }
		}
		AGameManager* GM = Cast<AGameManager>(GMActor);
		if (GM)
		{
			GM->SpawnBasicArmAt(FHex{ A.Q, A.R }, BeltDir % 6, TArray<FArmInstr>(), true);
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("HexPlacementComponent: cannot spawn Arm; no GameManager found."));
			if (GEngine)
			{
				GEngine->AddOnScreenDebugMessage(-1, 2.f, FColor::Red,
					TEXT("Cannot spawn Arm; no GameManager found."));
			}
		}
		return;
	}
	default:
		UE_LOG(LogTemp, Warning, TEXT("HexPlacementComponent: PlaceKind not set; skipping."));
		return;
	}

	Sim->PlaceTile(FHex{ A.Q, A.R }, T);

	if (bDebug)
	{
		if (T.Kind == ETileKind::Belt)
		{
			UE_LOG(LogTemp, Log, TEXT("HexPlacementComponent: Placed Belt at Hex(%d,%d) Dir=%d"),
				A.Q, A.R, (int32)BeltDir);
			if (GEngine)
			{
				GEngine->AddOnScreenDebugMessage(-1, 1.5f, FColor::Cyan,
					FString::Printf(TEXT("Placed Belt at Hex(%d,%d) Dir=%d"), A.Q, A.R, (int32)BeltDir));
			}
		}
		else
		{
			const FString KindName = StaticEnum<ETileKind>()->GetNameStringByValue((int64)T.Kind);
			UE_LOG(LogTemp, Log, TEXT("HexPlacementComponent: Placed %s at Hex(%d,%d)"),
				*KindName, A.Q, A.R);
			if (GEngine)
			{
				GEngine->AddOnScreenDebugMessage(-1, 1.5f, FColor::Cyan,
					FString::Printf(TEXT("Placed %s at Hex(%d,%d)"), *KindName, A.Q, A.R));
			}
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
	UE_LOG(LogTemp, Log, TEXT("HexPlacementComponent: Removed tile at Hex(%d,%d)"), A.Q, A.R);

	if (bDebug && GEngine)
	{
		GEngine->AddOnScreenDebugMessage(-1, 1.5f, FColor::Yellow,
			FString::Printf(TEXT("Removed tile at Hex(%d,%d)"), A.Q, A.R));
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

void UHexPlacementComponent::RotateCW() { BeltDir = (BeltDir + 1) % 6; }
void UHexPlacementComponent::RotateCCW() { BeltDir = (BeltDir + 5) % 6; }
void UHexPlacementComponent::SetPlaceKind(EPlaceKind NewKind) { PlaceKind = NewKind; }

/* ---------------- Preview / Hover ---------------- */

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
	case EPlaceKind::Output:
		return GenericTilePreviewMesh;
	case EPlaceKind::Belt:
		return BeltMeshPreview ? BeltMeshPreview : GenericTilePreviewMesh;
	case EPlaceKind::Arm:
		return ArmBasePreviewMesh ? ArmBasePreviewMesh : GenericTilePreviewMesh;
	default:
		return GenericTilePreviewMesh;
	}
}

bool UHexPlacementComponent::CanPlaceAt(const FHex& Hex) const
{
	if (!Sim) return true;
	// If Sim exposes a query, use it here. Placeholder returns true (allow all).
	// Example (uncomment if available):
	// const FTile Existing = Sim->GetTile(Hex);
	// return Existing.Kind == ETileKind::Empty;
	return true;
}

FVector UHexPlacementComponent::HexToWorld(const FHex& H) const
{
	// Compute hex center from axial coordinates (approximation based on chosen orientation).
	// Assumption: 'Tile' represents the hex radius (distance from center to a vertex).
	if (!Driver && !Sim)
	{
		return LastHoverWorld; // fallback
	}

	const float Tile = Driver ? Driver->GetTileSize() : (Sim ? Sim->TileSizeUU : 100.f);
	const float PlaneZ = Driver ? Driver->GetGridPlaneZ(GetOwner()) :
		(GetOwner() ? GetOwner()->GetActorLocation().Z : 0.f);
	const float YawDeg = Driver ? Driver->GetGridYaw() : 0.f;
	const EHexOrientation Ori = Driver ? Driver->GetGridOrientation() : EHexOrientation::PointyTop;

	// Axial q,r
	const float q = (float)H.Q;
	const float r = (float)H.R;

	FVector2D Local(0, 0);
	const float Sqrt3 = 1.7320508075688772f;

	if (Ori == EHexOrientation::PointyTop)
	{
		// pointy-top axial -> pixel
		// x = size * (sqrt(3) * q + sqrt(3)/2 * r)
		// y = size * (3/2 * r)
		Local.X = Tile * (Sqrt3 * q + (Sqrt3 * 0.5f) * r);
		Local.Y = Tile * (1.5f * r);
	}
	else
	{
		// flat-top axial -> pixel
		// x = size * (3/2 * q)
		// y = size * (sqrt(3) * r + sqrt(3)/2 * q)
		Local.X = Tile * (1.5f * q);
		Local.Y = Tile * (Sqrt3 * r + (Sqrt3 * 0.5f) * q);
	}

	// Rotate back into world space
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

// ADD near other includes if HexUtil not already available (it is via HexCoord.h)
// #include "Sim/HexCoord.h"

// --- Helper: compute yaw from HoverHex to its neighbor in BeltDir (center-to-center) ---
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

void UHexPlacementComponent::UpdatePreviewVisual()
{
	if (!bHoverValid || !GetWorld()) return;

	const bool bPlaceOK = CanPlaceAt(HoverHex);
	const FVector Center = HexToWorld(HoverHex) + FVector(0, 0, PreviewZOffset);

	// 1) Draw outline (kept)
	const float Tile = Driver ? Driver->GetTileSize() : (Sim ? Sim->TileSizeUU : 100.f);
	const float Radius = Tile * 0.95f;
	const FColor Color = bPlaceOK ? FColor(0, 200, 40, 255) : FColor(200, 20, 20, 255);

	DrawDebugCircle(
		GetWorld(),
		Center,
		Radius,
		6,
		Color,
		false,
		0.f,
		0,
		2.f,
		FVector(1, 0, 0),
		FVector(0, 1, 0),
		false
	);

	// 2) Direction arrow (Belts + Arms only) using actual neighbor center -> correct flat-edge traversal
	if (PlaceKind == EPlaceKind::Belt || PlaceKind == EPlaceKind::Arm)
	{
		const FHex Neighbor = HexUtil::Neighbor(HoverHex, (EHexDir)(BeltDir % 6));
		const FVector NCenter = HexToWorld(Neighbor) + FVector(0, 0, PreviewZOffset);
		const FVector DirVec = (NCenter - Center).GetSafeNormal();
		if (!DirVec.IsNearlyZero())
		{
			const float ArrowLen = (NCenter - Center).Size() * 0.55f; // shorter than full distance
			const FVector Tip = Center + DirVec * ArrowLen;
			DrawDebugLine(GetWorld(), Center, Tip, Color, false, 0.f, 0, 3.f);
			const FVector Side = FVector(-DirVec.Y, DirVec.X, 0.f);
			DrawDebugLine(GetWorld(), Tip, Tip - DirVec * (ArrowLen * 0.25f) + Side * (ArrowLen * 0.15f), Color, false, 0.f, 0, 2.f);
			DrawDebugLine(GetWorld(), Tip, Tip - DirVec * (ArrowLen * 0.25f) - Side * (ArrowLen * 0.15f), Color, false, 0.f, 0, 2.f);
		}
	}

	// 3) Mesh preview only for Belt / Arm (per request: no generic tile preview)
	if (PlaceKind == EPlaceKind::Belt || PlaceKind == EPlaceKind::Arm)
	{
		EnsurePreviewComponent();
		if (PreviewComp)
		{
			if (UStaticMesh* Mesh = SelectPreviewMesh())
			{
				if (PreviewComp->GetStaticMesh() != Mesh)
					PreviewComp->SetStaticMesh(Mesh);

				// Compute yaw via neighbor delta so rotation matches travel direction
				const float Yaw = ComputeDirYawFromNeighbor(this, HoverHex, BeltDir);
				const FRotator R(0.f, Yaw, 0.f);
				PreviewComp->SetWorldLocationAndRotation(Center, R);
				PreviewComp->SetWorldScale3D(FVector(PreviewScale));
				PreviewComp->SetVisibility(true);

				if (PreviewMID)
				{
					const FLinearColor Col = bPlaceOK
						? FLinearColor(0.f, 0.9f, 0.f, 0.5f)
						: FLinearColor(0.9f, 0.f, 0.f, 0.6f);
					PreviewMID->SetVectorParameterValue(FName("Tint"), Col);
				}
			}
			else
			{
				PreviewComp->SetVisibility(false);
			}
		}
	}
	else
	{
		if (PreviewComp)
			PreviewComp->SetVisibility(false);
	}
}
