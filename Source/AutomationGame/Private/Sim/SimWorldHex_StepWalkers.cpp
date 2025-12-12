#include "Sim/SimWorldHex.h"
#include "Sim/HexTypes.h"
#include "Sim/WalkerTypes.h"
#include "Sim/HexCoord.h"
#include "Algo/Sort.h"
#include "Actors/RequestBeacon.h"
#include "EngineUtils.h"

// Helper to check if a hex is occupied by obstacles
// HoldingFootHexes: Hexes occupied by feet holding items (should NOT be treated as obstacles)
static bool IsHexOccupied(const USimWorldHex* Sim, const FHex& H, const TSet<FHex>& HeldOrbHexes, const TSet<FHex>& ArmBaseHexes, const TSet<FHex>& HoldingFootHexes = TSet<FHex>())
{
	// Check for items (orbs on ground or belts)
	if (Sim->ItemMap.Contains(H))
	{
		return true;
	}
	
	// Check for held orbs (by arms)
	if (HeldOrbHexes.Contains(H))
	{
		return true;
	}
	
	// Check for arms
	if (ArmBaseHexes.Contains(H))
	{
		return true;
	}
	
	// Holding feet are NOT obstacles (they're raised and carrying items to be placed elsewhere)
	if (HoldingFootHexes.Contains(H))
	{
		return false;
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

// Helper: Get the center hex of a quadrant for a given foot
static FHex GetQuadrantCenterHex(const FHex& BodyHex, int32 FootIndex, int32 ReachDistance, float RotationOffsetDeg, const USimWorldHex* Sim)
{
	if (!Sim) return BodyHex;
	
	// Determine quadrant direction based on FootIndex
	// D=0 (South), L=1 (West), R=2 (East), U=3 (North)
	float QuadDQ = 0.f, QuadDR = 0.f;
	switch (FootIndex)
	{
	case 0: QuadDQ = -1.f; QuadDR = 0.f; break; // D/South (Q-)
	case 1: QuadDQ = 0.f; QuadDR = -1.f; break; // L/West (R-)
	case 2: QuadDQ = 0.f; QuadDR = 1.f; break;  // R/East (R+)
	case 3: QuadDQ = 1.f; QuadDR = 0.f; break;  // U/North (Q+)
	}
	
	// Apply rotation offset if any
	if (RotationOffsetDeg != 0.f)
	{
		float Rad = FMath::DegreesToRadians(RotationOffsetDeg);
		float C = FMath::Cos(Rad);
		float S = FMath::Sin(Rad);
		// Hex to Cartesian for the vector
		float vX = 1.7320508f * QuadDQ + 0.8660254f * QuadDR;
		float vY = 1.5f * QuadDR;
		// Rotate
		float rotX = vX * C - vY * S;
		float rotY = vX * S + vY * C;
		// Cartesian back to Hex
		QuadDQ = (rotX * 0.5773503f - rotY * 0.3333333f);
		QuadDR = rotY * 0.6666667f;
	}
	
	// Position at center of quadrant: use 60% of ReachDistance for a balanced position
	float CenterDistance = (float)ReachDistance * 0.6f;
	float IdealQ = (float)BodyHex.Q + QuadDQ * CenterDistance;
	float IdealR = (float)BodyHex.R + QuadDR * CenterDistance;
	
	return FHex(FMath::RoundToInt(IdealQ), FMath::RoundToInt(IdealR));
}

void USimWorldHex::StepWalkers()
{
	// Use cached occupancy sets (computed once per tick in StepSim after StepArms)
	const TSet<FHex>& HeldOrbHexes = CachedHeldOrbHexes;
	const TSet<FHex>& ArmBaseHexes = CachedArmBaseHexes;
	
	// Build set of hexes occupied by feet holding items (these should NOT be obstacles)
	TSet<FHex> HoldingFootHexes;
	for (const FWalkerState& W : Walkers)
	{
		if (W.DFoot.bHoldingItem) HoldingFootHexes.Add(W.DFoot.CurrentTile);
		if (W.LFoot.bHoldingItem) HoldingFootHexes.Add(W.LFoot.CurrentTile);
		if (W.RFoot.bHoldingItem) HoldingFootHexes.Add(W.RFoot.CurrentTile);
		if (W.UFoot.bHoldingItem) HoldingFootHexes.Add(W.UFoot.CurrentTile);
	}
	
	// PERFORMANCE: Cache all beacons at start of StepWalkers to avoid O(N²) TActorIterator calls in inner loops
	TMap<FHex, ARequestBeacon*> BeaconCache;
	TArray<ARequestBeacon*> AllBeacons;
	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<ARequestBeacon> It(World); It; ++It)
		{
			ARequestBeacon* Beacon = *It;
			if (Beacon)
			{
				BeaconCache.Add(FHex(Beacon->Q, Beacon->R), Beacon);
				AllBeacons.Add(Beacon);
			}
		}
	}
	
	// Check for Ready PickUp beacons and assign to idle walkers (using cached beacons)
	for (ARequestBeacon* Beacon : AllBeacons)
	{
		if (Beacon->BeaconRole == EBeaconRole::PickUp && Beacon->BeaconState == EBeaconState::Ready)
		{
			// Find nearest idle walker (no requests, standing or sitting, not holding item)
			FHex BeaconHex(Beacon->Q, Beacon->R);
			int32 BestWalkerIdx = -1;
			int32 BestDistance = MAX_int32;
			
			for (int32 WIdx = 0; WIdx < Walkers.Num(); ++WIdx)
			{
				FWalkerState& W = Walkers[WIdx];
				
				// Skip if walker has requests or processing a request
				if (W.Requests.Num() > 0 || W.bProcessingRequest)
					continue;
				
				// Allow Sitting or Standing walkers (Sitting will stand up when they get a request)
				if (W.Stance != EWalkerStance::Standing && W.Stance != EWalkerStance::Sitting)
					continue;
				
				// Check if any foot is holding an item
				bool bAnyFootHolding = W.DFoot.bHoldingItem || W.LFoot.bHoldingItem || 
									   W.RFoot.bHoldingItem || W.UFoot.bHoldingItem;
				if (bAnyFootHolding)
					continue;
				
				// Calculate distance to beacon
				int32 dq = BeaconHex.Q - W.BaseHex.Q;
				int32 dr = BeaconHex.R - W.BaseHex.R;
				int32 dist = FMath::Max(FMath::Max(FMath::Abs(dq), FMath::Abs(dr)), FMath::Abs(dq + dr));
				
				if (dist < BestDistance)
				{
					BestDistance = dist;
					BestWalkerIdx = WIdx;
				}
			}
			
			// Assign pickup request to best walker
			if (BestWalkerIdx >= 0)
			{
				FWalkerRequest PickUpReq;
				PickUpReq.TargetHex = BeaconHex;
				PickUpReq.Priority = 1000; // High priority
				Walkers[BestWalkerIdx].Requests.Add(PickUpReq);
				
				// Update beacon state to InProgress to prevent re-assignment
				Beacon->SetState(EBeaconState::InProgress);
				
				UE_LOG(LogTemp, Log, TEXT("Assigned PickUp beacon at (%d,%d) to Walker %d (distance: %d, stance: %d)"),
					BeaconHex.Q, BeaconHex.R, BestWalkerIdx, BestDistance, (int32)Walkers[BestWalkerIdx].Stance);
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
			// Don't sit down if any foot is holding an item
			bool bAnyFootHolding = W.DFoot.bHoldingItem || W.LFoot.bHoldingItem || 
								   W.RFoot.bHoldingItem || W.UFoot.bHoldingItem;
			if (!bAnyFootHolding)
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
		
		// Flag to indicate we're in pivot mode - skip normal foot movement
		bool bIsPivoting = false;
		
		// If processing a request, execute walking animation
		if (W.bProcessingRequest)
		{
			// Check if any foot is holding an item and if we're targeting a drop-off
			bool bHasHoldingFoot = false;
			int32 HoldingFootIndex = -1;
			FFootState* HoldingFoot = nullptr;
			TArray<FFootState*> AllFeet = GetAllFeet(W);
			for (int32 idx = 0; idx < AllFeet.Num(); ++idx)
			{
				if (AllFeet[idx]->bHoldingItem)
				{
					bHasHoldingFoot = true;
					HoldingFootIndex = idx;
					HoldingFoot = AllFeet[idx];
					break;
				}
			}
			
			// If we have a holding foot, find the actual DropOff beacon (not the offset target)
			if (bHasHoldingFoot && HoldingFoot)
			{
				// Find the actual DropOff beacon by looking for one with a paired PickUp that's InProgress
				FHex ActualDropOffHex;
				bool bFoundDropOff = false;
				for (ARequestBeacon* Beacon : AllBeacons)
				{
					if (Beacon->BeaconRole == EBeaconRole::DropOff && 
						Beacon->PairedBeacon && 
						Beacon->PairedBeacon->BeaconState == EBeaconState::InProgress)
					{
						ActualDropOffHex = FHex(Beacon->Q, Beacon->R);
						bFoundDropOff = true;
						break;
					}
				}
				
				if (bFoundDropOff)
				{
					// Calculate if the drop-off tile is within reach of the walker
					const int32 BodyToDropOffDQ = ActualDropOffHex.Q - W.BaseHex.Q;
					const int32 BodyToDropOffDR = ActualDropOffHex.R - W.BaseHex.R;
					const int32 BodyToDropOffDist = FMath::Max(FMath::Max(FMath::Abs(BodyToDropOffDQ), FMath::Abs(BodyToDropOffDR)), 
						FMath::Abs(BodyToDropOffDQ + BodyToDropOffDR));
					
					// If drop-off is within reach, check if holding foot can reach it from current quadrant
					if (BodyToDropOffDist <= W.ReachDistance)
					{
						// Determine which quadrant the drop-off is in
						const FString DropOffQuadrant = GetFixedQuadrantFromHex(W.BaseHex, ActualDropOffHex, W.QuadrantRotationOffset);
						
						// Determine which quadrant the holding foot is assigned to
						FString HoldingFootQuadrant;
						switch (HoldingFootIndex)
						{
						case 0: HoldingFootQuadrant = TEXT("S"); break; // D (down) -> South
						case 1: HoldingFootQuadrant = TEXT("W"); break; // L -> West
						case 2: HoldingFootQuadrant = TEXT("E"); break; // R -> East
						case 3: HoldingFootQuadrant = TEXT("N"); break; // U -> North
						default: HoldingFootQuadrant = TEXT("N"); break;
						}
						
						// If quadrants don't match, we need to shuffle feet to get the holding foot to the drop-off quadrant
						if (!DropOffQuadrant.Equals(HoldingFootQuadrant, ESearchCase::CaseSensitive))
						{
							// Set pivot flag to prevent normal walking from interfering
							bIsPivoting = true;
							
							UE_LOG(LogTemp, Log, TEXT("Walker %d: PIVOT NEEDED - Holding foot %d (%s) can't reach drop-off in %s quadrant. Shuffling non-holding feet..."),
								WalkerIndex, HoldingFootIndex, *HoldingFootQuadrant, *DropOffQuadrant);
							
							// Update holding foot's position to stay centered in its quadrant
							// Calculate actual body rotation from planted feet (same as BasicWalker.cpp does visually)
							{
								// Calculate body rotation from planted non-holding feet
								float TotalWeightedSin = 0.f;
								float TotalWeightedCos = 0.f;
								float TotalWeight = 0.f;
								
								for (int32 idx = 0; idx < AllFeet.Num(); ++idx)
								{
									if (idx == HoldingFootIndex) continue; // Skip holding foot
									FFootState* Foot = AllFeet[idx];
									if (Foot->StepState != EFootStepState::Planted) continue;
									
									// Calculate angle from body to this foot
									float DQ = (float)(Foot->CurrentTile.Q - W.BaseHex.Q);
									float DR = (float)(Foot->CurrentTile.R - W.BaseHex.R);
									float X = 1.7320508f * DQ + 0.8660254f * DR;
									float Y = 1.5f * DR;
									float Angle = FMath::Atan2(Y, X);
									
									float Weight = 1.0f;
									TotalWeightedSin += FMath::Sin(Angle) * Weight;
									TotalWeightedCos += FMath::Cos(Angle) * Weight;
									TotalWeight += Weight;
								}
								
								float ActualBodyRotation = 0.f;
								if (TotalWeight > KINDA_SMALL_NUMBER)
								{
									ActualBodyRotation = FMath::RadiansToDegrees(FMath::Atan2(TotalWeightedSin, TotalWeightedCos));
								}
								
								// Update sim's stored rotation offset to match actual body rotation
								W.QuadrantRotationOffset = ActualBodyRotation;
								
								// Now calculate holding foot's ideal position based on actual body rotation
								float QuadDQ = 0.f, QuadDR = 0.f;
								if (HoldingFootQuadrant == TEXT("N")) { QuadDQ = 1.f; QuadDR = 0.f; }
								else if (HoldingFootQuadrant == TEXT("E")) { QuadDQ = 0.f; QuadDR = 1.f; }
								else if (HoldingFootQuadrant == TEXT("S")) { QuadDQ = -1.f; QuadDR = 0.f; }
								else if (HoldingFootQuadrant == TEXT("W")) { QuadDQ = 0.f; QuadDR = -1.f; }
								
								// Apply body rotation
								if (ActualBodyRotation != 0.f)
								{
									float Rad = FMath::DegreesToRadians(ActualBodyRotation);
									float C = FMath::Cos(Rad);
									float S = FMath::Sin(Rad);
									float vX = 1.7320508f * QuadDQ + 0.8660254f * QuadDR;
									float vY = 1.5f * QuadDR;
									float rotX = vX * C - vY * S;
									float rotY = vX * S + vY * C;
									QuadDQ = (rotX * 0.5773503f - rotY * 0.3333333f);
									QuadDR = rotY * 0.6666667f;
								}
								
								// Calculate ideal position at comfortable distance (60% of reach)
								float IdealDist = (float)W.ReachDistance * 0.6f;
								float IdealQ = (float)W.BaseHex.Q + QuadDQ * IdealDist;
								float IdealR = (float)W.BaseHex.R + QuadDR * IdealDist;
								FHex IdealHex(FMath::RoundToInt(IdealQ), FMath::RoundToInt(IdealR));
								
								// Verify the ideal position is in the holding foot's quadrant
								const FString IdealQuad = GetFixedQuadrantFromHex(W.BaseHex, IdealHex, ActualBodyRotation);
								if (IdealQuad.Equals(HoldingFootQuadrant, ESearchCase::CaseSensitive) && IdealHex != HoldingFoot->CurrentTile)
								{
									// Update holding foot's logical position to quadrant center
									HoldingFoot->CurrentTile = IdealHex;
									HoldingFoot->TargetTile = IdealHex;
								}
							}
							
							// The approach: Move other feet to positions that would allow the walker body to rotate
							// such that the holding foot's quadrant aligns with the drop-off tile
							// This is "shuffling" - feet stay in their assigned quadrants but reposition to enable rotation
							
							// Calculate the ideal rotation angle that would align the holding foot with drop-off
							float DropOffAngle = 0.f;
							if (BodyToDropOffDQ != 0 || BodyToDropOffDR != 0)
							{
								float x = 1.7320508f * (float)BodyToDropOffDQ + 0.8660254f * (float)BodyToDropOffDR;
								float y = 1.5f * (float)BodyToDropOffDR;
								DropOffAngle = FMath::RadiansToDegrees(FMath::Atan2(y, x));
							}
							
							// Map holding foot quadrant to its base angle
							float HoldingFootBaseAngle = 0.f;
							if (HoldingFootQuadrant == TEXT("N")) HoldingFootBaseAngle = 0.f;
							else if (HoldingFootQuadrant == TEXT("E")) HoldingFootBaseAngle = 90.f;
							else if (HoldingFootQuadrant == TEXT("S")) HoldingFootBaseAngle = 180.f;
							else if (HoldingFootQuadrant == TEXT("W")) HoldingFootBaseAngle = 270.f;
							
							// The desired rotation would align holding foot quadrant with drop-off direction
							float DesiredRotation = DropOffAngle - HoldingFootBaseAngle;
							DesiredRotation = FRotator::NormalizeAxis(DesiredRotation);
							
							// Instead of directly setting QuadrantRotationOffset (which would violate quadrant assignments),
							// we encourage other feet to move within their quadrants to positions that would
							// naturally cause the body to rotate toward the desired orientation
							
							// PIVOT STRATEGY: Move feet to BOUNDARY tiles of their quadrant
							// Boundary tiles = tiles with a neighbor in the NEXT quadrant (in rotation direction)
							// These have maximum rotational influence
							
							// Quadrant angles: N=0°, E=90°, S=180°, W=270°
							// In math convention, increasing angles = CCW
							// So N→E→S→W (0→90→180→270) is actually CCW in angle terms
							// 
							// To rotate body so holding foot reaches drop-off:
							// - DesiredRotation > 0 means we need to add angle to body (CCW in math)
							// - DesiredRotation < 0 means we need to subtract angle (CW in math)
							//
							// For CCW body rotation (positive), holding foot's quadrant label moves "forward" in N→E→S→W order
							// So non-holding feet should step toward the NEXT quadrant in that order to pull the body
							
							auto GetNextQuadrantPositive = [](const FString& Quad) -> FString {
								// Next in N→E→S→W order (positive angle direction)
								if (Quad == TEXT("N")) return TEXT("E");
								if (Quad == TEXT("E")) return TEXT("S");
								if (Quad == TEXT("S")) return TEXT("W");
								if (Quad == TEXT("W")) return TEXT("N");
								return Quad;
							};
							
							auto GetNextQuadrantNegative = [](const FString& Quad) -> FString {
								// Previous in N→E→S→W order (negative angle direction)
								if (Quad == TEXT("N")) return TEXT("W");
								if (Quad == TEXT("W")) return TEXT("S");
								if (Quad == TEXT("S")) return TEXT("E");
								if (Quad == TEXT("E")) return TEXT("N");
								return Quad;
							};
							
							// Only move one foot per tick to allow rotation to accumulate
							int32 CurrentRaisedCount = 0;
							for (FFootState* CheckFoot : AllFeet)
							{
								if (CheckFoot->StepState != EFootStepState::Planted)
									CurrentRaisedCount++;
							}
							
							// Only shuffle if all feet are planted (one at a time)
							// BUT: holding foot should be considered "raised" for movement purposes even if technically planted
							// So we only count non-holding feet as needing to be planted
							int32 NonHoldingRaisedCount = 0;
							for (int32 idx = 0; idx < AllFeet.Num(); ++idx)
							{
								if (idx == HoldingFootIndex) continue; // Skip holding foot
								if (AllFeet[idx]->StepState != EFootStepState::Planted)
									NonHoldingRaisedCount++;
							}
							
							if (NonHoldingRaisedCount == 0)
							{
								// Positive DesiredRotation = need CCW body rotation = feet go to positive direction boundary
								// Negative DesiredRotation = need CW body rotation = feet go to negative direction boundary
								bool bPositiveRotation = (DesiredRotation > 0);
								
								// Find the best foot to shuffle
								int32 BestFootIndex = -1;
								FHex BestTarget;
								int32 BestBoundaryNeighbors = -1; // More boundary neighbors = better position
								
								for (int32 i = 0; i < AllFeet.Num(); ++i)
								{
									if (i == HoldingFootIndex) continue; // Skip the holding foot
									
									FFootState* Foot = AllFeet[i];
									if (Foot->StepState != EFootStepState::Planted) continue;
									
									// Get this foot's assigned quadrant
									FString FootQuadrant;
									switch (i)
									{
									case 0: FootQuadrant = TEXT("S"); break;
									case 1: FootQuadrant = TEXT("W"); break;
									case 2: FootQuadrant = TEXT("E"); break;
									case 3: FootQuadrant = TEXT("N"); break;
									}
									
									// What quadrant are we trying to be adjacent to?
									FString TargetBoundaryQuad = bPositiveRotation ? GetNextQuadrantPositive(FootQuadrant) : GetNextQuadrantNegative(FootQuadrant);
									
									// Search all tiles in this foot's quadrant
									// Find tiles that border the target quadrant (have neighbors in it)
									FHex FootBestTarget = Foot->CurrentTile;
									int32 FootBestBoundaryCount = 0;
									
									// First check if current position is already optimal
									int32 CurrentBoundaryCount = 0;
									for (uint8 dir = 0; dir < 6; ++dir)
									{
										FHex Neighbor = HexUtil::Neighbor(Foot->CurrentTile, static_cast<EHexDir>(dir));
										FString NeighborQuad = GetFixedQuadrantFromHex(W.BaseHex, Neighbor, W.QuadrantRotationOffset);
										if (NeighborQuad.Equals(TargetBoundaryQuad, ESearchCase::CaseSensitive))
										{
											CurrentBoundaryCount++;
										}
									}
									
									for (int32 dq = -W.ReachDistance; dq <= W.ReachDistance; ++dq)
									{
										for (int32 dr = -W.ReachDistance; dr <= W.ReachDistance; ++dr)
										{
											if (dq == 0 && dr == 0) continue;
											
											FHex Candidate(W.BaseHex.Q + dq, W.BaseHex.R + dr);
											
											// Check hex distance is within reach
											int32 Dist = FMath::Max(FMath::Max(FMath::Abs(dq), FMath::Abs(dr)),
												FMath::Abs(dq + dr));
											if (Dist < W.MinimumReachDistance || Dist > W.ReachDistance) continue;
											
											// Check it's in the correct quadrant
											const FString CandidateQuad = GetFixedQuadrantFromHex(W.BaseHex, Candidate, W.QuadrantRotationOffset);
											if (!CandidateQuad.Equals(FootQuadrant, ESearchCase::CaseSensitive)) continue;
											
											// Check if tile is occupied - skip occupied tiles
											if (IsHexOccupied(this, Candidate, CachedHeldOrbHexes, CachedArmBaseHexes, HoldingFootHexes)) continue;
											
											// Check if another foot is at this tile
											bool bOtherFootHere = false;
											for (int32 OtherIdx = 0; OtherIdx < AllFeet.Num(); ++OtherIdx)
											{
												if (OtherIdx == i) continue;
												if (AllFeet[OtherIdx]->CurrentTile == Candidate || AllFeet[OtherIdx]->TargetTile == Candidate)
												{
													bOtherFootHere = true;
													break;
												}
											}
											if (bOtherFootHere) continue;
											
											// Count how many neighbors are in the target boundary quadrant
											int32 BoundaryCount = 0;
											for (uint8 dir = 0; dir < 6; ++dir)
											{
												FHex Neighbor = HexUtil::Neighbor(Candidate, static_cast<EHexDir>(dir));
												FString NeighborQuad = GetFixedQuadrantFromHex(W.BaseHex, Neighbor, W.QuadrantRotationOffset);
												if (NeighborQuad.Equals(TargetBoundaryQuad, ESearchCase::CaseSensitive))
												{
													BoundaryCount++;
												}
											}
											
											// Prefer tiles with more boundary neighbors
											if (BoundaryCount > FootBestBoundaryCount)
											{
												FootBestBoundaryCount = BoundaryCount;
												FootBestTarget = Candidate;
											}
										}
									}
									
								// Only move if we can improve our boundary position
								if (FootBestTarget != Foot->CurrentTile && FootBestBoundaryCount > CurrentBoundaryCount)
								{
									// This foot can improve - check if it's the best option
									if (FootBestBoundaryCount > BestBoundaryNeighbors)
									{
										BestBoundaryNeighbors = FootBestBoundaryCount;
										BestFootIndex = i;
										BestTarget = FootBestTarget;
									}
								}
								}
								
								// Move the best foot
								if (BestFootIndex >= 0)
								{
									FFootState* Foot = AllFeet[BestFootIndex];
									FString FootQuad;
									switch (BestFootIndex)
									{
									case 0: FootQuad = TEXT("S"); break;
									case 1: FootQuad = TEXT("W"); break;
									case 2: FootQuad = TEXT("E"); break;
									case 3: FootQuad = TEXT("N"); break;
									}
									FString TargetBoundary = bPositiveRotation ? GetNextQuadrantPositive(FootQuad) : GetNextQuadrantNegative(FootQuad);
									
									Foot->TargetTile = BestTarget;
									Foot->StepState = EFootStepState::Raising;
									Foot->StepProgress = 0;
									UE_LOG(LogTemp, Log, TEXT("Walker %d: PIVOT STEP - Foot %d (%s) from (%d,%d) to (%d,%d), %d neighbors in %s"),
										WalkerIndex, BestFootIndex, *FootQuad,
										Foot->CurrentTile.Q, Foot->CurrentTile.R,
										BestTarget.Q, BestTarget.R,
										BestBoundaryNeighbors, *TargetBoundary);
								}
								else
								{
									// No foot can improve boundary position - all are at optimal spots
									// The body rotation should have updated; if drop-off still not in
									// holding foot's quadrant, we may need to wait for next tick
									UE_LOG(LogTemp, Log, TEXT("Walker %d: PIVOT - All feet at boundary, waiting for rotation update (DesiredRot=%.1f)"),
										WalkerIndex, DesiredRotation);
								}
							}
							else
							{
								// Some non-holding feet are not planted yet
								// Check if any are stuck in Moving for too long (> 30 ticks)
								for (int32 idx = 0; idx < AllFeet.Num(); ++idx)
								{
									if (idx == HoldingFootIndex) continue;
									FFootState* StuckFoot = AllFeet[idx];
									
									if (StuckFoot->StepState == EFootStepState::Moving && StuckFoot->StepProgress > 30)
									{
										// Foot is stuck hovering - find an alternative tile in its quadrant
										FString StuckQuadrant;
										switch (idx)
										{
										case 0: StuckQuadrant = TEXT("S"); break;
										case 1: StuckQuadrant = TEXT("W"); break;
										case 2: StuckQuadrant = TEXT("E"); break;
										case 3: StuckQuadrant = TEXT("N"); break;
										}
										
										// Find nearest unoccupied tile in quadrant
										FHex BestAlt = StuckFoot->TargetTile;
										float BestAltDist = MAX_flt;
										
										for (int32 dq = -W.ReachDistance; dq <= W.ReachDistance; ++dq)
										{
											for (int32 dr = -W.ReachDistance; dr <= W.ReachDistance; ++dr)
											{
												if (dq == 0 && dr == 0) continue;
												FHex TestHex(W.BaseHex.Q + dq, W.BaseHex.R + dr);
												
												int32 Dist = FMath::Max(FMath::Max(FMath::Abs(dq), FMath::Abs(dr)), FMath::Abs(dq + dr));
												if (Dist < W.MinimumReachDistance || Dist > W.ReachDistance) continue;
												
												const FString TestQuad = GetFixedQuadrantFromHex(W.BaseHex, TestHex, W.QuadrantRotationOffset);
												if (!TestQuad.Equals(StuckQuadrant, ESearchCase::CaseSensitive)) continue;
												
												if (IsHexOccupied(this, TestHex, CachedHeldOrbHexes, CachedArmBaseHexes, HoldingFootHexes)) continue;
												
												// Check other feet
												bool bOtherFootHere = false;
												for (int32 OtherIdx = 0; OtherIdx < AllFeet.Num(); ++OtherIdx)
												{
													if (OtherIdx == idx) continue;
													if (AllFeet[OtherIdx]->CurrentTile == TestHex || AllFeet[OtherIdx]->TargetTile == TestHex)
													{
														bOtherFootHere = true;
														break;
													}
												}
												if (bOtherFootHere) continue;
												
												// Distance from current target (prefer nearby tiles)
												float AltDist = FMath::Abs(TestHex.Q - StuckFoot->TargetTile.Q) + FMath::Abs(TestHex.R - StuckFoot->TargetTile.R);
												if (AltDist < BestAltDist)
												{
													BestAltDist = AltDist;
													BestAlt = TestHex;
												}
											}
										}
										
										if (BestAlt != StuckFoot->TargetTile)
										{
											UE_LOG(LogTemp, Log, TEXT("Walker %d: PIVOT - Unsticking Foot %d from (%d,%d) to (%d,%d)"),
												WalkerIndex, idx, StuckFoot->TargetTile.Q, StuckFoot->TargetTile.R, BestAlt.Q, BestAlt.R);
											StuckFoot->TargetTile = BestAlt;
											StuckFoot->StepProgress = 0; // Reset progress to give it time to reach new target
										}
									}
								}
							}
						}
						else
						{
							// Quadrants match! Holding foot can reach the drop-off
							// Tell the holding foot to target the drop-off hex
							if (HoldingFoot->TargetTile != ActualDropOffHex)
							{
								HoldingFoot->TargetTile = ActualDropOffHex;
								UE_LOG(LogTemp, Log, TEXT("Walker %d: Quadrants aligned! Holding foot %d targeting drop-off at (%d,%d)"),
									WalkerIndex, HoldingFootIndex, ActualDropOffHex.Q, ActualDropOffHex.R);
							}
						}
					}
				}
			}
			
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
			// If targeting a PickUp beacon, we must pick up the item (don't complete just by being near)
			// Otherwise, being within ReachDistance of the target is sufficient.
			const bool bIsReturningHome = (W.CurrentTargetHex == W.HomeHex);
			
			// Check if target is a PickUp beacon
			bool bIsPickUpTarget = false;
			// Check if target is a DropOff beacon
			bool bIsDropOffTarget = false;
			
			// Check active request first (in case CurrentTargetHex was offset)
			if (W.Requests.Num() > 0)
			{
				FHex RequestHex = W.Requests[0].TargetHex;
				if (ARequestBeacon** FoundBeacon = BeaconCache.Find(RequestHex))
				{
					ARequestBeacon* Beacon = *FoundBeacon;
					if (Beacon->BeaconRole == EBeaconRole::PickUp && Beacon->BeaconState == EBeaconState::InProgress)
					{
						bIsPickUpTarget = true;
					}
					else if (Beacon->BeaconRole == EBeaconRole::DropOff)
					{
						bIsDropOffTarget = true;
					}
				}
			}
			
			// If not found via request, check CurrentTargetHex
			if (!bIsPickUpTarget && !bIsDropOffTarget)
			{
				if (ARequestBeacon** FoundBeacon = BeaconCache.Find(W.CurrentTargetHex))
				{
					ARequestBeacon* Beacon = *FoundBeacon;
					if (Beacon->BeaconRole == EBeaconRole::PickUp && Beacon->BeaconState == EBeaconState::InProgress)
					{
						bIsPickUpTarget = true;
					}
					else if (Beacon->BeaconRole == EBeaconRole::DropOff)
					{
						bIsDropOffTarget = true;
					}
				}
			}
			
			// Implicit DropOff check: if we are holding an item, we should be treating any target 
			// near a DropOff beacon as a DropOff target.
			if (!bIsDropOffTarget && (W.DFoot.bHoldingItem || W.LFoot.bHoldingItem || W.RFoot.bHoldingItem || W.UFoot.bHoldingItem))
			{
				// Iterate over all cached beacons instead of TActorIterator
				for (ARequestBeacon* Beacon : AllBeacons)
				{
					if (Beacon->BeaconRole == EBeaconRole::DropOff)
					{
						// Check distance from current target (offset) to beacon
						const int32 dq = Beacon->Q - W.CurrentTargetHex.Q;
						const int32 dr = Beacon->R - W.CurrentTargetHex.R;
						const int32 dist = FMath::Max(FMath::Max(FMath::Abs(dq), FMath::Abs(dr)), FMath::Abs(dq + dr));
						
						// If target is within reach distance of a DropOff beacon, treat it as a DropOff mission
						if (dist <= W.ReachDistance + 2)
						{
							bIsDropOffTarget = true;
							UE_LOG(LogTemp, Verbose, TEXT("Walker %d: Implicitly detected DropOff target near (%d,%d) due to holding item"), 
								WalkerIndex, W.CurrentTargetHex.Q, W.CurrentTargetHex.R);
							break;
						}
					}
				}
			}
			
			bool bTargetReached = false;

			if (bIsReturningHome)
			{
				bTargetReached = (W.BaseHex == W.CurrentTargetHex);
			}
			else if (bIsPickUpTarget)
			{
				// For PickUp beacons, don't consider reached until a foot has picked up the item
				// Check if any foot is holding an item (which means pickup was successful)
				bool bAnyFootHolding = W.DFoot.bHoldingItem || W.LFoot.bHoldingItem || 
									   W.RFoot.bHoldingItem || W.UFoot.bHoldingItem;
				bTargetReached = bAnyFootHolding;
			}
			else if (bIsDropOffTarget)
			{
				// For DropOff beacons, don't consider reached until item has been dropped
				// Check if no foot is holding an item anymore (which means dropoff was successful)
				bool bAnyFootHolding = W.DFoot.bHoldingItem || W.LFoot.bHoldingItem || 
									   W.RFoot.bHoldingItem || W.UFoot.bHoldingItem;
				
				if (bAnyFootHolding)
				{
					// Find which foot is holding
					int32 HoldingFootIdx = W.DFoot.bHoldingItem ? 0 : (W.LFoot.bHoldingItem ? 1 : (W.RFoot.bHoldingItem ? 2 : 3));
					FFootState* DropOffHoldingFoot = nullptr;
					switch (HoldingFootIdx)
					{
					case 0: DropOffHoldingFoot = &W.DFoot; break;
					case 1: DropOffHoldingFoot = &W.LFoot; break;
					case 2: DropOffHoldingFoot = &W.RFoot; break;
					case 3: DropOffHoldingFoot = &W.UFoot; break;
					}
					
					// Check if the holding foot can reach the actual drop-off beacon tile
					if (DropOffHoldingFoot)
					{
						// Find the actual drop-off beacon location
						FHex ActualDropOffHex;
						bool bFoundDropOff = false;
						// Iterate over all cached beacons instead of TActorIterator
						for (ARequestBeacon* Beacon : AllBeacons)
						{
							if (Beacon->BeaconRole == EBeaconRole::DropOff)
							{
								// Check if this is the drop-off we're targeting
								FHex BeaconHex(Beacon->Q, Beacon->R);
									const int32 dq = BeaconHex.Q - W.CurrentTargetHex.Q;
									const int32 dr = BeaconHex.R - W.CurrentTargetHex.R;
									const int32 dist = FMath::Max(FMath::Max(FMath::Abs(dq), FMath::Abs(dr)), FMath::Abs(dq + dr));
									
									// If beacon is close to our target (within reach), it's probably our drop-off
									if (dist <= W.ReachDistance + 2) // Some tolerance
									{
										ActualDropOffHex = BeaconHex;
										bFoundDropOff = true;
										break;
									}
						}
					}
						
						if (bFoundDropOff)
						{
							// Check if holding foot's quadrant can reach the drop-off
							const FString DropOffQuadrant = GetFixedQuadrantFromHex(W.BaseHex, ActualDropOffHex, W.QuadrantRotationOffset);
							
							FString HoldingFootQuadrant;
							switch (HoldingFootIdx)
							{
							case 0: HoldingFootQuadrant = TEXT("S"); break; // D
							case 1: HoldingFootQuadrant = TEXT("W"); break; // L
							case 2: HoldingFootQuadrant = TEXT("E"); break; // R
							case 3: HoldingFootQuadrant = TEXT("N"); break; // U
							}
							
							// Check if drop-off is within reach distance
							const int32 bodyToDropOffDQ = ActualDropOffHex.Q - W.BaseHex.Q;
							const int32 bodyToDropOffDR = ActualDropOffHex.R - W.BaseHex.R;
							const int32 bodyToDropOffDist = FMath::Max(FMath::Max(FMath::Abs(bodyToDropOffDQ), FMath::Abs(bodyToDropOffDR)), 
								FMath::Abs(bodyToDropOffDQ + bodyToDropOffDR));
							
							if (!DropOffQuadrant.Equals(HoldingFootQuadrant, ESearchCase::CaseSensitive) && bodyToDropOffDist <= W.ReachDistance)
							{
								// Drop-off is in reach but wrong quadrant - keep trying to reposition
								bTargetReached = false;
								
								// Adjust target to move body so holding foot's quadrant aligns with drop-off
								// Calculate a new target that would put the drop-off in the holding foot's quadrant
								float OffsetDQ = 0.f, OffsetDR = 0.f;
								if (HoldingFootQuadrant == TEXT("N")) { OffsetDQ = -1.f; OffsetDR = 0.f; }
								else if (HoldingFootQuadrant == TEXT("E")) { OffsetDQ = 0.f; OffsetDR = -1.f; }
								else if (HoldingFootQuadrant == TEXT("S")) { OffsetDQ = 1.f; OffsetDR = 0.f; }
								else if (HoldingFootQuadrant == TEXT("W")) { OffsetDQ = 0.f; OffsetDR = 1.f; }
								
								float NewTargetDistance = (float)W.ReachDistance * 0.5f;
								int32 NewTargetQ = ActualDropOffHex.Q + FMath::RoundToInt(OffsetDQ * NewTargetDistance);
								int32 NewTargetR = ActualDropOffHex.R + FMath::RoundToInt(OffsetDR * NewTargetDistance);
								FHex NewTarget(NewTargetQ, NewTargetR);
								
								// Update the current target to encourage repositioning
								W.CurrentTargetHex = NewTarget;
								
								UE_LOG(LogTemp, Verbose, TEXT("Walker %d: Adjusting position - Foot %d (%s) can't reach DropOff in %s quadrant. New target: (%d,%d)"),
									WalkerIndex, HoldingFootIdx, *HoldingFootQuadrant, *DropOffQuadrant, NewTarget.Q, NewTarget.R);
							}
							else
							{
								// Either in correct quadrant or out of reach - let normal logic handle it
								bTargetReached = false;
								UE_LOG(LogTemp, Verbose, TEXT("Walker %d: Holding Foot %d still has item, continuing"), 
									WalkerIndex, HoldingFootIdx);
							}
						}
						else
						{
							// Couldn't find drop-off beacon
							bTargetReached = false;
						}
					}
				}
				else
				{
					// No longer holding - target reached!
					bTargetReached = true;
				}
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
				UE_LOG(LogTemp, Log, TEXT("Walker %d: Reached target (%d, %d) [IsHome=%d, IsPickUp=%d, IsDropOff=%d] and stabilized"), 
					WalkerIndex, W.CurrentTargetHex.Q, W.CurrentTargetHex.R, 
					bIsReturningHome ? 1 : 0, bIsPickUpTarget ? 1 : 0, bIsDropOffTarget ? 1 : 0);
				continue;
			}
			
			// NOTE: Body rotation is now derived from foot positions in BasicWalker.cpp
			// The sim does NOT directly set QuadrantRotationOffset - it's calculated from where feet step
			// To pivot the body, the foot scoring logic should encourage stepping to positions
			// that would naturally cause the body to rotate toward the drop-off alignment

			// NOTE: We still need to process foot state machine even when pivoting
			// The bIsPivoting flag will be used to skip TARGET CALCULATION, not state processing

			// Get all feet
			TArray<FFootState*> Feet = GetAllFeet(W);

			// PERFORMANCE: Check once per walker instead of per-tile whether any foot is holding
			bool bAnyFootHoldingInWalker = W.DFoot.bHoldingItem || W.LFoot.bHoldingItem || 
										   W.RFoot.bHoldingItem || W.UFoot.bHoldingItem;

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
					bool bTargetOccupied = IsHexOccupied(this, Feet[i]->TargetTile, HeldOrbHexes, ArmBaseHexes, HoldingFootHexes);
					
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
					// USER REQUEST: Holding feet must always be raised
					if (Foot.bHoldingItem)
					{
						Foot.HeightOffset = W.StepHeight;
					}
					// Special handling for feet holding items
					if (Foot.bHoldingItem)
					{
						// Find the DropOff beacon for this held item
						FHex DropOffHex = Foot.CurrentTile; // Default to current if not found
						bool bFoundDropOff = false;
						
						// Iterate over all cached beacons instead of TActorIterator
						for (ARequestBeacon* Beacon : AllBeacons)
						{
							if (Beacon->BeaconRole == EBeaconRole::DropOff && Beacon->PairedBeacon)
							{
								// Check if this is the paired DropOff (by checking if PickUp is InProgress)
								if (Beacon->PairedBeacon->BeaconState == EBeaconState::InProgress)
								{
									DropOffHex = FHex(Beacon->Q, Beacon->R);
									bFoundDropOff = true;
									break;
								}
							}
						}
						
						// Check if walker is in range of DropOff (Phase 1: Move Body)
						bool bInRangeOfDropOff = false;
						if (bFoundDropOff)
						{
							const int32 distQ = W.BaseHex.Q - DropOffHex.Q;
							const int32 distR = W.BaseHex.R - DropOffHex.R;
							const int32 dist = FMath::Max(FMath::Max(FMath::Abs(distQ), FMath::Abs(distR)), FMath::Abs(distQ + distR));
							bInRangeOfDropOff = (dist <= W.ReachDistance);
						}
						
						// Check if we are in the correct quadrant (Phase 2: Pivot)
						bool bInCorrectQuadrant = false;
						if (bFoundDropOff)
						{
							const FString DropOffQuadrant = GetFixedQuadrantFromHex(W.BaseHex, DropOffHex, W.QuadrantRotationOffset);
							FString HoldingFootQuadrant;
							// BUG FIX: Use FootIndex (current foot) not HoldingFootIdx (outer scope variable)
							switch (FootIndex)
							{
							case 0: HoldingFootQuadrant = TEXT("S"); break; // D
							case 1: HoldingFootQuadrant = TEXT("W"); break; // L
							case 2: HoldingFootQuadrant = TEXT("E"); break; // R
							case 3: HoldingFootQuadrant = TEXT("N"); break; // U
							}
							bInCorrectQuadrant = DropOffQuadrant.Equals(HoldingFootQuadrant, ESearchCase::CaseSensitive);
						}

						// Logic:
						// 1. If not in range -> Moving (handled by body logic elsewhere, but foot should center)
						// 2. If in range but wrong quadrant -> Pivot (handled by body logic elsewhere, foot should center)
						// 3. If in range AND correct quadrant -> Foot moves to DropOffHex (Ready to place)
						
						bool bReadyToPlace = (bInRangeOfDropOff && bInCorrectQuadrant);
						
						if (bReadyToPlace)
						{
							// Force target to DropOffHex
							if (Foot.TargetTile != DropOffHex)
							{
								Foot.TargetTile = DropOffHex;
								UE_LOG(LogTemp, Verbose, TEXT("Walker %d Foot %d: Ready to place at DropOff (%d,%d), forcing target"),
									WalkerIndex, FootIndex, DropOffHex.Q, DropOffHex.R);
							}
						}
						else
						{
							// Not ready to place (either moving to range or pivoting)
							// Force foot to maintain position at quadrant center
							FHex QuadrantCenter = GetQuadrantCenterHex(W.BaseHex, FootIndex, W.ReachDistance, W.QuadrantRotationOffset, this);
							
							// Only update if significantly different to allow some natural sway, 
							// but user request implies strict adherence. Let's enforce strictness.
							if (Foot.TargetTile != QuadrantCenter)
							{
								Foot.TargetTile = QuadrantCenter;
								UE_LOG(LogTemp, Verbose, TEXT("Walker %d Foot %d: Holding item, maintaining quadrant center at (%d,%d)"),
									WalkerIndex, FootIndex, QuadrantCenter.Q, QuadrantCenter.R);
							}
						}
							
							// If already at DropOff tile, attempt drop immediately
							if (Foot.CurrentTile == DropOffHex)
							{
								UE_LOG(LogTemp, Log, TEXT("Walker %d Foot %d: Already at DropOff tile (%d,%d), attempting drop directly"),
									WalkerIndex, FootIndex, DropOffHex.Q, DropOffHex.R);
								
								// Attempt drop directly (same logic as in Moving state)
								if (bFoundDropOff)
								{
									// Get beacon rotation
									// Get beacon rotation using cached lookup
									ARequestBeacon* DropOffBeacon = nullptr;
									if (ARequestBeacon** FoundBeacon = BeaconCache.Find(DropOffHex))
									{
										ARequestBeacon* Beacon = *FoundBeacon;
										if (Beacon->BeaconRole == EBeaconRole::DropOff)
										{
											DropOffBeacon = Beacon;
										}
									}
									
									if (DropOffBeacon)
									{
										UE_LOG(LogTemp, Log, TEXT("Walker %d Foot %d: Found DropOffBeacon, attempting placement"),
											WalkerIndex, FootIndex);
										
										float BeaconYaw = DropOffBeacon->GetActorRotation().Yaw;
										int32 Rot60 = FMath::RoundToInt(FRotator::NormalizeAxis(BeaconYaw) / 60.0f);
										if (Rot60 < 0) Rot60 += 6;
										Rot60 = Rot60 % 6;
										
										bool bDropSuccess = false;
										
										// Handle cluster drop
										if (Foot.HeldItem.bIsCluster && Foot.HeldItem.Orbs.Num() > 0)
										{
											UE_LOG(LogTemp, Log, TEXT("Walker %d Foot %d: Attempting cluster drop - %d orbs, GrabFacing=%d, BeaconRot=%d"),
												WalkerIndex, FootIndex, Foot.HeldItem.Orbs.Num(), Foot.HeldItem.GrabFacing, Rot60);
											
											int32 CurrentRot = (Foot.HeldItem.GrabFacing >= 0) ? ((int32)Foot.HeldItem.GrabFacing % 6) : 0;
											int32 RotDiff = (Rot60 - CurrentRot + 6) % 6;
											
											UE_LOG(LogTemp, Log, TEXT("  Calculating placement positions: CurrentRot=%d, TargetRot=%d, RotDiff=%d"),
												CurrentRot, Rot60, RotDiff);
											
											TArray<FHex> TargetHexes;
											bool bBlocked = false;
											
											for (int32 OrbIdx = 0; OrbIdx < Foot.HeldItem.Orbs.Num(); ++OrbIdx)
											{
												const FOrbData& OrbData = Foot.HeldItem.Orbs[OrbIdx];
												FHex Local(OrbData.dQ, OrbData.dR);
												
												UE_LOG(LogTemp, Log, TEXT("  Orb %d: Original dQ=%d, dR=%d"), OrbIdx, OrbData.dQ, OrbData.dR);
												
												for (int32 r = 0; r < RotDiff; ++r)
												{
													int32 OldQ = Local.Q;
													int32 OldR = Local.R;
													Local.Q = -OldR;
													Local.R = OldQ + OldR;
												}
												FHex WorldHex(DropOffHex.Q + Local.Q, DropOffHex.R + Local.R);
												
												UE_LOG(LogTemp, Log, TEXT("  Orb %d: After rotation dQ=%d, dR=%d, WorldPos=(%d,%d)"), 
													OrbIdx, Local.Q, Local.R, WorldHex.Q, WorldHex.R);
												
												// Check if blocked by items
												if (ItemMap.Contains(WorldHex))
												{
													bBlocked = true;
													UE_LOG(LogTemp, Warning, TEXT("  Orb placement blocked at (%d,%d) - item present"), WorldHex.Q, WorldHex.R);
													break;
												}
												
											// Check if blocked by THIS walker's own feet (excluding the holding foot)
											TArray<FFootState*> WalkerFeet = GetAllFeet(W);
											for (int32 CheckIdx = 0; CheckIdx < WalkerFeet.Num(); ++CheckIdx)
											{
												if (CheckIdx == FootIndex) continue; // Skip the holding foot itself
												if (WalkerFeet[CheckIdx]->CurrentTile == WorldHex || WalkerFeet[CheckIdx]->TargetTile == WorldHex)
												{
													bBlocked = true;
													UE_LOG(LogTemp, Warning, TEXT("  Orb placement blocked at (%d,%d) - own foot %d in the way"), 
														WorldHex.Q, WorldHex.R, CheckIdx);
													break;
												}
											}
												if (bBlocked) break;
												
												TargetHexes.Add(WorldHex);
											}
											
											if (!bBlocked)
											{
												// Final check: ensure no walker feet are in target positions
												// If any are, move them out of the way
												bool bFeetClear = true;
												TArray<FFootState*> WalkerFeet = GetAllFeet(W);
												TArray<int32> BlockingFootIndices;
												
												for (const FHex& TargetHex : TargetHexes)
												{
													for (int32 CheckIdx = 0; CheckIdx < WalkerFeet.Num(); ++CheckIdx)
													{
														if (CheckIdx == FootIndex) continue; // Skip holding foot
														if (WalkerFeet[CheckIdx]->CurrentTile == TargetHex)
														{
															bFeetClear = false;
															BlockingFootIndices.AddUnique(CheckIdx);
															UE_LOG(LogTemp, Warning, TEXT("  Final check: Foot %d at (%d,%d) blocking placement"),
																CheckIdx, TargetHex.Q, TargetHex.R);
														}
													}
												}
												
												if (!bFeetClear)
												{
													// Try to move blocking feet out of the way
													for (int32 BlockingIdx : BlockingFootIndices)
													{
														FFootState* BlockingFoot = WalkerFeet[BlockingIdx];
														
														// Find a safe tile for this foot to move to (outside cluster placement area)
														// Use a tile in its quadrant that's not in TargetHexes
														FString BlockingQuadrant;
														switch (BlockingIdx)
														{
														case 0: BlockingQuadrant = TEXT("S"); break;
														case 1: BlockingQuadrant = TEXT("W"); break;
														case 2: BlockingQuadrant = TEXT("E"); break;
														case 3: BlockingQuadrant = TEXT("N"); break;
														}
														
														// Find a tile that's not in TargetHexes
														FHex SafeTile = BlockingFoot->CurrentTile;
														for (int32 q = -W.ReachDistance; q <= W.ReachDistance; ++q)
														{
															for (int32 r = -W.ReachDistance; r <= W.ReachDistance; ++r)
															{
																FHex TestHex(W.BaseHex.Q + q, W.BaseHex.R + r);
																if (TargetHexes.Contains(TestHex)) continue; // Skip cluster placement tiles
																
																// Check if in quadrant
																const FString TestQuadrant = GetFixedQuadrantFromHex(W.BaseHex, TestHex, W.QuadrantRotationOffset);
																if (!TestQuadrant.Equals(BlockingQuadrant, ESearchCase::CaseSensitive)) continue;
																
																// Check reach distance
																const int32 dq = TestHex.Q - W.BaseHex.Q;
																const int32 dr = TestHex.R - W.BaseHex.R;
																const int32 HexDist = FMath::Max(FMath::Max(FMath::Abs(dq), FMath::Abs(dr)), FMath::Abs(dq + dr));
																if (HexDist == 0 || HexDist > W.ReachDistance) continue;
																
																// Found a safe tile
																SafeTile = TestHex;
																break;
															}
															if (SafeTile != BlockingFoot->CurrentTile) break;
														}
														
														if (SafeTile != BlockingFoot->CurrentTile)
														{
															BlockingFoot->TargetTile = SafeTile;
															UE_LOG(LogTemp, Log, TEXT("  Moving blocking Foot %d to (%d,%d) to clear path"),
																BlockingIdx, SafeTile.Q, SafeTile.R);
														}
													}
													
													UE_LOG(LogTemp, Warning, TEXT("  Cannot drop yet - feet in the way. Moved blocking feet, will retry."));
													break; // Exit Planted case, try again next tick after feet move
												}
												
												UE_LOG(LogTemp, Log, TEXT("  Placing %d orbs..."), Foot.HeldItem.Orbs.Num());
												for (int32 idx = 0; idx < Foot.HeldItem.Orbs.Num(); ++idx)
												{
													const FOrbData& OrbData = Foot.HeldItem.Orbs[idx];
													FOrb O;
													O.Type = (EOrbType)OrbData.Type;
													O.Color = (EOrbColor)OrbData.Color;
													O.Rot60 = 0;
													FItem NewItem = FItem::MakeSingle(O);
													NewItem.Id = (OrbData.ItemId > 0) ? OrbData.ItemId : NextItemId++;
													if (TryPutItem(TargetHexes[idx], NewItem))
													{
														UE_LOG(LogTemp, Verbose, TEXT("    Placed orb %d at (%d,%d)"), idx, TargetHexes[idx].Q, TargetHexes[idx].R);
													}
													else
													{
														UE_LOG(LogTemp, Error, TEXT("    FAILED to place orb %d at (%d,%d)"), idx, TargetHexes[idx].Q, TargetHexes[idx].R);
													}
												}
												bDropSuccess = true;
												UE_LOG(LogTemp, Log, TEXT("  Cluster placement complete, clearing held item"));
												
												// Bonds referencing these Ids were never removed; offsets will self-heal in CleanupBrokenBonds next tick.
											}
											else
											{
												UE_LOG(LogTemp, Warning, TEXT("  Cluster placement blocked!"));
											}
										}
										// Handle single orb drop
										else if (!Foot.HeldItem.bIsCluster)
										{
											if (!ItemMap.Contains(DropOffHex))
											{
												Foot.HeldItem.Single.Rot60 = (uint8)Rot60;
												if (TryPutItem(DropOffHex, Foot.HeldItem))
												{
													bDropSuccess = true;
												}
											}
										}
										
										if (bDropSuccess)
										{
											Foot.bHoldingItem = false;
											Foot.HeldItem = FItem();
											Foot.StepState = EFootStepState::Lowering;
											Foot.StepProgress = 0;
											
											if (DropOffBeacon->PairedBeacon)
											{
												DropOffBeacon->PairedBeacon->SetState(EBeaconState::NotReady);
											}
											DropOffBeacon->SetState(EBeaconState::NotReady);
											
											UE_LOG(LogTemp, Log, TEXT("Walker %d Foot %d: Dropped item at DropOff (immediate, was already at tile)"),
												WalkerIndex, FootIndex);

											// --- Return Home Logic ---
											if (W.Requests.Num() > 0)
											{
												W.Requests.RemoveAt(0); // Remove the completed DropOff request
											}
											// Clear any other pending requests just in case? No, might have queued tasks.
											// But usually DropOff is the end of a cycle.
											
											W.CurrentTargetHex = W.HomeHex;
											W.bProcessingRequest = true; // Ensure we start moving home
											
											break; // Exit Planted case
										}
										else
										{
											UE_LOG(LogTemp, Warning, TEXT("Walker %d Foot %d: Drop failed - bDropSuccess=false"),
												WalkerIndex, FootIndex);
										}
									}
									else
									{
										UE_LOG(LogTemp, Error, TEXT("Walker %d Foot %d: DropOffBeacon not found at (%d,%d)!"),
											WalkerIndex, FootIndex, DropOffHex.Q, DropOffHex.R);
									}
								}
								else
								{
									UE_LOG(LogTemp, Error, TEXT("Walker %d Foot %d: bFoundDropOff is false!"), WalkerIndex, FootIndex);
								}
									break; // Exit Planted case even if drop failed
								}

						
						// Continue to stepping logic below (don't break here unless already at target)
					}
					
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
					
					// For feet holding items, skip normal foot placement scoring - use the target already set above
					FHex BestTile = Foot.CurrentTile;
					float BestScore = MAX_flt; // Declare at outer scope so it's accessible in logs
					if (Foot.bHoldingItem && Foot.TargetTile != Foot.CurrentTile)
					{
						// Target already calculated above based on centered position or DropOff location
						BestTile = Foot.TargetTile;
						BestScore = 0.f; // Best score for holding items (already has target)
						
						UE_LOG(LogTemp, Verbose, TEXT("Walker %d Foot %d: Holding item, using pre-set target (%d,%d) from (%d,%d)"),
							WalkerIndex, FootIndex, BestTile.Q, BestTile.R, Foot.CurrentTile.Q, Foot.CurrentTile.R);
						// Skip to stepping logic
					}
					else
					{
					// Find best tile for this foot within ReachDistance and its assigned quadrant.
					// Goal: move foot toward target hex while respecting the leg's quadrant.
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
							
							// Check if this is a PickUp beacon tile (special handling)
							bool bIsPickUpBeacon = false;
							// Check if this is a DropOff beacon tile (special handling for holding feet)
							bool bIsDropOffBeacon = false;
							// PERFORMANCE: Use cached beacon lookup instead of TActorIterator
							if (ARequestBeacon** FoundBeacon = BeaconCache.Find(TestHex))
							{
								ARequestBeacon* Beacon = *FoundBeacon;
								
								// Check if this walker has a request for this beacon
								bool bHasRequestForThisBeacon = false;
								for (const FWalkerRequest& Req : W.Requests)
								{
									if (Req.TargetHex == TestHex)
									{
										bHasRequestForThisBeacon = true;
										break;
									}
								}
								
								// Only treat as PickUp beacon if this walker is assigned to it
								if (Beacon->BeaconRole == EBeaconRole::PickUp && Beacon->BeaconState == EBeaconState::InProgress && 
									!bAnyFootHoldingInWalker && bHasRequestForThisBeacon)
								{
									bIsPickUpBeacon = true;
								}
								else if (Beacon->BeaconRole == EBeaconRole::DropOff)
								{
									bIsDropOffBeacon = true;
								}
							}
							
							// Check if occupied
							bool bIsOccupied = IsHexOccupied(this, TestHex, HeldOrbHexes, ArmBaseHexes, HoldingFootHexes);
							
							// Treat PickUp beacon tiles as valid targets (not occupied for foot placement purposes)
							if (bIsPickUpBeacon)
							{
								bIsOccupied = false; // Override - allow stepping on PickUp beacons
							}
							// Treat DropOff beacon tiles as valid targets if this foot is holding an item
							else if (bIsDropOffBeacon && Foot.bHoldingItem)
							{
								bIsOccupied = false; // Override - allow stepping on DropOff beacons when holding
							}
							else if (bIsOccupied)
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
									
									// Add PickUp beacon bonus: make PickUp beacons VERY attractive
									const float PickUpBonus = bIsPickUpBeacon ? -1000.0f : 0.0f;
									
									// Add DropOff beacon bonus: make DropOff beacons VERY attractive when foot is holding
									const float DropOffBonus = (bIsDropOffBeacon && Foot.bHoldingItem) ? -1000.0f : 0.0f;
									
									const float CombinedScore = BodyDistToTarget + DistFromIdeal * 0.5f + StabilityPenalty + OccupiedPenalty + PickUpBonus + DropOffBonus; 
									
									// Track best VALID tile (no occupancy, or PickUp beacon, or DropOff beacon for holding feet) for detour fallback
									if ((!bIsOccupied || bIsPickUpBeacon || (bIsDropOffBeacon && Foot.bHoldingItem)) && CombinedScore < BestValidScore)
									{
										BestValidScore = CombinedScore;
										BestValidTile = TestHex;
									}

									// Only consider if it improves body position (or at least doesn't make it worse),
									// unless the tile is valid (not occupied) - in that case, allow slight regression to avoid obstacles.
									// PickUp and DropOff beacons are always valid targets regardless of distance.
									bool bDistOk = (BodyDistToTarget <= CurrentBodyDistToTarget);
									if (!bDistOk && (!bIsOccupied || bIsPickUpBeacon || (bIsDropOffBeacon && Foot.bHoldingItem)) && BodyDistToTarget <= CurrentBodyDistToTarget + 1.5f)
									{
										bDistOk = true;
									}
									if (bIsPickUpBeacon || (bIsDropOffBeacon && Foot.bHoldingItem))
									{
										bDistOk = true; // Always allow stepping to beacon targets
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
					} // End of normal foot placement logic (else block for !bHoldingItem)
					
					// If we found a better tile, start stepping
					// BUT: Skip if we're in pivot mode - pivot logic controls foot movement
					if (BestTile != Foot.CurrentTile && !bIsPivoting)
					{
						Foot.TargetTile = BestTile;
						Foot.StepState = EFootStepState::Raising;
						Foot.StepProgress = 0;
						Foot.HeightOffset = Foot.bHoldingItem ? W.StepHeight : 0.f; // Keep raised if holding
						RaisedCount++; // Update raised count
						RaisedIndices.Add(FootIndex);
						UE_LOG(LogTemp, Verbose, TEXT("Walker %d Foot %d: Starting to step from (%d,%d) to (%d,%d) - Score: %.1f, Holding: %d"),
							WalkerIndex, FootIndex, Foot.CurrentTile.Q, Foot.CurrentTile.R, 
							BestTile.Q, BestTile.R, BestScore, Foot.bHoldingItem ? 1 : 0);
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
					// If holding item, stay at max height
					if (Foot.bHoldingItem)
					{
						Foot.HeightOffset = W.StepHeight;
						// Skip raising animation, go straight to Moving
						Foot.StepState = EFootStepState::Moving;
						Foot.StepProgress = 0;
					}
					else
					{
						Foot.StepProgress++;
						Foot.HeightOffset = (float)Foot.StepProgress / (float)W.StepRaiseTicks * W.StepHeight;
						
						if (Foot.StepProgress >= W.StepRaiseTicks)
						{
							Foot.StepState = EFootStepState::Moving;
							Foot.StepProgress = 0;
							// UE_LOG(LogTemp, Log, TEXT("Walker %d Foot %d: Raised, now moving"), WalkerIndex, FootIndex);
						}
					}
					break;
				}
				
				case EFootStepState::Moving:
				{
					Foot.StepProgress++;
					
					// Continuous check: If we are hovering (not making progress toward a plant) or moving,
					// check if a better valid tile has become available.
					// This handles the case where a foot is waiting for a tile to clear, or moving to an occupied tile.
					
					// SKIP target re-evaluation if we're pivoting - pivot logic controls targets
					// Only check every few ticks to avoid performance hit
					if (Foot.StepProgress % 4 == 0 && !bIsPivoting)
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
						
						// PERFORMANCE: Check if current target is a PickUp beacon using cached lookup
						bool bCurrentTargetIsPickUp = false;
						if (ARequestBeacon** FoundBeacon = BeaconCache.Find(Foot.TargetTile))
						{
							ARequestBeacon* Beacon = *FoundBeacon;
							if (Beacon->BeaconRole == EBeaconRole::PickUp && Beacon->BeaconState == EBeaconState::InProgress)
							{
								bCurrentTargetIsPickUp = true;
							}
						}
						
						// Check if current target is occupied
						bool bCurrentTargetOccupied = IsHexOccupied(this, Foot.TargetTile, HeldOrbHexes, ArmBaseHexes, HoldingFootHexes);
						
						// Treat PickUp beacons as valid (not occupied)
						if (bCurrentTargetIsPickUp)
						{
							bCurrentTargetOccupied = false;
						}
						
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
								
								// PERFORMANCE: Check if this is a PickUp beacon using cached lookup
								bool bTestIsPickUp = false;
								if (ARequestBeacon** FoundBeacon = BeaconCache.Find(TestHex))
								{
									ARequestBeacon* Beacon = *FoundBeacon;
									if (Beacon->BeaconRole == EBeaconRole::PickUp && Beacon->BeaconState == EBeaconState::InProgress)
									{
										bTestIsPickUp = true;
									}
								}
								
								// Must be unoccupied to switch to it (unless it's a PickUp beacon)
								if (!bTestIsPickUp && IsHexOccupied(this, TestHex, HeldOrbHexes, ArmBaseHexes, HoldingFootHexes)) continue;
								
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
								UE_LOG(LogTemp, Verbose, TEXT("Walker %d Foot %d: Switching target from (%d,%d)[%s] to (%d,%d)[Free]"), 
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
						UE_LOG(LogTemp, Verbose, TEXT("Walker %d Foot %d: StepProgress complete, bHolding=%d, Target=(%d,%d)"),
							WalkerIndex, FootIndex, Foot.bHoldingItem ? 1 : 0, Foot.TargetTile.Q, Foot.TargetTile.R);
						
						// --- PickUp Logic ---
						if (!Foot.bHoldingItem)
						{
							// Check for PickUp beacon at TargetTile
							if (ItemMap.Contains(Foot.TargetTile))
							{
								ARequestBeacon* PickUpBeacon = nullptr;
								// Check for PickUp beacon at TargetTile using cached lookup
								if (ARequestBeacon** FoundBeacon = BeaconCache.Find(Foot.TargetTile))
								{
									ARequestBeacon* Beacon = *FoundBeacon;
									if (Beacon->BeaconRole == EBeaconRole::PickUp)
									{
										PickUpBeacon = Beacon;
									}
								}
								
								if (PickUpBeacon && PickUpBeacon->BeaconState == EBeaconState::InProgress)
								{
									// Check if another foot is already holding an item
									// Use already-declared AllFeet variable from outer scope
									bool bOtherFootHolding = false;
									for (int32 CheckIdx = 0; CheckIdx < AllFeet.Num(); ++CheckIdx)
									{
										if (CheckIdx != FootIndex && AllFeet[CheckIdx]->bHoldingItem)
										{
											bOtherFootHolding = true;
											UE_LOG(LogTemp, Warning, TEXT("Walker %d Foot %d: Cannot pick up - Foot %d already holding item"),
												WalkerIndex, FootIndex, CheckIdx);
											break;
										}
									}
									
									if (bOtherFootHolding)
									{
										continue; // Skip pickup, another foot is already holding
									}
									
									UE_LOG(LogTemp, Log, TEXT("Walker %d Foot %d: At PickUp beacon (%d,%d), attempting to pick up item"),
										WalkerIndex, FootIndex, Foot.TargetTile.Q, Foot.TargetTile.R);
									
									// Pick up cluster similar to how arms do it
									FItem* Found = ItemMap.Find(Foot.TargetTile);
									if (!Found || Found->Id == 0)
									{
										UE_LOG(LogTemp, Warning, TEXT("Walker %d Foot %d: No item found at PickUp beacon"), WalkerIndex, FootIndex);
										continue;
									}
									
									FItem PickedItem;
									bool bPickedSuccessfully = false;
									
									// Check if this orb is part of a bonded cluster
									TSet<int32> ClusterIds;
									UE_LOG(LogTemp, Log, TEXT("Walker %d Foot %d: Found item Id=%d at pickup tile, checking for bonds"),
										WalkerIndex, FootIndex, Found->Id);
									
									if (ItemToBonds.Contains(Found->Id))
									{
										// Traverse bond graph to find all connected orbs (BFS)
										TArray<int32> Stack;
										Stack.Add(Found->Id);
										ClusterIds.Add(Found->Id);
										UE_LOG(LogTemp, Log, TEXT("  Starting BFS with orb Id=%d"), Found->Id);
										
										while (Stack.Num() > 0)
										{
											int32 CurrentId = Stack.Pop();
											if (const TArray<int32>* BondList = ItemToBonds.Find(CurrentId))
											{
												for (int32 BondIdx : *BondList)
												{
													if (!Bonds.IsValidIndex(BondIdx)) continue;
													const FBond& Bond = Bonds[BondIdx];
													if (!Bond.bActive) continue;
													int32 OtherId = (Bond.ItemA == CurrentId) ? Bond.ItemB : Bond.ItemA;
													if (OtherId > 0 && !ClusterIds.Contains(OtherId))
													{
														ClusterIds.Add(OtherId);
														Stack.Add(OtherId);
													}
												}
											}
										}
									}
									
									// Pick up single orb
									if (ClusterIds.Num() <= 1)
									{
										if (TryTakeItem(Foot.TargetTile, PickedItem))
										{
											bPickedSuccessfully = true;
										}
									}
									// Pick up entire cluster
									else
									{
										// Get PickUp beacon rotation to preserve orientation
										float PickUpBeaconYaw = PickUpBeacon->GetActorRotation().Yaw;
										int32 PickUpRot60 = FMath::RoundToInt(FRotator::NormalizeAxis(PickUpBeaconYaw) / 60.0f);
										if (PickUpRot60 < 0) PickUpRot60 += 6;
										PickUpRot60 = PickUpRot60 % 6;
										
										FItem Cluster;
										Cluster.bIsCluster = true;
										Cluster.Id = Found->Id;
										Cluster.GrabFacing = (int8)PickUpRot60; // Store pickup beacon rotation to preserve orientation
										
										UE_LOG(LogTemp, Log, TEXT("  PickUp beacon rotation: Yaw=%.1f, Rot60=%d"), 
											PickUpBeaconYaw, PickUpRot60);
										
										UE_LOG(LogTemp, Log, TEXT("  Building cluster from %d connected orbs"), ClusterIds.Num());
										
										bool bAllFound = true;
										for (int32 ItemId : ClusterIds)
										{
											// Find hex location for this item
											FHex ItemHex;
											bool bItemFound = false;
											for (const auto& KV : ItemMap)
											{
												if (KV.Value.Id == ItemId)
												{
													ItemHex = KV.Key;
													bItemFound = true;
													break;
												}
											}
											
											if (!bItemFound)
											{
												UE_LOG(LogTemp, Warning, TEXT("  Orb Id=%d not found in ItemMap!"), ItemId);
												bAllFound = false;
												break;
											}
											
											const FItem* It = ItemMap.Find(ItemHex);
											if (It && !It->bIsCluster)
											{
												// Calculate relative position from pickup tile
												const FHex Delta(ItemHex.Q - Foot.TargetTile.Q, ItemHex.R - Foot.TargetTile.R);
												FOrbData Orb;
												Orb.Type = (uint8)It->Single.Type;
												Orb.Color = (uint8)It->Single.Color;
												Orb.ItemId = It->Id;
												Orb.dQ = (int32)Delta.Q;
												Orb.dR = (int32)Delta.R;
												Cluster.Orbs.Add(Orb);
												
												UE_LOG(LogTemp, Verbose, TEXT("  Added orb Id=%d at (%d,%d), dQ=%d, dR=%d, Type=%d"),
													ItemId, ItemHex.Q, ItemHex.R, Delta.Q, Delta.R, Orb.Type);
											}
											else
											{
												UE_LOG(LogTemp, Warning, TEXT("  Item Id=%d is already a cluster or invalid!"), ItemId);
											}
										}
										
										if (bAllFound && Cluster.Orbs.Num() > 0)
										{
											// Remove all orbs from ItemMap
											for (const FOrbData& Sub : Cluster.Orbs)
											{
												for (auto It = ItemMap.CreateIterator(); It; ++It)
												{
													if (It.Value().Id == Sub.ItemId)
													{
														It.RemoveCurrent();
														break;
													}
												}
											}
											
											PickedItem = Cluster;
											bPickedSuccessfully = true;
										}
									}
									
									if (bPickedSuccessfully)
									{
										UE_LOG(LogTemp, Log, TEXT("Walker %d Foot %d: Picked up item - bIsCluster=%d, Orbs.Num=%d"),
											WalkerIndex, FootIndex, PickedItem.bIsCluster ? 1 : 0, PickedItem.Orbs.Num());
										
										Foot.HeldItem = PickedItem;
										Foot.bHoldingItem = true;
										
										// Update foot position and transition to Planted state
										// This allows the centered-quadrant position logic to run on next tick
										Foot.CurrentTile = Foot.TargetTile;
										Foot.TargetTile = Foot.CurrentTile;
										Foot.StepState = EFootStepState::Planted; // Transition to Planted
										Foot.StepProgress = 0;
										Foot.HeightOffset = W.StepHeight; // Keep raised
										
										// Beacon is already InProgress (set when request was assigned)
										
										// Trigger DropOff request
										if (PickUpBeacon->PairedBeacon)
										{
											FHex DropOffHex(PickUpBeacon->PairedBeacon->Q, PickUpBeacon->PairedBeacon->R);
											
											// Calculate a target position where the holding foot can reach the drop-off
											// Don't target the drop-off directly - target a position offset by foot reach
											
											// Get the holding foot's quadrant
											FString HoldingFootQuadrant;
											switch (FootIndex)
											{
											case 0: HoldingFootQuadrant = TEXT("S"); break; // D
											case 1: HoldingFootQuadrant = TEXT("W"); break; // L
											case 2: HoldingFootQuadrant = TEXT("E"); break; // R
											case 3: HoldingFootQuadrant = TEXT("N"); break; // U
											}
											
											// Calculate offset based on foot quadrant - position body so foot can reach drop-off
											float OffsetDQ = 0.f, OffsetDR = 0.f;
											if (HoldingFootQuadrant == TEXT("N")) { OffsetDQ = -1.f; OffsetDR = 0.f; } // Body south of drop-off
											else if (HoldingFootQuadrant == TEXT("E")) { OffsetDQ = 0.f; OffsetDR = -1.f; } // Body west of drop-off
											else if (HoldingFootQuadrant == TEXT("S")) { OffsetDQ = 1.f; OffsetDR = 0.f; } // Body north of drop-off
											else if (HoldingFootQuadrant == TEXT("W")) { OffsetDQ = 0.f; OffsetDR = 1.f; } // Body east of drop-off
											
											// Position body at a distance where foot can comfortably reach
											float OffsetDistance = (float)W.ReachDistance * 0.7f; // 70% of reach for comfortable placement
											int32 TargetQ = DropOffHex.Q + FMath::RoundToInt(OffsetDQ * OffsetDistance);
											int32 TargetR = DropOffHex.R + FMath::RoundToInt(OffsetDR * OffsetDistance);
											FHex WalkerTarget(TargetQ, TargetR);
											
											W.Requests.Empty();
											FWalkerRequest DropReq;
											DropReq.TargetHex = WalkerTarget;
											DropReq.Priority = 2000;
											W.Requests.Add(DropReq);
											W.bProcessingRequest = false; // Force re-evaluation
											
											UE_LOG(LogTemp, Log, TEXT("Walker %d Foot %d: Picked up item from (%d,%d). Moving body to (%d,%d) so %s foot can reach DropOff at (%d,%d)."),
												WalkerIndex, FootIndex, Foot.CurrentTile.Q, Foot.CurrentTile.R, 
												WalkerTarget.Q, WalkerTarget.R, *HoldingFootQuadrant,
												DropOffHex.Q, DropOffHex.R);
										}
										continue; // Skip normal transition
									}
								}
							}
						}
						// --- DropOff Logic ---
						else if (Foot.bHoldingItem)
						{
							UE_LOG(LogTemp, Log, TEXT("Walker %d Foot %d: Holding item and reached target (%d,%d), checking for DropOff beacon"),
								WalkerIndex, FootIndex, Foot.TargetTile.Q, Foot.TargetTile.R);
							
							// Check for DropOff beacon at TargetTile
							ARequestBeacon* DropOffBeacon = nullptr;
							// Check for DropOff beacon at target using cached lookup
							if (ARequestBeacon** FoundBeacon = BeaconCache.Find(Foot.TargetTile))
							{
								ARequestBeacon* Beacon = *FoundBeacon;
								if (Beacon->BeaconRole == EBeaconRole::DropOff)
								{
									DropOffBeacon = Beacon;
									UE_LOG(LogTemp, Log, TEXT("Walker %d Foot %d: Found DropOff beacon at target"),
										WalkerIndex, FootIndex);
								}
							}
							
							if (DropOffBeacon)
							{
								// Calculate beacon rotation
								float BeaconYaw = DropOffBeacon->GetActorRotation().Yaw;
								int32 Rot60 = FMath::RoundToInt(FRotator::NormalizeAxis(BeaconYaw) / 60.0f);
								if (Rot60 < 0) Rot60 += 6;
								Rot60 = Rot60 % 6;
								
								bool bPlacedSuccessfully = false;
								
								if (!Foot.HeldItem.bIsCluster)
								{
									// Single orb - simple placement
									if (!ItemMap.Contains(Foot.TargetTile))
									{
										Foot.HeldItem.Single.Rot60 = (uint8)Rot60;
										if (TryPutItem(Foot.TargetTile, Foot.HeldItem))
										{
											bPlacedSuccessfully = true;
										}
									}
								}
								else
								{
									// Cluster - place each orb at its rotated world position
									// Rotate cluster to match beacon rotation
									int32 CurrentRot = (Foot.HeldItem.GrabFacing >= 0) ? ((int32)Foot.HeldItem.GrabFacing % 6) : 0;
									int32 RotDiff = (Rot60 - CurrentRot + 6) % 6;
									
									UE_LOG(LogTemp, Log, TEXT("Walker %d Foot %d: Placing cluster of %d orbs at (%d,%d), Rot=%d, RotDiff=%d"),
										WalkerIndex, FootIndex, Foot.HeldItem.Orbs.Num(), Foot.TargetTile.Q, Foot.TargetTile.R, Rot60, RotDiff);
									
									// Calculate target hex for each orb
									TArray<FHex> TargetHexes;
									bool bBlocked = false;
									
									for (const FOrbData& OrbData : Foot.HeldItem.Orbs)
									{
										// Start with orb's relative position
										FHex Local(OrbData.dQ, OrbData.dR);
										
										// Apply rotation
										for (int32 r = 0; r < RotDiff; ++r)
										{
											int32 OldQ = Local.Q;
											int32 OldR = Local.R;
											Local.Q = -OldR;
											Local.R = OldQ + OldR;
										}
										
										// Calculate world position
										FHex WorldHex(Foot.TargetTile.Q + Local.Q, Foot.TargetTile.R + Local.R);
										
										// Check if blocked by items
										if (ItemMap.Contains(WorldHex))
										{
											bBlocked = true;
											UE_LOG(LogTemp, Warning, TEXT("  Orb at (%d,%d) blocked - item present"), WorldHex.Q, WorldHex.R);
											break;
										}
										
												// Check if blocked by THIS walker's own feet (excluding the holding foot)
												TArray<FFootState*> WalkerFeet = GetAllFeet(W);
												for (int32 CheckIdx = 0; CheckIdx < WalkerFeet.Num(); ++CheckIdx)
												{
													if (CheckIdx == FootIndex) continue; // Skip the holding foot itself
													if (WalkerFeet[CheckIdx]->CurrentTile == WorldHex || WalkerFeet[CheckIdx]->TargetTile == WorldHex)
													{
														bBlocked = true;
														UE_LOG(LogTemp, Warning, TEXT("  Orb at (%d,%d) blocked - own foot %d in the way"), 
															WorldHex.Q, WorldHex.R, CheckIdx);
														break;
													}
												}
										if (bBlocked) break;
										
										TargetHexes.Add(WorldHex);
									}
									
									if (!bBlocked)
									{
										// Place each orb individually
										int32 PlacedCount = 0;
										for (int32 idx = 0; idx < Foot.HeldItem.Orbs.Num(); ++idx)
										{
											const FOrbData& OrbData = Foot.HeldItem.Orbs[idx];
											FOrb O;
											O.Type = (EOrbType)OrbData.Type;
											O.Color = (EOrbColor)OrbData.Color;
											O.Rot60 = 0; // Cluster orbs don't have individual rotations
											
											FItem NewItem = FItem::MakeSingle(O);
											if (OrbData.ItemId > 0)
											{
												NewItem.Id = OrbData.ItemId;
											}
											else
											{
												NewItem.Id = NextItemId++;
											}
											
											if (TryPutItem(TargetHexes[idx], NewItem))
											{
												PlacedCount++;
											}
											else
											{
												UE_LOG(LogTemp, Warning, TEXT("  Failed to place orb %d at (%d,%d)"), 
													idx, TargetHexes[idx].Q, TargetHexes[idx].R);
											}
										}
										
										if (PlacedCount == Foot.HeldItem.Orbs.Num())
										{
											bPlacedSuccessfully = true;
											UE_LOG(LogTemp, Log, TEXT("Walker %d Foot %d: Successfully placed %d orbs"), 
												WalkerIndex, FootIndex, PlacedCount);
											
											// IMPORTANT: Rebuild ItemToBonds for placed orbs to maintain bonds
											// The bonds exist but ItemToBonds needs to be updated after placement
											// We need to rebuild the entire bond map for the cluster
											for (int32 BondIdx = 0; BondIdx < Bonds.Num(); ++BondIdx)
											{
												const FBond& Bond = Bonds[BondIdx];
												if (!Bond.bActive) continue;
												
												// Check if this bond involves any orbs from our cluster
												bool bInvolvesCluster = false;
												for (const FOrbData& OrbData : Foot.HeldItem.Orbs)
												{
													if (OrbData.ItemId > 0 && (Bond.ItemA == OrbData.ItemId || Bond.ItemB == OrbData.ItemId))
													{
														bInvolvesCluster = true;
														break;
													}
												}
												
												if (bInvolvesCluster)
												{
													// Add bond to BOTH items' lists
													if (Bond.ItemA > 0)
													{
														TArray<int32>& BondListA = ItemToBonds.FindOrAdd(Bond.ItemA);
														if (!BondListA.Contains(BondIdx))
														{
															BondListA.Add(BondIdx);
															UE_LOG(LogTemp, Verbose, TEXT("    Restored bond link: Item %d -> Bond %d"),
																Bond.ItemA, BondIdx);
														}
													}
													
													if (Bond.ItemB > 0)
													{
														TArray<int32>& BondListB = ItemToBonds.FindOrAdd(Bond.ItemB);
														if (!BondListB.Contains(BondIdx))
														{
															BondListB.Add(BondIdx);
															UE_LOG(LogTemp, Verbose, TEXT("    Restored bond link: Item %d -> Bond %d"),
																Bond.ItemB, BondIdx);
														}
													}
												}
											}
											
											UE_LOG(LogTemp, Log, TEXT("  Rebuilt ItemToBonds for %d orbs in cluster"), Foot.HeldItem.Orbs.Num());
										}
										else
										{
											UE_LOG(LogTemp, Error, TEXT("Walker %d Foot %d: Only placed %d/%d orbs!"), 
												WalkerIndex, FootIndex, PlacedCount, Foot.HeldItem.Orbs.Num());
										}
									}
									else
									{
										UE_LOG(LogTemp, Warning, TEXT("Walker %d Foot %d: Cannot place cluster - tiles blocked"), 
											WalkerIndex, FootIndex);
									}
								}
								
								if (bPlacedSuccessfully)
								{
									Foot.bHoldingItem = false;
									Foot.HeldItem = FItem();
									
									// Transition to Lowering (visual plant)
									Foot.CurrentTile = Foot.TargetTile;
									Foot.StepState = EFootStepState::Lowering;
									Foot.StepProgress = 0;
									
									// Set beacon states
									if (DropOffBeacon->PairedBeacon)
									{
										DropOffBeacon->PairedBeacon->SetState(EBeaconState::NotReady);
									}
									DropOffBeacon->SetState(EBeaconState::NotReady);
									
									UE_LOG(LogTemp, Log, TEXT("Walker %d Foot %d: Dropped cluster at (%d,%d) with %d orbs, rotation %d."),
										WalkerIndex, FootIndex, Foot.TargetTile.Q, Foot.TargetTile.R, 
										Foot.HeldItem.bIsCluster ? Foot.HeldItem.Orbs.Num() : 1, Rot60);
									continue; 
								}
							}
								else
								{
									// Drop failed - tile(s) occupied, hover/wait
									// Stay in Moving state with current position updated
									Foot.CurrentTile = Foot.TargetTile;
									Foot.HeightOffset = W.StepHeight; // Keep raised
									UE_LOG(LogTemp, Verbose, TEXT("Walker %d Foot %d: Waiting to drop - target tile(s) occupied at (%d,%d)"),
										WalkerIndex, FootIndex, Foot.TargetTile.Q, Foot.TargetTile.R);
								}
							}
							else
							{
								// At DropOff target but no beacon found
								UE_LOG(LogTemp, Warning, TEXT("Walker %d Foot %d: At DropOff target (%d,%d) but no DropOff beacon found"),
									WalkerIndex, FootIndex, Foot.TargetTile.Q, Foot.TargetTile.R);
							}
						}

						// Check if the target tile is occupied before lowering
						// If foot is holding an item, transition to Planted instead of Lowering (stay raised)
						if (Foot.bHoldingItem)
						{
							// Holding item - transition to Planted but keep height
							Foot.CurrentTile = Foot.TargetTile;
							Foot.StepState = EFootStepState::Planted;
							Foot.StepProgress = 0;
							Foot.HeightOffset = W.StepHeight; // Keep raised
							// UE_LOG(LogTemp, Log, TEXT("Walker %d Foot %d: Reached position while holding item, staying raised"), WalkerIndex, FootIndex);
						}
						else
						{
							// Not holding item - check occupancy
							bool bTargetOccupied = IsHexOccupied(this, Foot.TargetTile, HeldOrbHexes, ArmBaseHexes, HoldingFootHexes);
							
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
				// Force height offset if holding item
				if (Foot.bHoldingItem)
				{
					Foot.HeightOffset = W.StepHeight;
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
