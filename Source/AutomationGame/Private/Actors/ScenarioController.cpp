//#include "Actors/ScenarioController.h"
//#include "Actors/OrbSimDriver.h"
//#include "Sim/SimWorldHex.h"
//#include "Sim/Tiles.h"
//#include "Engine/World.h"
//
//AScenarioController::AScenarioController()
//{
//    PrimaryActorTick.bCanEverTick = true;
//}
//
//void AScenarioController::BeginPlay()
//{
//    Super::BeginPlay();
//    Driver = AOrbSimDriver::Find(GetWorld());
//    Sim = Driver ? Driver->GetSim() : nullptr;
//    EnsureTiles();
//}
//
//void AScenarioController::Tick(float DeltaSeconds)
//{
//    Super::Tick(DeltaSeconds);
//    if (!Sim)
//    {
//        Driver = AOrbSimDriver::Find(GetWorld());
//        Sim = Driver ? Driver->GetSim() : nullptr;
//        if (Sim) EnsureTiles();
//    }
//}
//
//void AScenarioController::EnsureTiles()
//{
//    if (!Sim) return;
//    Sim->PlaceTile(InputLocation,  FTile{ETileKind::Input,  0});
//    Sim->PlaceTile(OutputLocation, FTile{ETileKind::Output, 0});
//}
