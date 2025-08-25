#pragma once

#include "AutomationGameMinimal.h"
#include "Sim/HexCoord.h"
#include "GameFramework/Actor.h"
#include "OrbSimDriver.generated.h"

class USimWorldHex;
class AGameManager;

/**
 * Drives the hex-world simulation.
 * IMPORTANT: Simulation steps ONLY when GameTickManager broadcasts OnFixedTick.
 * Tick() is used only to update an interpolation alpha for rendering.
 */
UCLASS()
class AUTOMATIONGAME_API AOrbSimDriver : public AActor
{
	GENERATED_BODY()

public:
	AOrbSimDriver();

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

	/** Called by GameTickManager each fixed step. */
	UFUNCTION() void HandleFixedTickSeconds(float StepSeconds);

	// ---------------------------------------------------------------------
	// Grid / environment knobs (consumed by placement & rendering)
	// ---------------------------------------------------------------------

	/** Centralized grid anchor (used by placement & picking). */
	UPROPERTY(EditAnywhere, Category = "Grid")
	AActor* GridAnchor = nullptr;

	/** If true, use GridAnchor's Z for the mouse hit plane. */
	UPROPERTY(EditAnywhere, Category = "Grid")
	bool bUseAnchorZForPicking = true;

	/** PointyTop vs FlatTop orientation of the hex grid. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid")
	EHexOrientation GridOrientation = EHexOrientation::PointyTop;

	/** World yaw (deg) for the grid around the anchor. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid", meta = (ClampMin = -360.0, ClampMax = 360.0))
	float GridYawDeg = 0.f;

	/** Single knob for hex spacing used across systems (source of truth here). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid", meta = (ClampMin = 1.0))
	float TileSizeUU = 120.f;

	/** Visual conventions for belts */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Belts")
	int32 BeltVisualDirOffset = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Belts")
	bool bBeltYawClockwise = true;

	/** Optional reference to a manager if you want to sync knobs externally (not required). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Framework")
	AGameManager* GM = nullptr;

	// ---------------------------------------------------------------------
	// Accessors used across the project (kept public so other systems compile)
	// ---------------------------------------------------------------------

	UFUNCTION(BlueprintCallable, Category = "Sim")
	USimWorldHex* GetSim() const { return Sim; }

	/** Interp alpha in [0..1) within the current fixed step (for render lerp). */
	UFUNCTION(BlueprintCallable, Category = "Sim")
	float GetStepAlpha() const { return StepAlpha; }

	UFUNCTION(BlueprintCallable, Category = "Grid") AActor* GetGridAnchor() const { return GridAnchor; }
	UFUNCTION(BlueprintCallable, Category = "Grid") float           GetGridPlaneZ(const AActor* FallbackOwner) const;
	UFUNCTION(BlueprintCallable, Category = "Grid") EHexOrientation GetGridOrientation() const { return GridOrientation; }
	UFUNCTION(BlueprintCallable, Category = "Grid") float           GetGridYaw() const { return GridYawDeg; }
	UFUNCTION(BlueprintCallable, Category = "Grid") float           GetTileSize() const { return TileSizeUU; }

	UFUNCTION(BlueprintCallable, Category = "Belts") int32 GetBeltVisualOffset() const { return BeltVisualDirOffset; }
	UFUNCTION(BlueprintCallable, Category = "Belts") bool  IsBeltYawClockwise() const { return bBeltYawClockwise; }

	/** Utility to find the first driver in the world. */
	static AOrbSimDriver* Find(UWorld* World);

private:
	UPROPERTY(Transient)
	USimWorldHex* Sim = nullptr;

	/** Interpolation factor [0..1]; reset to 0 each fixed step. */
	float StepAlpha = 0.f;

	bool bSubscribed = false;
};
