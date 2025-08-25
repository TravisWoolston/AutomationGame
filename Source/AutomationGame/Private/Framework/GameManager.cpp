#include "Framework/GameManager.h"
#include "Framework/GameTickManager.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
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
	{
		for (TActorIterator<AGameTickManager> It(W); It; ++It) { TickManager = *It; break; }
		if (!TickManager) UE_LOG(LogTemp, Warning, TEXT("GameManager::AutoFindRefs - No GameTickManager found."));
	}

	if (!OrbSimDriver)
	{
		for (TActorIterator<AActor> It(W); It; ++It)
		{
			AActor* A = *It; if (!A) continue;
			if (A->GetClass()->GetName().Contains(TEXT("OrbSimDriver"))) { OrbSimDriver = A; break; }
		}
		if (!OrbSimDriver && OrbSimDriverClass)
		{
			FActorSpawnParameters S; S.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			OrbSimDriver = W->SpawnActor<AActor>(OrbSimDriverClass, GetActorTransform(), S);
			UE_LOG(LogTemp, Log, TEXT("GameManager::AutoFindRefs - Spawned OrbSimDriver: %s"), OrbSimDriver ? *OrbSimDriver->GetName() : TEXT("FAILED"));
		}
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
