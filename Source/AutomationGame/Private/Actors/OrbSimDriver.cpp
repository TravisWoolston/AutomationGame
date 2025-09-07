#include "Actors/OrbSimDriver.h"
#include "Framework/GameTickManager.h"
#include "Framework/GameManager.h"
#include "Sim/SimWorldHex.h"
#include "Engine/World.h"
#include "EngineUtils.h"

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
			// Keep sim's spacing aligned with our knob at start.
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









