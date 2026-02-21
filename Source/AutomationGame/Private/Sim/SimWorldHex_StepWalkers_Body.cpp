#include "Sim/HexCoord.h"
#include "Sim/HexTypes.h"
#include "Sim/SimWorldHex.h"
#include "Sim/WalkerTypes.h"


void USimWorldHex::UpdateWalkerBodyPosition(FWalkerState &W,
                                            int32 WalkerIndex) {
  if (W.Stance == EWalkerStance::Standing ||
      W.Stance == EWalkerStance::StandingUp ||
      W.Stance == EWalkerStance::SittingDown) {
    TArray<FFootState *> CenteringFeet = GetAllFeet(W);
    TArray<FHex> EffectiveFootHexes;

    for (FFootState *Foot : CenteringFeet) {
      EffectiveFootHexes.Add(Foot->CurrentTile);
    }

    if (EffectiveFootHexes.Num() > 0) {
      int32 SumQ = 0;
      int32 SumR = 0;
      for (const FHex &FootHex : EffectiveFootHexes) {
        SumQ += FootHex.Q;
        SumR += FootHex.R;
      }
      int32 AvgQ = FMath::RoundToInt((float)SumQ / EffectiveFootHexes.Num());
      int32 AvgR = FMath::RoundToInt((float)SumR / EffectiveFootHexes.Num());
      FHex ProposedBodyHex(AvgQ, AvgR, W.BaseHex.Level);

      bool bValidBodyPosition = true;
      for (FFootState *Foot : CenteringFeet) {
        if (Foot->StepState == EFootStepState::Planted ||
            (Foot->StepState == EFootStepState::Lowering &&
             Foot->StepProgress + 1 >= W.StepLowerTicks)) {
          const FHex &FootHex = Foot->CurrentTile;
          const int32 dq = FootHex.Q - ProposedBodyHex.Q;
          const int32 dr = FootHex.R - ProposedBodyHex.R;
          const int32 HexDist = FMath::Max(
              FMath::Max(FMath::Abs(dq), FMath::Abs(dr)), FMath::Abs(dq + dr));

          if ((float)HexDist < W.MinimumReachDistance ||
              HexDist > W.ReachDistance) {
            bValidBodyPosition = false;
            break;
          }
        }
      }

      bool bCurrentValid = true;
      for (FFootState *Foot : CenteringFeet) {
        if (Foot->StepState == EFootStepState::Planted ||
            (Foot->StepState == EFootStepState::Lowering &&
             Foot->StepProgress + 1 >= W.StepLowerTicks)) {
          const int32 HexDist = HexUtil::Dist(W.BaseHex, Foot->CurrentTile);
          if ((float)HexDist < W.MinimumReachDistance ||
              HexDist > W.ReachDistance) {
            bCurrentValid = false;
            break;
          }
        }
      }

      // Hysteresis: Only move if current is invalid OR if proposed is
      // significantly better/different
      const int32 ProposedDist = HexUtil::Dist(W.BaseHex, ProposedBodyHex);

      // CRITICAL: If we're processing a request and the proposed body position
      // would be AT the target hex, BLOCK this unless a foot is already there.
      // Feet can't step on hexes at distance 0 from the body, so the body must
      // stay offset. ONLY apply this logic if the request is a direct beacon
      // interaction (Priority 1000). For offsets (Priority 2000) and ReturnHome
      // (Priority -1), the body SHOULD reach the target.
      bool bBlockMoveToTarget = false;
      if (W.bProcessingRequest && ProposedBodyHex == W.CurrentTargetHex) {
        bool bIsDirectBeaconTask = false;
        if (W.Requests.Num() > 0 && W.Requests[0].Priority == 1000) {
          bIsDirectBeaconTask = true;
        }

        if (bIsDirectBeaconTask) {
          bool bFootAtTarget = false;
          for (FFootState *F : CenteringFeet) {
            if (F->CurrentTile == W.CurrentTargetHex) {
              bFootAtTarget = true;
              break;
            }
          }
          if (!bFootAtTarget) {
            // Block movement to target - body must stay offset so feet can
            // reach it
            bBlockMoveToTarget = true;
          }
        }
      }

      if (bBlockMoveToTarget) {
        // Don't update body position - keep current to allow feet to reach
        // target Only skip if current position is valid
        if (bCurrentValid) {
          // Stay at current position
        } else {
          // Current is invalid but we can't go to target either
          // Try to find a valid offset position (1 hex away from target)
          bool bFoundOffset = false;
          for (int32 dir = 0; dir < 6 && !bFoundOffset; ++dir) {
            FHex OffsetHex =
                HexUtil::Neighbor(W.CurrentTargetHex, (EHexDir)dir);

            // Check if all feet would be in reach from this offset position
            bool bAllFeetValid = true;
            for (FFootState *Foot : CenteringFeet) {
              if (Foot->StepState == EFootStepState::Planted ||
                  (Foot->StepState == EFootStepState::Lowering &&
                   Foot->StepProgress + 1 >= W.StepLowerTicks)) {
                const int32 HexDist =
                    HexUtil::Dist(OffsetHex, Foot->CurrentTile);
                if ((float)HexDist < W.MinimumReachDistance ||
                    HexDist > W.ReachDistance) {
                  bAllFeetValid = false;
                  break;
                }
              }
            }

            if (bAllFeetValid) {
              W.BaseHex = OffsetHex;
              bFoundOffset = true;
            }
          }

          if (!bFoundOffset) {
            // Fallback: use proposed but log warning
            W.BaseHex = ProposedBodyHex;
            UE_LOG(LogTemp, Warning,
                   TEXT("Walker %d: Forced to position body at target (no "
                        "valid offset found)"),
                   WalkerIndex);
          }
        }
      } else if (bValidBodyPosition) {
        const bool bIsAtTarget =
            (W.bProcessingRequest && ProposedBodyHex == W.CurrentTargetHex);
        if (!bCurrentValid || ProposedDist >= 1 || bIsAtTarget) {
          W.BaseHex = ProposedBodyHex;
        }
      } else if (!bCurrentValid) {
        // Even if proposed is "invalid" by constraints, if current is also
        // invalid, move to proposed as it's at least the geometric center.
        W.BaseHex = ProposedBodyHex;
        UE_LOG(LogTemp, Verbose,
               TEXT("Walker %d: Forcing body position to geographic center"),
               WalkerIndex);
      }
    }
  }
}

void USimWorldHex::ProcessWalkerInstructions(FWalkerState &W,
                                             int32 WalkerIndex) {
  // Legacy program support
  if (W.Program.Num() > 0 && W.Requests.Num() == 0 && !W.bProcessingRequest &&
      W.Stance == EWalkerStance::Standing) {
    if (W.WaitCounter > 0) {
      --W.WaitCounter;
      return;
    }

    const FWalkerInstr &Inst = W.Program[W.PC];
    switch (Inst.Cmd) {
    case EWalkerCmd::MoveTo: {
      W.TargetHex = FHex(Inst.Arg1, Inst.Arg2, W.BaseHex.Level);
      W.bMoving = true;
      W.MovementProgress = 0.0f;
      W.PC = (W.PC + 1) % W.Program.Num();
      break;
    }
    case EWalkerCmd::Wait: {
      W.WaitCounter = Inst.Arg1;
      W.PC = (W.PC + 1) % W.Program.Num();
      break;
    }
    case EWalkerCmd::Reset: {
      W.TargetHex = W.HomeHex;
      W.bMoving = true;
      W.MovementProgress = 0.0f;
      W.PC = (W.PC + 1) % W.Program.Num();
      break;
    }
    default:
      W.PC = (W.PC + 1) % W.Program.Num();
      break;
    }
  }
}
