#include "Components/HexPlacementComponent.h"
#include "Actors/OrbSimDriver.h"
#include "Sim/SimWorldHex.h"
#include "Sim/Tiles.h"
#include "Sim/HexCoord.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Actor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "DrawDebugHelpers.h"

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
    if (!bUseInputPolling) return;

    if ((!Driver || !Sim) && GetWorld()) ReacquireSimIfNeeded(GetWorld(), Driver, Sim);
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
        if (GEngine) GEngine->AddOnScreenDebugMessage(-1, 2.f, FColor::Red, TEXT("No Sim/Driver found; cannot place."));
        return;
    }

    const float Tile = Driver ? Driver->GetTileSize() : Sim->TileSizeUU;
    const AActor* Anchor = GetAnchor();
    const FVector2D OriginXY = Anchor
        ? FVector2D(Anchor->GetActorLocation().X, Anchor->GetActorLocation().Y)
        : FVector2D::ZeroVector;

    FVector2D P(World.X - OriginXY.X, World.Y - OriginXY.Y);

    const float Yaw = Driver ? Driver->GetGridYaw() : 0.f;
    const EHexOrientation Ori = Driver ? Driver->GetGridOrientation() : EHexOrientation::PointyTop;

    const FVector2D Local = HexCoord::Rotate2D(P, -Yaw);
    const FHexAxial A = HexCoord::XYToAxialRound(Local, Tile, Ori);

    FTile T;
    switch (PlaceKind)
    {
        case EPlaceKind::Input:  T = FTile{ETileKind::Input,  0}; break;
        case EPlaceKind::Output: T = FTile{ETileKind::Output, 0}; break;
        case EPlaceKind::Belt:   T = FTile{ETileKind::Belt,   (uint8)BeltDir}; break;
        default: return;
    }
    Sim->PlaceTile(FHex{A.Q, A.R}, T);

    if (bDebug && GEngine)
    {
        GEngine->AddOnScreenDebugMessage(-1, 1.5f, FColor::Cyan,
            FString::Printf(TEXT("Placed %s at Hex(%d,%d)"),
                *StaticEnum<ETileKind>()->GetNameStringByValue((int64)T.Kind), A.Q, A.R));
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

    Sim->PlaceTile(FHex{A.Q, A.R}, FTile{ETileKind::Empty, 0});
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

void UHexPlacementComponent::RotateCW(){ BeltDir = (BeltDir + 1) % 6; }
void UHexPlacementComponent::RotateCCW(){ BeltDir = (BeltDir + 5) % 6; }
void UHexPlacementComponent::SetPlaceKind(EPlaceKind NewKind){ PlaceKind = NewKind; }
