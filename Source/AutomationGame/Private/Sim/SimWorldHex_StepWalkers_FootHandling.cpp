#include "Sim/SimWorldHex.h"
#include "Sim/HexTypes.h"
#include "Sim/WalkerTypes.h"
#include "Sim/HexCoord.h"
#include "Actors/RequestBeacon.h"
#include "Actors/OrbSimDriver.h"
#include "Framework/GameManager.h"
#include "Algo/Sort.h"

void USimWorldHex::HandleFootStateProcessing(FWalkerState& W, int32 WalkerIndex, const TSet<FHex>& HeldOrbHexes, const TSet<FHex>& ArmBaseHexes, const TMap<FHex, ARequestBeacon*>& BeaconCache, const TSet<FHex>& HoldingFootHexes, bool bIsPivoting)
{
	TArray<FFootState*> Feet = GetAllFeet(W);
	const int32 NumFeet = Feet.Num();
	TArray<float> FootBodyDist; FootBodyDist.SetNum(NumFeet);
	
	int32 RaisedCount = 0;
	TArray<int32> RaisedIndices;
	for (int32 i = 0; i < NumFeet; ++i)
	{
		FootBodyDist[i] = (float)HexUtil::Dist(Feet[i]->CurrentTile, W.BaseHex);
		if (Feet[i]->StepState == EFootStepState::Planted) { Feet[i]->LastPlantDuration++; }
		else { Feet[i]->LastPlantDuration = 0; RaisedCount++; RaisedIndices.Add(i); }
	}

	TArray<int32> FootOrder = { 0, 1, 2, 3 };
	Algo::Sort(FootOrder, [&](int32 A, int32 B) {
		if (Feet[A]->LastPlantDuration != Feet[B]->LastPlantDuration) return Feet[A]->LastPlantDuration > Feet[B]->LastPlantDuration;
		return FootBodyDist[A] > FootBodyDist[B];
	});

	for (int32 FootIndex : FootOrder)
	{
		FFootState& Foot = *Feet[FootIndex];
		
		// Update horizontal intent every tick for feet in flight (Raising or Moving)
		if (Foot.StepState == EFootStepState::Raising || Foot.StepState == EFootStepState::Moving)
		{
			UpdateFootHorizontalTarget(W, Foot, FootIndex, HeldOrbHexes, ArmBaseHexes, HoldingFootHexes, BeaconCache, bIsPivoting);
		}

		switch (Foot.StepState)
		{
		case EFootStepState::Planted:
			HandleFootPlanted(W, Foot, WalkerIndex, FootIndex, HeldOrbHexes, ArmBaseHexes, HoldingFootHexes, BeaconCache, bIsPivoting, RaisedCount, RaisedIndices);
			break;
		case EFootStepState::Raising:
			Foot.StepProgress++;
			if (Foot.bHoldingItem) { Foot.HeightOffset = W.StepHeight; }
			else { Foot.HeightOffset = (float)Foot.StepProgress / (float)W.StepRaiseTicks * W.StepHeight; }
			
			if (Foot.StepProgress >= W.StepRaiseTicks)
			{
				Foot.StepState = EFootStepState::Moving;
				Foot.StepProgress = 0;
			}
			break;
		case EFootStepState::Moving:
			HandleFootMoving(W, Foot, WalkerIndex, FootIndex, HeldOrbHexes, ArmBaseHexes, HoldingFootHexes, BeaconCache, bIsPivoting);
			break;
		case EFootStepState::Lowering:
			Foot.StepProgress++;
			if (Foot.bHoldingItem) { Foot.HeightOffset = W.StepHeight; }
			else { Foot.HeightOffset = W.StepHeight * (1.f - (float)Foot.StepProgress / (float)W.StepLowerTicks); }
			
			if (Foot.StepProgress >= W.StepLowerTicks)
			{
				Foot.CurrentTile = Foot.TargetTile;
				Foot.StepState = EFootStepState::Planted;
				Foot.StepProgress = 0;
				Foot.HeightOffset = 0.f;
				
				// Trigger footstep wave (splash effect when foot lands)
				if (AOrbSimDriver* Driver = Cast<AOrbSimDriver>(GetOuter()))
				{
					if (Driver->GM)
					{
						FVector FootWorldPos = Driver->GM->GetWorldForHex(Foot.CurrentTile);
						Driver->GM->DrawWaveToRenderTarget(
							FootWorldPos,
							Driver->GM->WalkerFootWaveHeight,
							Driver->GM->WalkerFootWaveRadius,
							Driver->GM->WalkerFootWaveFalloff,
							Driver->GM->WalkerFootWaveLifetime
						);
					}
				}
			}
			break;
		}
		if (Foot.bHoldingItem) Foot.HeightOffset = W.StepHeight;
	}
}

void USimWorldHex::HandleFootPlanted(FWalkerState& W, FFootState& Foot, int32 WalkerIndex, int32 FootIndex, const TSet<FHex>& HeldOrbHexes, const TSet<FHex>& ArmBaseHexes, const TSet<FHex>& HoldingFootHexes, const TMap<FHex, ARequestBeacon*>& BeaconCache, bool bIsPivoting, int32& RaisedCount, TArray<int32>& RaisedIndices)
{
	// CRITICAL: If holding foot is Planted at drop-off tile and quadrant is now correct,
	// start a step so TryWalkerDropOff gets called to perform placement.
	if (Foot.bHoldingItem && Foot.bHasDesignatedDropOff && Foot.CurrentTile == Foot.DesignatedDropOffHex)
	{
		// Check if now in correct quadrant for placement
		int32 dist = HexUtil::Dist(W.BaseHex, Foot.DesignatedDropOffHex);
		bool bInRange = (dist <= W.ReachDistance);
		
		const FString DropOffQuad = GetFixedQuadrantFromHex(W.BaseHex, Foot.DesignatedDropOffHex, W.QuadrantRotationOffset);
		FString FootQuad;
		switch (FootIndex)
		{
		case 0: FootQuad = TEXT("S"); break;
		case 1: FootQuad = TEXT("W"); break;
		case 2: FootQuad = TEXT("E"); break;
		case 3: FootQuad = TEXT("N"); break;
		}
		bool bInCorrectQuadrant = DropOffQuad.Equals(FootQuad, ESearchCase::CaseSensitive);
		
		if (bInRange && bInCorrectQuadrant)
		{
			// Ready to place! Start a step toward the same tile to trigger TryWalkerDropOff
			Foot.TargetTile = Foot.CurrentTile;
			Foot.StepState = EFootStepState::Moving;
			Foot.StepProgress = 0;
			Foot.CurrentMoveDurationTicks = 0.f; // Finish immediately
			RaisedCount++; RaisedIndices.Add(FootIndex);
			return;
		}
	}
	
	// CRITICAL: If non-holding foot is Planted at pickup beacon tile that's InProgress,
	// start a step so TryWalkerPickUp gets called to perform pickup.
	if (!Foot.bHoldingItem && Foot.CurrentTile == W.CurrentTargetHex)
	{
		if (ARequestBeacon* const* BeaconPtr = BeaconCache.Find(Foot.CurrentTile))
		{
			if ((*BeaconPtr)->BeaconRole == EBeaconRole::PickUp && (*BeaconPtr)->BeaconState == EBeaconState::InProgress)
			{
				// Check that no other foot is already holding
				TArray<FFootState*> AllFeet = GetAllFeet(W);
				bool bAnotherFootHolding = false;
				for (int32 i = 0; i < AllFeet.Num(); ++i)
				{
					if (i != FootIndex && AllFeet[i]->bHoldingItem)
					{
						bAnotherFootHolding = true;
						break;
					}
				}
				
				if (!bAnotherFootHolding && ItemMap.Contains(Foot.CurrentTile))
				{
					// Ready to pick up! Start a step toward the same tile to trigger TryWalkerPickUp
					Foot.TargetTile = Foot.CurrentTile;
					Foot.StepState = EFootStepState::Moving;
					Foot.StepProgress = 0;
					Foot.CurrentMoveDurationTicks = 0.f; // Finish immediately
					RaisedCount++; RaisedIndices.Add(FootIndex);
					return;
				}
			}
		}
	}
	
	// Stability guard: don't start new steps if we are arrived and others are still landing
	if (!W.bProcessingRequest && RaisedCount > 0) return;
	
	// Balance guard: allow up to 2 opposite feet to move
	if (RaisedCount >= 2) return;
	if (RaisedCount == 1 && AreFeetAdjacent(RaisedIndices[0], FootIndex)) return;

	// Calculate per-quadrant axial baseline from rotation
	FString FootQuadrant; switch (FootIndex) { case 0: FootQuadrant = TEXT("S"); break; case 1: FootQuadrant = TEXT("W"); break; case 2: FootQuadrant = TEXT("E"); break; case 3: FootQuadrant = TEXT("N"); break; }
	float TargetAngleRad = 0.f; switch (FootIndex) { case 3: TargetAngleRad = -PI * 0.5f; break; case 2: TargetAngleRad = 0.f; break; case 0: TargetAngleRad = PI * 0.5f; break; case 1: TargetAngleRad = PI; break; }
	
	const float TotalRad = TargetAngleRad + FMath::DegreesToRadians(W.QuadrantRotationOffset);
	const float CQ = FMath::Cos(TotalRad); const float SQ = FMath::Sin(TotalRad);
	const float QuadDQ = (0.5773503f * CQ - 0.3333333f * SQ); const float QuadDR = (0.6666667f * SQ);

	float DistToTarget = (float)HexUtil::Dist(W.BaseHex, W.CurrentTargetHex);

	// Calculate Ideal Point in Cartesian space (centered on Body)
	const float HEX_SCALE = 1.7320508f;
	float IdealDist = (float)W.ReachDistance * 0.75f * HEX_SCALE;
	float MaxForwardLead = (float)W.ReachDistance * 1.0f * HEX_SCALE;
	float LeadDist = FMath::Min(MaxForwardLead, DistToTarget * HEX_SCALE);
	
	// During pivoting, reduce forward lead and add rotation bias
	float RotationBiasRad = 0.f;
	if (bIsPivoting)
	{
		LeadDist = FMath::Min(MaxForwardLead * 0.5f, DistToTarget * HEX_SCALE * 0.5f);
		
		// Find the holding foot's drop-off direction and calculate rotation needed
		TArray<FFootState*> AllFeet = GetAllFeet(W);
		for (int32 i = 0; i < AllFeet.Num(); ++i)
		{
			FFootState* HoldingFoot = AllFeet[i];
			if (HoldingFoot->bHoldingItem && HoldingFoot->bHasDesignatedDropOff)
			{
				// Calculate angle to drop-off from body
				float dq = (float)(HoldingFoot->DesignatedDropOffHex.Q - W.BaseHex.Q);
				float dr = (float)(HoldingFoot->DesignatedDropOffHex.R - W.BaseHex.R);
				float dropX = 1.7320508f * dq + 0.8660254f * dr;
				float dropY = 1.5f * dr;
				float DropOffAngle = FMath::Atan2(dropY, dropX);
				
				// Calculate holding foot's ideal quadrant angle (where it SHOULD be to reach drop-off)
				float HoldingFootIdealAngle = 0.f;
				switch (i)
				{
				case 3: HoldingFootIdealAngle = -PI * 0.5f; break; // N
				case 2: HoldingFootIdealAngle = 0.f; break;         // E
				case 0: HoldingFootIdealAngle = PI * 0.5f; break;  // S
				case 1: HoldingFootIdealAngle = PI; break;          // W
				}
				float CurrentQuadrantAngle = HoldingFootIdealAngle + FMath::DegreesToRadians(W.QuadrantRotationOffset);
				
				// Rotation needed: difference between drop-off angle and current quadrant angle
				float RotNeeded = DropOffAngle - CurrentQuadrantAngle;
				while (RotNeeded > PI) RotNeeded -= 2.f * PI;
				while (RotNeeded < -PI) RotNeeded += 2.f * PI;
				
				// Apply a small fraction of the needed rotation as bias for non-holding feet
				// This encourages feet to move in a direction that rotates the walker
				if (i != FootIndex) // Only apply bias to non-holding feet
				{
					RotationBiasRad = RotNeeded * 0.3f; // 30% of needed rotation as bias
				}
				break;
			}
		}
	}

	// World Move Direction (UX, UY)
	float UX = 0.f, UY = 0.f;
	if (DistToTarget > 0.01f)
	{
		float MDQ = (float)(W.CurrentTargetHex.Q - W.BaseHex.Q); float MDR = (float)(W.CurrentTargetHex.R - W.BaseHex.R);
		float MX = 1.7320508f * MDQ + 0.8660254f * MDR; float MY = 1.5f * MDR;
		float MLen = FMath::Sqrt(MX * MX + MY * MY);
		if (MLen > 0.1f) { UX = MX / MLen; UY = MY / MLen; }
	}

	// Combine Quadrant vector and Movement Lead, with rotation bias during pivoting
	// Apply rotation bias to the quadrant vector to encourage walker rotation
	float BiasedCQ = FMath::Cos(TotalRad + RotationBiasRad);
	float BiasedSQ = FMath::Sin(TotalRad + RotationBiasRad);
	float WorldIdealX = BiasedCQ * IdealDist + UX * LeadDist;
	float WorldIdealY = BiasedSQ * IdealDist + UY * LeadDist;

	// Clamp to physical reach
	float IdealLen = FMath::Sqrt(WorldIdealX * WorldIdealX + WorldIdealY * WorldIdealY);
	float ClampedReach = (float)W.ReachDistance * HEX_SCALE - 0.2f;
	if (IdealLen > ClampedReach)
	{
		float S = ClampedReach / IdealLen;
		WorldIdealX *= S; WorldIdealY *= S;
	}

	// Convert back to Axial Q/R relative to Body
	float RelQ = (0.5773503f * WorldIdealX - 0.3333333f * WorldIdealY);
	float RelR = (0.6666667f * WorldIdealY);
	float IdealQ = (float)W.BaseHex.Q + RelQ;
	float IdealR = (float)W.BaseHex.R + RelR;
	float IdealX = 1.7320508f * IdealQ + 0.8660254f * IdealR; float IdealY = 1.5f * IdealR;

	auto GetScore = [&](const FHex& H) {
		float TX = 1.7320508f * (float)H.Q + 0.8660254f * (float)H.R; float TY = 1.5f * (float)H.R;
		return FMath::Sqrt(FMath::Square(TX - IdealX) + FMath::Square(TY - IdealY));
	};

	float CurrentScore = GetScore(Foot.CurrentTile);
	
	bool bOnTargetPlacement = false;
	if (ARequestBeacon* const* BeaconPtr = BeaconCache.Find(Foot.CurrentTile))
	{
		if ((*BeaconPtr)->BeaconRole == EBeaconRole::PickUp && (*BeaconPtr)->BeaconState == EBeaconState::InProgress && Foot.CurrentTile == W.CurrentTargetHex)
			bOnTargetPlacement = true;
	}
	if (!bOnTargetPlacement && Foot.bHoldingItem && Foot.bHasDesignatedDropOff && Foot.CurrentTile == Foot.DesignatedDropOffHex)
		bOnTargetPlacement = true;

	// Holding Foot Priority: If we are holding an item, our 'IdealPoint' is the drop-off itself
	if (Foot.bHoldingItem && Foot.bHasDesignatedDropOff)
	{
		int32 DD = HexUtil::Dist(W.BaseHex, Foot.DesignatedDropOffHex);
		if (DD <= W.ReachDistance)
		{
			IdealQ = (float)Foot.DesignatedDropOffHex.Q;
			IdealR = (float)Foot.DesignatedDropOffHex.R;
			IdealX = 1.7320508f * IdealQ + 0.8660254f * IdealR; IdealY = 1.5f * IdealR;
			CurrentScore = GetScore(Foot.CurrentTile);
		}
	}

	// Stability threshold: Freshly planted feet (duration < 4) or task-committed feet should not move.
	bool bFreshlyPlanted = Foot.LastPlantDuration < 4; 
	
	// BUGFIX: When processing a request with distance to cover OR pivoting, be more aggressive about finding tiles.
	// Use lower threshold when actively moving toward a target to prevent "stuck" behavior.
	// Also use aggressive thresholds when pivoting (drop-off in reach but wrong quadrant) to encourage rotation.
	float ScoreThreshold = 0.5f * HEX_SCALE;
	float HysteresisThreshold = 0.2f * HEX_SCALE;
	if (W.bProcessingRequest && (DistToTarget > 0.5f || bIsPivoting))
	{
		// More aggressive thresholds when we have a target to reach or need to pivot
		ScoreThreshold = 0.25f * HEX_SCALE;
		HysteresisThreshold = 0.1f * HEX_SCALE;
	}
	
	if (!bOnTargetPlacement && !bFreshlyPlanted && CurrentScore > ScoreThreshold)
	{
		FHex BestTile = Foot.CurrentTile;
		float BestScore = CurrentScore;

		// Perform full reach search to ensure we never 'starve' a leg if the ideal point is outside its quadrant
		for (int32 q = -W.ReachDistance; q <= W.ReachDistance; ++q)
		{
			for (int32 r = -W.ReachDistance; r <= W.ReachDistance; ++r)
			{
				FHex TestHex(W.BaseHex.Q + q, W.BaseHex.R + r);
				if (TestHex == Foot.CurrentTile) continue;
				int32 dist = HexUtil::Dist(W.BaseHex, TestHex);
				if (dist == 0 || dist > W.ReachDistance || (float)dist < W.MinimumReachDistance) continue;
				
				bool bIsDesignatedDropOff = (Foot.bHoldingItem && Foot.bHasDesignatedDropOff && TestHex == Foot.DesignatedDropOffHex);
				if (!bIsDesignatedDropOff && !GetFixedQuadrantFromHex(W.BaseHex, TestHex, W.QuadrantRotationOffset).Equals(FootQuadrant)) continue;
				
				bool bTestIsTarget = false;
				if (TestHex == W.CurrentTargetHex || bIsDesignatedDropOff)
				{
					if (ARequestBeacon* const* B = BeaconCache.Find(TestHex))
						if ((*B)->BeaconRole == EBeaconRole::PickUp || (*B)->BeaconRole == EBeaconRole::DropOff) bTestIsTarget = true;
				}

				if (!bTestIsTarget && IsHexOccupied(TestHex, HeldOrbHexes, ArmBaseHexes, HoldingFootHexes)) continue;
				float Score = GetScore(TestHex);
				if (bTestIsTarget) Score -= 100.f; // Massive priority for task hexes
				if (Score < BestScore - HysteresisThreshold) { BestScore = Score; BestTile = TestHex; }
			}
		}

		if (BestTile != Foot.CurrentTile)
		{
			// Foot target changed - reset timeout counter
			if (Foot.TargetTile != BestTile)
			{
				W.TicksSinceLastFootTargetChange = 0;
			}
			
			Foot.TargetTile = BestTile;
			Foot.StepState = EFootStepState::Raising;
			Foot.StepProgress = 0;
			
			// Calculate initial move duration
			float MoveDist = (float)HexUtil::Dist(Foot.CurrentTile, Foot.TargetTile);
			Foot.CurrentMoveDurationTicks = (W.TilesPerStep > 0.001f) ? (MoveDist / W.TilesPerStep) : 1.0f;
			
			RaisedCount++; RaisedIndices.Add(FootIndex);
		}
	}
}

void USimWorldHex::HandleFootMoving(FWalkerState& W, FFootState& Foot, int32 WalkerIndex, int32 FootIndex, const TSet<FHex>& HeldOrbHexes, const TSet<FHex>& ArmBaseHexes, const TSet<FHex>& HoldingFootHexes, const TMap<FHex, ARequestBeacon*>& BeaconCache, bool bIsPivoting)
{
	Foot.StepProgress++;
	if (Foot.StepProgress >= (int32)FMath::CeilToFloat(Foot.CurrentMoveDurationTicks))
	{
		if (!Foot.bHoldingItem)
		{
			TryWalkerPickUp(W, Foot, WalkerIndex, FootIndex, BeaconCache, GetAllFeet(W));
		}
		else
		{
			if (TryWalkerDropOff(W, Foot, WalkerIndex, FootIndex, HeldOrbHexes, ArmBaseHexes, HoldingFootHexes, BeaconCache))
			{
				// Success
			}
			else
			{
				// Blocked
				// Keep searching for a valid spot or wait
				return;
			}
		}

		if (Foot.StepProgress >= (int32)FMath::CeilToFloat(Foot.CurrentMoveDurationTicks))
		{
			Foot.CurrentTile = Foot.TargetTile;
			if (Foot.bHoldingItem)
			{
				Foot.StepState = EFootStepState::Planted;
				Foot.HeightOffset = W.StepHeight;
			}
			else
			{
				Foot.StepState = EFootStepState::Lowering;
			}
			Foot.StepProgress = 0;
		}
	}
}

void USimWorldHex::UpdateFootHorizontalTarget(FWalkerState& W, FFootState& Foot, int32 FootIndex, const TSet<FHex>& HeldOrbHexes, const TSet<FHex>& ArmBaseHexes, const TSet<FHex>& HoldingFootHexes, const TMap<FHex, ARequestBeacon*>& BeaconCache, bool bIsPivoting)
{
	const float HEX_SCALE = 1.7320508f;
	FString FootQuadrant; switch (FootIndex) { case 0: FootQuadrant = TEXT("S"); break; case 1: FootQuadrant = TEXT("W"); break; case 2: FootQuadrant = TEXT("E"); break; case 3: FootQuadrant = TEXT("N"); break; }
	float TargetAngleRad = 0.f; switch (FootIndex) { case 3: TargetAngleRad = -PI * 0.5f; break; case 2: TargetAngleRad = 0.f; break; case 0: TargetAngleRad = PI * 0.5f; break; case 1: TargetAngleRad = PI; break; }
	const float TotalRad = TargetAngleRad + FMath::DegreesToRadians(W.QuadrantRotationOffset);
	const float CQ = FMath::Cos(TotalRad); const float SQ = FMath::Sin(TotalRad);

	float DistToTarget = (float)HexUtil::Dist(W.BaseHex, W.CurrentTargetHex);
	float IdealDist = (float)W.ReachDistance * 0.75f * HEX_SCALE;
	float MaxForwardLead = (float)W.ReachDistance * 1.0f * HEX_SCALE;
	float LeadDist = FMath::Min(MaxForwardLead, DistToTarget * HEX_SCALE);
	
	// During pivoting, reduce forward lead and add rotation bias
	float RotationBiasRad = 0.f;
	if (bIsPivoting)
	{
		LeadDist = FMath::Min(MaxForwardLead * 0.5f, DistToTarget * HEX_SCALE * 0.5f);
		
		// Find the holding foot's drop-off direction and calculate rotation needed
		TArray<FFootState*> AllFeet = GetAllFeet(W);
		for (int32 i = 0; i < AllFeet.Num(); ++i)
		{
			FFootState* HoldingFoot = AllFeet[i];
			if (HoldingFoot->bHoldingItem && HoldingFoot->bHasDesignatedDropOff)
			{
				// Calculate angle to drop-off from body
				float dq = (float)(HoldingFoot->DesignatedDropOffHex.Q - W.BaseHex.Q);
				float dr = (float)(HoldingFoot->DesignatedDropOffHex.R - W.BaseHex.R);
				float dropX = 1.7320508f * dq + 0.8660254f * dr;
				float dropY = 1.5f * dr;
				float DropOffAngle = FMath::Atan2(dropY, dropX);
				
				// Calculate holding foot's ideal quadrant angle
				float HoldingFootIdealAngle = 0.f;
				switch (i)
				{
				case 3: HoldingFootIdealAngle = -PI * 0.5f; break; // N
				case 2: HoldingFootIdealAngle = 0.f; break;         // E
				case 0: HoldingFootIdealAngle = PI * 0.5f; break;  // S
				case 1: HoldingFootIdealAngle = PI; break;          // W
				}
				float CurrentQuadrantAngle = HoldingFootIdealAngle + FMath::DegreesToRadians(W.QuadrantRotationOffset);
				
				// Rotation needed: difference between drop-off angle and current quadrant angle
				float RotNeeded = DropOffAngle - CurrentQuadrantAngle;
				while (RotNeeded > PI) RotNeeded -= 2.f * PI;
				while (RotNeeded < -PI) RotNeeded += 2.f * PI;
				
				// Apply rotation bias to non-holding feet
				if (i != FootIndex)
				{
					RotationBiasRad = RotNeeded * 0.3f;
				}
				break;
			}
		}
	}

	float UX = 0.f, UY = 0.f;
	if (W.bProcessingRequest && DistToTarget > 0.01f)
	{
		float MDQ = (float)(W.CurrentTargetHex.Q - W.BaseHex.Q); float MDR = (float)(W.CurrentTargetHex.R - W.BaseHex.R);
		float MX = 1.7320508f * MDQ + 0.8660254f * MDR; float MY = 1.5f * MDR;
		float MLen = FMath::Sqrt(MX * MX + MY * MY);
		if (MLen > 0.1f) { UX = MX / MLen; UY = MY / MLen; }
	}
	
	// Apply rotation bias to ideal point
	float BiasedCQ = FMath::Cos(TotalRad + RotationBiasRad);
	float BiasedSQ = FMath::Sin(TotalRad + RotationBiasRad);
	float WorldIdealX = BiasedCQ * IdealDist + UX * LeadDist;
	float WorldIdealY = BiasedSQ * IdealDist + UY * LeadDist;
	float IdealLen = FMath::Sqrt(WorldIdealX * WorldIdealX + WorldIdealY * WorldIdealY);
	float ClampedReach = (float)W.ReachDistance * HEX_SCALE - 0.2f;
	if (IdealLen > ClampedReach) { float S = ClampedReach / IdealLen; WorldIdealX *= S; WorldIdealY *= S; }

	float RelQ = (0.5773503f * WorldIdealX - 0.3333333f * WorldIdealY);
	float RelR = (0.6666667f * WorldIdealY);
	float IdealQ = (float)W.BaseHex.Q + RelQ;
	float IdealR = (float)W.BaseHex.R + RelR;
	float IdealX = 1.7320508f * IdealQ + 0.8660254f * IdealR; float IdealY = 1.5f * IdealR;
	
	auto GetScore = [&](const FHex& H) {
		float TX = 1.7320508f * (float)H.Q + 0.8660254f * (float)H.R; float TY = 1.5f * (float)H.R;
		return FMath::Sqrt(FMath::Square(TX - IdealX) + FMath::Square(TY - IdealY));
	};

	// Holding Foot Priority: If we are holding an item, our 'IdealPoint' is the drop-off itself
	if (Foot.bHoldingItem && Foot.bHasDesignatedDropOff)
	{
		int32 DD = HexUtil::Dist(W.BaseHex, Foot.DesignatedDropOffHex);
		if (DD <= W.ReachDistance)
		{
			IdealQ = (float)Foot.DesignatedDropOffHex.Q;
			IdealR = (float)Foot.DesignatedDropOffHex.R;
			IdealX = 1.7320508f * IdealQ + 0.8660254f * IdealR; IdealY = 1.5f * IdealR;
		}
	}

	// Commitment guard: only lock if we are actually at the intended task spot
	bool bTargetIsTask = false;
	if (Foot.TargetTile == W.CurrentTargetHex)
	{
		if (ARequestBeacon* const* BeaconPtr = BeaconCache.Find(Foot.TargetTile))
		{
			if ((*BeaconPtr)->BeaconRole == EBeaconRole::PickUp || (*BeaconPtr)->BeaconRole == EBeaconRole::DropOff)
				bTargetIsTask = true;
		}
	}
	if (!bTargetIsTask && Foot.bHoldingItem && Foot.bHasDesignatedDropOff && Foot.TargetTile == Foot.DesignatedDropOffHex)
		bTargetIsTask = true;

	if (bTargetIsTask) return;

	float BestScore = GetScore(Foot.TargetTile);
	
	// BUGFIX: When processing a request OR pivoting, use lower hysteresis to be more responsive
	float HysteresisThreshold = 0.4f * HEX_SCALE;
	if (W.bProcessingRequest && (DistToTarget > 0.5f || bIsPivoting))
	{
		HysteresisThreshold = 0.2f * HEX_SCALE;
	}

	// In mid-air, use full axial search for precision
	for (int32 q = -W.ReachDistance; q <= W.ReachDistance; ++q)
	{
		for (int32 r = -W.ReachDistance; r <= W.ReachDistance; ++r)
		{
			FHex TestHex(W.BaseHex.Q + q, W.BaseHex.R + r);
			int32 dist = HexUtil::Dist(W.BaseHex, TestHex);
			if (dist == 0 || dist > W.ReachDistance || (float)dist < W.MinimumReachDistance) continue;
				
			bool bIsDesignatedDropOff = (Foot.bHoldingItem && Foot.bHasDesignatedDropOff && TestHex == Foot.DesignatedDropOffHex);
			if (!bIsDesignatedDropOff && !GetFixedQuadrantFromHex(W.BaseHex, TestHex, W.QuadrantRotationOffset).Equals(FootQuadrant)) continue;
				
			bool bTestIsTarget = false;
			if (TestHex == W.CurrentTargetHex || bIsDesignatedDropOff)
			{
				if (ARequestBeacon* const* B = BeaconCache.Find(TestHex))
					if ((*B)->BeaconRole == EBeaconRole::PickUp || (*B)->BeaconRole == EBeaconRole::DropOff) bTestIsTarget = true;
			}

			if (!bTestIsTarget && IsHexOccupied(TestHex, HeldOrbHexes, ArmBaseHexes, HoldingFootHexes)) continue;
			float Score = GetScore(TestHex);
			if (bTestIsTarget) Score -= 100.f;
			if (Score < BestScore - HysteresisThreshold) 
			{ 
				BestScore = Score; 
				// Foot target changed - reset timeout counter
				if (Foot.TargetTile != TestHex)
				{
					W.TicksSinceLastFootTargetChange = 0;
				}
				Foot.TargetTile = TestHex; 
				
				// Update dynamic move duration when target changes mid-flight
				float MoveDist = (float)HexUtil::Dist(Foot.CurrentTile, Foot.TargetTile);
				Foot.CurrentMoveDurationTicks = (W.TilesPerStep > 0.001f) ? (MoveDist / W.TilesPerStep) : 1.0f;
			}
		}
	}
}
