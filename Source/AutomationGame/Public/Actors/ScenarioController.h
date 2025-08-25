//#pragma once
//#include "CoreMinimal.h"
//#include "GameFramework/Actor.h"
//#include "Sim/HexTypes.h"
//#include "ScenarioController.generated.h"
//
//class USimWorldHex;
//class AOrbSimDriver;
//
///** Minimal scenario helper: ensures an Input and Output tile exist. */
//UCLASS()
//class AUTOMATIONGAME_API AScenarioController : public AActor
//{
//	GENERATED_BODY()
//public:
//	AScenarioController();
//
//	virtual void BeginPlay() override;
//	virtual void Tick(float DeltaSeconds) override;
//
//	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Scenario")
//	FHex InputLocation = FHex(0, 0);
//
//	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Scenario")
//	FHex OutputLocation = FHex(0, -1);
//
//private:
//	void EnsureTiles();
//	UPROPERTY(Transient) USimWorldHex* Sim = nullptr;
//	UPROPERTY(Transient) AOrbSimDriver* Driver = nullptr;
//};
