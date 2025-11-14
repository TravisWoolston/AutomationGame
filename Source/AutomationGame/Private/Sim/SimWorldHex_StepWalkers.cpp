#include "Sim/SimWorldHex.h"
#include "Sim/HexTypes.h"
#include "Sim/WalkerTypes.h"
#include "Sim/HexCoord.h"

// Helper to check if a hex is occupied by obstacles
static bool IsHexOccupied(const USimWorldHex* Sim, const FHex& H, const TSet<FHex>& HeldOrbHexes, const TSet<FHex>& ArmBaseHexes)
{
	// Check for items (orbs on ground or belts)
	if (Sim->ItemMap.Contains(H))
	{
		return true;
	}
	
	// Check for held orbs
	if (HeldOrbHexes.Contains(H))
	{
		return true;
	}
	
	// Check for arms
	if (ArmBaseHexes.Contains(H))
	{
		return true;
	}
	
	// Check for belt tiles (walkers can't step on belts)
	if (const FTile* Tile = Sim->TileMap.Find(H))
	{
		if (Tile->Kind == ETileKind::Belt)
		{
			return true;
		}
	}
	
	return false;
}

// Helper to get all feet from a walker state
static TArray<FFootState*> GetAllFeet(FWalkerState& W)
{
	return { &W.DFoot, &W.LFoot, &W.RFoot, &W.UFoot };
}

// Helper to check if two feet are adjacent (using bone naming convention)
static bool AreFeetAdjacent(int32 FootIndexA, int32 FootIndexB)
{
	// Foot order: D=0, L=1, R=2, U=3
	// Adjacent pairs: (D,L), (L,U), (U,R), (R,D)
	if (FootIndexA == FootIndexB) return false;
	
	const int32 MinIdx = FMath::Min(FootIndexA, FootIndexB);
	const int32 MaxIdx = FMath::Max(FootIndexA, FootIndexB);
	
	// Check adjacent pairs
	if ((MinIdx == 0 && MaxIdx == 1) || // D-L
	    (MinIdx == 1 && MaxIdx == 3) || // L-U
	    (MinIdx == 0 && MaxIdx == 2) || // D-R
	    (MinIdx == 2 && MaxIdx == 3))   // R-U
	{
		return true;
	}
	
	return false;
}

void USimWorldHex::StepWalkers()
{
	// Build occupancy sets for obstacle detection
	TSet<FHex> HeldOrbHexes;
	TSet<FHex> ArmBaseHexes;
	
	for (const FArmState& A : Arms)
	{
		// Add arm base hex
		ArmBaseHexes.Add(A.BaseHex);
		
		if (!A.bHolding) continue;

		// Calculate grip hex
		FHex GripHex = A.BaseHex;
		for (int i = 0; i < A.ArmLen; ++i)
			GripHex = HexUtil::Neighbor(GripHex, A.Facing);

		if (!A.Held.bIsCluster)
		{
			HeldOrbHexes.Add(GripHex);
		}
		else
		{
			// Add all orbs in held cluster
			const uint8 cur = (uint8)A.Facing % 6;
			const int8 grab = (int8)A.Held.GrabFacing;
			const uint8 facingDelta = (grab >= 0)
				? (uint8)(((int32)cur - (int32)(grab % 6) + 6) % 6)
				: cur;
			const uint8 ccw = (uint8)((6 - facingDelta) % 6);

			auto Rot60 = [](FHex D, uint8 StepsCCW)
				{
					for (uint8 i = 0; i < (StepsCCW % 6); ++i)
					{
						int32 q = D.Q;
						int32 r = D.R;
						D.Q = -r;
						D.R = q + r;
					}
					return D;
				};

			for (const FOrbData& Sub : A.Held.Orbs)
			{
				FHex Local(Sub.dQ, Sub.dR);
				FHex Rot = Rot60(Local, ccw);
				FHex WorldPos(GripHex.Q + Rot.Q, GripHex.R + Rot.R);
				HeldOrbHexes.Add(WorldPos);
			}
		}
	}
	
	// Step each walker
	for (int32 WalkerIndex = 0; WalkerIndex < Walkers.Num(); ++WalkerIndex)
	{
		FWalkerState& W = Walkers[WalkerIndex];
		
		// Handle stance transitions (sitting <-> standing)
		if (W.Stance == EWalkerStance::StandingUp)
		{
			W.StanceTransitionProgress++;
			if (W.StanceTransitionProgress >= 3)
			{
				W.Stance = EWalkerStance::Standing;
				W.StanceTransitionProgress = 0;
				UE_LOG(LogTemp, Log, TEXT("Walker %d: Finished standing up, now standing"), WalkerIndex);
			}
			continue; // Don't process other logic during transition
		}
		else if (W.Stance == EWalkerStance::SittingDown)
		{
			W.StanceTransitionProgress++;
			if (W.StanceTransitionProgress >= 3)
			{
				W.Stance = EWalkerStance::Sitting;
				W.StanceTransitionProgress = 0;
				UE_LOG(LogTemp, Log, TEXT("Walker %d: Finished sitting down, now sitting"), WalkerIndex);
			}
			continue; // Don't process other logic during transition
		}
		
		// If sitting and have requests, start standing up
		if (W.Stance == EWalkerStance::Sitting && W.Requests.Num() > 0)
		{
			W.Stance = EWalkerStance::StandingUp;
			W.StanceTransitionProgress = 0;
			UE_LOG(LogTemp, Log, TEXT("Walker %d: Starting to stand up (has %d requests)"), WalkerIndex, W.Requests.Num());
			continue;
		}
		
		// If standing and no requests, check if we're at home - if so, sit down
		if (W.Stance == EWalkerStance::Standing && W.Requests.Num() == 0 && !W.bProcessingRequest)
		{
			// Check if at home
			bool bAtHome = (W.BaseHex == W.HomeHex);
			if (!bAtHome)
			{
				// Need to return home - create a request
				FWalkerRequest ReturnRequest;
				ReturnRequest.TargetHex = W.HomeHex;
				ReturnRequest.Priority = -1; // Low priority
				W.Requests.Insert(ReturnRequest, 0);
			}
			else
			{
				// At home, sit down
				W.Stance = EWalkerStance::SittingDown;
				W.StanceTransitionProgress = 0;
				continue;
			}
		}
		
		// Only process movement when standing
		if (W.Stance != EWalkerStance::Standing)
		{
			continue;
		}
		
		// Process current request or start new one
		if (!W.bProcessingRequest && W.Requests.Num() > 0)
		{
			// Start processing next request
			W.CurrentTargetHex = W.Requests[0].TargetHex;
			W.Requests.RemoveAt(0);
			W.bProcessingRequest = true;
			UE_LOG(LogTemp, Log, TEXT("Walker %d: Started processing request to move to (%d, %d)"), 
				WalkerIndex, W.CurrentTargetHex.Q, W.CurrentTargetHex.R);
		}
		
		// If processing a request, execute walking animation
		if (W.bProcessingRequest)
		{
			// Check if we've reached the target
			if (W.BaseHex == W.CurrentTargetHex)
			{
				// Reached target, finish request
				W.bProcessingRequest = false;
				UE_LOG(LogTemp, Log, TEXT("Walker %d: Reached target (%d, %d)"), 
					WalkerIndex, W.CurrentTargetHex.Q, W.CurrentTargetHex.R);
				continue;
			}
			
			// Get all feet
			TArray<FFootState*> Feet = GetAllFeet(W);
			
			// Count how many feet are currently raised (not planted)
			int32 RaisedCount = 0;
			TArray<int32> RaisedIndices;
			for (int32 i = 0; i < Feet.Num(); ++i)
			{
				if (Feet[i]->StepState != EFootStepState::Planted)
				{
					RaisedCount++;
					RaisedIndices.Add(i);
				}
			}
			
			// Process feet in step sequence
			for (int32 FootIndex = 0; FootIndex < Feet.Num(); ++FootIndex)
			{
				FFootState& Foot = *Feet[FootIndex];
				
				switch (Foot.StepState)
				{
				case EFootStepState::Planted:
				{
					// Can this foot start stepping?
					// Conditions:
					// 1. Less than 2 feet are raised
					// 2. No adjacent feet are raised
					// 3. Foot needs to move (not at ideal position)
					
					// Debug: Log foot state once
					static bool bLoggedFootState = false;
					if (!bLoggedFootState)
					{
						UE_LOG(LogTemp, Log, TEXT("Walker %d Foot %d: Current=(%d,%d), Target=(%d,%d), RaisedCount=%d, ReachDistance=%d"),
							WalkerIndex, FootIndex, Foot.CurrentTile.Q, Foot.CurrentTile.R,
							Foot.TargetTile.Q, Foot.TargetTile.R, RaisedCount, W.ReachDistance);
						if (FootIndex == 3) bLoggedFootState = true;
					}
					
					if (RaisedCount >= 2)
					{
						break; // Too many feet raised
					}
					
					// Check adjacent feet
					bool bAdjacentRaised = false;
					for (int32 RaisedIdx : RaisedIndices)
					{
						if (AreFeetAdjacent(FootIndex, RaisedIdx))
						{
							bAdjacentRaised = true;
							break;
						}
					}
					if (bAdjacentRaised)
					{
						break; // Adjacent foot is raised
					}
					
					// Find best tile for this foot within ReachDistance
					// Goal: move foot toward target hex
					FHex BestTile = Foot.CurrentTile;
					float BestScore = MAX_flt;
					int32 TestedTiles = 0;
					int32 OccupiedTiles = 0;
					int32 OtherFootTiles = 0;
					
					for (int32 q = -W.ReachDistance; q <= W.ReachDistance; ++q)
					{
						for (int32 r = -W.ReachDistance; r <= W.ReachDistance; ++r)
						{
							FHex TestHex(Foot.CurrentTile.Q + q, Foot.CurrentTile.R + r);
							
							// Check hex distance from foot
							const int32 dq = q;
							const int32 dr = r;
							const int32 HexDist = FMath::Max(FMath::Max(FMath::Abs(dq), FMath::Abs(dr)), FMath::Abs(dq + dr));
							if (HexDist > W.ReachDistance || HexDist == 0)
							{
								continue; // Out of reach or same tile
							}
							
							TestedTiles++;
							
							// Check if occupied
							if (IsHexOccupied(this, TestHex, HeldOrbHexes, ArmBaseHexes))
							{
								OccupiedTiles++;
								continue; // Occupied
							}
							
							// Check if another foot is already at this hex (but allow targeting same tile for now)
							bool bOtherFootHere = false;
							for (int32 OtherIdx = 0; OtherIdx < Feet.Num(); ++OtherIdx)
							{
								if (OtherIdx == FootIndex) continue;
								// Only block if another foot is currently at this position
								// Don't block based on TargetTile to avoid deadlocks
								if (Feet[OtherIdx]->CurrentTile == TestHex)
								{
									bOtherFootHere = true;
									break;
								}
							}
							if (bOtherFootHere)
							{
								OtherFootTiles++;
								continue; // Another foot is here
							}
							
							// Score based on distance to target hex
							const int32 ToTargetDQ = W.CurrentTargetHex.Q - TestHex.Q;
							const int32 ToTargetDR = W.CurrentTargetHex.R - TestHex.R;
							const float DistToTarget = FMath::Max(FMath::Max(FMath::Abs(float(ToTargetDQ)), FMath::Abs(float(ToTargetDR))), FMath::Abs(float(ToTargetDQ + ToTargetDR)));
							
							// Also prefer tiles that are in the general direction of the target
							const int32 CurrentToTargetDQ = W.CurrentTargetHex.Q - Foot.CurrentTile.Q;
							const int32 CurrentToTargetDR = W.CurrentTargetHex.R - Foot.CurrentTile.R;
							const float CurrentDistToTarget = FMath::Max(FMath::Max(FMath::Abs(float(CurrentToTargetDQ)), FMath::Abs(float(CurrentToTargetDR))), FMath::Abs(float(CurrentToTargetDQ + CurrentToTargetDR)));
							
							// Only step if it gets us closer to the target (or at least not farther)
							if (DistToTarget <= CurrentDistToTarget && DistToTarget < BestScore)
							{
								BestScore = DistToTarget;
								BestTile = TestHex;
							}
						}
					}
					
					// Debug: Log search results if no tile found
					if (BestTile == Foot.CurrentTile && TestedTiles > 0)
					{
						UE_LOG(LogTemp, Log, TEXT("Walker %d Foot %d: No valid tile found - Tested=%d, Occupied=%d, OtherFeet=%d, ReachDist=%d"),
							WalkerIndex, FootIndex, TestedTiles, OccupiedTiles, OtherFootTiles, W.ReachDistance);
					}
					
					// If we found a better tile, start stepping
					if (BestTile != Foot.CurrentTile)
					{
						Foot.TargetTile = BestTile;
						Foot.StepState = EFootStepState::Raising;
						Foot.StepProgress = 0;
						Foot.HeightOffset = 0.f;
						RaisedCount++; // Update raised count
						RaisedIndices.Add(FootIndex);
						UE_LOG(LogTemp, Log, TEXT("Walker %d Foot %d: Starting to step from (%d,%d) to (%d,%d) - Score: %.1f"),
							WalkerIndex, FootIndex, Foot.CurrentTile.Q, Foot.CurrentTile.R, 
							BestTile.Q, BestTile.R, BestScore);
					}
					else
					{
						// Debug: Log why foot isn't moving
						static int32 DebugCounter = 0;
						if (++DebugCounter % 60 == 0) // Log occasionally
						{
							UE_LOG(LogTemp, Verbose, TEXT("Walker %d Foot %d: No better tile found (Current: %d,%d, Target: %d,%d, BestScore: %.1f)"),
								WalkerIndex, FootIndex, Foot.CurrentTile.Q, Foot.CurrentTile.R,
								W.CurrentTargetHex.Q, W.CurrentTargetHex.R, BestScore);
						}
					}
					break;
				}
				
				case EFootStepState::Raising:
				{
					Foot.StepProgress++;
					Foot.HeightOffset = (float)Foot.StepProgress / (float)W.StepRaiseTicks * W.StepHeight;
					
					if (Foot.StepProgress >= W.StepRaiseTicks)
					{
						Foot.StepState = EFootStepState::Moving;
						Foot.StepProgress = 0;
						UE_LOG(LogTemp, Log, TEXT("Walker %d Foot %d: Raised, now moving"), WalkerIndex, FootIndex);
					}
					break;
				}
				
				case EFootStepState::Moving:
				{
					Foot.StepProgress++;
					// Move toward target tile
					// For simplicity, move directly to target after ticks complete
					if (Foot.StepProgress >= W.StepMoveTicks)
					{
						Foot.CurrentTile = Foot.TargetTile;
						Foot.StepState = EFootStepState::Lowering;
						Foot.StepProgress = 0;
						UE_LOG(LogTemp, Log, TEXT("Walker %d Foot %d: Moved to (%d,%d), now lowering"), 
							WalkerIndex, FootIndex, Foot.CurrentTile.Q, Foot.CurrentTile.R);
					}
					break;
				}
				
				case EFootStepState::Lowering:
				{
					Foot.StepProgress++;
					Foot.HeightOffset = W.StepHeight * (1.f - (float)Foot.StepProgress / (float)W.StepLowerTicks);
					
					if (Foot.StepProgress >= W.StepLowerTicks)
					{
						Foot.StepState = EFootStepState::Planted;
						Foot.StepProgress = 0;
						Foot.HeightOffset = 0.f;
						UE_LOG(LogTemp, Log, TEXT("Walker %d Foot %d: Planted at (%d,%d)"), 
							WalkerIndex, FootIndex, Foot.CurrentTile.Q, Foot.CurrentTile.R);
					}
					break;
				}
				}
			}
			
			// Update body position to center of feet (average of foot positions)
			if (Feet.Num() == 4)
			{
				int32 AvgQ = (W.DFoot.CurrentTile.Q + W.LFoot.CurrentTile.Q + W.RFoot.CurrentTile.Q + W.UFoot.CurrentTile.Q) / 4;
				int32 AvgR = (W.DFoot.CurrentTile.R + W.LFoot.CurrentTile.R + W.RFoot.CurrentTile.R + W.UFoot.CurrentTile.R) / 4;
				W.BaseHex = FHex(AvgQ, AvgR);
			}
		}
		
		// Legacy program support (for backward compatibility with scenarios)
		// If walker has a program and no requests, process program
		if (W.Program.Num() > 0 && W.Requests.Num() == 0 && !W.bProcessingRequest && W.Stance == EWalkerStance::Standing)
		{
			// Handle wait counter
			if (W.WaitCounter > 0)
			{
				--W.WaitCounter;
				continue;
			}
			
			// If currently moving, complete movement immediately
			// Legacy movement system no longer used - walker movement is now based on foot positions
			if (W.bMoving)
			{
				W.BaseHex = W.TargetHex;
				W.bMoving = false;
				W.MovementProgress = 0.0f;
				// Advance to next instruction
				W.PC = (W.PC + 1) % W.Program.Num();
				continue;
			}
			
			// Execute current instruction
			if (W.PC >= W.Program.Num())
			{
				W.PC = 0; // Loop back
			}
			
			const FWalkerInstr& Instr = W.Program[W.PC];
			
			switch (Instr.Cmd)
			{
			case EWalkerCmd::NoOp:
				W.PC = (W.PC + 1) % W.Program.Num();
				break;
				
			case EWalkerCmd::Wait:
				W.WaitCounter = FMath::Max(1, Instr.Arg1);
				W.PC = (W.PC + 1) % W.Program.Num();
				break;
				
			case EWalkerCmd::MoveTo:
			{
				FHex TargetHex(Instr.Arg1, Instr.Arg2);
				W.TargetHex = TargetHex;
				W.bMoving = true;
				W.MovementProgress = 0.0f;
				// Don't advance PC yet - wait until movement completes
				break;
			}
			
			case EWalkerCmd::Reset:
			{
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
}
