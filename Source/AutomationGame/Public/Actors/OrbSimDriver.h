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
#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

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

	/** Visual conventions for belts */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Belts")
	int32 BeltVisualDirOffset = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Belts")
	bool bBeltYawClockwise = true;

	/** Debug toggles */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	bool bDebugLogs = false;
	
	/** Draw debug spheres at all orb simulation locations every frame. 
	 * Shows: Board orbs (colored by type), held orbs (yellow/orange), cluster sub-orbs with offsets */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	bool bDebugSpheres = false;
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	bool bDebugLines = false;
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	bool bDebugBonds = false;
	
	/** Size of debug spheres for all orb types */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug", meta = (ClampMin = "1.0", ClampMax = "50.0"))
	float DebugSphereSize = 8.0f;

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
	
	/** Set grid parameters from GameManager (source of truth) */
	UFUNCTION(BlueprintCallable, Category = "Grid")
	void SetGridParameters(EHexOrientation InOrientation, float InYawDeg, float InTileSizeUU, AActor* InAnchor);

	// Apply sphere settings as dictated by GameManager (no public editable toggle anymore)
	void ApplySphereSettings(bool bEnable, float InRadius) { bSphereGrid = bEnable; SphereRadiusUU = InRadius; }
	bool IsSphereEnabled() const { return bSphereGrid; }
	float GetSphereRadius() const { return SphereRadiusUU; }

	// (Deprecated) kept for backward compatibility – just forwards to SetGridParameters + ApplySphereSettings
	UFUNCTION(BlueprintCallable, Category="Grid", meta=(DeprecatedFunction, DeprecationMessage="Use GameManager settings instead."))
	void SetGridParametersSphere(EHexOrientation InOrientation, float InYawDeg, float InTileSizeUU, AActor* InAnchor, bool bInSphere, float InSphereRadius)
	{
		SetGridParameters(InOrientation, InYawDeg, InTileSizeUU, InAnchor);
		ApplySphereSettings(bInSphere, InSphereRadius);
	}
	
	/** Sync debug settings to all BasicArm actors in the world */
	UFUNCTION(BlueprintCallable, Category = "Debug")
	void SyncDebugSettingsToArms();
	
	/** Force sync debug settings to arms and sim (useful for runtime changes) */
	UFUNCTION(BlueprintCallable, Category = "Debug")
	void ForceSyncDebugSettings();
	
	/** Map a simulation hex coordinate to world space (handles sphere mapping if enabled). */
	UFUNCTION(BlueprintCallable, Category="Grid")
	FVector MapHexToWorld(const FHex& H) const;

	/** Inverse of MapHexToWorld (best-effort for sphere mapping). Returns true if success. */
	UFUNCTION(BlueprintCallable, Category="Grid")
	bool WorldToHex(const FVector& World, FHex& OutHex) const;

	/** Draw debug spheres for all orbs in the simulation.
	 * Called every frame when bDebugSpheres is enabled.
	 * Draws spheres for:
	 * - Board items (single orbs and clusters) with type-based colors
	 * - Held items by arms (yellow/orange) with proper arm rotation
	 * - Cluster sub-orbs with hex coordinate offsets
	 * - Debug text showing item IDs and cluster information */
	void DrawAllOrbDebugSpheres();

	/** Get debug color for orb type */
	FColor GetDebugColorForOrbType(uint8 OrbType) const;

	/** Get debug sphere size (uses DebugSphereSize variable for all orbs) */
	float GetDebugSizeForOrbType(uint8 OrbType, bool bIsSubOrb) const;

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
	
	/** Grid parameters (set by GameManager - source of truth) */
	EHexOrientation GridOrientation = EHexOrientation::PointyTop;
	float GridYawDeg = 0.f;
	float TileSizeUU = 120.f;

	// Sphere mapping extents (rotated planar XY bounds) for converting axial -> lon/lat
	float SphereMinX = -1.f;
	float SphereMaxX = 1.f;
	float SphereMinY = -1.f;
	float SphereMaxY = 1.f;

	// Internal sphere state (no longer publicly editable)
	bool  bSphereGrid = false;
	float SphereRadiusUU = 2000.f;

public:
	void SetSphereMappingExtents(float InMinX, float InMaxX, float InMinY, float InMaxY, float InRadius)
	{
		SphereMinX = InMinX; SphereMaxX = InMaxX; SphereMinY = InMinY; SphereMaxY = InMaxY; SphereRadiusUU = InRadius; bSphereGrid = true;
	}
};
