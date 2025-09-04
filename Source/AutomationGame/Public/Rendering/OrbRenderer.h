#pragma once
#include "AutomationGameMinimal.h"
#include "Components/ActorComponent.h"
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
	UPROPERTY(EditAnywhere, Category = "Visual") UStaticMesh* OrbMesh = nullptr;
	UPROPERTY(EditAnywhere, Category = "Visual") UMaterialInterface* OrbMaterial = nullptr;
	UPROPERTY(EditAnywhere, Category = "Visual") float OrbScale = 0.30f;

	/** If true use sim's belt interpolation (BeltProgress + FrameAlpha) instead of prev/curr hex lerp. */
	UPROPERTY(EditAnywhere, Category = "Visual")
	bool bUseSimVisualInterpolation = true;

private:
	void EnsureISM();
	void RebuildCurrPos();
	void RebuildInstances();
	AActor* GetAnchor() const;

	UPROPERTY(Transient) UInstancedStaticMeshComponent* ISM = nullptr;
	UPROPERTY(Transient) AOrbSimDriver* Driver = nullptr;
	UPROPERTY(Transient) USimWorldHex* Sim = nullptr;

	TMap<int32, FVector> PrevPos, CurrPos;
	TMap<int32, FRotator> PrevRot, CurrRot;
	float PrevAlpha = 1.f;
	float DirToYaw(uint8 Dir) const;

	int32 LastSeenStepCounter = -1;
};