#include "Sim/SimWorldHex.h"
#include "Sim/HexTypes.h"
#include "Sim/WalkerTypes.h"
#include "Sim/HexCoord.h"
#include "Actors/RequestBeacon.h"
#include "EngineUtils.h"

void USimWorldHex::StepWalkers()
{
	// Use cached occupancy sets
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
	
	// Cache all beacons
	TMap<FHex, ARequestBeacon*> BeaconCache;
	TArray<ARequestBeacon*> AllBeacons;
	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<ARequestBeacon> It(World); It; ++It)
		{
			if (ARequestBeacon* Beacon = *It)
			{
				BeaconCache.Add(FHex(Beacon->Q, Beacon->R), Beacon);
				AllBeacons.Add(Beacon);
			}
		}
	}
	
	// 1. Assignment phase
	ProcessWalkerRequestAssignment(AllBeacons, BeaconCache);
	
	// 2. Individual Walker Tick phase
	for (int32 WalkerIndex = 0; WalkerIndex < Walkers.Num(); ++WalkerIndex)
	{
		ProcessWalkerTick(WalkerIndex, HeldOrbHexes, ArmBaseHexes, BeaconCache);
	}

	// 3. Cleanup phase: Reset any "InProgress" beacons that are no longer being targeted by any walker
	TSet<FHex> ActiveTargetHexes;
	for (const FWalkerState& W : Walkers)
	{
		if (W.bProcessingRequest)
		{
			ActiveTargetHexes.Add(W.CurrentTargetHex);
		}
		
		// CRITICAL: Also check pending requests that haven't been 'processed' yet (e.g., while standing up)
		if (W.Requests.Num() > 0)
		{
			ActiveTargetHexes.Add(W.Requests[0].TargetHex);
		}

		// Also check holding feet destinations
		if (W.DFoot.bHoldingItem && W.DFoot.bHasDesignatedDropOff) ActiveTargetHexes.Add(W.DFoot.DesignatedDropOffHex);
		if (W.LFoot.bHoldingItem && W.LFoot.bHasDesignatedDropOff) ActiveTargetHexes.Add(W.LFoot.DesignatedDropOffHex);
		if (W.RFoot.bHoldingItem && W.RFoot.bHasDesignatedDropOff) ActiveTargetHexes.Add(W.RFoot.DesignatedDropOffHex);
		if (W.UFoot.bHoldingItem && W.UFoot.bHasDesignatedDropOff) ActiveTargetHexes.Add(W.UFoot.DesignatedDropOffHex);
	}

	for (ARequestBeacon* Beacon : AllBeacons)
	{
		if (Beacon->BeaconState == EBeaconState::InProgress)
		{
			FHex BHex(Beacon->Q, Beacon->R);
			if (!ActiveTargetHexes.Contains(BHex))
			{
				Beacon->SetState(EBeaconState::NotReady); // Will be set back to Ready by Tick() if it's a PickUp avec item
			}
		}
	}
}

void USimWorldHex::ProcessWalkerRequestAssignment(const TArray<ARequestBeacon*>& AllBeacons, const TMap<FHex, ARequestBeacon*>& BeaconCache)
{
	for (ARequestBeacon* Beacon : AllBeacons)
	{
		if (Beacon->BeaconRole == EBeaconRole::PickUp && Beacon->BeaconState == EBeaconState::Ready)
		{
			FHex BeaconHex(Beacon->Q, Beacon->R);
			if (!ItemMap.Contains(BeaconHex)) continue;

			int32 BestWalkerIdx = -1;
			int32 BestDistance = MAX_int32;
			
			for (int32 WIdx = 0; WIdx < Walkers.Num(); ++WIdx)
			{
				FWalkerState& W = Walkers[WIdx];
				if (W.Requests.Num() > 0 || W.bProcessingRequest) continue;
				if (W.Stance != EWalkerStance::Standing && W.Stance != EWalkerStance::Sitting) continue;
				
				bool bAnyFootHolding = W.DFoot.bHoldingItem || W.LFoot.bHoldingItem || 
									   W.RFoot.bHoldingItem || W.UFoot.bHoldingItem;
				if (bAnyFootHolding) continue;
				
				int32 dist = HexUtil::Dist(W.BaseHex, BeaconHex);
				if (dist < BestDistance)
				{
					BestDistance = dist;
					BestWalkerIdx = WIdx;
				}
			}
			
			if (BestWalkerIdx >= 0)
			{
				FWalkerRequest PickUpReq;
				PickUpReq.TargetHex = BeaconHex;
				PickUpReq.Priority = 1000;
				Walkers[BestWalkerIdx].Requests.Add(PickUpReq);
				Beacon->SetState(EBeaconState::InProgress);
				
				UE_LOG(LogTemp, Log, TEXT("Assigned PickUp beacon at (%d,%d) to Walker %d"),
					BeaconHex.Q, BeaconHex.R, BestWalkerIdx);
			}
		}
	}
}
