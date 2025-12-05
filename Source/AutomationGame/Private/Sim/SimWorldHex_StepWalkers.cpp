#include "Sim/SimWorldHex.h"
#include "Sim/HexTypes.h"
#include "Sim/WalkerTypes.h"
#include "Sim/HexCoord.h"
#include "Algo/Sort.h"

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
	
	/*
	// Check for belt tiles (walkers can't step on belts)
	if (const FTile* Tile = Sim->TileMap.Find(H))
	{
		if (Tile->Kind == ETileKind::Belt)
		{
			return true;
		}
	}
	*/
	
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
	// We treat the physical legs in clockwise order around the body as U, R, D, L.
	// Index mapping (walker state order -> clockwise ring position):
	//   D=0 -> ring index 2 (Down/South)
	//   L=1 -> ring index 3 (Left/West)
	//   R=2 -> ring index 1 (Right/East)
	//   U=3 -> ring index 0 (Up/North)
	// Adjacent means neighbors on this ring: (U,R), (R,D), (D,L), (L,U).
	if (FootIndexA == FootIndexB) return false;

	auto GetRingIndex = [](int32 FootIndex) -> int32
	{
		switch (FootIndex)
		{
		case 0: return 2; // D
		case 1: return 3; // L
		case 2: return 1; // R
		case 3: return 0; // U
		default: return -1;
		}
	};

	const int32 RingA = GetRingIndex(FootIndexA);
	const int32 RingB = GetRingIndex(FootIndexB);
	if (RingA < 0 || RingB < 0) return false;

	// Neighbors on 4‑element ring: diff of 1 or 3 (wraparound)
	const int32 Diff = FMath::Abs(RingA - RingB);
	return (Diff == 1 || Diff == 3);
}

// Helper: map a hex around BodyHex into a compass quadrant label N/E/S/W using rotation offset
static FString GetFixedQuadrantFromHex(const FHex& BodyHex, const FHex& TestHex, float RotationOffsetDeg)
{
	const int32 q = TestHex.Q - BodyHex.Q;
	const int32 r = TestHex.R - BodyHex.R;

	// Convert to Cartesian (Pointy Top, Size 1) to get angle
	// x = sqrt(3) * q + sqrt(3)/2 * r
	// y = 1.5 * r
	const float x = 1.7320508f * (float)q + 0.8660254f * (float)r;
	const float y = 1.5f * (float)r;

	// Calculate angle in degrees (-180 to 180)
	// 0 degrees is along +X axis (Forward/North in UE standard)
	float AngleDeg = FMath::RadiansToDegrees(FMath::Atan2(y, x));

	// Apply rotation offset
	AngleDeg -= RotationOffsetDeg;

	// Normalize to -180..180
	while (AngleDeg > 180.f) AngleDeg -= 360.f;
	while (AngleDeg <= -180.f) AngleDeg += 360.f;

	// Assign based on angle (centered on axes)
	// N (Forward/X+): -45 to 45
	// E (Right/Y+): 45 to 135
	// S (Back/X-): 135 to 180 or -180 to -135
	// W (Left/Y-): -135 to -45

	if (AngleDeg >= -45.f && AngleDeg < 45.f) return TEXT("N");
	if (AngleDeg >= 45.f && AngleDeg < 135.f) return TEXT("E");
	if (AngleDeg >= -135.f && AngleDeg < -45.f) return TEXT("W");
	return TEXT("S");
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
			W.Requests[0].AttemptTicks = 0; // Reset attempt counter
			// Don't remove yet - wait until complete or failed
			W.bProcessingRequest = true;
			UE_LOG(LogTemp, Log, TEXT("Walker %d: Started processing request to move to (%d, %d)"), 
				WalkerIndex, W.CurrentTargetHex.Q, W.CurrentTargetHex.R);
		}
		
		// If processing a request, execute walking animation
		if (W.bProcessingRequest)
		{
			// Increment attempt counter
			// Since we keep the request at index 0 until complete, we can update it there
			if (W.Requests.Num() > 0)
			{
				W.Requests[0].AttemptTicks++;
				
				// Timeout check: If attempts exceed limit (e.g. 300 ticks = 5-10 seconds depending on tick rate)
				// Cancel request and return home
				if (W.Requests[0].AttemptTicks > 300)
				{
					UE_LOG(LogTemp, Warning, TEXT("Walker %d: Request to (%d,%d) timed out after %d ticks. Returning home."), 
						WalkerIndex, W.CurrentTargetHex.Q, W.CurrentTargetHex.R, W.Requests[0].AttemptTicks);
					
					W.bProcessingRequest = false;
					W.Requests.RemoveAt(0);
					
					// Add return home request with high priority
					FWalkerRequest HomeReq;
					HomeReq.TargetHex = W.HomeHex;
					HomeReq.Priority = 1000;
					W.Requests.Insert(HomeReq, 0);
					
					continue;
				}
			}
			else
			{
				// Should not happen if bProcessingRequest is true
				W.bProcessingRequest = false;
				continue;
			}

			// Check if we've reached the target AND are stable (all feet planted)
			// This ensures the body is truly centered and settled before moving on
			bool bAllFeetPlanted = true;
			TArray<FFootState*> CheckFeet = GetAllFeet(W);
			for (FFootState* F : CheckFeet)
			{
				if (F->StepState != EFootStepState::Planted)
				{
					bAllFeetPlanted = false;
					break;
				}
			}

			// Determine success condition:
			// If returning Home, we must be exactly at HomeHex.
			// Otherwise, being within ReachDistance of the target is sufficient.
			const bool bIsReturningHome = (W.CurrentTargetHex == W.HomeHex);
			bool bTargetReached = false;

			if (bIsReturningHome)
			{
				bTargetReached = (W.BaseHex == W.CurrentTargetHex);
			}
			else
			{
				const int32 distQ = W.BaseHex.Q - W.CurrentTargetHex.Q;
				const int32 distR = W.BaseHex.R - W.CurrentTargetHex.R;
				const int32 dist = FMath::Max(FMath::Max(FMath::Abs(distQ), FMath::Abs(distR)), FMath::Abs(distQ + distR));
				bTargetReached = (dist <= W.ReachDistance);
			}

			if (bTargetReached && bAllFeetPlanted)
			{
				// Reached target and stable, finish request
				W.bProcessingRequest = false;
				if (W.Requests.Num() > 0)
				{
					W.Requests.RemoveAt(0);
				}
				UE_LOG(LogTemp, Log, TEXT("Walker %d: Reached target (%d, %d) [Dist=%d, IsHome=%d] and stabilized"), 
					WalkerIndex, W.CurrentTargetHex.Q, W.CurrentTargetHex.R, 
					0, bIsReturningHome ? 1 : 0);
				continue;
			}
			
			// Get all feet
			TArray<FFootState*> Feet = GetAllFeet(W);

			// Precompute distance of each foot from the body center (hex distance), used to
			// prioritize feet that are closer to the edge of their allowed reach.
			const int32 NumFeet = Feet.Num();
			TArray<float> FootBodyDist;
			FootBodyDist.SetNum(NumFeet);
			for (int32 i = 0; i < NumFeet; ++i)
			{
				const FHex& FH = Feet[i]->CurrentTile;
				const int32 dq = FH.Q - W.BaseHex.Q;
				const int32 dr = FH.R - W.BaseHex.R;
				const int32 HexDist = FMath::Max(FMath::Max(FMath::Abs(dq), FMath::Abs(dr)), FMath::Abs(dq + dr));
				FootBodyDist[i] = (float)HexDist;
			}
			
			// Count how many feet are currently raised (not planted)
			int32 RaisedCount = 0;
			TArray<int32> RaisedIndices;
			for (int32 i = 0; i < NumFeet; ++i)
			{
				// Update LastPlantDuration for all feet
				if (Feet[i]->StepState == EFootStepState::Planted)
				{
					Feet[i]->LastPlantDuration++;
				}
				else
				{
					Feet[i]->LastPlantDuration = 0;
					
					// Feet moving to VALID (unoccupied) tiles do NOT count towards the limit.
					// This encourages fluid movement when the path is clear.
					// Only feet moving to occupied tiles (hovering/stuck) or in transition count.
					bool bTargetOccupied = IsHexOccupied(this, Feet[i]->TargetTile, HeldOrbHexes, ArmBaseHexes);
					
					// If target is valid (not occupied), we don't count it as a "blocking" raised foot
					// But if we are simply raising/lowering vertically, we should probably still count it to avoid 4-foot lift
					// Actually, user request: "Feet moving to valid tiles should not count towards the raised feet limit."
					
					if (bTargetOccupied)
					{
						RaisedCount++;
					}
					
					RaisedIndices.Add(i);
				}
			}

			// Determine processing order based on multiple factors:
			// 1. Time since last move (LastPlantDuration) - PRIMARY
			// 2. Distance from body (Reach) - SECONDARY
			
			TArray<int32> FootOrder;
			for (int32 i = 0; i < NumFeet; ++i)
			{
				FootOrder.Add(i);
			}
			
			FootOrder.Sort([&](int32 A, int32 B)
			{
				// Sort descending (Higher score = Process first)
				
				// Helper to get neighbor indices (same as AreFeetAdjacent but returning indices)
				auto GetNeighbors = [](int32 Idx) -> TArray<int32>
				{
					switch (Idx)
					{
					case 0: return { 1, 2 }; // D -> L, R
					case 1: return { 0, 3 }; // L -> D, U
					case 2: return { 0, 3 }; // R -> D, U
					case 3: return { 1, 2 }; // U -> L, R
					default: return {};
					}
				};

				// Factor 1: Time since last move (Favor feet that have been planted longer)
				int32 TimeScoreA = Feet[A]->LastPlantDuration;
				int32 TimeScoreB = Feet[B]->LastPlantDuration;
				
				// Factor 2: Distance from body (Favor feet at edge of reach)
				float DistScoreA = FootBodyDist[A];
				float DistScoreB = FootBodyDist[B];
				
				// Factor 3: Neighbor Favorability (Gait Pattern)
				// We want to move feet whose neighbors are STABLE (recently moved/low duration).
				// If neighbors have HIGH duration, it means *they* need to move soon, so I should yield.
				// So, penalize score based on neighbor duration.
				float NeighborPenaltyA = 0.f;
				for (int32 N : GetNeighbors(A)) NeighborPenaltyA += (float)Feet[N]->LastPlantDuration;
				
				float NeighborPenaltyB = 0.f;
				for (int32 N : GetNeighbors(B)) NeighborPenaltyB += (float)Feet[N]->LastPlantDuration;
				
				// Combine scores
				// Time dominates, then Neighbor Penalty (to force alternating), then Distance
				// Weightings:
				// Time: 10.0
				// Neighbor Penalty: -5.0 (Significant influence)
				// Distance: 1.0
				
				float TotalScoreA = (float)TimeScoreA * 10.0f - NeighborPenaltyA * 5.0f + DistScoreA;
				float TotalScoreB = (float)TimeScoreB * 10.0f - NeighborPenaltyB * 5.0f + DistScoreB;
				
				return TotalScoreA > TotalScoreB;
			});
			
			// Process feet in prioritized order
			for (int32 OrderIdx = 0; OrderIdx < FootOrder.Num(); ++OrderIdx)
			{
				const int32 FootIndex = FootOrder[OrderIdx];
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
					
					if (RaisedCount >= 1)
					{
						// Check progress of the raised foot for gait sequencing
						// Allow overlapping steps if the current step is > 50% complete
						bool bCanOverlap = false;
						if (RaisedCount == 1 && RaisedIndices.Num() > 0)
						{
							int32 RaisedIdx = RaisedIndices[0];
							const FFootState& RaisedFoot = *Feet[RaisedIdx];
							
							int32 TotalTicks = W.StepRaiseTicks + W.StepMoveTicks + W.StepLowerTicks;
							int32 CurrentTick = 0;
							if (RaisedFoot.StepState == EFootStepState::Raising) 
								CurrentTick = RaisedFoot.StepProgress;
							else if (RaisedFoot.StepState == EFootStepState::Moving) 
								CurrentTick = W.StepRaiseTicks + RaisedFoot.StepProgress;
							else if (RaisedFoot.StepState == EFootStepState::Lowering) 
								CurrentTick = W.StepRaiseTicks + W.StepMoveTicks + RaisedFoot.StepProgress;
							
							if (CurrentTick > TotalTicks / 2)
							{
								bCanOverlap = true;
							}
						}
						
						if (!bCanOverlap)
						{
							break; // Wait for sequence
						}
						
						// Safety check: Ensure at least TWO feet remain planted
						// PlantedCount = Total - RaisedCount.
						// If we raise this foot, RaisedCount increases by 1.
						// So we need (NumFeet - (RaisedCount + 1)) >= 2.
						// => NumFeet - RaisedCount - 1 >= 2
						// => NumFeet - RaisedCount >= 3
						// => RaisedCount <= NumFeet - 3
						if (RaisedCount > (NumFeet - 3))
						{
							break; // Must leave at least two feet planted
						}
					}
					
					
					// Updated constraint: Limit to 2 raised feet (so 2 are planted)
					// UNLESS we are just checking for "valid" moves in the RaisedCount logic above?
					// The user asked: "require atleast one foot to be actually placed on a tile"
					// My calculation of RaisedCount excludes feet moving to valid tiles.
					// But physically, those feet ARE raised.
					// So I need a separate count of PHYSICALLY raised feet to ensure stability.
					
					int32 PhysicallyRaisedCount = 0;
					for (int32 i = 0; i < NumFeet; ++i)
					{
						if (Feet[i]->StepState != EFootStepState::Planted)
						{
							PhysicallyRaisedCount++;
						}
					}
					
					if (PhysicallyRaisedCount >= (NumFeet - 2))
					{
						break; // Don't lift if it would leave less than 2 feet planted
					}
					
					/*
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
					*/
					
					// Find best tile for this foot within ReachDistance and its assigned quadrant.
					// Goal: move foot toward target hex while respecting the leg's quadrant.
					FHex BestTile = Foot.CurrentTile;
					float BestScore = MAX_flt;
					int32 TestedTiles = 0;
					int32 OccupiedTiles = 0;
					int32 OtherFootTiles = 0;
					int32 ValidQuadrantTiles = 0;
					
					// Map this foot index to its compass quadrant: D->S, L->W, R->E, U->N
					FString FootQuadrant;
					switch (FootIndex)
					{
					case 0: FootQuadrant = TEXT("S"); break; // D (down) -> South
					case 1: FootQuadrant = TEXT("W"); break; // L -> West
					case 2: FootQuadrant = TEXT("E"); break; // R -> East
					case 3: FootQuadrant = TEXT("N"); break; // U -> North
					default: FootQuadrant = TEXT("N"); break;
					}
					
					// Calculate Move Direction (for bias)
					float MoveDQ = (float)(W.CurrentTargetHex.Q - W.BaseHex.Q);
					float MoveDR = (float)(W.CurrentTargetHex.R - W.BaseHex.R);
					float MoveLen = FMath::Sqrt(MoveDQ*MoveDQ + MoveDR*MoveDR + (MoveDQ+MoveDR)*(MoveDQ+MoveDR));
					if (MoveLen > 0.001f) { MoveDQ /= MoveLen; MoveDR /= MoveLen; }

					// Calculate Ideal Hex for this foot (used for scoring and fallback)
					float QuadDQ = 0.f, QuadDR = 0.f;
					// Corrected Quadrant Vectors (aligning with Hex axes)
					// N (X+, 0 deg) -> (1, 0)
					// E (Y+, 60-90 deg) -> (0, 1) [60 deg]
					// S (X-, 180 deg) -> (-1, 0)
					// W (Y-, -120 to -60 deg) -> (0, -1) [-120 deg]
					if (FootQuadrant == TEXT("N")) { QuadDQ = 1.f; QuadDR = 0.f; }
					else if (FootQuadrant == TEXT("E")) { QuadDQ = 0.f; QuadDR = 1.f; }
					else if (FootQuadrant == TEXT("S")) { QuadDQ = -1.f; QuadDR = 0.f; }
					else if (FootQuadrant == TEXT("W")) { QuadDQ = 0.f; QuadDR = -1.f; }
					
					// Apply rotation to the ideal leg offset vector
					if (W.QuadrantRotationOffset != 0.f)
					{
						float Rad = FMath::DegreesToRadians(W.QuadrantRotationOffset);
						float C = FMath::Cos(Rad);
						float S = FMath::Sin(Rad);
						
						// Hex to Cartesian for the vector
						float vX = 1.7320508f * QuadDQ + 0.8660254f * QuadDR;
						float vY = 1.5f * QuadDR;
						
						// Rotate
						float rotX = vX * C - vY * S;
						float rotY = vX * S + vY * C;
						
						// Cartesian back to Hex
						float rotQ = (rotX * 0.5773503f - rotY * 0.3333333f);
						float rotR = rotY * 0.6666667f;
						
						QuadDQ = rotQ;
						QuadDR = rotR;
					}
					
					float IdealDist = (float)W.ReachDistance * 0.6f;
					float LeadDist = (float)W.ReachDistance * 0.4f;
					float IdealQ = (float)W.BaseHex.Q + QuadDQ * IdealDist + MoveDQ * LeadDist;
					float IdealR = (float)W.BaseHex.R + QuadDR * IdealDist + MoveDR * LeadDist;
					FHex IdealHex(FMath::RoundToInt(IdealQ), FMath::RoundToInt(IdealR));

					// Track best VALID tile separately for fallback (detour)
					FHex BestValidTile = Foot.CurrentTile;
					float BestValidScore = MAX_flt;

					for (int32 q = -W.ReachDistance; q <= W.ReachDistance; ++q)
					{
						for (int32 r = -W.ReachDistance; r <= W.ReachDistance; ++r)
						{
							// Candidate tiles are chosen around the body center so that each leg
							// cannot exceed its global reach distance from the body.
							FHex TestHex(W.BaseHex.Q + q, W.BaseHex.R + r);
							
							// Check hex distance from body
							const int32 dq = TestHex.Q - W.BaseHex.Q;
							const int32 dr = TestHex.R - W.BaseHex.R;
							const int32 HexDist = FMath::Max(FMath::Max(FMath::Abs(dq), FMath::Abs(dr)), FMath::Abs(dq + dr));
							const float HexDistFloat = (float)HexDist;
							
							// Must be within ReachDistance and at least MinimumReachDistance from body
							// Apply hysteresis for the current tile to prevent oscillation at boundaries
							bool bIsCurrentTile = (TestHex == Foot.CurrentTile);
							float Hysteresis = bIsCurrentTile ? 0.5f : 0.f;
							
							if (HexDist == 0 || HexDist > (W.ReachDistance + (bIsCurrentTile ? 1 : 0)) || 
								HexDistFloat < (W.MinimumReachDistance - Hysteresis))
							{
								continue; // Out of reach, body tile, or too close to body
							}

							// Enforce per‑leg quadrant: skip tiles outside this foot's compass sector.
							const FString FixedQuadrant = GetFixedQuadrantFromHex(W.BaseHex, TestHex, W.QuadrantRotationOffset);
							if (!FixedQuadrant.Equals(FootQuadrant, ESearchCase::CaseSensitive))
							{
								continue;
							}
							
							TestedTiles++;
							
							// Check if occupied
							bool bIsOccupied = IsHexOccupied(this, TestHex, HeldOrbHexes, ArmBaseHexes);
							if (bIsOccupied)
							{
								OccupiedTiles++;
								// Allow occupied tiles as candidates (for hovering), but penalize them heavily later
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
							
							if (!bIsOccupied)
							{
								ValidQuadrantTiles++; // Found a valid, non-occupied tile in quadrant
							}
							
			// Score based on how well this foot position helps center the body on the target tile
			// Calculate what the body position would be if this foot were at TestHex
			// (assuming other feet stay where they are)
			int32 ProjectedBodySumQ = 0;
			int32 ProjectedBodySumR = 0;
			int32 TotalFeetCount = Feet.Num(); // Always use all feet for centering (imaginary or real)
			
			for (int32 OtherIdx = 0; OtherIdx < Feet.Num(); ++OtherIdx)
			{
				if (OtherIdx == FootIndex)
				{
					// Use the candidate position for this foot
					ProjectedBodySumQ += TestHex.Q;
					ProjectedBodySumR += TestHex.R;
				}
				else
				{
					// For all other feet, use their CurrentTile.
					// If they are raised/moving, CurrentTile represents their logical position (or hovering target).
					// This allows the body to "lean" into the step using imaginary positions of raised feet.
					FFootState* OtherFoot = Feet[OtherIdx];
					ProjectedBodySumQ += OtherFoot->CurrentTile.Q;
					ProjectedBodySumR += OtherFoot->CurrentTile.R;
				}
			}
			
			// Calculate float average for precise scoring
			float AvgBodyQ = (float)ProjectedBodySumQ / (float)TotalFeetCount;
			float AvgBodyR = (float)ProjectedBodySumR / (float)TotalFeetCount;
			
			// Use RoundToInt for integer grid validation (matches body update logic)
			int32 PBodyQ = FMath::RoundToInt(AvgBodyQ);
			int32 PBodyR = FMath::RoundToInt(AvgBodyR);
			FHex ProjectedBodyHex(PBodyQ, PBodyR);
			
			// Validate that projected body position respects MinimumReachDistance for all PLANTED feet only
			bool bProjectedBodyValid = true;
			for (int32 OtherIdx = 0; OtherIdx < Feet.Num(); ++OtherIdx)
			{
				FFootState* OtherFoot = Feet[OtherIdx];
				// Only check constraints for feet that are firmly planted on the ground
				// Raised/Moving feet are flexible and don't constrain the body hard (they move with it)
				if (OtherFoot->StepState == EFootStepState::Planted || 
					(OtherFoot->StepState == EFootStepState::Lowering && OtherFoot->StepProgress + 1 >= W.StepLowerTicks))
				{
					FHex OtherFootHex;
					if (OtherIdx == FootIndex) OtherFootHex = TestHex;
					else OtherFootHex = OtherFoot->CurrentTile;
					
					const int32 fdq = OtherFootHex.Q - ProjectedBodyHex.Q;
					const int32 fdr = OtherFootHex.R - ProjectedBodyHex.R;
					const int32 fHexDist = FMath::Max(FMath::Max(FMath::Abs(fdq), FMath::Abs(fdr)), FMath::Abs(fdq + fdr));
					const float fHexDistFloat = (float)fHexDist;
					
					// Check MinimumReachDistance
					if (fHexDistFloat < W.MinimumReachDistance)
					{
						bProjectedBodyValid = false;
						break;
					}
					
					// Check ReachDistance
					if (fHexDist > W.ReachDistance)
					{
						bProjectedBodyValid = false;
						break;
					}
				}
			}
								
								// If invalid due to MinimumReachDistance or ReachDistance, try to optimize body position
								if (!bProjectedBodyValid)
								{
									float MinDistUnder = 0.f;
									float MaxDistOver = 0.f;
									FHex ClosestFootHex;
									FHex FurthestFootHex;
									
									for (int32 OtherIdx = 0; OtherIdx < Feet.Num(); ++OtherIdx)
									{
										FFootState* OtherFoot = Feet[OtherIdx];
										FHex OtherFootHex;
										
										if (OtherIdx == FootIndex) OtherFootHex = TestHex;
										else if (OtherFoot->StepState == EFootStepState::Planted || 
											(OtherFoot->StepState == EFootStepState::Lowering && OtherFoot->StepProgress + 1 >= W.StepLowerTicks))
											OtherFootHex = OtherFoot->CurrentTile;
										else continue;
										
										const int32 fdq = OtherFootHex.Q - ProjectedBodyHex.Q;
										const int32 fdr = OtherFootHex.R - ProjectedBodyHex.R;
										const float fHexDist = (float)FMath::Max(FMath::Max(FMath::Abs(fdq), FMath::Abs(fdr)), FMath::Abs(fdq + fdr));
										
										if (W.MinimumReachDistance > 0.f && fHexDist < W.MinimumReachDistance)
										{
											float Under = W.MinimumReachDistance - fHexDist;
											if (Under > MinDistUnder) { MinDistUnder = Under; ClosestFootHex = OtherFootHex; }
										}
										
										if (W.ReachDistance > 0 && fHexDist > W.ReachDistance)
										{
											float Over = fHexDist - (float)W.ReachDistance;
											if (Over > MaxDistOver) { MaxDistOver = Over; FurthestFootHex = OtherFootHex; }
										}
									}
									
									// Attempt optimization
									bool bAttemptOptimization = false;
									FHex OptimizedBodyHex = ProjectedBodyHex;
									
									if (MaxDistOver > 0.f)
									{
										// Pull body towards furthest foot
										const int32 bdq = FurthestFootHex.Q - ProjectedBodyHex.Q;
										const int32 bdr = FurthestFootHex.R - ProjectedBodyHex.R;
										float Dist = FMath::Max(FMath::Max(FMath::Abs((float)bdq), FMath::Abs((float)bdr)), FMath::Abs((float)(bdq + bdr)));
										if (Dist > 0.1f)
										{
											const float Scale = (MaxDistOver + 0.1f) / Dist;
											int32 MoveQ = FMath::RoundToInt((float)bdq * Scale);
											int32 MoveR = FMath::RoundToInt((float)bdr * Scale);
											OptimizedBodyHex.Q += MoveQ;
											OptimizedBodyHex.R += MoveR;
											bAttemptOptimization = true;
										}
									}
									else if (MinDistUnder > 0.f)
									{
										// Push body away from closest foot
										const int32 bdq = ProjectedBodyHex.Q - ClosestFootHex.Q;
										const int32 bdr = ProjectedBodyHex.R - ClosestFootHex.R;
										float Dist = FMath::Max(FMath::Max(FMath::Abs((float)bdq), FMath::Abs((float)bdr)), FMath::Abs((float)(bdq + bdr)));
										if (Dist > 0.1f)
										{
											const float Scale = (MinDistUnder + 0.1f) / Dist;
											int32 MoveQ = FMath::RoundToInt((float)bdq * Scale);
											int32 MoveR = FMath::RoundToInt((float)bdr * Scale);
											OptimizedBodyHex.Q += MoveQ;
											OptimizedBodyHex.R += MoveR;
											bAttemptOptimization = true;
										}
									}
									
									if (bAttemptOptimization)
									{
										// Re-validate
										bool bNewValid = true;
										for (int32 OtherIdx = 0; OtherIdx < Feet.Num(); ++OtherIdx)
										{
											FFootState* OtherFoot = Feet[OtherIdx];
											FHex OtherFootHex;
											if (OtherIdx == FootIndex) OtherFootHex = TestHex;
											else if (OtherFoot->StepState == EFootStepState::Planted || 
												(OtherFoot->StepState == EFootStepState::Lowering && OtherFoot->StepProgress + 1 >= W.StepLowerTicks))
												OtherFootHex = OtherFoot->CurrentTile;
											else continue;
											
											const int32 fdq = OtherFootHex.Q - OptimizedBodyHex.Q;
											const int32 fdr = OtherFootHex.R - OptimizedBodyHex.R;
											const float fHexDist = (float)FMath::Max(FMath::Max(FMath::Abs(fdq), FMath::Abs(fdr)), FMath::Abs(fdq + fdr));
											
											if ((W.MinimumReachDistance > 0.f && fHexDist < W.MinimumReachDistance) || 
												(W.ReachDistance > 0 && fHexDist > W.ReachDistance))
											{
												bNewValid = false;
												break;
											}
										}
										
										if (bNewValid)
										{
											ProjectedBodyHex = OptimizedBodyHex;
											AvgBodyQ = (float)OptimizedBodyHex.Q;
											AvgBodyR = (float)OptimizedBodyHex.R;
											bProjectedBodyValid = true;
										}
									}
								}
								
								// Only consider this tile if projected body position is valid
								if (!bProjectedBodyValid)
								{
									continue;
								}
								
								// Calculate current body distance to target
								const int32 CurrentBodyToTargetDQ = W.CurrentTargetHex.Q - W.BaseHex.Q;
								const int32 CurrentBodyToTargetDR = W.CurrentTargetHex.R - W.BaseHex.R;
								const float CurrentBodyDistToTarget = FMath::Max(FMath::Max(FMath::Abs(float(CurrentBodyToTargetDQ)), FMath::Abs(float(CurrentBodyToTargetDR))), FMath::Abs(float(CurrentBodyToTargetDQ + CurrentBodyToTargetDR)));
								
								// Only attempt to center body on target when close enough (within reach distance)
								// This prevents legs from trying to adjust when too far away
								bool bShouldCenterOnTarget = (CurrentBodyDistToTarget <= W.ReachDistance);
								
								if (bShouldCenterOnTarget)
								{
									// Score based on how close the projected body position is to the target
									// Use float precision to avoid getting stuck when integer rounding prevents updates
									const float BodyToTargetDQ = (float)W.CurrentTargetHex.Q - AvgBodyQ;
									const float BodyToTargetDR = (float)W.CurrentTargetHex.R - AvgBodyR;
									const float BodyDistToTarget = FMath::Max(FMath::Max(FMath::Abs(BodyToTargetDQ), FMath::Abs(BodyToTargetDR)), FMath::Abs(BodyToTargetDQ + BodyToTargetDR));
									
									// Prefer positions that move the body closer to the target (or at least not farther)
									// Use a combined score: body distance to target (primary) + "Ideal Stance" bias
									// IdealQ/IdealR are pre-calculated
									
									float DistFromIdeal = FMath::Sqrt(FMath::Square(TestHex.Q - IdealQ) + FMath::Square(TestHex.R - IdealR));
									
									// Add stability bonus: penalize moving to a new tile unless it's significantly better
									const float StabilityPenalty = (TestHex == Foot.CurrentTile) ? 0.0f : 0.5f;
									
									// Add occupied penalty: prefer free tiles massively over occupied ones
									const float OccupiedPenalty = bIsOccupied ? 1000.0f : 0.0f;
									
									const float CombinedScore = BodyDistToTarget + DistFromIdeal * 0.5f + StabilityPenalty + OccupiedPenalty; 
									
									// Track best VALID tile (no occupancy) for detour fallback
									if (!bIsOccupied && CombinedScore < BestValidScore)
									{
										BestValidScore = CombinedScore;
										BestValidTile = TestHex;
									}

									// Only consider if it improves body position (or at least doesn't make it worse),
									// unless the tile is valid (not occupied) - in that case, allow slight regression to avoid obstacles.
									// This prioritizes finding a valid footing over hovering on an occupied ideal tile.
									bool bDistOk = (BodyDistToTarget <= CurrentBodyDistToTarget);
									if (!bDistOk && !bIsOccupied && BodyDistToTarget <= CurrentBodyDistToTarget + 1.5f)
									{
										bDistOk = true;
									}

									if (bDistOk && CombinedScore < BestScore)
									{
										BestScore = CombinedScore;
										BestTile = TestHex;
									}
								}
								else
								{
									// When far from target, use simpler scoring: just move toward target
									const int32 ToTargetDQ = W.CurrentTargetHex.Q - TestHex.Q;
									const int32 ToTargetDR = W.CurrentTargetHex.R - TestHex.R;
									const float DistToTarget = FMath::Max(FMath::Max(FMath::Abs(float(ToTargetDQ)), FMath::Abs(float(ToTargetDR))), FMath::Abs(float(ToTargetDQ + ToTargetDR)));
									
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
						}
					
					// Fallback: If strict search failed to find a new tile (kept BestTile as CurrentTile), 
					// but we found a valid detour (BestValidTile != CurrentTile), use the valid detour.
					// This allows the walker to move around obstacles even if it temporarily increases distance to target.
					if (BestTile == Foot.CurrentTile && BestValidTile != Foot.CurrentTile)
					{
						BestTile = BestValidTile;
						BestScore = BestValidScore;
					}

					// Handle "Frozen" raised feet: If still no valid tile found (BestTile == CurrentTile),
					// and the foot is raised/hovering (not Planted), force update to Ideal Location.
					// This ensures that if the body rotates or moves, hovering feet update their target to the new ideal
					// even if that ideal tile is occupied or invalid, preventing them from looking frozen at an old location.
					if (BestTile == Foot.CurrentTile && Foot.StepState != EFootStepState::Planted)
					{
						BestTile = IdealHex;
						// Score doesn't matter here as we are forcing the assignment
					}

					// Debug: Log search results if no tile found
					if (BestTile == Foot.CurrentTile && ValidQuadrantTiles == 0 && W.BaseHex != W.CurrentTargetHex)
					{
						// If we found NO valid tiles in our assigned quadrant, consider rotating the body.
						// This handles the case where a walker moves in a direction that aligns legs unfavorably.
						
						FHex DeltaHex(W.CurrentTargetHex.Q - W.BaseHex.Q, W.CurrentTargetHex.R - W.BaseHex.R);
						float x = 1.7320508f * (float)DeltaHex.Q + 0.8660254f * (float)DeltaHex.R;
						float y = 1.5f * (float)DeltaHex.R;
						float TargetAngle = FMath::RadiansToDegrees(FMath::Atan2(y, x));
						
						// Check if this rotation is valid for currently planted feet
						bool bRotationValid = true;
						TArray<FFootState*> PlantedFeet = GetAllFeet(W);
						
						for (int32 i = 0; i < PlantedFeet.Num(); ++i)
						{
							FFootState* PFoot = PlantedFeet[i];
							if (PFoot->StepState != EFootStepState::Planted) continue;
							
							FString PFootQuadrant;
							switch (i) {
								case 0: PFootQuadrant = TEXT("S"); break;
								case 1: PFootQuadrant = TEXT("W"); break;
								case 2: PFootQuadrant = TEXT("E"); break;
								case 3: PFootQuadrant = TEXT("N"); break;
								default: PFootQuadrant = TEXT("N"); break;
							}
							
							const FString CurrentQuadrant = GetFixedQuadrantFromHex(W.BaseHex, PFoot->CurrentTile, TargetAngle);
							if (!CurrentQuadrant.Equals(PFootQuadrant, ESearchCase::CaseSensitive))
							{
								bRotationValid = false;
								break;
							}
						}
						
						if (bRotationValid)
						{
							W.QuadrantRotationOffset = TargetAngle;
							UE_LOG(LogTemp, Verbose, TEXT("Walker %d: Forced rotation to %.1f due to blocked quadrant (Foot %d)"), WalkerIndex, TargetAngle, FootIndex);
						}
					}

					// Debug: Log search results if no tile found
					if (BestTile == Foot.CurrentTile && TestedTiles > 0)
					{
						UE_LOG(LogTemp, VeryVerbose, TEXT("Walker %d Foot %d: Staying at current tile - Tested=%d, Occupied=%d, OtherFeet=%d, ReachDist=%d"),
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
						// UE_LOG(LogTemp, Log, TEXT("Walker %d Foot %d: Raised, now moving"), WalkerIndex, FootIndex);
					}
					break;
				}
				
				case EFootStepState::Moving:
				{
					Foot.StepProgress++;
					
					// Continuous check: If we are hovering (not making progress toward a plant) or moving,
					// check if a better valid tile has become available.
					// This handles the case where a foot is waiting for a tile to clear, or moving to an occupied tile.
					
					// Only check every few ticks to avoid performance hit
					if (Foot.StepProgress % 4 == 0)
					{
						// Panic Mode: If foot has been hovering for too long (> 60 ticks), relax constraints to avoid freezing
						bool bPanicMode = (Foot.StepProgress > 60);

						// Re-evaluate best tile using similar logic to Planted state
						// We want to find if there's a valid (unoccupied) tile we can switch to
						// that is better than just hovering here.
						
						// Calculate Ideal Hex (same as in Planted state)
						// Map this foot index to its compass quadrant
						FString FootQuadrant;
						switch (FootIndex)
						{
						case 0: FootQuadrant = TEXT("S"); break; // D (down) -> South
						case 1: FootQuadrant = TEXT("W"); break; // L -> West
						case 2: FootQuadrant = TEXT("E"); break; // R -> East
						case 3: FootQuadrant = TEXT("N"); break; // U -> North
						default: FootQuadrant = TEXT("N"); break;
						}
						
						float MoveDQ = (float)(W.CurrentTargetHex.Q - W.BaseHex.Q);
						float MoveDR = (float)(W.CurrentTargetHex.R - W.BaseHex.R);
						float MoveLen = FMath::Sqrt(MoveDQ*MoveDQ + MoveDR*MoveDR + (MoveDQ+MoveDR)*(MoveDQ+MoveDR));
						if (MoveLen > 0.001f) { MoveDQ /= MoveLen; MoveDR /= MoveLen; }

						// Ideal Hex calculation (simplified from Planted state - reuse if possible or duplicate)
						float QuadDQ = 0.f, QuadDR = 0.f;
						if (FootQuadrant == TEXT("N")) { QuadDQ = 1.f; QuadDR = 0.f; }
						else if (FootQuadrant == TEXT("E")) { QuadDQ = 0.f; QuadDR = 1.f; }
						else if (FootQuadrant == TEXT("S")) { QuadDQ = -1.f; QuadDR = 0.f; }
						else if (FootQuadrant == TEXT("W")) { QuadDQ = 0.f; QuadDR = -1.f; }
						
						if (W.QuadrantRotationOffset != 0.f)
						{
							float Rad = FMath::DegreesToRadians(W.QuadrantRotationOffset);
							float C = FMath::Cos(Rad);
							float S = FMath::Sin(Rad);
							float vX = 1.7320508f * QuadDQ + 0.8660254f * QuadDR;
							float vY = 1.5f * QuadDR;
							float rotX = vX * C - vY * S;
							float rotY = vX * S + vY * C;
							QuadDQ = (rotX * 0.5773503f - rotY * 0.3333333f);
							QuadDR = rotY * 0.6666667f;
						}
						
						float IdealDist = (float)W.ReachDistance * 0.6f;
						float LeadDist = (float)W.ReachDistance * 0.4f;
						float IdealQ = (float)W.BaseHex.Q + QuadDQ * IdealDist + MoveDQ * LeadDist;
						float IdealR = (float)W.BaseHex.R + QuadDR * IdealDist + MoveDR * LeadDist;
						// FHex IdealHex(FMath::RoundToInt(IdealQ), FMath::RoundToInt(IdealR)); // Unused here directly

						// Search for a better valid tile
						FHex BestSwitchTile = Foot.TargetTile; // Default to keeping current target
						float BestSwitchScore = MAX_flt;
						bool bFoundBetter = false;
						
						// Check if current target is occupied
						bool bCurrentTargetOccupied = IsHexOccupied(this, Foot.TargetTile, HeldOrbHexes, ArmBaseHexes);
						
						// If current target is valid (unoccupied), we have a score baseline.
						// If occupied, we treat baseline as infinite (desperate to switch).
						if (!bCurrentTargetOccupied)
						{
							// Calculate score for current target
							float DistFromIdeal = FMath::Sqrt(FMath::Square(Foot.TargetTile.Q - IdealQ) + FMath::Square(Foot.TargetTile.R - IdealR));
							// Use simplified score
							BestSwitchScore = DistFromIdeal;
						}

						for (int32 q = -W.ReachDistance; q <= W.ReachDistance; ++q)
						{
							for (int32 r = -W.ReachDistance; r <= W.ReachDistance; ++r)
							{
								FHex TestHex(W.BaseHex.Q + q, W.BaseHex.R + r);
								
								// Basic validity checks (Reach, MinReach, Quadrant)
								const int32 dq = TestHex.Q - W.BaseHex.Q;
								const int32 dr = TestHex.R - W.BaseHex.R;
								const int32 HexDist = FMath::Max(FMath::Max(FMath::Abs(dq), FMath::Abs(dr)), FMath::Abs(dq + dr));
								const float HexDistFloat = (float)HexDist;
								
								if (HexDist == 0 || HexDist > W.ReachDistance || HexDistFloat < W.MinimumReachDistance) continue;
								
								const FString FixedQuadrant = GetFixedQuadrantFromHex(W.BaseHex, TestHex, W.QuadrantRotationOffset);
								// In Panic Mode, allow any quadrant to find a valid tile
								if (!bPanicMode && !FixedQuadrant.Equals(FootQuadrant, ESearchCase::CaseSensitive)) continue;
								
								// Must be unoccupied to switch to it
								if (IsHexOccupied(this, TestHex, HeldOrbHexes, ArmBaseHexes)) continue;
								
								// Must not be occupied by another foot
								bool bOtherFootHere = false;
								for (int32 OtherIdx = 0; OtherIdx < Feet.Num(); ++OtherIdx)
								{
									if (OtherIdx == FootIndex) continue;
									if (Feet[OtherIdx]->CurrentTile == TestHex)
									{
										bOtherFootHere = true;
										break;
									}
								}
								if (bOtherFootHere) continue;

								// Scoring
								float DistFromIdeal = FMath::Sqrt(FMath::Square(TestHex.Q - IdealQ) + FMath::Square(TestHex.R - IdealR));
								float Score = DistFromIdeal; // Simplified: just want closest to ideal that is valid
								
								// If current target is occupied or we are in panic mode, ANY valid tile is better/acceptable.
								// If current target is valid, new tile must be significantly better to justify switching mid-move.
								float Threshold = (bCurrentTargetOccupied || bPanicMode) ? MAX_flt : BestSwitchScore - 0.5f;
								
								if (Score < Threshold)
								{
									BestSwitchScore = Score;
									BestSwitchTile = TestHex;
									bFoundBetter = true;
									Threshold = Score; // Update threshold
								}
							}
						}
						
						if (bFoundBetter)
						{
							// Switch target!
							UE_LOG(LogTemp, Log, TEXT("Walker %d Foot %d: Switching target from (%d,%d)[%s] to (%d,%d)[Free]"), 
								WalkerIndex, FootIndex, 
								Foot.TargetTile.Q, Foot.TargetTile.R, 
								bCurrentTargetOccupied ? TEXT("Occ") : TEXT("Free"),
								BestSwitchTile.Q, BestSwitchTile.R);
								
							Foot.TargetTile = BestSwitchTile;
							// Do NOT reset step progress here.
							// Resetting it causes the foot to stay in the air indefinitely if switches happen frequently.
							// Let it continue its arc to the new target.
							// Foot.StepProgress = 0; 
						}
					}
					
					// Move toward target tile
					// For simplicity, move directly to target after ticks complete
					if (Foot.StepProgress >= W.StepMoveTicks)
					{
						// Check if the target tile is occupied before lowering
						// If occupied, stay in the raised (moving) state to "hover"
						bool bTargetOccupied = IsHexOccupied(this, Foot.TargetTile, HeldOrbHexes, ArmBaseHexes);
						
						if (bTargetOccupied)
						{
							// Hovering behavior:
							// Update CurrentTile to the target so we are logically "at" the new location (minimizing distance when it clears)
							// But stay in Moving state (don't lower/plant)
							Foot.CurrentTile = Foot.TargetTile;
							// Foot.StepProgress = W.StepMoveTicks; // REMOVED CLAMP to allow periodic re-evaluation
							
							// UE_LOG(LogTemp, Verbose, TEXT("Walker %d Foot %d: Hovering at (%d,%d) due to occupancy"), WalkerIndex, FootIndex, Foot.TargetTile.Q, Foot.TargetTile.R);
						}
						else
						{
							Foot.CurrentTile = Foot.TargetTile;
							Foot.StepState = EFootStepState::Lowering;
							Foot.StepProgress = 0;
							// UE_LOG(LogTemp, Log, TEXT("Walker %d Foot %d: Moved to (%d,%d), now lowering"), 
							// 	WalkerIndex, FootIndex, Foot.CurrentTile.Q, Foot.CurrentTile.R);
						}
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
						// UE_LOG(LogTemp, Log, TEXT("Walker %d Foot %d: Planted at (%d,%d)"), 
						// 	WalkerIndex, FootIndex, Foot.CurrentTile.Q, Foot.CurrentTile.R);
					}
					break;
				}
				}
			}
			
		}
		
		// Update body position to center of feet (average of foot positions)
		// This should always update, not just when processing requests, so legs know their range
		// Body position must respect MinimumReachDistance - no foot should be closer than MinimumReachDistance
		if (W.Stance == EWalkerStance::Standing || W.Stance == EWalkerStance::StandingUp || W.Stance == EWalkerStance::SittingDown)
		{
			// Calculate average of all feet (including raised ones)
			// For raised feet (Raising, Moving, Hovering), use their CurrentTile as their effective position for balance
			TArray<FFootState*> AllFeet = GetAllFeet(W);
			TArray<FHex> EffectiveFootHexes;
			
			for (FFootState* Foot : AllFeet)
			{
				// Use CurrentTile for all feet.
				// For planted feet, this is where they are.
				// For moving/raised feet, this is either where they started or where they are hovering.
				EffectiveFootHexes.Add(Foot->CurrentTile);
			}
			
			if (EffectiveFootHexes.Num() > 0)
			{
				// Calculate initial average body position (using RoundToInt for better centering)
				int32 SumQ = 0;
				int32 SumR = 0;
				for (const FHex& FootHex : EffectiveFootHexes)
				{
					SumQ += FootHex.Q;
					SumR += FootHex.R;
				}
				// Use RoundToInt to get the closest integer hex to the true center
				int32 AvgQ = FMath::RoundToInt((float)SumQ / EffectiveFootHexes.Num());
				int32 AvgR = FMath::RoundToInt((float)SumR / EffectiveFootHexes.Num());
				FHex ProposedBodyHex(AvgQ, AvgR);
				
				// Validate that no PLANTED foot violates MinimumReachDistance with this body position.
				// Raised feet don't constrain the body (they move with it/around it), but planted feet must remain reachable.
				bool bValidBodyPosition = true;
				for (FFootState* Foot : AllFeet)
				{
					// Only check constraints for feet that are firmly on the ground
					if (Foot->StepState == EFootStepState::Planted || 
						(Foot->StepState == EFootStepState::Lowering && Foot->StepProgress + 1 >= W.StepLowerTicks))
					{
						const FHex& FootHex = Foot->CurrentTile;
						const int32 dq = FootHex.Q - ProposedBodyHex.Q;
						const int32 dr = FootHex.R - ProposedBodyHex.R;
						const int32 HexDist = FMath::Max(FMath::Max(FMath::Abs(dq), FMath::Abs(dr)), FMath::Abs(dq + dr));
						const float HexDistFloat = (float)HexDist;
						
						// Check if foot would be too close to body
						if (HexDistFloat < W.MinimumReachDistance)
						{
							bValidBodyPosition = false;
							break;
						}
						// Check ReachDistance (body moving away from planted foot)
						if (HexDist > W.ReachDistance)
						{
							bValidBodyPosition = false;
							break;
						}
					}
				}
				
				if (bValidBodyPosition)
				{
					// Body position is valid - use it
					W.BaseHex = ProposedBodyHex;
				}
				else
				{
					// Body position would violate MinimumReachDistance/ReachDistance for planted feet.
					// Optimize to find valid position.
					// Strategy: Start at proposed (ideal) position and clamp to valid region.
					
					// This is complex on hex grid. Simple heuristic:
					// Check if current body position is valid (it should be).
					// If so, try to move one step towards proposed position?
					// Or just keep current position if proposed is invalid.
					
					// For now, fallback to keeping current position if we can't find a valid one easily.
					// In future, could implement iterative pull towards ideal center.
					
					// Try to salvage: Check if current W.BaseHex is still valid?
					// (It might not be if feet moved).
					
					// Re-verify current position against planted constraints
					bool bCurrentValid = true;
					for (FFootState* Foot : AllFeet)
					{
						if (Foot->StepState == EFootStepState::Planted || 
							(Foot->StepState == EFootStepState::Lowering && Foot->StepProgress + 1 >= W.StepLowerTicks))
						{
							const FHex& FootHex = Foot->CurrentTile;
							const int32 dq = FootHex.Q - W.BaseHex.Q;
							const int32 dr = FootHex.R - W.BaseHex.R;
							const int32 HexDist = FMath::Max(FMath::Max(FMath::Abs(dq), FMath::Abs(dr)), FMath::Abs(dq + dr));
							
							if ((float)HexDist < W.MinimumReachDistance || HexDist > W.ReachDistance)
							{
								bCurrentValid = false;
								break;
							}
						}
					}
					
					if (!bCurrentValid)
					{
						// Current is invalid, proposed is invalid.
						// Panic: Force body to proposed anyway? Or try to find nearest valid?
						// Actually, let's try the optimization logic from before but apply it to planted feet only.
						
						// ... (Optimization logic could go here, but reusing the previous logic is safest)
						// Use the original logic that pulls body towards constraints
						
						FHex OptimizedBodyHex = ProposedBodyHex;
						// (Simplified pull logic for planted feet only)
						// ...
						
						// Fallback: just use proposed if we are stuck.
						// Better to clip visually than break simulation logic.
						// But for now, log warning and keep current.
						UE_LOG(LogTemp, Verbose, TEXT("Walker %d: Body update constrained by planted feet."), WalkerIndex);
					}
				}
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
