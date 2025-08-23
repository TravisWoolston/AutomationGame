#pragma once
#include "AutomationGameMinimal.h"
#include "Sim/HexCoord.h"
#include "GameFramework/Actor.h"
#include "Framework/GameManager.h"
#include "OrbSimDriver.generated.h"

class USimWorldHex;
class UStaticMesh;
class UMaterialInterface;

UCLASS()
class AUTOMATIONGAME_API AOrbSimDriver : public AActor
{
	GENERATED_BODY()
public:
	AOrbSimDriver();

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

	// --- Sim access ---
	UFUNCTION(BlueprintCallable, Category = "Sim") USimWorldHex* GetSim() const { return Sim; }
	UFUNCTION(BlueprintCallable, Category = "Sim") float GetStepAlpha() const { return StepAlpha; }

	// Called by GameTickManager to advance simulation in fixed steps
	UFUNCTION() void HandleFixedTickSeconds(float StepSeconds);

	// --- Grid configuration ---
	/** Centralized grid anchor (set this ONCE here). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid") AActor* GridAnchor = nullptr;

	/** Use anchor Z for click plane. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid") bool bUseAnchorZForPicking = true;

	/** Grid orientation (matches your HexGrid visual). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid") EHexOrientation GridOrientation = EHexOrientation::PointyTop;

	/** World yaw (degrees) of the hex grid, applied around anchor. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid") float GridYawDeg = 0.f;

	/** Single knob for hex spacing used by placement/renderer/factory/sim. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid") float TileSizeUU = 120.f;

	// --- Belt visualization parameters (assignable in editor) ---
	/** Mesh used for straight belt tiles (optional). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visual|Belts")
	UStaticMesh* BeltMesh = nullptr;

	/** Mesh used for belt turns/corners (optional). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visual|Belts")
	UStaticMesh* BeltTurnMesh = nullptr;

	/** Optional material override for belt instances. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visual|Belts")
	UMaterialInterface* BeltMaterial = nullptr;

	/** Uniform/non-uniform scale applied when rendering belts. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visual|Belts")
	FVector BeltScale = FVector(1.f, 1.f, 1.f);

	/** Z offset applied to belt instances to avoid z-fighting with tiles. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visual|Belts")
	float BeltZOffset = 10.f;

	/** Visual offset for belt arrows (0=none). Set to 3 if your sim moves opposite to the arrow. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Belts") int32 BeltVisualDirOffset = 0;

	/** If true, increasing belt dir (E/RotateCW) rotates the mesh clockwise (negative yaw step). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Belts") bool bBeltYawClockwise = true;

	/** Optional dependency on GameManager for initial params. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Framework") AGameManager* GM = nullptr;

	// --- Public getters used by other systems (HexFactory, Placement, Renderer, etc) ---
	UFUNCTION(BlueprintCallable, Category = "Grid") AActor* GetGridAnchor() const { return GridAnchor; }
	UFUNCTION(BlueprintCallable, Category = "Grid") FVector2D GetGridOriginXY() const;
	UFUNCTION(BlueprintCallable, Category = "Grid") float GetGridPlaneZ(const AActor* FallbackOwner) const;
	UFUNCTION(BlueprintCallable, Category = "Grid") EHexOrientation GetGridOrientation() const { return GridOrientation; }
	UFUNCTION(BlueprintCallable, Category = "Grid") float GetGridYaw() const { return GridYawDeg; }
	UFUNCTION(BlueprintCallable, Category = "Grid") float GetTileSize() const { return TileSizeUU; }
	UFUNCTION(BlueprintCallable, Category = "Belts") int32 GetBeltVisualOffset() const { return BeltVisualDirOffset; }
	UFUNCTION(BlueprintCallable, Category = "Belts") bool IsBeltYawClockwise() const { return bBeltYawClockwise; }

	// New: belt asset getters (for use by renderers / factories)
	UFUNCTION(BlueprintPure, Category = "Visual|Belts") UStaticMesh* GetBeltMesh() const { return BeltMesh; }
	UFUNCTION(BlueprintPure, Category = "Visual|Belts") UStaticMesh* GetBeltTurnMesh() const { return BeltTurnMesh; }
	UFUNCTION(BlueprintPure, Category = "Visual|Belts") UMaterialInterface* GetBeltMaterial() const { return BeltMaterial; }
	UFUNCTION(BlueprintPure, Category = "Visual|Belts") FVector GetBeltScale() const { return BeltScale; }
	UFUNCTION(BlueprintPure, Category = "Visual|Belts") float GetBeltZOffset() const { return BeltZOffset; }

	/** Utility to find the first driver in world */
	static AOrbSimDriver* Find(UWorld* World);

private:
	UPROPERTY(Transient) USimWorldHex* Sim = nullptr;
	float Accum = 0.f;
	float StepAlpha = 0.f;
};
