#include "Actors/RequestBeacon.h"
#include "Sim/HexCoord.h"
#include "Sim/HexTypes.h"
#include "Sim/SimWorldHex.h"
#include "Sim/WalkerTypes.h"

void USimWorldHex::ProcessWalkerTick(
    int32 WalkerIndex, const TSet<FHex> &HeldOrbHexes,
    const TSet<FHex> &ArmBaseHexes,
    const TMap<FHex, ARequestBeacon *> &BeaconCache) {
  FWalkerState &W = Walkers[WalkerIndex];

  // Update rotation based on feet
  W.QuadrantRotationOffset = CalculateSimWalkerRotation(W);

  // Stance transitions
  if (W.Stance == EWalkerStance::StandingUp) {
    W.StanceTransitionProgress++;
    if (W.StanceTransitionProgress >= 3) {
      W.Stance = EWalkerStance::Standing;
      W.StanceTransitionProgress = 0;
    }
    return;
  } else if (W.Stance == EWalkerStance::SittingDown) {
    W.StanceTransitionProgress++;
    if (W.StanceTransitionProgress >= 3) {
      W.Stance = EWalkerStance::Sitting;
      W.StanceTransitionProgress = 0;
    }
    return;
  }

  if (W.Stance == EWalkerStance::Sitting && W.Requests.Num() > 0) {
    W.Stance = EWalkerStance::StandingUp;
    W.StanceTransitionProgress = 0;
    return;
  }

  // Cancel logic for stale PickUp/Gather requests
  bool bAnyFootHolding = W.DFoot.bHoldingItem || W.LFoot.bHoldingItem ||
                         W.RFoot.bHoldingItem || W.UFoot.bHoldingItem;
  bool bAllFeetPlanted = W.DFoot.StepState == EFootStepState::Planted &&
                         W.LFoot.StepState == EFootStepState::Planted &&
                         W.RFoot.StepState == EFootStepState::Planted &&
                         W.UFoot.StepState == EFootStepState::Planted;

  if (!bAnyFootHolding && W.Requests.Num() > 0 && W.bProcessingRequest) {
    FHex RequestTarget = W.Requests[0].TargetHex;
    if (ARequestBeacon *const *BeaconPtr = BeaconCache.Find(RequestTarget)) {
      // Cancel stale PickUp requests (item gone)
      if ((*BeaconPtr)->BeaconRole == EBeaconRole::PickUp &&
          !ItemMap.Contains(RequestTarget)) {
        (*BeaconPtr)->SetState(EBeaconState::NotReady);
        W.Requests.RemoveAt(0);
        W.bProcessingRequest = false;
        W.CurrentTargetHex = W.BaseHex;

        W.DFoot.bGatherPhaseActive = false;
        W.DFoot.bScatterPhaseActive = false;
        W.LFoot.bGatherPhaseActive = false;
        W.LFoot.bScatterPhaseActive = false;
        W.RFoot.bGatherPhaseActive = false;
        W.RFoot.bScatterPhaseActive = false;
        W.UFoot.bGatherPhaseActive = false;
        W.UFoot.bScatterPhaseActive = false;
      }
      // Cancel stale Gather requests (beacon no longer InProgress, checked via
      // CheckReadiness)
      else if ((*BeaconPtr)->BeaconRole == EBeaconRole::Gather &&
               (*BeaconPtr)->BeaconState == EBeaconState::NotReady) {
        W.Requests.RemoveAt(0);
        W.bProcessingRequest = false;
        W.CurrentTargetHex = W.BaseHex;

        W.DFoot.bGatherPhaseActive = false;
        W.DFoot.bScatterPhaseActive = false;
        W.LFoot.bGatherPhaseActive = false;
        W.LFoot.bScatterPhaseActive = false;
        W.RFoot.bGatherPhaseActive = false;
        W.RFoot.bScatterPhaseActive = false;
        W.UFoot.bGatherPhaseActive = false;
        W.UFoot.bScatterPhaseActive = false;
      }
    }
  }

  // Idle logic
  if (W.Stance == EWalkerStance::Standing && W.Requests.Num() == 0 &&
      !W.bProcessingRequest) {
    if (!bAnyFootHolding) {
      if (W.BaseHex != W.HomeHex) {
        FWalkerRequest ReturnRequest;
        ReturnRequest.TargetHex = W.HomeHex;
        ReturnRequest.Priority = -1;
        W.Requests.Insert(ReturnRequest, 0);
      } else if (bAllFeetPlanted) {
        W.Stance = EWalkerStance::SittingDown;
        W.StanceTransitionProgress = 0;
        return;
      }
    }
  }

  if (W.Stance != EWalkerStance::Standing && bAllFeetPlanted)
    return;

  // Decrement cooldown if active
  if (W.RequestCooldownTicks > 0) {
    W.RequestCooldownTicks--;
  }

  // Request processing
  // Allow processing return-home requests even during cooldown
  bool bCanProcessNewRequest =
      (W.RequestCooldownTicks == 0 ||
       (W.Requests.Num() > 0 && W.Requests[0].Priority < 0));

  if (!W.bProcessingRequest && W.Requests.Num() > 0 && bCanProcessNewRequest) {
    W.CurrentTargetHex = W.Requests[0].TargetHex;
    W.Requests[0].AttemptTicks = 0;
    W.bProcessingRequest = true;
    W.TicksSinceLastFootTargetChange = 0; // Reset when starting new request
  }

  bool bIsPivoting = false;
  if (W.bProcessingRequest) {
    W.Requests[0].AttemptTicks++;

    // Check for timeout: if walker is in range of pickup beacon but feet
    // haven't updated for 5 ticks
    if (W.Requests.Num() > 0 && !bAnyFootHolding) {
      FHex RequestTarget = W.Requests[0].TargetHex;
      // UE_LOG(LogTemp, Warning, TEXT("Walker %d: bProcessingRequest=true,
      // Requests.Num()=%d, bAnyFootHolding=%d, RequestTarget=(%d,%d)"),
      // 	WalkerIndex, W.Requests.Num(), bAnyFootHolding ? 1 : 0,
      // RequestTarget.Q, RequestTarget.R);

      if (ARequestBeacon *const *BeaconPtr = BeaconCache.Find(RequestTarget)) {
        // UE_LOG(LogTemp, Warning, TEXT("Walker %d: Beacon found, Role=%d,
        // State=%d"), 	WalkerIndex, (int32)(*BeaconPtr)->BeaconRole,
        // (int32)(*BeaconPtr)->BeaconState);

        if (((*BeaconPtr)->BeaconRole == EBeaconRole::PickUp ||
             (*BeaconPtr)->BeaconRole == EBeaconRole::Gather) &&
            (*BeaconPtr)->BeaconState == EBeaconState::InProgress) {
          // Check if walker is in range of the pickup tile
          int32 DistToTarget = HexUtil::Dist(W.BaseHex, RequestTarget);
          bool bInRange = DistToTarget <= W.ReachDistance;

          // UE_LOG(LogTemp, Warning, TEXT("Walker %d: Processing PickUp request
          // at (%d,%d), DistToTarget=%d, ReachDistance=%d, bInRange=%d,
          // CurrentCounter=%d"), 	WalkerIndex, RequestTarget.Q,
          // RequestTarget.R, DistToTarget, W.ReachDistance, bInRange ? 1 : 0,
          // W.TicksSinceLastFootTargetChange);

          if (bInRange) {
            // Only increment timeout counter when in range
            // Check if this is the first tick in range by checking if counter
            // was reset (0) when we were out of range If counter is 0 and we
            // just entered range, start counting from 1 Otherwise, increment it
            if (W.TicksSinceLastFootTargetChange == 0) {
              // First tick in range - start counting
              W.TicksSinceLastFootTargetChange = 1;
            } else {
              // Already counting - increment
              W.TicksSinceLastFootTargetChange++;
            }
            // UE_LOG(LogTemp, Warning, TEXT("Walker %d: IN RANGE -
            // TicksSinceLastFootTargetChange: %d"), WalkerIndex,
            // W.TicksSinceLastFootTargetChange);
            if (W.TicksSinceLastFootTargetChange >= 20) {
              // Timeout: cancel request, return home, set cooldown
              // UE_LOG(LogTemp, Warning, TEXT("Walker %d: Pickup request
              // timeout - in range but feet stuck for %d ticks (threshold:
              // 500). Canceling and returning home."), 	WalkerIndex,
              // W.TicksSinceLastFootTargetChange);

              // Set beacon back to Ready (or NotReady if item is gone)
              if (ItemMap.Contains(RequestTarget)) {
                (*BeaconPtr)->SetState(EBeaconState::Ready);
              } else {
                (*BeaconPtr)->SetState(EBeaconState::NotReady);
              }

              // Clear current request
              W.Requests.RemoveAt(0);
              W.bProcessingRequest = false;

              // CRITICAL: Clear all foot task flags to prevent zombie stepping
              W.DFoot.bGatherPhaseActive = false;
              W.DFoot.bScatterPhaseActive = false;
              W.DFoot.bGathering = false;
              W.DFoot.bHasDesignatedDropOff = false;
              W.DFoot.bHasSourcePickUp = false;

              W.LFoot.bGatherPhaseActive = false;
              W.LFoot.bScatterPhaseActive = false;
              W.LFoot.bGathering = false;
              W.LFoot.bHasDesignatedDropOff = false;
              W.LFoot.bHasSourcePickUp = false;

              W.RFoot.bGatherPhaseActive = false;
              W.RFoot.bScatterPhaseActive = false;
              W.RFoot.bGathering = false;
              W.RFoot.bHasDesignatedDropOff = false;
              W.RFoot.bHasSourcePickUp = false;

              W.UFoot.bGatherPhaseActive = false;
              W.UFoot.bScatterPhaseActive = false;
              W.UFoot.bGathering = false;
              W.UFoot.bHasDesignatedDropOff = false;
              W.UFoot.bHasSourcePickUp = false;

              W.CurrentTargetHex = W.BaseHex;
              W.TicksSinceLastFootTargetChange = 0;

              // Add return home request
              if (W.BaseHex != W.HomeHex) {
                FWalkerRequest ReturnRequest;
                ReturnRequest.TargetHex = W.HomeHex;
                ReturnRequest.Priority = -1;
                W.Requests.Insert(ReturnRequest, 0);
              }

              // Set cooldown period (5 ticks) before accepting new requests
              W.RequestCooldownTicks = 5;

              // Early return to skip rest of processing
              return;
            }
          } else {
            // Not in range yet - reset counter (walker is still approaching)
            // UE_LOG(LogTemp, Warning, TEXT("Walker %d: NOT IN RANGE -
            // Resetting counter from %d to 0"), WalkerIndex,
            // W.TicksSinceLastFootTargetChange);
            W.TicksSinceLastFootTargetChange = 0;
          }
        }
      }
    }

    // Pivot detection
    TArray<FFootState *> AllFeet = GetAllFeet(W);
    for (FFootState *Foot : AllFeet) {
      if (Foot->bHoldingItem) {
        const FString DropOffQuad = GetFixedQuadrantFromHex(
            W.BaseHex, Foot->DesignatedDropOffHex, W.QuadrantRotationOffset);
        FString FootQuad;
        int32 FootIdx =
            (Foot == &W.DFoot
                 ? 0
                 : (Foot == &W.LFoot ? 1 : (Foot == &W.RFoot ? 2 : 3)));
        switch (FootIdx) {
        case 0:
          FootQuad = TEXT("S");
          break;
        case 1:
          FootQuad = TEXT("W");
          break;
        case 2:
          FootQuad = TEXT("E");
          break;
        case 3:
          FootQuad = TEXT("N");
          break;
        }

        if (!DropOffQuad.Equals(FootQuad)) {
          bIsPivoting = true;
          break;
        }
      }
    }
  }

  // Completion check for non-beacon moves (e.g., ReturnHome or body-positioning
  // for drop-off)
  if (W.bProcessingRequest && W.BaseHex == W.CurrentTargetHex) {
    bool bAnyFootAwaitingDropOff = false;
    if (W.DFoot.bHoldingItem && W.DFoot.bHasDesignatedDropOff)
      bAnyFootAwaitingDropOff = true;
    if (W.LFoot.bHoldingItem && W.LFoot.bHasDesignatedDropOff)
      bAnyFootAwaitingDropOff = true;
    if (W.RFoot.bHoldingItem && W.RFoot.bHasDesignatedDropOff)
      bAnyFootAwaitingDropOff = true;
    if (W.UFoot.bHoldingItem && W.UFoot.bHasDesignatedDropOff)
      bAnyFootAwaitingDropOff = true;

    bool bAnyGatherActive = false;
    if (W.DFoot.bGatherPhaseActive || W.LFoot.bGatherPhaseActive ||
        W.RFoot.bGatherPhaseActive || W.UFoot.bGatherPhaseActive) {
      bAnyGatherActive = true;
    }

    if (!bAnyFootAwaitingDropOff && !bAnyGatherActive &&
        !BeaconCache.Contains(W.CurrentTargetHex)) {
      if (W.Requests.Num() > 0) {
        UE_LOG(LogTemp, Log,
               TEXT("Walker %d: Movement request complete at (%d,%d)"),
               WalkerIndex, W.CurrentTargetHex.Q, W.CurrentTargetHex.R);
        W.Requests.RemoveAt(0);
      }
      W.bProcessingRequest = false;
      W.CurrentTargetHex = W.BaseHex;
    }
  }

  // Collected occupied set for feet (GLOBAL set of all walkers' holding feet)
  TSet<FHex> GlobalHoldingFootHexes;
  for (const FWalkerState &OtherW : Walkers) {
    if (OtherW.DFoot.bHoldingItem)
      GlobalHoldingFootHexes.Add(OtherW.DFoot.CurrentTile);
    if (OtherW.LFoot.bHoldingItem)
      GlobalHoldingFootHexes.Add(OtherW.LFoot.CurrentTile);
    if (OtherW.RFoot.bHoldingItem)
      GlobalHoldingFootHexes.Add(OtherW.RFoot.CurrentTile);
    if (OtherW.UFoot.bHoldingItem)
      GlobalHoldingFootHexes.Add(OtherW.UFoot.CurrentTile);
  }

  // Leg processing
  HandleFootStateProcessing(W, WalkerIndex, HeldOrbHexes, ArmBaseHexes,
                            BeaconCache, GlobalHoldingFootHexes, bIsPivoting);

  // Body processing
  UpdateWalkerBodyPosition(W, WalkerIndex);

  // Instruction processing (legacy or future)
  ProcessWalkerInstructions(W, WalkerIndex);
}
