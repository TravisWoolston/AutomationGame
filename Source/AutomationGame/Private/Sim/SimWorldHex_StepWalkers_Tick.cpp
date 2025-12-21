#include "Sim/SimWorldHex.h"
#include "Sim/HexTypes.h"
#include "Sim/WalkerTypes.h"
#include "Sim/HexCoord.h"
#include "Actors/RequestBeacon.h"

void USimWorldHex::ProcessWalkerTick(int32 WalkerIndex, const TSet<FHex>& HeldOrbHexes, const TSet<FHex>& ArmBaseHexes, const TMap<FHex, ARequestBeacon*>& BeaconCache)
{
	FWalkerState& W = Walkers[WalkerIndex];

	// Update rotation based on feet
	W.QuadrantRotationOffset = CalculateSimWalkerRotation(W);
	
	// Stance transitions
	if (W.Stance == EWalkerStance::StandingUp)
	{
		W.StanceTransitionProgress++;
		if (W.StanceTransitionProgress >= 3)
		{
			W.Stance = EWalkerStance::Standing;
			W.StanceTransitionProgress = 0;
		}
		return;
	}
	else if (W.Stance == EWalkerStance::SittingDown)
	{
		W.StanceTransitionProgress++;
		if (W.StanceTransitionProgress >= 3)
		{
			W.Stance = EWalkerStance::Sitting;
			W.StanceTransitionProgress = 0;
		}
		return;
	}
	
	if (W.Stance == EWalkerStance::Sitting && W.Requests.Num() > 0)
	{
		W.Stance = EWalkerStance::StandingUp;
		W.StanceTransitionProgress = 0;
		return;
	}
	
	// Cancel logic for stale PickUp requests
	bool bAnyFootHolding = W.DFoot.bHoldingItem || W.LFoot.bHoldingItem || 
						   W.RFoot.bHoldingItem || W.UFoot.bHoldingItem;
	bool bAllFeetPlanted = W.DFoot.StepState == EFootStepState::Planted && 
						   W.LFoot.StepState == EFootStepState::Planted && 
						   W.RFoot.StepState == EFootStepState::Planted && 
						   W.UFoot.StepState == EFootStepState::Planted;

	if (!bAnyFootHolding && W.Requests.Num() > 0 && W.bProcessingRequest)
	{
		FHex RequestTarget = W.Requests[0].TargetHex;
		if (ARequestBeacon* const* BeaconPtr = BeaconCache.Find(RequestTarget))
		{
			if ((*BeaconPtr)->BeaconRole == EBeaconRole::PickUp && !ItemMap.Contains(RequestTarget))
			{
				(*BeaconPtr)->SetState(EBeaconState::NotReady);
				W.Requests.RemoveAt(0);
				W.bProcessingRequest = false;
				W.CurrentTargetHex = W.BaseHex;
			}
		}
	}
	
	// Idle logic
	if (W.Stance == EWalkerStance::Standing && W.Requests.Num() == 0 && !W.bProcessingRequest)
	{
		if (!bAnyFootHolding)
		{
			if (W.BaseHex != W.HomeHex)
			{
				FWalkerRequest ReturnRequest;
				ReturnRequest.TargetHex = W.HomeHex;
				ReturnRequest.Priority = -1;
				W.Requests.Insert(ReturnRequest, 0);
			}
			else if (bAllFeetPlanted)
			{
				W.Stance = EWalkerStance::SittingDown;
				W.StanceTransitionProgress = 0;
				return;
			}
		}
	}
	
	if (W.Stance != EWalkerStance::Standing && bAllFeetPlanted) return;
	
	// Request processing
	if (!W.bProcessingRequest && W.Requests.Num() > 0)
	{
		W.CurrentTargetHex = W.Requests[0].TargetHex;
		W.Requests[0].AttemptTicks = 0;
		W.bProcessingRequest = true;
	}
	
	bool bIsPivoting = false;
	if (W.bProcessingRequest)
	{
		W.Requests[0].AttemptTicks++;
		
		// Pivot detection
		TArray<FFootState*> AllFeet = GetAllFeet(W);
		for (FFootState* Foot : AllFeet)
		{
			if (Foot->bHoldingItem)
			{
				const FString DropOffQuad = GetFixedQuadrantFromHex(W.BaseHex, Foot->DesignatedDropOffHex, W.QuadrantRotationOffset);
				FString FootQuad;
				int32 FootIdx = (Foot == &W.DFoot ? 0 : (Foot == &W.LFoot ? 1 : (Foot == &W.RFoot ? 2 : 3)));
				switch (FootIdx) { case 0: FootQuad = TEXT("S"); break; case 1: FootQuad = TEXT("W"); break; case 2: FootQuad = TEXT("E"); break; case 3: FootQuad = TEXT("N"); break; }
				
				if (!DropOffQuad.Equals(FootQuad))
				{
					bIsPivoting = true;
					break;
				}
			}
		}
	}

	// Completion check for non-beacon moves (e.g., ReturnHome or body-positioning for drop-off)
	if (W.bProcessingRequest && W.BaseHex == W.CurrentTargetHex)
	{
		bool bAnyFootAwaitingDropOff = false;
		if (W.DFoot.bHoldingItem && W.DFoot.bHasDesignatedDropOff) bAnyFootAwaitingDropOff = true;
		if (W.LFoot.bHoldingItem && W.LFoot.bHasDesignatedDropOff) bAnyFootAwaitingDropOff = true;
		if (W.RFoot.bHoldingItem && W.RFoot.bHasDesignatedDropOff) bAnyFootAwaitingDropOff = true;
		if (W.UFoot.bHoldingItem && W.UFoot.bHasDesignatedDropOff) bAnyFootAwaitingDropOff = true;

		if (!bAnyFootAwaitingDropOff && !BeaconCache.Contains(W.CurrentTargetHex))
		{
			if (W.Requests.Num() > 0)
			{
				UE_LOG(LogTemp, Log, TEXT("Walker %d: Movement request complete at (%d,%d)"), WalkerIndex, W.CurrentTargetHex.Q, W.CurrentTargetHex.R);
				W.Requests.RemoveAt(0);
			}
			W.bProcessingRequest = false;
			W.CurrentTargetHex = W.BaseHex;
		}
	}

	// Collected occupied set for feet (GLOBAL set of all walkers' holding feet)
	TSet<FHex> GlobalHoldingFootHexes;
	for (const FWalkerState& OtherW : Walkers)
	{
		if (OtherW.DFoot.bHoldingItem) GlobalHoldingFootHexes.Add(OtherW.DFoot.CurrentTile);
		if (OtherW.LFoot.bHoldingItem) GlobalHoldingFootHexes.Add(OtherW.LFoot.CurrentTile);
		if (OtherW.RFoot.bHoldingItem) GlobalHoldingFootHexes.Add(OtherW.RFoot.CurrentTile);
		if (OtherW.UFoot.bHoldingItem) GlobalHoldingFootHexes.Add(OtherW.UFoot.CurrentTile);
	}

	// Leg processing
	HandleFootStateProcessing(W, WalkerIndex, HeldOrbHexes, ArmBaseHexes, BeaconCache, GlobalHoldingFootHexes, bIsPivoting);
	
	// Body processing
	UpdateWalkerBodyPosition(W, WalkerIndex);
	
	// Instruction processing (legacy or future)
	ProcessWalkerInstructions(W, WalkerIndex);
}
