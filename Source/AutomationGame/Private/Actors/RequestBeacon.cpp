#include "Actors/RequestBeacon.h"
#include "Actors/OrbSimDriver.h"
#include "Components/DecalComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "EngineUtils.h"
#include "Framework/GameManager.h"
#include "Sim/HexTypes.h"
#include "Sim/SimWorldHex.h"

ARequestBeacon::ARequestBeacon() {
  PrimaryActorTick.bCanEverTick = true;

  RootComp = CreateDefaultSubobject<USceneComponent>(TEXT("RootComp"));
  RootComponent = RootComp;

  MeshComp = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("MeshComp"));
  MeshComp->SetupAttachment(RootComp);
  MeshComp->SetCollisionEnabled(ECollisionEnabled::NoCollision);

  DecalComp = CreateDefaultSubobject<UDecalComponent>(TEXT("DecalComp"));
  DecalComp->SetupAttachment(MeshComp);
  // Default decal rotation to face up (pitch -90)
  DecalComp->SetRelativeRotation(FRotator(-90.0f, 0.0f, 0.0f));
}

void ARequestBeacon::BeginPlay() {
  Super::BeginPlay();
  // Find Driver and Sim
  if (UWorld *World = GetWorld()) {
    Driver = AOrbSimDriver::Find(World);
    if (Driver) {
      Sim = Driver->GetSim();
    }

    // Initialize IsGridVisible from GameManager's current state
    for (TActorIterator<AGameManager> It(World); It; ++It) {
      AGameManager *GM = *It;
      if (GM) {
        if (UInstancedStaticMeshComponent *ISM =
                GM->FindComponentByClass<UInstancedStaticMeshComponent>()) {
          SetIsGridVisible(ISM->IsVisible());
        }
        break;
      }
    }
  }
  SyncWorldLocation();
  UpdateVisuals();
}

void ARequestBeacon::Tick(float DeltaTime) {
  Super::Tick(DeltaTime);
  CheckReadiness();
}

void ARequestBeacon::CheckReadiness() {
  // Only check for PickUp, Gather, DropOff, and Scatter beacons
  if (BeaconRole != EBeaconRole::PickUp && BeaconRole != EBeaconRole::Gather &&
      BeaconRole != EBeaconRole::DropOff && BeaconRole != EBeaconRole::Scatter)
    return;

  // Need Sim
  if (!Sim && !Driver) {
    // Try finding again
    if (UWorld *World = GetWorld()) {
      Driver = AOrbSimDriver::Find(World);
      if (Driver)
        Sim = Driver->GetSim();
    }
  }

  if (!Sim)
    return;

  // Don't override InProgress state from here
  if (BeaconState == EBeaconState::InProgress) {
    return;
  }

  if (BeaconRole == EBeaconRole::PickUp || BeaconRole == EBeaconRole::DropOff) {
    // Standard PickUp/DropOff: check tile content at beacon location
    FHex Location(Q, R, Level);
    bool bOccupied = false;

    if (Sim->ItemMap.Contains(Location)) {
      const FItem *Item = Sim->ItemMap.Find(Location);
      if (Item && Item->Id != 0) {
        bOccupied = true;
      }
    }

    EBeaconState DesiredState = EBeaconState::NotReady;
    if (BeaconRole == EBeaconRole::PickUp) {
      DesiredState = bOccupied ? EBeaconState::Ready : EBeaconState::NotReady;
    } else {
      // DropOff is Ready if EMPTY
      DesiredState = bOccupied ? EBeaconState::NotReady : EBeaconState::Ready;
    }

    if (BeaconState != DesiredState) {
      SetState(DesiredState);
    }
  } else if (BeaconRole == EBeaconRole::Gather ||
             BeaconRole == EBeaconRole::Scatter) {
    FHex Center(Q, R, Level);
    bool bFoundQualifyingSlot = false;

    for (int32 q = -Radius; q <= Radius; ++q) {
      for (int32 r = -Radius; r <= Radius; ++r) {
        FHex TestHex(Center.Q + q, Center.R + r, Center.Level);
        int32 dist = HexUtil::Dist(Center, TestHex);
        if (dist > Radius)
          continue;

        const FItem *Item = Sim->ItemMap.Find(TestHex);
        bool bHasItem = (Item && Item->Id != 0);

        if (BeaconRole == EBeaconRole::Gather) {
          // Gather: check for unbonded single orbs of matching type
          if (dist == 0 || !bHasItem)
            continue;
          if (Item->bIsCluster || Item->bIsLocked)
            continue;
          if (FilterOrbType > 0 && (uint8)Item->Single.Type != FilterOrbType)
            continue;
          if (Sim->IsItemBonded(Item->Id))
            continue;

          bFoundQualifyingSlot = true;
          break;
        } else {
          // Scatter: check for empty slots
          if (!bHasItem) {
            bFoundQualifyingSlot = true;
            break;
          }
        }
      }
      if (bFoundQualifyingSlot)
        break;
    }

    EBeaconState DesiredState =
        bFoundQualifyingSlot ? EBeaconState::Ready : EBeaconState::NotReady;
    if (BeaconState != DesiredState) {
      SetState(DesiredState);
    }
  }
}

void ARequestBeacon::OnConstruction(const FTransform &Transform) {
  Super::OnConstruction(Transform);
  SyncWorldLocation();
  UpdateVisuals();
}

void ARequestBeacon::SetState(EBeaconState NewState) {
  if (BeaconState != NewState) {
    BeaconState = NewState;
    UpdateVisuals();
    OnStateChanged.Broadcast(BeaconState);
  }
}

void ARequestBeacon::SetIsGridVisible(bool bVisible) {
  if (IsGridVisible != bVisible) {
    IsGridVisible = bVisible;
    OnGridVisibilityChanged.Broadcast(bVisible);
  }
}

void ARequestBeacon::UpdateVisuals() {
  // Update Mesh Material based on State
  if (MeshComp) {
    UMaterialInterface *MaterialToUse = nullptr;
    switch (BeaconState) {
    case EBeaconState::Ready:
      MaterialToUse = ReadyMaterial;
      break;
    case EBeaconState::InProgress:
      MaterialToUse = InProgressMaterial;
      break;
    case EBeaconState::NotReady:
      MaterialToUse = NotReadyMaterial;
      break;
    }

    if (MaterialToUse) {
      MeshComp->SetMaterial(0, MaterialToUse);
    }
  }

  // Update Decal Material based on Role/Type
  if (DecalComp) {
    UMaterialInterface *DecalMat = nullptr;
    if (BeaconRole == EBeaconRole::PickUp ||
        BeaconRole == EBeaconRole::Gather) {
      DecalMat = PickupDecalMaterial;
    } else if (BeaconRole == EBeaconRole::DropOff ||
               BeaconRole == EBeaconRole::Scatter) {
      DecalMat = DropOffDecalMaterial;
    }

    if (DecalMat) {
      DecalComp->SetDecalMaterial(DecalMat);
    }
  }
}

void ARequestBeacon::SyncWorldLocation() {
  if (!Driver) {
    if (UWorld *World = GetWorld()) {
      Driver = AOrbSimDriver::Find(World);
    }
  }

  if (Driver) {
    FVector Location = Driver->MapHexToWorld(FHex(Q, R, Level));
    SetActorLocation(Location);
  }
}

void ARequestBeacon::Destroyed() {
  Super::Destroyed();

  // If we have a paired beacon, destroy it too
  if (PairedBeacon && IsValid(PairedBeacon)) {
    // Avoid infinite recursion by clearing the pointer on the partner first
    PairedBeacon->PairedBeacon = nullptr;
    PairedBeacon->Destroy();
    PairedBeacon = nullptr;
  }
}
