#include "Rendering/OrbRenderer.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Actors/OrbSimDriver.h"
#include "Sim/SimWorldHex.h"
#include "Sim/HexTypes.h"
#include "Sim/HexCoord.h"
#include "Engine/World.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "UObject/ConstructorHelpers.h"
#include "Engine/Engine.h"
#include "GameFramework/Actor.h"

UOrbRenderer::UOrbRenderer()
{
    PrimaryComponentTick.bCanEverTick = true;

    static ConstructorHelpers::FObjectFinder<UStaticMesh> SphereMesh(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
    static ConstructorHelpers::FObjectFinder<UMaterialInterface> BasicMat(TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
    if (!OrbMesh && SphereMesh.Succeeded()) OrbMesh = SphereMesh.Object;
    if (!OrbMaterial && BasicMat.Succeeded()) OrbMaterial = BasicMat.Object;
}

void UOrbRenderer::OnRegister()
{
    Super::OnRegister();
    EnsureISM();
}

void UOrbRenderer::BeginPlay()
{
    Super::BeginPlay();

    EnsureISM();

    if (UWorld* W = GetWorld())
    {
        Driver = AOrbSimDriver::Find(W);
        if (Driver) Sim = Driver->GetSim();
    }

    RebuildCurrPos();
    PrevPos = CurrPos;
}

void UOrbRenderer::EnsureISM()
{
    if (ISM) return;

    AActor* Owner = GetOwner();
    if (!Owner) return;

    ISM = NewObject<UInstancedStaticMeshComponent>(Owner, TEXT("OrbRenderer_ISM"));
    ISM->RegisterComponent();

    if (Owner->GetRootComponent())
    {
        ISM->AttachToComponent(Owner->GetRootComponent(), FAttachmentTransformRules::KeepRelativeTransform);
    }

    if (OrbMesh) ISM->SetStaticMesh(OrbMesh);
    if (OrbMaterial) ISM->SetMaterial(0, OrbMaterial);
    ISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
}

AActor* UOrbRenderer::GetAnchor() const
{
    if (bOverrideAnchor && AnchorOverride) return AnchorOverride;
    return Driver ? Driver->GetGridAnchor() : nullptr;
}

void UOrbRenderer::TickComponent(float DeltaTime, enum ELevelTick, FActorComponentTickFunction*)
{
    if (bAutoReacquire && (!Driver || !Sim))
    {
        if (UWorld* W = GetWorld())
        {
            Driver = AOrbSimDriver::Find(W);
            if (Driver) Sim = Driver->GetSim();
            if (Sim) { RebuildCurrPos(); PrevPos = CurrPos; }
        }
    }
    if (!Driver) return;

    const float Alpha = Driver->GetStepAlpha();

    // If a new step started, rebuild currents
    if (Alpha < PrevAlpha)
    {
        PrevPos = CurrPos;
        RebuildCurrPos();
    }
    PrevAlpha = Alpha;

    RebuildInstances();
}

void UOrbRenderer::RebuildCurrPos()
{
    CurrPos.Reset();
    if (!Sim) return;

    const AActor* Anchor = GetAnchor();
    const FVector AnchorV = Anchor ? Anchor->GetActorLocation() : FVector::ZeroVector;
    const float Tile = Driver ? Driver->GetTileSize() : Sim->TileSizeUU;
    const float Yaw = Driver ? Driver->GetGridYaw() : 0.f;
    const EHexOrientation Ori = Driver ? Driver->GetGridOrientation() : EHexOrientation::PointyTop;

    for (const auto& KV : Sim->ItemMap)
    {
        const int32 Id = KV.Value.Id;
        if (Id == 0) continue;
        const FHexAxial A(KV.Key.Q, KV.Key.R);
        const FVector2D LocalXY = HexCoord::AxialToXY(A, Tile, Ori);
        const FVector2D RotXY = HexCoord::Rotate2D(LocalXY, Yaw);
        const FVector Loc = AnchorV + FVector(RotXY.X, RotXY.Y, 0.f);
        CurrPos.Add(Id, Loc);
    }

    for (const FArmState& S : Sim->Arms)
    {
        if (!S.bHolding) continue;
        FHex Grip = S.BaseHex;
        for (int i=0;i<S.ArmLen;++i) Grip = HexUtil::Neighbor(Grip, S.Facing);
        const FHexAxial A(Grip.Q, Grip.R);
        const FVector2D LocalXY = HexCoord::AxialToXY(A, Tile, Ori);
        const FVector2D RotXY = HexCoord::Rotate2D(LocalXY, Yaw);
        const FVector Loc = AnchorV + FVector(RotXY.X, RotXY.Y, 0.f);
        if (S.Held.Id != 0) CurrPos.Add(S.Held.Id, Loc);
    }
}

void UOrbRenderer::RebuildInstances()
{
    EnsureISM();
    if (!ISM || !Driver) return;
    const float Alpha = Driver->GetStepAlpha();

    TSet<int32> Keys;
    PrevPos.GetKeys(Keys);
    TArray<int32> CurrKeys; CurrPos.GetKeys(CurrKeys);
    for (int32 K : CurrKeys) Keys.Add(K);

    ISM->ClearInstances();
    if (!OrbMesh) return;

    for (int32 Id : Keys)
    {
        const FVector* A = PrevPos.Find(Id);
        const FVector* B = CurrPos.Find(Id);
        if (!A && !B) continue;
        const FVector From = A ? *A : (B ? *B : FVector::ZeroVector);
        const FVector To   = B ? *B : From;
        const FVector P    = FMath::Lerp(From, To, Alpha);
        ISM->AddInstance(FTransform(FRotator::ZeroRotator, P, FVector(OrbScale)));
    }
}
