#pragma once
#include "AutomationGameMinimal.h"
#include "Components/ActorComponent.h"
#include "Sim/ArmTypes.h"
#include "Sim/HexTypes.h"
#include "Sim/HexCoord.h"          // <-- Needed for EHexOrientation
#include "Sim/OrbTypes.h"          // <-- Needed for EOrbType
#include "OrbRenderer.generated.h"

class UInstancedStaticMeshComponent;
class USimWorldHex;
class AOrbSimDriver;
class UStaticMesh;
class UMaterialInterface;

/**
 * Renders orbs from the sim via an ISM. Robust to load order:
 * - Reacquires the driver/sim automatically if missing.
 * - Creates ISM in OnRegister so it works when the component is already placed in the level.
 */
UCLASS(ClassGroup = (Custom), meta = (BlueprintSpawnableComponent))
class AUTOMATIONGAME_API UOrbRenderer : public UActorComponent
{
	GENERATED_BODY()

public:
	UOrbRenderer();

	/** Create the ISM as soon as the component registers */
	virtual void OnRegister() override;
	virtual void BeginPlay() override;
	virtual void TickComponent(float DeltaTime, enum ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	/** Optional override; otherwise we read anchor from the driver */
	UPROPERTY(EditAnywhere, Category = "Grid") bool bOverrideAnchor = false;
	UPROPERTY(EditAnywhere, Category = "Grid", meta = (EditCondition = "bOverrideAnchor")) AActor* AnchorOverride = nullptr;

	/** Auto repair references if driver/sim is not found at BeginPlay (eg. spawned later) */
	UPROPERTY(EditAnywhere, Category = "Debug") bool bAutoReacquire = true;

	/** Basic visuals */
	// Note: OrbMesh and OrbMaterial are now managed by UOrbMaterialManager
	UPROPERTY(EditAnywhere, Category = "Visual") float OrbScale = 0.30f;

	/** Bond connection visuals */
	UPROPERTY(EditAnywhere, Category = "Bonds") UStaticMesh* BondedOrbMesh = nullptr;
	UPROPERTY(EditAnywhere, Category = "Bonds") UMaterialInterface* BondedOrbMaterial = nullptr;
	UPROPERTY(EditAnywhere, Category = "Bonds") float BondedOrbScale = 0.15f;

	/** If true use sim's belt interpolation (BeltProgress + FrameAlpha) instead of prev/curr hex lerp. */
	UPROPERTY(EditAnywhere, Category = "Visual")
	bool bUseSimVisualInterpolation = true;

private:
	void EnsureISM();
	void RebuildCurrPos();
	void RebuildInstances();
	void RebuildBondedOrbs();
	AActor* GetAnchor() const;

	UPROPERTY(Transient) TMap<EOrbType, UInstancedStaticMeshComponent*> OrbTypeISMs;
	UPROPERTY(Transient) UInstancedStaticMeshComponent* BondedOrbISM = nullptr;
	UPROPERTY(Transient) AOrbSimDriver* Driver = nullptr;
	UPROPERTY(Transient) USimWorldHex* Sim = nullptr;

	TMap<int32, FVector> PrevPos, CurrPos;
	TMap<int32, FRotator> PrevRot, CurrRot;
	float PrevAlpha = 1.f;
	float DirToYaw(uint8 Dir) const;

	int32 LastSeenStepCounter = -1;

protected:
	// New: arm facing history for arc interpolation
	TMap<int32, uint8> PrevArmFacing;
	TMap<int32, uint8> CurrArmFacing;

	// Helper for rotational interpolation of held items
	FVector ComputeArmGripAtYaw(
		const FArmState& S,
		float YawDeg,
		const AActor* Anchor,
		float Tile,
		float GridYaw,
		EHexOrientation Ori) const;

	// Render animated orbs for input/output effects
	void RenderAnimatedOrbs();
};