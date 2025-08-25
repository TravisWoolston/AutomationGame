#include "Actors/OrbSimDriver.h"
#include "Sim/SimWorldHex.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Framework/GameTickManager.h"

AOrbSimDriver::AOrbSimDriver()
{
	PrimaryActorTick.bCanEverTick = false;
	UE_LOG(LogTemp, Verbose, TEXT("OrbSimDriver ctor: %s"), *GetNameSafe(this));
}

void AOrbSimDriver::BeginPlay()
{
	Super::BeginPlay();
	UE_LOG(LogTemp, Log, TEXT("OrbSimDriver::BeginPlay on %s"), *GetName());
	if (!Sim)
	{
		Sim = NewObject<USimWorldHex>(this);
		Sim->Initialize(20);
		UE_LOG(LogTemp, Log, TEXT("OrbSimDriver: Created Sim object %s"), *GetNameSafe(Sim));
	}
	// Align sim spacing to our knob
	if (GM == nullptr)
	{
		for (TActorIterator<AGameManager> It(GetWorld()); It; ++It)
		{
			GM = *It;
			break;
		}
		UE_LOG(LogTemp, Log, TEXT("OrbSimDriver: Resolved GM=%s"), *GetNameSafe(GM));
	}
	if (Sim && GM != nullptr) {
		Sim->TileSizeUU = GM->TileSizeUU;
		TileSizeUU = Sim->TileSizeUU;
		UE_LOG(LogTemp, Log, TEXT("OrbSimDriver: TileSize set to %.2f from GM"), TileSizeUU);
	}

	// Try to bind to the global GameTickManager
	if (AGameTickManager* M = AGameTickManager::Get(this))
	{
		M->OnFixedTick.AddDynamic(this, &AOrbSimDriver::HandleFixedTickSeconds);
		UE_LOG(LogTemp, Log, TEXT("OrbSimDriver: Bound to GameTickManager %s"), *GetNameSafe(M));
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("OrbSimDriver: No GameTickManager found. Sim will only advance via Tick()."));
	}
}

//void AOrbSimDriver::Tick(float DeltaSeconds)
//{
//	Super::Tick(DeltaSeconds);
//	if (!Sim) return;
//
//	// Keep sim spacing in sync if user tweaks TileSizeUU in editor during PIE
//	if (!FMath::IsNearlyEqual(Sim->TileSizeUU, TileSizeUU))
//	{
//		Sim->TileSizeUU = TileSizeUU;
//		Sim->TileRevision++; // force a visual rebuild
//	}
//
//	Accum += DeltaSeconds;
//	const float Step = Sim->FixedStepSec;
//	while (Accum >= Step)
//	{
//		Accum -= Step;
//		Sim->StepSim();
//		StepAlpha = 0.f;
//	}
//	StepAlpha = Accum / Step;
//}

void AOrbSimDriver::HandleFixedTickSeconds(float StepSeconds)
{
	if (!Sim)
	{
		UE_LOG(LogTemp, Warning, TEXT("OrbSimDriver::HandleFixedTickSeconds called but Sim is null."));
		return;
	}
	// Advance simulation by fixed step
	Sim->StepSim();
	StepAlpha = 0.f;
	UE_LOG(LogTemp, Verbose, TEXT("OrbSimDriver: FixedStep %.3fs applied."), StepSeconds);
}

FVector2D AOrbSimDriver::GetGridOriginXY() const
{
	if (GridAnchor)
	{
		const FVector L = GridAnchor->GetActorLocation();
		return FVector2D(L.X, L.Y);
	}
	return FVector2D::ZeroVector;
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
