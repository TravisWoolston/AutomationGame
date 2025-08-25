#include "Framework/GameManager.h"
#include "Framework/GameTickManager.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Actors/BasicArm.h"
#include "Actors/OrbSimDriver.h"
#include "Sim/HexCoord.h"
#include "DrawDebugHelpers.h"

AGameManager::AGameManager()
{
	PrimaryActorTick.bCanEverTick = true; // needed for auto-recenter
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

	const float S = FMath::Max(1.f, TileSizeUU);
	const FVector Origin = GridAnchor ? GridAnchor->GetActorLocation() : GetActorLocation();

	const auto ToWorld = [S, Origin](int32 Q, int32 R)->FVector
		{
			// pointy-top axial spacing
			const float X = S * (1.5f * Q);
			const float Y = S * ((FMath::Sqrt(3.f) / 2.f) * (2 * R + Q));
			return Origin + FVector(X, Y, 0.f);
		};

	for (int r = -Radius; r <= Radius; ++r)
	{
		for (int q = -Radius; q <= Radius; ++q)
		{
			const int s = -q - r;
			if (FMath::Max3(FMath::Abs(q), FMath::Abs(r), FMath::Abs(s)) > Radius) continue;
			const FVector P = ToWorld(q, r);
			ISM->AddInstance(FTransform(FRotator::ZeroRotator, P, FVector(1)));
		}
	}
}

void AGameManager::Rebuild()
{
	EnsureISM();
	BuildGridInstances();
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

	// Compute world from grid
	const float Tile = OrbSimDriver->GetTileSize();
	const float GridYaw = OrbSimDriver->GetGridYaw();
	const EHexOrientation Ori = OrbSimDriver->GetGridOrientation();
	const AActor* Anchor = OrbSimDriver->GetGridAnchor();
	const FVector AnchorLoc = Anchor ? Anchor->GetActorLocation() : FVector::ZeroVector;

	const FVector2D XY = HexCoord::AxialToXY(FHexAxial(Hex.Q, Hex.R), Tile, Ori);
	const FVector2D RotXY = HexCoord::Rotate2D(XY, GridYaw);
	const FVector   Loc = AnchorLoc + FVector(RotXY.X, RotXY.Y, 0.f);
	const float     Yaw = GridYaw + 60.f * float(Facing % 6);

	FActorSpawnParameters S; S.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	ABasicArm* Arm = GetWorld()->SpawnActor<ABasicArm>(BasicArmClass, FTransform(FRotator(0, Yaw, 0), Loc), S);
	if (!Arm) return nullptr;

	Arm->Driver = OrbSimDriver;
	Arm->Q = Hex.Q;
	Arm->R = Hex.R;
	Arm->FacingDir = Facing;
	Arm->SetProgram(Program, bLoop);

	return Arm;
}
