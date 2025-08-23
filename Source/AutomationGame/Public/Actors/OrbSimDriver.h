#pragma once
#include "AutomationGameMinimal.h"
#include "Sim/HexCoord.h"
#include "GameFramework/Actor.h"
#include "OrbSimDriver.generated.h"

class USimWorldHex;

UCLASS()
class AUTOMATIONGAME_API AOrbSimDriver : public AActor
{
    GENERATED_BODY()
public:
    AOrbSimDriver();

    virtual void BeginPlay() override;
    virtual void Tick(float DeltaSeconds) override;

    UFUNCTION(BlueprintCallable, Category="Sim") USimWorldHex* GetSim() const { return Sim; }
    UFUNCTION(BlueprintCallable, Category="Sim") float GetStepAlpha() const { return StepAlpha; }

    /** Centralized grid anchor (set this ONCE here). */
    UPROPERTY(EditAnywhere, Category="Grid") AActor* GridAnchor = nullptr;

    /** Use anchor Z for click plane. */
    UPROPERTY(EditAnywhere, Category="Grid") bool bUseAnchorZForPicking = true;

    /** Grid orientation (matches your HexGrid visual). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Grid") EHexOrientation GridOrientation = EHexOrientation::PointyTop;

    /** World yaw (degrees) of the hex grid, applied around anchor. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Grid") float GridYawDeg = 0.f;

    /** Single knob for hex spacing used by placement/renderer/factory/sim. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Grid") float TileSizeUU = 120.f;

    /** Visual offset for belt arrows (0=none). Set to 3 if your sim moves opposite to the arrow. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Belts") int32 BeltVisualDirOffset = 0;

    /** If true, increasing belt dir (E/RotateCW) rotates the mesh clockwise (negative yaw step). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Belts") bool bBeltYawClockwise = true;

    // Helpers/getters
    UFUNCTION(BlueprintCallable, Category="Grid") AActor* GetGridAnchor() const { return GridAnchor; }
    UFUNCTION(BlueprintCallable, Category="Grid") FVector2D GetGridOriginXY() const;
    UFUNCTION(BlueprintCallable, Category="Grid") float GetGridPlaneZ(const AActor* FallbackOwner) const;
    UFUNCTION(BlueprintCallable, Category="Grid") EHexOrientation GetGridOrientation() const { return GridOrientation; }
    UFUNCTION(BlueprintCallable, Category="Grid") float GetGridYaw() const { return GridYawDeg; }
    UFUNCTION(BlueprintCallable, Category="Grid") float GetTileSize() const { return TileSizeUU; }
    UFUNCTION(BlueprintCallable, Category="Belts") int32 GetBeltVisualOffset() const { return BeltVisualDirOffset; }
    UFUNCTION(BlueprintCallable, Category="Belts") bool IsBeltYawClockwise() const { return bBeltYawClockwise; }

    /** Utility to find the first driver in world */
    static AOrbSimDriver* Find(UWorld* World);

private:
    UPROPERTY(Transient) USimWorldHex* Sim = nullptr;
    float Accum = 0.f;
    float StepAlpha = 0.f;
};
