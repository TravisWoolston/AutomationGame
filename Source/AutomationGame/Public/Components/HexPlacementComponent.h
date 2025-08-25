#pragma once
#include "AutomationGameMinimal.h"
#include "Components/ActorComponent.h"
#include "HexPlacementComponent.generated.h"

class USimWorldHex; class APlayerController; class AActor; class AOrbSimDriver;

UENUM(BlueprintType)
enum class EPlaceKind : uint8 { Input, Output, Belt, Arm };

/** Click-to-place; uses OrbSimDriver's GridAnchor by default (single source of truth). */
UCLASS(ClassGroup = (Custom), meta = (BlueprintSpawnableComponent))
class AUTOMATIONGAME_API UHexPlacementComponent : public UActorComponent
{
	GENERATED_BODY()
public:
	UHexPlacementComponent();

	/** Thing to place */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Place") EPlaceKind PlaceKind = EPlaceKind::Input;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Place") uint8 BeltDir = 0;

	/** Optional anchor override to NOT use driver. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid") bool bOverrideAnchor = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid", meta = (EditCondition = "bOverrideAnchor")) AActor* AnchorOverride = nullptr;

	/** Poll mouse directly (no input mappings needed). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Input") bool bUseInputPolling = false;

	/** Debug overlay: prints and draws pick positions. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug") bool bDebug = true;

	// Input API
	UFUNCTION(BlueprintCallable, Category = "Place") void HandlePlace(bool bPressed);
	UFUNCTION(BlueprintCallable, Category = "Place") void HandleRemove(bool bPressed);
	UFUNCTION(BlueprintCallable, Category = "Place") void RotateCW();
	UFUNCTION(BlueprintCallable, Category = "Place") void RotateCCW();
	UFUNCTION(BlueprintCallable, Category = "Place") void SetPlaceKind(EPlaceKind NewKind);

protected:
	virtual void BeginPlay() override;
	virtual void TickComponent(float DeltaTime, enum ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

private:
	AOrbSimDriver* Driver = nullptr;
	USimWorldHex* Sim = nullptr;
	APlayerController* PC = nullptr;
	bool bWasClickingL = false;
	bool bWasClickingR = false;

	bool HitPlane(FVector& OutWorld) const;
	void DoPlaceAt(const FVector& World);
	void DoRemoveAt(const FVector& World);

	FORCEINLINE AActor* GetAnchor() const;
};
