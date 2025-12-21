#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "RequestBeacon.generated.h"

UENUM(BlueprintType)
enum class EBeaconType : uint8
{
	Relocate,
	// Add more types here as needed
};

UENUM(BlueprintType)
enum class EBeaconState : uint8
{
	Ready,
	InProgress,
	NotReady
};

UENUM(BlueprintType)
enum class EBeaconRole : uint8
{
	None,
	PickUp,
	DropOff
};

// Grid visibility delegate
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnGridVisibilityChanged, bool, bIsVisible);

// State change delegate
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnStateChanged, EBeaconState, NewState);

UCLASS()
class AUTOMATIONGAME_API ARequestBeacon : public AActor
{
	GENERATED_BODY()

public:
	ARequestBeacon();

protected:
	virtual void BeginPlay() override;
	virtual void OnConstruction(const FTransform& Transform) override;

public:
	// Components
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	class USceneComponent* RootComp;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	class UStaticMeshComponent* MeshComp;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	class UDecalComponent* DecalComp;

	// Properties
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Beacon")
	EBeaconType BeaconType = EBeaconType::Relocate;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Beacon")
	EBeaconState BeaconState = EBeaconState::NotReady;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Beacon")
	EBeaconRole BeaconRole = EBeaconRole::None;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Beacon")
	ARequestBeacon* PairedBeacon;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid")
	int32 Q = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid")
	int32 R = 0;

	UPROPERTY(BlueprintAssignable, Category = "Beacon")
	FOnGridVisibilityChanged OnGridVisibilityChanged;

	UPROPERTY(BlueprintAssignable, Category = "Beacon")
	FOnStateChanged OnStateChanged;

	// Grid visibility getter
	UFUNCTION(BlueprintPure, Category = "Beacon")
	bool GetIsGridVisible() const { return IsGridVisible; }

	// Grid visibility setter (fires delegate)
	UFUNCTION(BlueprintCallable, Category = "Beacon")
	void SetIsGridVisible(bool bVisible);

	// Visuals
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visuals")
	UMaterialInterface* ReadyMaterial;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visuals")
	UMaterialInterface* InProgressMaterial;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visuals")
	UMaterialInterface* NotReadyMaterial;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visuals")
	UMaterialInterface* PickupDecalMaterial;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visuals")
	UMaterialInterface* DropOffDecalMaterial;

	// Functions
	UFUNCTION(BlueprintCallable, Category = "Beacon")
	void SetState(EBeaconState NewState);

	UFUNCTION(BlueprintCallable, Category = "Beacon")
	void UpdateVisuals();

	// Handle destruction to remove paired beacon
	virtual void Destroyed() override;
	virtual void Tick(float DeltaTime) override;

private:
	void CheckReadiness();
	class AOrbSimDriver* Driver = nullptr;
	class USimWorldHex* Sim = nullptr;
	bool IsGridVisible = true;
};
