#include "Framework/GameManager.h"
#include "Framework/GameTickManager.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"

AGameManager::AGameManager()
{
	PrimaryActorTick.bCanEverTick = true;

	// Create the ISM as root so it's easy to see in editor
	GridISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("GridISM"));
	SetRootComponent(GridISM);
	GridISM->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	GridISM->SetCollisionResponseToAllChannels(ECR_Block);
}

void AGameManager::OnConstruction(const FTransform&)
{
	// Keep Terrain consistent with exposed radius in the editor
	GenerateFilledRadius(GridRadius);
	BuildGridFromTerrain();
}

void AGameManager::BeginPlay()
{
	Super::BeginPlay();
	AutoFindRefs();
	TryLoadInitial();
	LastGridCenter = GetGridOrigin();

	// Ensure grid is visible at runtime
	GenerateFilledRadius(GridRadius);
	BuildGridFromTerrain();
}

void AGameManager::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (!bAutoExpandAtRuntime) return;

	const float Dist = FVector::Dist2D(LastGridCenter, GetGridOrigin());
	if (Dist > RecenterThresholdUU)
	{
		// Re-center & rebuild in-place (same radius for now)
		LastGridCenter = GetGridOrigin();
		BuildGridFromTerrain();
	}
}

void AGameManager::AutoFindRefs()
{
	UWorld* W = GetWorld(); if (!W) return;

	if (!TickManager)
		for (TActorIterator<AGameTickManager> It(W); It; ++It) { TickManager = *It; break; }

	if (!OrbSimDriver)
		for (TActorIterator<AActor> It(W); It; ++It)
		{
			AActor* A = *It; if (!A) continue;
			if (A->GetClass()->GetName().Contains(TEXT("OrbSimDriver"))) { OrbSimDriver = A; break; }
		}

	if (!OrbSimDriver && OrbSimDriverClass)
	{
		FActorSpawnParameters S; S.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		OrbSimDriver = W->SpawnActor<AActor>(OrbSimDriverClass, GetActorTransform(), S);
	}
}

void AGameManager::TryLoadInitial()
{
	if (InitialMapToLoad.IsEmpty() || !OrbSimDriver) return;
	const FString Cmd = FString::Printf(TEXT("LoadMap %s"), *InitialMapToLoad);
	if (!OrbSimDriver->CallFunctionByNameWithArguments(*Cmd, *GLog, nullptr, true))
	{
		UE_LOG(LogTemp, Warning, TEXT("GameManager: OrbSimDriver didn't accept Exec 'LoadMap'."));
	}
}

void AGameManager::EnsureGridComponents()
{
	if (!GridISM)
	{
		GridISM = NewObject<UInstancedStaticMeshComponent>(this, TEXT("GridISM_Runtime"));
		GridISM->SetupAttachment(GetRootComponent());
		GridISM->RegisterComponent();
	}
}

FVector AGameManager::GetGridOrigin() const
{
	if (GridAnchor)
	{
		return GridAnchor->GetComponentLocation();
	}
	return GetActorLocation();
}

void AGameManager::GenerateFilledRadius(int32 Radius)
{
	Terrain.Reset();
	const int32 R = FMath::Max(0, Radius);
	for (int q = -R; q <= R; ++q)
	{
		for (int r = -R; r <= R; ++r)
		{
			const int s = -q - r;
			if (FMath::Max3(FMath::Abs(q), FMath::Abs(r), FMath::Abs(s)) <= R)
			{
				Terrain.Add(FHex(q, r));
			}
		}
	}
}

void AGameManager::BuildGridFromTerrain()
{
	EnsureGridComponents();

	GridISM->ClearInstances();
	if (TileMesh) GridISM->SetStaticMesh(TileMesh);
	if (TileMaterial) GridISM->SetMaterial(0, TileMaterial);

	const float S = FMath::Max(1.f, TileSizeUU);
	const FVector Origin = GetGridOrigin();

	auto HexToWorld = [S, Origin](const FHex& H)->FVector
	{
		// Pointy-top axial to world (no yaw for now)
		const float x = S * (1.5f * H.Q);
		const float y = S * ((FMath::Sqrt(3.f)/2.f) * (2*H.R + H.Q));
		return Origin + FVector(x, y, 0.f);
	};

	for (const FHex& H : Terrain)
	{
		const FVector P = HexToWorld(H);
		GridISM->AddInstance(FTransform(FRotator::ZeroRotator, P, FVector::OneVector));
	}

	LastGridCenter = Origin;
}

void AGameManager::Rebuild()
{
	GenerateFilledRadius(GridRadius);
	BuildGridFromTerrain();
}

void AGameManager::SetGridRadius(int32 Radius)
{
	GridRadius = FMath::Max(0, Radius);
	Rebuild();
}

void AGameManager::SetTileMeshAndMaterial(UStaticMesh* Mesh, UMaterialInterface* Mat)
{
	TileMesh = Mesh;
	TileMaterial = Mat;
	BuildGridFromTerrain();
}

AGameManager* AGameManager::FindInWorld(const UObject* WorldContextObject)
{
	if (!WorldContextObject) return nullptr;
	if (const UWorld* World = WorldContextObject->GetWorld())
	{
		for (TActorIterator<AGameManager> It(World); It; ++It)
		{
			return *It;
		}
	}
	return nullptr;
}
