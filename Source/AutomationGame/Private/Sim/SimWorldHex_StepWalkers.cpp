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
			W.Requests.RemoveAt(0);
			W.bProcessingRequest = true;
			UE_LOG(LogTemp, Log, TEXT("Walker %d: Started processing request to move to (%d, %d)"), 
				WalkerIndex, W.CurrentTargetHex.Q, W.CurrentTargetHex.R);
		}
		
		// If processing a request, execute walking animation
		if (W.bProcessingRequest)
		{
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

			if (W.BaseHex == W.CurrentTargetHex && bAllFeetPlanted)
			{
				// Reached target and stable, finish request
				W.bProcessingRequest = false;
				UE_LOG(LogTemp, Log, TEXT("Walker %d: Reached target (%d, %d) and stabilized"), 
					WalkerIndex, W.CurrentTargetHex.Q, W.CurrentTargetHex.R);
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
				if (Feet[i]->StepState != EFootStepState::Planted)
				{
					RaisedCount++;
					RaisedIndices.Add(i);
				}
			}

			// Determine processing order: feet nearer the edge of their reach (larger distance
			// from the body) are considered first, so legs near their quadrant boundary move earlier.
			TArray<int32> FootOrder;
			for (int32 i = 0; i < NumFeet; ++i)
			{
				FootOrder.Add(i);
			}
			FootOrder.Sort([&](int32 A, int32 B)
			{
				// Sort descending by distance from body (farther first)
				return FootBodyDist[A] > FootBodyDist[B];
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
						
						if (RaisedCount >= 3)
						{
							break; // Hard limit of 2 feet
						}
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
					
					// Find best tile for this foot within ReachDistance and its assigned quadrant.
					// Goal: move foot toward target hex while respecting the leg's quadrant.
					FHex BestTile = Foot.CurrentTile;
					float BestScore = MAX_flt;
					int32 TestedTiles = 0;
					int32 OccupiedTiles = 0;
					int32 OtherFootTiles = 0;
					
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
							
							// Score based on how well this foot position helps center the body on the target tile
							// Calculate what the body position would be if this foot were at TestHex
							// (assuming other feet stay where they are)
							int32 ProjectedBodySumQ = 0;
							int32 ProjectedBodySumR = 0;
							int32 PlantedCount = 0;
							
							for (int32 OtherIdx = 0; OtherIdx < Feet.Num(); ++OtherIdx)
							{
								if (OtherIdx == FootIndex)
								{
									// Use the candidate position for this foot
									ProjectedBodySumQ += TestHex.Q;
									ProjectedBodySumR += TestHex.R;
									PlantedCount++;
								}
								else
								{
									// Use current position for other feet (only if they're planted or about to be)
									FFootState* OtherFoot = Feet[OtherIdx];
									if (OtherFoot->StepState == EFootStepState::Planted || 
										(OtherFoot->StepState == EFootStepState::Lowering && OtherFoot->StepProgress + 1 >= W.StepLowerTicks))
									{
										ProjectedBodySumQ += OtherFoot->CurrentTile.Q;
										ProjectedBodySumR += OtherFoot->CurrentTile.R;
										PlantedCount++;
									}
								}
							}
							
							if (PlantedCount > 0)
							{
								// Calculate float average for precise scoring
								float AvgBodyQ = (float)ProjectedBodySumQ / (float)PlantedCount;
								float AvgBodyR = (float)ProjectedBodySumR / (float)PlantedCount;
								
								// Use RoundToInt for integer grid validation (matches body update logic)
								int32 PBodyQ = FMath::RoundToInt(AvgBodyQ);
								int32 PBodyR = FMath::RoundToInt(AvgBodyR);
								FHex ProjectedBodyHex(PBodyQ, PBodyR);
								
								// Validate that projected body position respects MinimumReachDistance for all planted feet
								bool bProjectedBodyValid = true;
								for (int32 OtherIdx = 0; OtherIdx < Feet.Num(); ++OtherIdx)
								{
									FFootState* OtherFoot = Feet[OtherIdx];
									FHex OtherFootHex;
									
									if (OtherIdx == FootIndex)
									{
										OtherFootHex = TestHex; // Use candidate position for this foot
									}
									else if (OtherFoot->StepState == EFootStepState::Planted || 
										(OtherFoot->StepState == EFootStepState::Lowering && OtherFoot->StepProgress + 1 >= W.StepLowerTicks))
									{
										OtherFootHex = OtherFoot->CurrentTile; // Use current position for other planted feet
									}
									else
									{
										continue; // Skip non-planted feet
									}
									
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
									// Calculate "Ideal" foot position: rest position + forward lead
									float QuadDQ = 0.f, QuadDR = 0.f;
									if (FootQuadrant == TEXT("S")) { QuadDQ = 0.f; QuadDR = 1.f; }
									else if (FootQuadrant == TEXT("W")) { QuadDQ = -1.f; QuadDR = 0.f; }
									else if (FootQuadrant == TEXT("E")) { QuadDQ = 1.f; QuadDR = 0.f; }
									else if (FootQuadrant == TEXT("N")) { QuadDQ = 0.f; QuadDR = -1.f; }
									
									float IdealDist = (float)W.ReachDistance * 0.6f;
									float LeadDist = (float)W.ReachDistance * 0.4f;
									float IdealQ = (float)W.BaseHex.Q + QuadDQ * IdealDist + MoveDQ * LeadDist;
									float IdealR = (float)W.BaseHex.R + QuadDR * IdealDist + MoveDR * LeadDist;
									
									float DistFromIdeal = FMath::Sqrt(FMath::Square(TestHex.Q - IdealQ) + FMath::Square(TestHex.R - IdealR));
									
									// Add stability bonus: penalize moving to a new tile unless it's significantly better
									const float StabilityPenalty = (TestHex == Foot.CurrentTile) ? 0.0f : 0.5f;
									
									const float CombinedScore = BodyDistToTarget + DistFromIdeal * 0.5f + StabilityPenalty; 
									
									// Only consider if it improves body position (or at least doesn't make it worse)
									if (BodyDistToTarget <= CurrentBodyDistToTarget && CombinedScore < BestScore)
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
			
		}
		
		// Update body position to center of feet (average of foot positions)
		// This should always update, not just when processing requests, so legs know their range
		// Body position must respect MinimumReachDistance - no foot should be closer than MinimumReachDistance
		if (W.Stance == EWalkerStance::Standing || W.Stance == EWalkerStance::StandingUp || W.Stance == EWalkerStance::SittingDown)
		{
			// Calculate average of all planted feet (feet that are currently at their CurrentTile)
			TArray<FFootState*> AllFeet = GetAllFeet(W);
			TArray<FHex> PlantedFootHexes;
			
			for (FFootState* Foot : AllFeet)
			{
				// Include feet that are planted, or feet that are lowering and will be planted soon
				if (Foot->StepState == EFootStepState::Planted || 
					(Foot->StepState == EFootStepState::Lowering && Foot->StepProgress + 1 >= W.StepLowerTicks))
				{
					PlantedFootHexes.Add(Foot->CurrentTile);
				}
			}
			
			if (PlantedFootHexes.Num() > 0)
			{
				// Calculate initial average body position (using RoundToInt for better centering)
				int32 SumQ = 0;
				int32 SumR = 0;
				for (const FHex& FootHex : PlantedFootHexes)
				{
					SumQ += FootHex.Q;
					SumR += FootHex.R;
				}
				// Use RoundToInt to get the closest integer hex to the true center
				int32 AvgQ = FMath::RoundToInt((float)SumQ / PlantedFootHexes.Num());
				int32 AvgR = FMath::RoundToInt((float)SumR / PlantedFootHexes.Num());
				FHex ProposedBodyHex(AvgQ, AvgR);
				
				// Validate that no foot violates MinimumReachDistance with this body position
				bool bValidBodyPosition = true;
				for (const FHex& FootHex : PlantedFootHexes)
				{
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
				}
				
				if (bValidBodyPosition)
				{
					// Body position is valid - use it
					W.BaseHex = ProposedBodyHex;
				}
				else
				{
					// Body position would violate MinimumReachDistance - optimize to find valid position
					// Strategy: Try to move body away from feet that are too close
					// Find the foot that's closest to the proposed body position
					float MinDist = MAX_flt;
					FHex ClosestFootHex;
					for (const FHex& FootHex : PlantedFootHexes)
					{
						const int32 dq = FootHex.Q - ProposedBodyHex.Q;
						const int32 dr = FootHex.R - ProposedBodyHex.R;
						const int32 HexDist = FMath::Max(FMath::Max(FMath::Abs(dq), FMath::Abs(dr)), FMath::Abs(dq + dr));
						const float HexDistFloat = (float)HexDist;
						
						if (HexDistFloat < MinDist)
						{
							MinDist = HexDistFloat;
							ClosestFootHex = FootHex;
						}
					}
					
					// If closest foot is too close, move body away from it
					if (MinDist < W.MinimumReachDistance)
					{
						// Calculate direction from closest foot to proposed body
						const int32 dq = ProposedBodyHex.Q - ClosestFootHex.Q;
						const int32 dr = ProposedBodyHex.R - ClosestFootHex.R;
						
						// Normalize direction (approximate)
						float Dist = FMath::Max(FMath::Max(FMath::Abs((float)dq), FMath::Abs((float)dr)), FMath::Abs((float)(dq + dr)));
						if (Dist > 0.1f)
						{
							// Move body away from closest foot by at least MinimumReachDistance
							const float RequiredDist = W.MinimumReachDistance + 0.1f; // Add small buffer
							const float Scale = RequiredDist / Dist;
							
							// Calculate new body position
							int32 NewQ = ClosestFootHex.Q + FMath::RoundToInt((float)dq * Scale);
							int32 NewR = ClosestFootHex.R + FMath::RoundToInt((float)dr * Scale);
							
							// Validate new position doesn't violate constraints for other feet
							FHex NewBodyHex(NewQ, NewR);
							bool bNewPositionValid = true;
							for (const FHex& FootHex : PlantedFootHexes)
							{
								const int32 fdq = FootHex.Q - NewBodyHex.Q;
								const int32 fdr = FootHex.R - NewBodyHex.R;
								const int32 fHexDist = FMath::Max(FMath::Max(FMath::Abs(fdq), FMath::Abs(fdr)), FMath::Abs(fdq + fdr));
								const float fHexDistFloat = (float)fHexDist;
								
								// Check MinimumReachDistance
								if (fHexDistFloat < W.MinimumReachDistance)
								{
									bNewPositionValid = false;
									break;
								}
								
								// Check ReachDistance
								if (fHexDist > W.ReachDistance)
								{
									bNewPositionValid = false;
									break;
								}
							}
							
							if (bNewPositionValid)
							{
								W.BaseHex = NewBodyHex;
							}
							else
							{
								// Fallback: keep current body position if we can't find a valid one
								// This prevents the walker from getting stuck
								UE_LOG(LogTemp, Warning, TEXT("Walker %d: Could not find valid body position respecting MinimumReachDistance=%.1f, keeping current position"), 
									WalkerIndex, W.MinimumReachDistance);
							}
						}
					}
					else
					{
						// Shouldn't happen, but use proposed position
						W.BaseHex = ProposedBodyHex;
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
