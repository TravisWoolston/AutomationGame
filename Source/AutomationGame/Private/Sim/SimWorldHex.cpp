#include "Sim/SimWorldHex.h"
#include "Engine/Engine.h"
#include "Sim/HexCoord.h"
#include "Actors/BasicArm.h"
#include "Actors/OrbSimDriver.h"
#include "EngineUtils.h"
#include "DrawDebugHelpers.h"
#include "AGLogChannels.h"
#include "Algo/Reverse.h"

// Constructor
USimWorldHex::USimWorldHex()
{
	// Initialize all containers to prevent memory corruption
	TileMap.Empty();
	ItemMap.Empty();
	Arms.Empty();
	Walkers.Empty();
	OrbAt.Empty();
	BeltProgress.Empty();
	Bonds.Empty();
	ItemToBonds.Empty();
	SimFacingByArm.Empty();
	SimArmLenByArm.Empty();
	SimGrabFacingByArm.Empty();
	PendingFacingByArm.Empty();
	PendingArmLenByArm.Empty();
	PendingGrabFacingByArm.Empty();
	SkipInputThisFrame.Empty();
	SkipOutputThisFrame.Empty();
	NewlyPlacedItems.Empty();
	ArmsReadyForUnbond.Empty();
	PendingUnbonds.Empty();
	RecentlyDroppedByArm.Empty();
	OrbsNewlyMovedToOutput.Empty();
	OrbsNewlyMovedByBelts.Empty();
	OutputTileGates.Empty();
	BeltTriggerSignals.Empty();
	AnimatedOrbs.Empty();
	HeldItemLastHex.Empty();
	PendingBondFormations.Empty();
	PendingGrabs.Empty();
	BondedArmPairs.Empty();
	PendingHeldMerges.Empty();
	CollisionDebugData.Empty();
	PredictiveCollisionDebugData.Empty();
	ItemsMovedLastStep.Empty();
	PrevItemHex.Empty();
}

// Destructor - explicit cleanup to prevent memory corruption
USimWorldHex::~USimWorldHex()
{
	// Clear all containers in a safe order to prevent corruption
	// Clear complex nested structures first
	ItemToBonds.Empty();
	
	// Clear all other containers
	TileMap.Empty();
	ItemMap.Empty();
	Arms.Empty();
	OrbAt.Empty();
	BeltProgress.Empty();
	Bonds.Empty();
	SimFacingByArm.Empty();
	SimArmLenByArm.Empty();
	SimGrabFacingByArm.Empty();
	PendingFacingByArm.Empty();
	PendingArmLenByArm.Empty();
	PendingGrabFacingByArm.Empty();
	SkipInputThisFrame.Empty();
	SkipOutputThisFrame.Empty();
	NewlyPlacedItems.Empty();
	ArmsReadyForUnbond.Empty();
	PendingUnbonds.Empty();
	RecentlyDroppedByArm.Empty();
	OrbsNewlyMovedToOutput.Empty();
	OrbsNewlyMovedByBelts.Empty();
	OutputTileGates.Empty();
	BeltTriggerSignals.Empty();
	AnimatedOrbs.Empty();
	HeldItemLastHex.Empty();
	PendingBondFormations.Empty();
	PendingGrabs.Empty();
	BondedArmPairs.Empty();
	PendingHeldMerges.Empty();
	CollisionDebugData.Empty();
	PredictiveCollisionDebugData.Empty();
	ItemsMovedLastStep.Empty();
	PrevItemHex.Empty();
}

// Near top (after includes) add if not already in header:
static constexpr float DEFAULT_BELT_SPEED_TILES_PER_SECOND = 0.0f;
// Helper used only inside StepBonds to index held cluster sub‑orbs.
struct FHeldOrbRef
{
	int32 ArmIndex = -1;
	int32 SubIdx = -1;
};


void USimWorldHex::Initialize(int32 /*Radius*/)
{
	UE_LOG(LogTemp, Warning, TEXT("SimWorldHex::Initialize() clearing %d arms and %d walkers from simulation"), Arms.Num(), Walkers.Num());
	TileMap.Empty();
	ItemMap.Empty();
	Arms.Empty();
	Walkers.Empty();
	OutputCount = 0;
	TileRevision = 0;
	NextItemId = 1;
	OrbAt.Empty();
	BeltProgress.Empty();
	Bonds.Empty();
	ItemToBonds.Empty();
	SimFacingByArm.Empty();
	SimArmLenByArm.Empty();
	SimGrabFacingByArm.Empty();
	PendingFacingByArm.Empty();
	PendingArmLenByArm.Empty();
	PendingGrabFacingByArm.Empty();
}




// ---------- NEW: Add missing definitions ----------

int32 USimWorldHex::AddArm(const FArmState& Arm)
{
	int32 NewIndex = Arms.Add(Arm);
	// Clamp / sanitize a few runtime fields (defensive)
	FArmState& S = Arms[NewIndex];
	S.PC = S.PC % FMath::Max(1, S.Tape.Num());
	S.WaitCounter = 0;
	// Ensure facing in 0..5
	S.Facing = (EHexDir)((uint8)S.Facing % 6);
	S.HomeFacing = (EHexDir)((uint8)S.HomeFacing % 6);
	S.ArmLen = FMath::Clamp<int32>(S.ArmLen <= 0 ? 1 : S.ArmLen, 1, 6);
	S.HomeArmLen = FMath::Clamp<int32>(S.HomeArmLen <= 0 ? 1 : S.HomeArmLen, 1, 6);
	// Initialize rotation completed to true (no rotation in progress)
	S.bRotationCompleted = true;

	// Seed sim state to current values
	SimFacingByArm.Add(NewIndex, static_cast<uint8>(S.Facing));
	SimArmLenByArm.Add(NewIndex, S.ArmLen);
	SimGrabFacingByArm.Add(NewIndex, int8(-1)); // no held cluster yet

	UE_LOG(LogTemp, Warning, TEXT("SimWorldHex::AddArm - Added arm at (%d,%d) facing=%d assigned SimIndex=%d (total arms: %d)"),
		Arm.BaseHex.Q, Arm.BaseHex.R, (int32)Arm.Facing, NewIndex, Arms.Num());

	// Mark arm base cache as dirty so it gets rebuilt
	bArmBaseCacheDirty = true;
	
	return NewIndex;
}

int32 USimWorldHex::AddWalker(const FWalkerState& Walker)
{
	int32 NewIndex = Walkers.Add(Walker);
	FWalkerState& S = Walkers[NewIndex];
	S.BaseHex = Walker.BaseHex;
	S.HomeHex = Walker.HomeHex;
	S.Stance = EWalkerStance::Sitting;
	S.StanceTransitionProgress = 0;
	S.bProcessingRequest = false;
	
	// Initialize foot positions to outer tiles at ReachDistance
	// Find the axis-aligned tiles at ReachDistance for each direction
	// Quadrant mapping: D->S (R+), L->W (Q-), R->E (Q+), U->N (R-)
	const int32 ReachDist = FMath::Max(1, S.ReachDistance); // Ensure at least 1
	
	// For axis-aligned directions at ReachDistance:
	// D (South): Q unchanged, R + ReachDist
	// L (West): Q - ReachDist, R unchanged  
	// R (East): Q + ReachDist, R unchanged
	// U (North): Q unchanged, R - ReachDist
	FHex OuterTiles[4] = {
		FHex(S.HomeHex.Q, S.HomeHex.R + ReachDist),      // D -> S (South)
		FHex(S.HomeHex.Q - ReachDist, S.HomeHex.R),      // L -> W (West)
		FHex(S.HomeHex.Q + ReachDist, S.HomeHex.R),      // R -> E (East)
		FHex(S.HomeHex.Q, S.HomeHex.R - ReachDist)       // U -> N (North)
	};
	
	// Set foot positions to outer tiles
	S.DFoot.CurrentTile = OuterTiles[0];  // D -> S
	S.DFoot.TargetTile = S.DFoot.CurrentTile;
	S.DFoot.StepState = EFootStepState::Planted;
	
	S.LFoot.CurrentTile = OuterTiles[1];  // L -> W
	S.LFoot.TargetTile = S.LFoot.CurrentTile;
	S.LFoot.StepState = EFootStepState::Planted;
	
	S.RFoot.CurrentTile = OuterTiles[2];  // R -> E
	S.RFoot.TargetTile = S.RFoot.CurrentTile;
	S.RFoot.StepState = EFootStepState::Planted;
	
	S.UFoot.CurrentTile = OuterTiles[3];  // U -> N
	S.UFoot.TargetTile = S.UFoot.CurrentTile;
	S.UFoot.StepState = EFootStepState::Planted;
	
	// Legacy support
	S.PC = S.PC % FMath::Max(1, S.Program.Num());
	S.WaitCounter = 0;
	S.bMoving = false;
	S.MovementProgress = 0.0f;
	
	UE_LOG(LogTemp, Warning, TEXT("SimWorldHex::AddWalker - Added walker at (%d,%d) assigned SimIndex=%d (total walkers: %d)"),
		Walker.BaseHex.Q, Walker.BaseHex.R, NewIndex, Walkers.Num());

	return NewIndex;
}

int32 USimWorldHex::FindNearestAvailableWalker(const FHex& FromHex)
{
	int32 NearestIndex = INDEX_NONE;
	int32 NearestIdleIndex = INDEX_NONE;
	float NearestDist = MAX_flt;
	float NearestIdleDist = MAX_flt;
	
	for (int32 i = 0; i < Walkers.Num(); ++i)
	{
		const FWalkerState& W = Walkers[i];
		const bool bIsBusy = W.bProcessingRequest || W.Requests.Num() > 0;
		
		// Calculate hex distance
		const int32 dQ = W.BaseHex.Q - FromHex.Q;
		const int32 dR = W.BaseHex.R - FromHex.R;
		const float Dist = FMath::Max(FMath::Max(FMath::Abs(float(dQ)), FMath::Abs(float(dR))), FMath::Abs(float(dQ + dR)));
		
		// Track nearest ANY walker
		if (Dist < NearestDist)
		{
			NearestDist = Dist;
			NearestIndex = i;
		}

		// Track nearest IDLE walker
		if (!bIsBusy && Dist < NearestIdleDist)
		{
			NearestIdleDist = Dist;
			NearestIdleIndex = i;
		}
	}
	
	// Prefer idle walkers
	if (NearestIdleIndex != INDEX_NONE)
	{
		return NearestIdleIndex;
	}

	return NearestIndex;
}

bool USimWorldHex::RequestWalkerMoveTo(int32 WalkerIndex, const FHex& TargetHex, int32 Priority)
{
	if (WalkerIndex < 0 || WalkerIndex >= Walkers.Num())
	{
		UE_LOG(LogTemp, Warning, TEXT("SimWorldHex::RequestWalkerMoveTo: Invalid walker index %d (total: %d)"), WalkerIndex, Walkers.Num());
		return false;
	}
	
	FWalkerState& W = Walkers[WalkerIndex];
	
	FWalkerRequest Request;
	Request.TargetHex = TargetHex;
	Request.Priority = Priority;
	
	// Insert request sorted by priority (higher priority first)
	int32 InsertIndex = W.Requests.Num();
	for (int32 i = 0; i < W.Requests.Num(); ++i)
	{
		if (Request.Priority > W.Requests[i].Priority)
		{
			InsertIndex = i;
			break;
		}
	}
	W.Requests.Insert(Request, InsertIndex);
	
	UE_LOG(LogTemp, Log, TEXT("SimWorldHex::RequestWalkerMoveTo: Added request to walker %d to move to (%d, %d) - Total requests: %d, Stance: %d"),
		WalkerIndex, TargetHex.Q, TargetHex.R, W.Requests.Num(), (int32)W.Stance);
	
	return true;
}

bool USimWorldHex::TryPutItem(const FHex& H, const FItem& I)
{
	if (ItemMap.Contains(H))
	{
		return false; // already occupied
	}

	// Check for walker feet (Current or Target tiles)
	// EXCLUDE feet that are holding items (they're raised and not obstacles)
	for (const FWalkerState& W : Walkers)
	{
		if (!W.DFoot.bHoldingItem && (W.DFoot.CurrentTile == H || W.DFoot.TargetTile == H))
			return false; // occupied by walker foot
		if (!W.LFoot.bHoldingItem && (W.LFoot.CurrentTile == H || W.LFoot.TargetTile == H))
			return false;
		if (!W.RFoot.bHoldingItem && (W.RFoot.CurrentTile == H || W.RFoot.TargetTile == H))
			return false;
		if (!W.UFoot.bHoldingItem && (W.UFoot.CurrentTile == H || W.UFoot.TargetTile == H))
			return false;
	}

	FItem Copy = I;
	// Assign an Id if caller forgot (StepGlyphs sets it, but be safe)
	if (Copy.Id == 0)
	{
		Copy.Id = NextItemId++;
	}

	// Items placed by arms should start with bIsReady = false to prevent immediate belt pulling
	// This gives items at least one tick to settle before being moved by belts
	Copy.bIsReady = false;

	// Special case: Check if placing on a bond/unbond tile area
	const FTile* Tile = TileMap.Find(H);
	bool bIsOnBondArea = false;

	if (Tile && (Tile->Kind == ETileKind::Bond || Tile->Kind == ETileKind::Unbond))
	{
		// Placing directly on a primary bond/unbond tile
		bIsOnBondArea = true;
	}
	else
	{
		// Check if placing on a partner area of a bond/unbond glyph
		for (const auto& TileKV : TileMap)
		{
			const FTile& CheckTile = TileKV.Value;
			if (CheckTile.Kind != ETileKind::Bond && CheckTile.Kind != ETileKind::Unbond) continue;

			// Calculate this glyph's partner hex
			const uint8 CheckGlyphDir = (uint8)(CheckTile.Arg & 0xFF);
			const FHex CheckPartnerHex = HexUtil::Neighbor(TileKV.Key, (EHexDir)(CheckGlyphDir % 6));

			// If we're placing on this glyph's partner hex
			if (CheckPartnerHex == H)
			{
				bIsOnBondArea = true;
				break;
			}
		}
	}

	// Note: Items placed on bond/unbond areas (primary or partner) keep bIsReady = false
	// Items placed elsewhere also start with bIsReady = false to prevent immediate pulling
	// The bIsReady flag will be set to true at the end of the current simulation tick

	ItemMap.Add(H, Copy);
	OrbAt.Add(H, KindFromItem(Copy));

	// Add to NewlyPlacedItems to prevent belt processing this tick
	NewlyPlacedItems.Add(Copy.Id);

	return true;
}

bool USimWorldHex::TryTakeItem(const FHex& H, FItem& Out)
{
	if (FItem* Found = ItemMap.Find(H))
	{
		Out = *Found;
		ItemMap.Remove(H);
		OrbAt.Remove(H);
		// Belt progress bookkeeping
		if (Out.Id != 0)
		{
			BeltProgress.Remove(Out.Id);
		}
		return true;
	}
	return false;
}

// --------------------------------------------------

bool USimWorldHex::PlaceTile(const FHex& H, const FTile& T)
{
		// Create a mutable copy of the tile to potentially convert Bond -> InputBond/OutputBond/ClusterOutput
		FTile TileToPlace = T;

	if (T.Kind == ETileKind::Bond)
	{
		const FHex Partner = HexUtil::Neighbor(H, (EHexDir)(T.Arg % 6));
			const FTile* PartnerTile = TileMap.Find(Partner);
			const FTile* CurrentTile = TileMap.Find(H);

			// Auto-convert Bond tiles based on what they're placed between
			if (PartnerTile)
			{
				// Check if placing on Input tile with Input partner -> InputBond
				if (CurrentTile && CurrentTile->Kind == ETileKind::Input && PartnerTile->Kind == ETileKind::Input)
				{
					TileToPlace.Kind = ETileKind::InputBond;
					UE_LOG(LogTemp, Log, TEXT("Bond placed on Input tile with Input partner - converting to InputBond"));
				}
			// Check if placing on Output tile with Output partner -> ClusterOutput (merged)
			else if (CurrentTile && CurrentTile->Kind == ETileKind::Output && PartnerTile->Kind == ETileKind::Output)
			{
			// IMPORTANT: Save the orb types BEFORE modifying the TileMap!
			// Once we modify the TileMap, the PartnerTile pointer may become invalid or point to modified data
			uint8 CurrentOrbType = CurrentTile->Arg;
			uint8 PartnerOrbType = PartnerTile->Arg;
			
			UE_LOG(LogTemp, Log, TEXT("PlaceTile: Converting Output+Output to ClusterOutput"));
			UE_LOG(LogTemp, Log, TEXT("  CurrentTile at (%d,%d): Kind=%d Arg=%d"), H.Q, H.R, (int32)CurrentTile->Kind, CurrentOrbType);
			UE_LOG(LogTemp, Log, TEXT("  PartnerTile at (%d,%d): Kind=%d Arg=%d"), Partner.Q, Partner.R, (int32)PartnerTile->Kind, PartnerOrbType);
			
			// Auto-generate a cluster template for these two outputs
			uint8 TemplateId = AutoGenerateClusterTemplateForOutputs(H, Partner, CurrentOrbType, PartnerOrbType);
			
			TileToPlace.Kind = ETileKind::ClusterOutput;
			TileToPlace.Arg = TemplateId;
				
				// Also convert the partner tile to ClusterOutput with the same template
				FTile PartnerClusterTile = *PartnerTile;
				PartnerClusterTile.Kind = ETileKind::ClusterOutput;
				PartnerClusterTile.Arg = TemplateId;
				TileMap.Add(Partner, PartnerClusterTile);
				
				// Register the partnership for merged rendering
				IOBondPartners.Add(H, Partner);
				IOBondPartners.Add(Partner, H);
				
			// Store merged tile info for rendering (use the "primary" hex as key)
			FHex PrimaryHex = (H.Q < Partner.Q || (H.Q == Partner.Q && H.R < Partner.R)) ? H : Partner;
			FMergedTileInfo MergedInfo;
			MergedInfo.Hex1 = H;
			MergedInfo.Hex2 = Partner;
			MergedInfo.OrbType1 = CurrentOrbType;
			MergedInfo.OrbType2 = PartnerOrbType;
			MergedInfo.TemplateId = TemplateId;
			MergedClusterOutputs.Add(PrimaryHex, MergedInfo);
			
			UE_LOG(LogTemp, Log, TEXT("Bond placed on Output tiles - converting to merged ClusterOutput with template %d"), TemplateId);
			UE_LOG(LogTemp, Log, TEXT("  Hex1=(%d,%d) Type1=%d, Hex2=(%d,%d) Type2=%d, PrimaryKey=(%d,%d)"), 
				H.Q, H.R, CurrentOrbType, Partner.Q, Partner.R, PartnerOrbType, PrimaryHex.Q, PrimaryHex.R);
			}
			// Check if placing on ClusterOutput tile with Output partner -> Expand cluster
			else if (CurrentTile && CurrentTile->Kind == ETileKind::ClusterOutput && PartnerTile->Kind == ETileKind::Output)
			{
				uint8 ExistingTemplateId = CurrentTile->Arg;
				uint8 NewOrbType = PartnerTile->Arg;
				
				// Find all tiles in the existing cluster
				TArray<FHex> ExistingClusterTiles = FindAllTilesInCluster(H);
				
				// Find the center-most tile from EXISTING cluster
				int32 OldSumQ = 0;
				int32 OldSumR = 0;
				for (const FHex& ClusterHex : ExistingClusterTiles)
				{
					OldSumQ += ClusterHex.Q;
					OldSumR += ClusterHex.R;
				}
				
				int32 OldCenterQ = FMath::RoundToInt((float)OldSumQ / ExistingClusterTiles.Num());
				int32 OldCenterR = FMath::RoundToInt((float)OldSumR / ExistingClusterTiles.Num());
				
				FHex OldBaseHex = ExistingClusterTiles[0];
				int32 OldMinDist = INT_MAX;
				for (const FHex& ClusterHex : ExistingClusterTiles)
				{
					int32 dQ = ClusterHex.Q - OldCenterQ;
					int32 dR = ClusterHex.R - OldCenterR;
					int32 dist = FMath::Abs(dQ) + FMath::Abs(dR) + FMath::Abs(dQ + dR);
					if (dist < OldMinDist)
					{
						OldMinDist = dist;
						OldBaseHex = ClusterHex;
					}
				}
				
				// Build the new cluster including the new tile
				TArray<FHex> AllClusterHexes = ExistingClusterTiles;
				AllClusterHexes.Add(Partner);
				
				// Find the center-most tile from the EXPANDED cluster
				int32 NewSumQ = OldSumQ + Partner.Q;
				int32 NewSumR = OldSumR + Partner.R;
				int32 NewCenterQ = FMath::RoundToInt((float)NewSumQ / AllClusterHexes.Num());
				int32 NewCenterR = FMath::RoundToInt((float)NewSumR / AllClusterHexes.Num());
				
				FHex NewBaseHex = AllClusterHexes[0];
				int32 NewMinDist = INT_MAX;
				for (const FHex& ClusterHex : AllClusterHexes)
				{
					int32 dQ = ClusterHex.Q - NewCenterQ;
					int32 dR = ClusterHex.R - NewCenterR;
					int32 dist = FMath::Abs(dQ) + FMath::Abs(dR) + FMath::Abs(dQ + dR);
					if (dist < NewMinDist)
					{
						NewMinDist = dist;
						NewBaseHex = ClusterHex;
					}
				}
				
				// Build a map of hex -> orb type for all tiles
				TMap<FHex, uint8> HexToOrbType;
				for (const FHex& ClusterHex : ExistingClusterTiles)
				{
					// Get orb type from MergedClusterOutputs
					for (const auto& MergedKV : MergedClusterOutputs)
					{
						if (MergedKV.Value.Hex1 == ClusterHex)
						{
							HexToOrbType.Add(ClusterHex, MergedKV.Value.OrbType1);
							break;
						}
						else if (MergedKV.Value.Hex2 == ClusterHex)
						{
							HexToOrbType.Add(ClusterHex, MergedKV.Value.OrbType2);
							break;
						}
					}
				}
				HexToOrbType.Add(Partner, NewOrbType);
				
				// Expand the template (overwrites same template ID)
				uint8 ExpandedTemplateId = ExpandClusterTemplate(ExistingTemplateId, OldBaseHex, NewBaseHex, AllClusterHexes, HexToOrbType);
				
				// No need to update existing tiles - template ID stays the same!
				UE_LOG(LogTemp, Warning, TEXT("→ Template %d expanded in place, %d existing tiles keep same ID"), ExpandedTemplateId, ExistingClusterTiles.Num());
				
				// Convert partner to ClusterOutput with the same template ID
				FTile PartnerClusterTile = *PartnerTile;
				PartnerClusterTile.Kind = ETileKind::ClusterOutput;
				PartnerClusterTile.Arg = ExpandedTemplateId;
				TileMap.Add(Partner, PartnerClusterTile);
				
				// Register partnership between H and Partner
				IOBondPartners.Add(H, Partner);
				IOBondPartners.Add(Partner, H);
				
				// Rebuild all MergedClusterOutputs for this cluster
				// First, remove all old merged entries for this cluster
				TArray<FHex> KeysToRemove;
				for (const auto& MergedKV : MergedClusterOutputs)
				{
					if (ExistingClusterTiles.Contains(MergedKV.Value.Hex1) || ExistingClusterTiles.Contains(MergedKV.Value.Hex2))
					{
						KeysToRemove.Add(MergedKV.Key);
					}
				}
				for (const FHex& Key : KeysToRemove)
				{
					MergedClusterOutputs.Remove(Key);
				}
				
				// Now rebuild merged outputs for all connected pairs in the expanded cluster
				TArray<FHex> AllClusterTiles = ExistingClusterTiles;
				AllClusterTiles.Add(Partner);
				
				for (const FHex& TileHex : AllClusterTiles)
				{
					// Find all partners of this tile
					for (const auto& PartnerKV : IOBondPartners)
					{
						if (PartnerKV.Key == TileHex && AllClusterTiles.Contains(PartnerKV.Value))
						{
							FHex PartnerHex = PartnerKV.Value;
							
							// Only create merged info once per pair (use the "primary" hex as key)
							FHex PrimaryHex = (TileHex.Q < PartnerHex.Q || (TileHex.Q == PartnerHex.Q && TileHex.R < PartnerHex.R)) ? TileHex : PartnerHex;
							
							if (!MergedClusterOutputs.Contains(PrimaryHex))
							{
								// Get orb types from the template
								const FClusterTemplate* Template = ClusterTemplates.Find(ExpandedTemplateId);
								if (Template)
								{
									// Find the orb types for these two hexes
									uint8 OrbType1 = 0;
									uint8 OrbType2 = 0;
									
									for (const FOrbData& OrbData : Template->Orbs)
									{
										FHex OrbHex;
										OrbHex.Q = NewBaseHex.Q + OrbData.dQ;
										OrbHex.R = NewBaseHex.R + OrbData.dR;
										
										if (OrbHex == TileHex)
											OrbType1 = OrbData.Type;
										if (OrbHex == PartnerHex)
											OrbType2 = OrbData.Type;
									}
									
									FMergedTileInfo NewMergedInfo;
									NewMergedInfo.Hex1 = TileHex;
									NewMergedInfo.Hex2 = PartnerHex;
									NewMergedInfo.OrbType1 = OrbType1;
									NewMergedInfo.OrbType2 = OrbType2;
									NewMergedInfo.TemplateId = ExpandedTemplateId;
									MergedClusterOutputs.Add(PrimaryHex, NewMergedInfo);
								}
							}
						}
					}
				}
				
				UE_LOG(LogTemp, Log, TEXT("Bond placed on ClusterOutput + Output - expanded cluster from template %d to %d with %d total tiles"), 
					ExistingTemplateId, ExpandedTemplateId, AllClusterTiles.Num());
				
				// Don't place the bond tile - the ClusterOutput tile should remain as-is
				return true;
			}
			// Check if placing on Output tile with ClusterOutput partner -> Expand cluster
			else if (CurrentTile && CurrentTile->Kind == ETileKind::Output && PartnerTile->Kind == ETileKind::ClusterOutput)
			{
				uint8 ExistingTemplateId = PartnerTile->Arg;
				uint8 NewOrbType = CurrentTile->Arg;
				
				// Find all tiles in the existing cluster
				TArray<FHex> ExistingClusterTiles = FindAllTilesInCluster(Partner);
				
				// Find the center-most tile from EXISTING cluster
				int32 OldSumQ = 0;
				int32 OldSumR = 0;
				for (const FHex& ClusterHex : ExistingClusterTiles)
				{
					OldSumQ += ClusterHex.Q;
					OldSumR += ClusterHex.R;
				}
				
				int32 OldCenterQ = FMath::RoundToInt((float)OldSumQ / ExistingClusterTiles.Num());
				int32 OldCenterR = FMath::RoundToInt((float)OldSumR / ExistingClusterTiles.Num());
				
				FHex OldBaseHex = ExistingClusterTiles[0];
				int32 OldMinDist = INT_MAX;
				for (const FHex& ClusterHex : ExistingClusterTiles)
				{
					int32 dQ = ClusterHex.Q - OldCenterQ;
					int32 dR = ClusterHex.R - OldCenterR;
					int32 dist = FMath::Abs(dQ) + FMath::Abs(dR) + FMath::Abs(dQ + dR);
					if (dist < OldMinDist)
					{
						OldMinDist = dist;
						OldBaseHex = ClusterHex;
					}
				}
				
				// Build the new cluster including the new tile
				TArray<FHex> AllClusterHexes = ExistingClusterTiles;
				AllClusterHexes.Add(H);
				
				// Find the center-most tile from the EXPANDED cluster
				int32 SumQ = 0;
				int32 SumR = 0;
				for (const FHex& ClusterHex : AllClusterHexes)
				{
					SumQ += ClusterHex.Q;
					SumR += ClusterHex.R;
				}
				
				int32 CenterQ = FMath::RoundToInt((float)SumQ / AllClusterHexes.Num());
				int32 CenterR = FMath::RoundToInt((float)SumR / AllClusterHexes.Num());
				
				FHex NewBaseHex = AllClusterHexes[0];
				int32 MinDist = INT_MAX;
				for (const FHex& ClusterHex : AllClusterHexes)
				{
					int32 dQ = ClusterHex.Q - CenterQ;
					int32 dR = ClusterHex.R - CenterR;
					int32 dist = FMath::Abs(dQ) + FMath::Abs(dR) + FMath::Abs(dQ + dR);
					if (dist < MinDist)
					{
						MinDist = dist;
						NewBaseHex = ClusterHex;
					}
				}
				
				// Build a map of hex -> orb type for all tiles
				TMap<FHex, uint8> HexToOrbType;
				for (const FHex& ClusterHex : ExistingClusterTiles)
				{
					// Get orb type from MergedClusterOutputs
					for (const auto& MergedKV : MergedClusterOutputs)
					{
						if (MergedKV.Value.Hex1 == ClusterHex)
						{
							HexToOrbType.Add(ClusterHex, MergedKV.Value.OrbType1);
							break;
						}
						else if (MergedKV.Value.Hex2 == ClusterHex)
						{
							HexToOrbType.Add(ClusterHex, MergedKV.Value.OrbType2);
							break;
						}
					}
				}
				HexToOrbType.Add(H, NewOrbType);
				
				// Expand the template to include all tiles
				uint8 ExpandedTemplateId = ExpandClusterTemplate(ExistingTemplateId, OldBaseHex, NewBaseHex, AllClusterHexes, HexToOrbType);
				
				// Update ALL tiles in the existing cluster to use the new template ID
				UE_LOG(LogTemp, Warning, TEXT("→ Updating %d existing tiles to use new template %d:"), ExistingClusterTiles.Num(), ExpandedTemplateId);
				for (const FHex& ClusterHex : ExistingClusterTiles)
				{
					FTile* ClusterTile = TileMap.Find(ClusterHex);
					if (ClusterTile && ClusterTile->Kind == ETileKind::ClusterOutput)
					{
						uint8 OldTemplateId = ClusterTile->Arg;
						ClusterTile->Arg = ExpandedTemplateId;
						UE_LOG(LogTemp, Warning, TEXT("  ✓ Tile (%d,%d): Template %d -> %d"), 
							ClusterHex.Q, ClusterHex.R, OldTemplateId, ExpandedTemplateId);
					}
				}
				
				// Convert current tile to ClusterOutput with the new template
				TileToPlace.Kind = ETileKind::ClusterOutput;
				TileToPlace.Arg = ExpandedTemplateId;
				
				// Register partnership between H and Partner
				IOBondPartners.Add(H, Partner);
				IOBondPartners.Add(Partner, H);
				
				// Rebuild all MergedClusterOutputs for this cluster
				// First, remove all old merged entries for this cluster
				TArray<FHex> KeysToRemove;
				for (const auto& MergedKV : MergedClusterOutputs)
				{
					if (ExistingClusterTiles.Contains(MergedKV.Value.Hex1) || ExistingClusterTiles.Contains(MergedKV.Value.Hex2))
					{
						KeysToRemove.Add(MergedKV.Key);
					}
				}
				for (const FHex& Key : KeysToRemove)
				{
					MergedClusterOutputs.Remove(Key);
				}
				
				// Now rebuild merged outputs for all connected pairs in the expanded cluster
				TArray<FHex> AllClusterTiles = ExistingClusterTiles;
				AllClusterTiles.Add(H);
				
				for (const FHex& TileHex : AllClusterTiles)
				{
					// Find all partners of this tile
					for (const auto& PartnerKV : IOBondPartners)
					{
						if (PartnerKV.Key == TileHex && AllClusterTiles.Contains(PartnerKV.Value))
						{
							FHex PartnerHex = PartnerKV.Value;
							
							// Only create merged info once per pair (use the "primary" hex as key)
							FHex PrimaryHex = (TileHex.Q < PartnerHex.Q || (TileHex.Q == PartnerHex.Q && TileHex.R < PartnerHex.R)) ? TileHex : PartnerHex;
							
							if (!MergedClusterOutputs.Contains(PrimaryHex))
							{
								// Get orb types from the template
								const FClusterTemplate* Template = ClusterTemplates.Find(ExpandedTemplateId);
								if (Template)
								{
									// Find the orb types for these two hexes
									uint8 OrbType1 = 0;
									uint8 OrbType2 = 0;
									
									for (const FOrbData& OrbData : Template->Orbs)
									{
										FHex OrbHex;
										OrbHex.Q = NewBaseHex.Q + OrbData.dQ;
										OrbHex.R = NewBaseHex.R + OrbData.dR;
										
										if (OrbHex == TileHex)
											OrbType1 = OrbData.Type;
										if (OrbHex == PartnerHex)
											OrbType2 = OrbData.Type;
									}
									
									FMergedTileInfo NewMergedInfo;
									NewMergedInfo.Hex1 = TileHex;
									NewMergedInfo.Hex2 = PartnerHex;
									NewMergedInfo.OrbType1 = OrbType1;
									NewMergedInfo.OrbType2 = OrbType2;
									NewMergedInfo.TemplateId = ExpandedTemplateId;
									MergedClusterOutputs.Add(PrimaryHex, NewMergedInfo);
								}
							}
						}
					}
				}
				
				UE_LOG(LogTemp, Log, TEXT("Bond placed on Output + ClusterOutput - expanded cluster template %d to %d total tiles"), 
					ExpandedTemplateId, AllClusterTiles.Num());
			}
				// Check if placing on empty with Input partner -> InputBond
				else if ((!CurrentTile || CurrentTile->Kind == ETileKind::Empty) && PartnerTile->Kind == ETileKind::Input)
				{
					TileToPlace.Kind = ETileKind::InputBond;
					UE_LOG(LogTemp, Log, TEXT("Bond placed between Input tiles - converting to InputBond"));
				}
			// Check if placing on empty with Output partner -> need another Output to make ClusterOutput
			else if ((!CurrentTile || CurrentTile->Kind == ETileKind::Empty) && PartnerTile->Kind == ETileKind::Output)
			{
				// Can't create cluster output with only one output tile
				TileToPlace.Kind = ETileKind::OutputBond;
				UE_LOG(LogTemp, Log, TEXT("Bond placed on empty with Output partner - converting to OutputBond"));
			}
			// Check if placing on empty with ClusterOutput partner -> can't extend without an Output tile here
			else if ((!CurrentTile || CurrentTile->Kind == ETileKind::Empty) && PartnerTile->Kind == ETileKind::ClusterOutput)
			{
				TileToPlace.Kind = ETileKind::OutputBond;
				UE_LOG(LogTemp, Log, TEXT("Bond placed on empty with ClusterOutput partner - converting to OutputBond"));
			}
			// Standard Bond tile validation (not Input or Output)
			// Allow bonds to be placed on/between: Empty, Bond, Input, Output, InputBond, OutputBond, ClusterInput, ClusterOutput
			else if (PartnerTile->Kind != ETileKind::Empty && 
					 PartnerTile->Kind != ETileKind::Bond &&
					 PartnerTile->Kind != ETileKind::Input &&
					 PartnerTile->Kind != ETileKind::Output &&
					 PartnerTile->Kind != ETileKind::InputBond &&
					 PartnerTile->Kind != ETileKind::OutputBond &&
					 PartnerTile->Kind != ETileKind::ClusterInput &&
					 PartnerTile->Kind != ETileKind::ClusterOutput)
			{
				UE_LOG(LogTemp, Warning, TEXT("Bond placement blocked: partner hex occupied by incompatible tile type."));
			return false;
		}
	}
		}
	if (TileToPlace.Kind == ETileKind::Unbond)
	{
		const FHex Partner = HexUtil::Neighbor(H, (EHexDir)(TileToPlace.Arg % 6));
		if (TileMap.Contains(Partner) &&
			TileMap[Partner].Kind != ETileKind::Empty &&
			TileMap[Partner].Kind != ETileKind::Unbond)
		{
			UE_LOG(LogTemp, Warning, TEXT("Unbond placement blocked: partner hex occupied."));
			return false;
		}
	}
	if (TileToPlace.Kind == ETileKind::InputBond)
	{
		const FHex Partner = HexUtil::Neighbor(H, (EHexDir)(TileToPlace.Arg % 6));
		// Verify partner tile is an Input tile
		const FTile* PartnerTile = TileMap.Find(Partner);
		if (!PartnerTile || PartnerTile->Kind != ETileKind::Input)
		{
			UE_LOG(LogTemp, Warning, TEXT("InputBond placement blocked: partner must be an Input tile."));
			return false;
		}
		// Register the bond partnership
		IOBondPartners.Add(H, Partner);
		IOBondPartners.Add(Partner, H);
	}
	if (TileToPlace.Kind == ETileKind::OutputBond)
	{
		const FHex Partner = HexUtil::Neighbor(H, (EHexDir)(TileToPlace.Arg % 6));
		// Verify partner tile is an Output tile
		const FTile* PartnerTile = TileMap.Find(Partner);
		if (!PartnerTile || PartnerTile->Kind != ETileKind::Output)
		{
			UE_LOG(LogTemp, Warning, TEXT("OutputBond placement blocked: partner must be an Output tile."));
			return false;
		}
		// Register the bond partnership
		IOBondPartners.Add(H, Partner);
		IOBondPartners.Add(Partner, H);
	}
	if (TileToPlace.Kind == ETileKind::Empty)
	{
		const bool bHad = TileMap.Remove(H) > 0;
		if (bHad)
		{
			// NOTE: We NO LONGER remove bonds whose Root == H.
			// Bonds now persist until explicitly broken by a glyph.
			ItemMap.Remove(H);
			OrbAt.Remove(H);
			
			// Remove any IO bond partnerships
			if (const FHex* Partner = IOBondPartners.Find(H))
			{
				IOBondPartners.Remove(*Partner);
				IOBondPartners.Remove(H);
			}
			
			++TileRevision;
		}
		return true;
	}

	TileMap.Add(H, TileToPlace);
	++TileRevision;
	
	// ClusterOutput tiles are now directly placeable, no auto-conversion
	
	return true;
}

// --- Cluster Template API ---
void USimWorldHex::RegisterClusterTemplate(uint8 TemplateId, const TArray<FOrbData>& Orbs, const TArray<TPair<int32, int32>>& BondPairs, const FString& Name)
{
	FClusterTemplate NewTemplate;
	NewTemplate.Orbs = Orbs;
	NewTemplate.Bonds = BondPairs;
	NewTemplate.Name = Name;
	ClusterTemplates.Add(TemplateId, NewTemplate);
	
	UE_LOG(LogTemp, Log, TEXT("======================================"));
	UE_LOG(LogTemp, Log, TEXT("Registered cluster template %d: %s"), TemplateId, *Name);
	UE_LOG(LogTemp, Log, TEXT("  Orbs: %d"), Orbs.Num());
	for (int32 i = 0; i < Orbs.Num(); i++)
	{
		const FOrbData& Orb = Orbs[i];
		UE_LOG(LogTemp, Log, TEXT("    [%d] Type=%d at relative (%d,%d)"), i, Orb.Type, Orb.dQ, Orb.dR);
	}
	UE_LOG(LogTemp, Log, TEXT("  Bonds: %d"), BondPairs.Num());
	for (int32 i = 0; i < BondPairs.Num(); i++)
	{
		const TPair<int32, int32>& Bond = BondPairs[i];
		UE_LOG(LogTemp, Log, TEXT("    Bond %d <-> %d"), Bond.Key, Bond.Value);
	}
	UE_LOG(LogTemp, Log, TEXT("======================================"));
}

bool USimWorldHex::ValidateClusterAgainstTemplate(const FItem& Cluster, uint8 TemplateId) const
{
	const FClusterTemplate* Template = ClusterTemplates.Find(TemplateId);
	if (!Template) return false;
	
	if (!Cluster.bIsCluster) return false;
	if (Cluster.Orbs.Num() != Template->Orbs.Num()) return false;
	
	// TODO: Implement full pattern matching with rotation support
	// For now, just check orb count and types
	for (int32 i = 0; i < Template->Orbs.Num(); i++)
	{
		bool bFoundMatch = false;
		for (const FOrbData& ClusterOrb : Cluster.Orbs)
		{
			if (ClusterOrb.Type == Template->Orbs[i].Type)
			{
				bFoundMatch = true;
				break;
			}
		}
		if (!bFoundMatch) return false;
	}
	
	return true;
}

uint8 USimWorldHex::AutoGenerateClusterTemplateForOutputs(const FHex& Hex1, const FHex& Hex2, uint8 OrbType1, uint8 OrbType2)
{
	// Calculate relative position of Hex2 from Hex1
	int32 dQ = Hex2.Q - Hex1.Q;
	int32 dR = Hex2.R - Hex1.R;
	
	// Create a 2-orb cluster template
	TArray<FOrbData> Orbs;
	
	FOrbData Orb1;
	Orb1.Type = OrbType1;
	Orb1.dQ = 0;
	Orb1.dR = 0;
	Orbs.Add(Orb1);
	
	FOrbData Orb2;
	Orb2.Type = OrbType2;
	Orb2.dQ = dQ;
	Orb2.dR = dR;
	Orbs.Add(Orb2);
	
	// Create bond between the two orbs
	TArray<TPair<int32, int32>> BondPairs;
	BondPairs.Add(TPair<int32, int32>(0, 1));
	
	// Generate unique template ID
	uint8 TemplateId = NextAutoTemplateId++;
	
	// Register the template
	FString TemplateName = FString::Printf(TEXT("AutoGen_%d_%d_at_%d,%d"), OrbType1, OrbType2, Hex1.Q, Hex1.R);
	RegisterClusterTemplate(TemplateId, Orbs, BondPairs, TemplateName);
	
	return TemplateId;
}

void USimWorldHex::AutoConvertAdjacentOutputsToCluster(const FHex& NewOutputHex, uint8 NewOrbType)
{
	// Check all 6 neighbors for Output or ClusterOutput tiles
	TArray<FHex> AdjacentOutputs;
	
	for (int32 Dir = 0; Dir < 6; Dir++)
	{
		FHex NeighborHex = HexUtil::Neighbor(NewOutputHex, (EHexDir)Dir);
		const FTile* NeighborTile = TileMap.Find(NeighborHex);
		
		if (NeighborTile && (NeighborTile->Kind == ETileKind::Output || NeighborTile->Kind == ETileKind::ClusterOutput))
		{
			AdjacentOutputs.Add(NeighborHex);
		}
	}
	
	// If no adjacent outputs, nothing to convert
	if (AdjacentOutputs.Num() == 0)
	{
		return;
	}
	
	UE_LOG(LogTemp, Log, TEXT("AutoConvert: Output at (%d,%d) type %d has %d adjacent outputs/clusters"), 
		NewOutputHex.Q, NewOutputHex.R, NewOrbType, AdjacentOutputs.Num());
	
	// Convert the newly placed output to ClusterOutput
	FTile* NewTile = TileMap.Find(NewOutputHex);
	if (NewTile)
	{
		NewTile->Kind = ETileKind::ClusterOutput;
		NewTile->Arg = NewOrbType; // Keep the orb type
	}
	
	// Convert all adjacent Output tiles to ClusterOutput (keeping their orb types)
	for (const FHex& AdjacentHex : AdjacentOutputs)
	{
		FTile* AdjacentTile = TileMap.Find(AdjacentHex);
		if (AdjacentTile)
		{
			if (AdjacentTile->Kind == ETileKind::Output)
			{
				// Convert Output to ClusterOutput, preserving the orb type
				AdjacentTile->Kind = ETileKind::ClusterOutput;
				// Arg already contains the orb type, so no change needed
				
				UE_LOG(LogTemp, Log, TEXT("  Converted adjacent Output at (%d,%d) type %d to ClusterOutput"), 
					AdjacentHex.Q, AdjacentHex.R, AdjacentTile->Arg);
			}
			else if (AdjacentTile->Kind == ETileKind::ClusterOutput)
			{
				// Already a ClusterOutput, no change needed
				UE_LOG(LogTemp, Log, TEXT("  Adjacent tile at (%d,%d) type %d already ClusterOutput"), 
					AdjacentHex.Q, AdjacentHex.R, AdjacentTile->Arg);
			}
		}
	}
	
	++TileRevision;
}

uint8 USimWorldHex::ExpandClusterTemplate(uint8 ExistingTemplateId, const FHex& OldBaseHex, const FHex& NewBaseHex, const TArray<FHex>& AllClusterHexes, const TMap<FHex, uint8>& HexToOrbType)
{
	UE_LOG(LogTemp, Warning, TEXT("╔════════════════════════════════════════"));
	UE_LOG(LogTemp, Warning, TEXT("║ EXPANDING CLUSTER TEMPLATE %d"), ExistingTemplateId);
	UE_LOG(LogTemp, Warning, TEXT("╠════════════════════════════════════════"));
	
	// Get the existing template
	const FClusterTemplate* ExistingTemplate = ClusterTemplates.Find(ExistingTemplateId);
	if (!ExistingTemplate)
	{
		UE_LOG(LogTemp, Error, TEXT("║ ERROR: Template %d not found!"), ExistingTemplateId);
		UE_LOG(LogTemp, Warning, TEXT("╚════════════════════════════════════════"));
		return ExistingTemplateId;
	}
	
	UE_LOG(LogTemp, Warning, TEXT("║ Old template had %d orbs, %d bonds"), ExistingTemplate->Orbs.Num(), ExistingTemplate->Bonds.Num());
	UE_LOG(LogTemp, Warning, TEXT("║ OldBaseHex: (%d,%d)"), OldBaseHex.Q, OldBaseHex.R);
	UE_LOG(LogTemp, Warning, TEXT("║ NewBaseHex: (%d,%d) %s"), 
		NewBaseHex.Q, NewBaseHex.R, 
		(OldBaseHex.Q == NewBaseHex.Q && OldBaseHex.R == NewBaseHex.R) ? TEXT("[SAME]") : TEXT("[CHANGED]"));
	UE_LOG(LogTemp, Warning, TEXT("║ Total cluster tiles: %d"), AllClusterHexes.Num());
	
	// Rebuild the entire template relative to the new BaseHex
	// This ensures all coordinates are correct even if BaseHex changed
	TArray<FOrbData> NewOrbs;
	UE_LOG(LogTemp, Warning, TEXT("║"));
	UE_LOG(LogTemp, Warning, TEXT("║ Rebuilding orbs relative to NewBaseHex:"));
	for (const FHex& ClusterHex : AllClusterHexes)
	{
		FOrbData NewOrb;
		const uint8* OrbType = HexToOrbType.Find(ClusterHex);
		NewOrb.Type = OrbType ? *OrbType : 0;
		NewOrb.dQ = ClusterHex.Q - NewBaseHex.Q;
		NewOrb.dR = ClusterHex.R - NewBaseHex.R;
		NewOrbs.Add(NewOrb);
		
		UE_LOG(LogTemp, Warning, TEXT("║   Orb %d: World(%d,%d) -> Relative(%d,%d) Type=%d"), 
			NewOrbs.Num() - 1, ClusterHex.Q, ClusterHex.R, NewOrb.dQ, NewOrb.dR, NewOrb.Type);
	}
	
	// Rebuild bonds between all adjacent tiles in the cluster
	TArray<TPair<int32, int32>> NewBondPairs;
	UE_LOG(LogTemp, Warning, TEXT("║"));
	UE_LOG(LogTemp, Warning, TEXT("║ Rebuilding bonds between adjacent tiles:"));
	for (int32 i = 0; i < AllClusterHexes.Num(); i++)
	{
		for (int32 j = i + 1; j < AllClusterHexes.Num(); j++)
		{
			const FHex& Hex1 = AllClusterHexes[i];
			const FHex& Hex2 = AllClusterHexes[j];
			
			// Check if adjacent (hex distance == 1)
			int32 dQ = Hex2.Q - Hex1.Q;
			int32 dR = Hex2.R - Hex1.R;
			int32 dist = (FMath::Abs(dQ) + FMath::Abs(dR) + FMath::Abs(dQ + dR)) / 2;
			
			if (dist == 1)
			{
				NewBondPairs.Add(TPair<int32, int32>(i, j));
				UE_LOG(LogTemp, Warning, TEXT("║   Bond: Orb[%d] <-> Orb[%d]  (Hexes (%d,%d)-(%d,%d))"), 
					i, j, Hex1.Q, Hex1.R, Hex2.Q, Hex2.R);
			}
		}
	}
	
	// REUSE the existing template ID instead of generating a new one
	UE_LOG(LogTemp, Warning, TEXT("║"));
	UE_LOG(LogTemp, Warning, TEXT("║ REUSING TEMPLATE ID: %d (keeping same ID)"), ExistingTemplateId);
	UE_LOG(LogTemp, Warning, TEXT("║ Total orbs: %d"), NewOrbs.Num());
	UE_LOG(LogTemp, Warning, TEXT("║ Total bonds: %d"), NewBondPairs.Num());
	UE_LOG(LogTemp, Warning, TEXT("╚════════════════════════════════════════"));
	
	// Overwrite the existing template with the expanded one
	FString TemplateName = FString::Printf(TEXT("Expanded_%d_%dorbs"), ExistingTemplateId, NewOrbs.Num());
	RegisterClusterTemplate(ExistingTemplateId, NewOrbs, NewBondPairs, TemplateName);
	
	return ExistingTemplateId;
}

TArray<FHex> USimWorldHex::FindAllTilesInCluster(const FHex& StartHex) const
{
	TArray<FHex> ClusterTiles;
	TSet<FHex> Visited;
	TArray<FHex> ToVisit;
	
	ToVisit.Add(StartHex);
	Visited.Add(StartHex);
	
	UE_LOG(LogTemp, Warning, TEXT("FindAllTilesInCluster: Starting from (%d,%d)"), StartHex.Q, StartHex.R);
	
	while (ToVisit.Num() > 0)
	{
		FHex CurrentHex = ToVisit.Pop();
		ClusterTiles.Add(CurrentHex);
		
		UE_LOG(LogTemp, Warning, TEXT("  Processing tile (%d,%d)"), CurrentHex.Q, CurrentHex.R);
		
		// Find all partners of this hex (check BOTH directions since IOBondPartners is not symmetric)
		for (const auto& PartnerKV : IOBondPartners)
		{
			FHex PartnerHex;
			bool bFoundPartner = false;
			
			// Check if CurrentHex is the KEY (A->B, we're at A)
			if (PartnerKV.Key == CurrentHex)
			{
				PartnerHex = PartnerKV.Value;
				bFoundPartner = true;
			}
			// Check if CurrentHex is the VALUE (A->B, we're at B)
			else if (PartnerKV.Value == CurrentHex)
			{
				PartnerHex = PartnerKV.Key;
				bFoundPartner = true;
			}
			
			if (bFoundPartner)
			{
				// Check if partner is a ClusterOutput tile
				const FTile* PartnerTile = TileMap.Find(PartnerHex);
				if (PartnerTile && PartnerTile->Kind == ETileKind::ClusterOutput)
				{
					if (!Visited.Contains(PartnerHex))
					{

						Visited.Add(PartnerHex);
						ToVisit.Add(PartnerHex);
					}
				}
			}
		}
	}
	
	UE_LOG(LogTemp, Warning, TEXT("  Total tiles found in cluster: %d"), ClusterTiles.Num());
	
	return ClusterTiles;
}

// --- Bond-breaking public API (called by future glyph logic) ---
bool USimWorldHex::BreakBondBetween(int32 ItemA, int32 ItemB)
{
	for (int32 i = 0; i < Bonds.Num(); ++i)
	{
		const FBond& B = Bonds[i];
		if ((B.ItemA == ItemA && B.ItemB == ItemB) ||
			(B.ItemA == ItemB && B.ItemB == ItemA))
		{
			RemoveBondIndex(i);
			return true;
		}
	}
	return false;
}

int32 USimWorldHex::BreakAllBondsForItem(int32 ItemId)
{
	int32 Removed = 0;
	// Gather first to avoid index churn while removing
	TArray<int32> ToRemove;
	for (int32 i = 0; i < Bonds.Num(); ++i)
	{
		const FBond& B = Bonds[i];
		if (B.ItemA == ItemId || B.ItemB == ItemId)
			ToRemove.Add(i);
	}
	// Remove in reverse order
	for (int32 idx = ToRemove.Num() - 1; idx >= 0; --idx)
	{
		RemoveBondIndex(ToRemove[idx]);
		++Removed;
	}
	return Removed;
}


// --- CleanupBrokenBonds: only remove if an item vanished; otherwise update offsets ---
void USimWorldHex::CleanupBrokenBonds(const TSet<int32>& HeldIds)
{
	if (Bonds.Num() == 0) return;

	// Reconstruct item positions, including synthetic positions for held items (so bonds persist while held)
	TMap<int32, FHex> ItemPos;
	ItemPos.Reserve(ItemMap.Num() + HeldIds.Num());

	for (const auto& KV : ItemMap)
	{
		if (KV.Value.Id != 0)
			ItemPos.Add(KV.Value.Id, KV.Key);
	}

	for (int32 HeldId : HeldIds)
	{
		if (const FHex* Last = HeldItemLastHex.Find(HeldId))
		{
			// Only add if not already present (should not be, since held items are removed from ItemMap)
			ItemPos.FindOrAdd(HeldId, *Last);
		}
	}

	for (int32 i = Bonds.Num() - 1; i >= 0; --i)
	{
		FBond& B = Bonds[i];
		const FHex* AHex = ItemPos.Find(B.ItemA);
		const FHex* BHex = ItemPos.Find(B.ItemB);

		// If either truly gone (not on board and no last-held position), remove
		if (!AHex || !BHex)
		{
			RemoveBondIndex(i);
			continue;
		}

		// Update stored relative offset if it changed (items may have translated / rotated logically)
		const int32 dq = BHex->Q - AHex->Q;
		const int32 dr = BHex->R - AHex->R;
		if (dq != B.dQ || dr != B.dR)
		{
			B.dQ = (int8)dq;
			B.dR = (int8)dr;
			B.Root = *AHex; // keep root near first item for debug
			if (bDevDebugBonds && GEngine)
			{
				GEngine->AddOnScreenDebugMessage(-1, 0.3f, FColor::Silver,
					FString::Printf(TEXT("Bond offset updated (held ok): %d-%d dq=%d dr=%d"),
						B.ItemA, B.ItemB, dq, dr));
			}
		}

		// Update HeldItemLastHex for held items to their current virtual positions
		// This ensures the virtual positions stay current as the arm moves/rotates
		if (HeldIds.Contains(B.ItemA))
		{
			HeldItemLastHex.FindOrAdd(B.ItemA, *AHex);
		}
		if (HeldIds.Contains(B.ItemB))
		{
			HeldItemLastHex.FindOrAdd(B.ItemB, *BHex);
		}
	}
}

void USimWorldHex::MoveBondedClusterOnBelt(int32 SeedItemId, const FHex& FromHex, const FHex& ToHex, const FTile& BeltTile, TMap<int32, FHex>& ItemHex)
{
	// Recursively move all bonded items when one item in a cluster moves on a belt
	// This ensures the entire cluster moves as a cohesive unit

	// Check if the seed item has already been moved this tick (prevents infinite loops)
	FItem* SeedItem = ItemMap.Find(FromHex);
	if (!SeedItem || SeedItem->Id != SeedItemId)
	{
		return; // Item doesn't exist at expected location
	}

	// If the item was already moved this tick, check if it's at the target location
	if (SeedItem->LocationUpdatedThisTick)
	{
		// Verify if the item is already at the destination - if so, we're done
		if (FItem* DestItem = ItemMap.Find(ToHex))
		{
			if (DestItem->Id == SeedItemId)
			{
				return; // Already moved to destination successfully
			}
		}
		return; // Already moved but not to our destination, let other movement handle it
	}

	// Calculate the movement vector
	const int32 dQ = ToHex.Q - FromHex.Q;
	const int32 dR = ToHex.R - FromHex.R;

	// First, gather all bonded items that need to be moved as part of this cluster
	TSet<int32> ClusterItemIds;
	TArray<int32> Stack;
	Stack.Add(SeedItemId);
	ClusterItemIds.Add(SeedItemId);

	// Use BFS to find all connected items through bonds
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
				if (OtherId > 0 && !ClusterItemIds.Contains(OtherId))
				{
					// Check if this item is on the board and on a belt
					const FHex* OtherHex = ItemHex.Find(OtherId);
					if (OtherHex)
					{
						const FTile* OtherTile = TileMap.Find(*OtherHex);
						if (OtherTile && OtherTile->Kind == ETileKind::Belt)
						{
							FItem* OtherItem = ItemMap.Find(*OtherHex);
							if (OtherItem && OtherItem->Id == OtherId && !OtherItem->LocationUpdatedThisTick)
							{
								ClusterItemIds.Add(OtherId);
								Stack.Add(OtherId);
							}
						}
					}
				}
			}
		}
	}

	// Check if all destination hexes are available
	bool bCanMoveCluster = true;
	TArray<TPair<int32, FHex>> MovePairs; // ItemId -> destination hex

	for (int32 ItemId : ClusterItemIds)
	{
		const FHex* CurrentHex = ItemHex.Find(ItemId);
		if (!CurrentHex)
		{
			bCanMoveCluster = false;
			break;
		}

		FHex DestHex(CurrentHex->Q + dQ, CurrentHex->R + dR);

		// Check if destination is blocked by non-cluster items
		if (ItemMap.Contains(DestHex))
		{
			FItem* DestItem = ItemMap.Find(DestHex);
			if (DestItem && !ClusterItemIds.Contains(DestItem->Id))
			{
				bCanMoveCluster = false;
				break;
			}
		}

		MovePairs.Add(TPair<int32, FHex>(ItemId, DestHex));
	}

	if (!bCanMoveCluster)
	{
		// Cannot move the entire cluster, abort
		return;
	}

	// Now move all items in the cluster atomically
	TMap<FHex, FItem> TempStorage; // Store items temporarily to avoid conflicts

	// First pass: remove all items from their current positions
	for (const TPair<int32, FHex>& MovePair : MovePairs)
	{
		int32 ItemId = MovePair.Key;
		const FHex* CurrentHex = ItemHex.Find(ItemId);
		if (!CurrentHex) continue;

		FItem* Item = ItemMap.Find(*CurrentHex);
		if (!Item || Item->Id != ItemId) continue;

		// Store item temporarily
		FItem MovedItem = *Item;
		MovedItem.LocationUpdatedThisTick = true;
		TempStorage.Add(MovePair.Value, MovedItem);

		// Remove from current position
		ItemMap.Remove(*CurrentHex);
		OrbAt.Remove(*CurrentHex);
	}

	// Second pass: place all items at their new positions
	for (const TPair<FHex, FItem>& TempPair : TempStorage)
	{
		const FHex& DestHex = TempPair.Key;
		const FItem& Item = TempPair.Value;

		// Place item at destination
		ItemMap.Add(DestHex, Item);
		OrbAt.Add(DestHex, KindFromItem(Item));

		// Update ItemHex lookup
		ItemHex[Item.Id] = DestHex;

		// Update belt progress
		if (float* Progress = BeltProgress.Find(Item.Id))
		{
			*Progress -= 1.f;
			if (*Progress < 0.f) *Progress = 0.f;
		}
	}
}

void USimWorldHex::MoveBondedItemRecursive(int32 ItemId, const FHex& FromHex, int32 dQ, int32 dR, TMap<int32, FHex>& ItemHex)
{
	// This function is now deprecated in favor of the atomic cluster movement in MoveBondedClusterOnBelt
	// Keep it for compatibility but make it do nothing to avoid conflicts
	return;
}

// ============================================================
// OCCUPANCY CACHE - computed once per tick, shared across steps
// ============================================================
void USimWorldHex::UpdateArmBaseCache()
{
	CachedArmBaseHexes.Reset();
	for (const FArmState& A : Arms)
	{
		CachedArmBaseHexes.Add(A.BaseHex);
	}
	bArmBaseCacheDirty = false;
}

void USimWorldHex::UpdateHeldOrbCache()
{
	CachedHeldOrbHexes.Reset();
	
	// Also update arm base cache if dirty
	if (bArmBaseCacheDirty)
	{
		UpdateArmBaseCache();
	}
	
	// Helper lambda for rotation
	auto Rot60 = [](FHex D, uint8 StepsCCW) -> FHex
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
	
	for (const FArmState& A : Arms)
	{
		if (!A.bHolding) continue;

		// Calculate grip hex
		FHex GripHex = A.BaseHex;
		for (int i = 0; i < A.ArmLen; ++i)
			GripHex = HexUtil::Neighbor(GripHex, A.Facing);

		if (!A.Held.bIsCluster)
		{
			CachedHeldOrbHexes.Add(GripHex);
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

			for (const FOrbData& Sub : A.Held.Orbs)
			{
				FHex Local(Sub.dQ, Sub.dR);
				FHex Rot = Rot60(Local, ccw);
				FHex WorldPos(GripHex.Q + Rot.Q, GripHex.R + Rot.R);
				CachedHeldOrbHexes.Add(WorldPos);
			}
		}
	}
}

void USimWorldHex::StepSim()
{
	PrevItemHex.Empty();
	for (const auto& KV : ItemMap)
		if (KV.Value.Id != 0)
			PrevItemHex.Add(KV.Value.Id, KV.Key);
	ItemsMovedLastStep.Reset();

	// Reset LocationUpdatedThisTick flag for all items at the start of each tick
	for (auto& KV : ItemMap)
	{
		KV.Value.LocationUpdatedThisTick = false;
	}

	// Clear collision debug data for this frame
	CollisionDebugData.Empty();

	++StepCounter;
	SkipInputThisFrame.Reset();
	SkipOutputThisFrame.Reset();
	OrbsNewlyMovedToOutput.Reset();
	OrbsNewlyMovedByBelts.Reset();

	// SpawnInputs();          // pass 1 (empty at start)
	StepArms();
	
	// Update occupancy cache after StepArms - HeldOrbHexes and ArmBaseHexes are 
	// now valid for use by StepWalkers and StepBelts
	UpdateHeldOrbCache();
	
	StepWalkers();
	StepBelts();
	SpawnInputsAfterBelts(); // NEW pass 2 (inputs just vacated by belt moves)
	StepBonds();
	CheckAndRemoveDisconnectedOrbsFromHeldClusters();
	// Second pass for arms - allows immediate grab after unbonding
	StepArmsSecondPass();
	// Second pass for bonds - allows bond formation after rotations complete
	StepBondsSecondPass();

	ProcessOutputs();

	// Debug visualization - draw bond cylinders when DebugSpheres is enabled
	DrawBondDebugLines();
}
void USimWorldHex::StepGlyphs()
{
	// Legacy entry point if other code still calls it:
	ProcessOutputs();
}




// SpawnInputs() moved to SimWorldHex_InputOutput.cpp

// SpawnInputsAfterBelts() moved to SimWorldHex_InputOutput.cpp

// DoesItemMatchOutputType() moved to SimWorldHex_InputOutput.cpp

// ProcessOutputs() moved to SimWorldHex_InputOutput.cpp

// TryArmPickItem() and TryArmPlaceItem() moved to SimWorldHex_InputOutput.cpp

// PeekItemAt(), TryPlaceOrbAt(), TryPickupOrbAt(), HasOrbAt(), GetOrbKindAt() moved to SimWorldHex_InputOutput.cpp

void USimWorldHex::AddItemBondLink(int32 ItemId, int32 BondIndex)
{
	// Safety check: validate parameters
	if (ItemId <= 0 || BondIndex < 0) return;
	
	TArray<int32>& List = ItemToBonds.FindOrAdd(ItemId);
	List.Add(BondIndex);
}

void USimWorldHex::RemoveItemBondLink(int32 ItemId, int32 BondIndex)
{
	// Safety check: validate parameters
	if (ItemId <= 0 || BondIndex < 0) return;
	
	if (TArray<int32>* List = ItemToBonds.Find(ItemId))
	{
		List->Remove(BondIndex);
		if (List->Num() == 0)
			ItemToBonds.Remove(ItemId);
	}
}

void USimWorldHex::TryFormBondAt(const FHex& Root, uint8 Dir, const TSet<int32>& HeldIds, TSet<int32>& Newly)
{
	const FHex AHex = Root;
	const FHex BHex = HexUtil::Neighbor(Root, (EHexDir)(Dir % 6));

	FItem* ItemA = ItemMap.Find(AHex);
	FItem* ItemB = ItemMap.Find(BHex);
	if (!ItemA || !ItemB) return;
	if (ItemA->Id == 0 || ItemB->Id == 0) return;
	if (ItemA->Id == ItemB->Id) return;

	if (HeldIds.Contains(ItemA->Id) || HeldIds.Contains(ItemB->Id)) return;
	if (Newly.Contains(ItemA->Id) || Newly.Contains(ItemB->Id)) { /* allow multiple bonds same tick but avoid duplication */ }

	// Prevent duplicate bond between same two items (order-agnostic)
	auto AlreadyBondedPair = [&](int32 AId, int32 BId)
		{
			if (const TArray<int32>* AList = ItemToBonds.Find(AId))
			{
				for (int32 BondIdx : *AList)
				{
					const FBond& Existing = Bonds[BondIdx];
					if ((Existing.ItemA == AId && Existing.ItemB == BId) ||
						(Existing.ItemA == BId && Existing.ItemB == AId))
						return true;
				}
			}
			return false;
		};
	if (AlreadyBondedPair(ItemA->Id, ItemB->Id)) return;

	FBond NewB;
	NewB.ItemA = ItemA->Id;
	NewB.ItemB = ItemB->Id;
	NewB.Root = Root;
	NewB.dQ = (int8)(BHex.Q - AHex.Q);
	NewB.dR = (int8)(BHex.R - AHex.R);
	NewB.bActive = true;

	const int32 Index = Bonds.Add(NewB);
	AddItemBondLink(NewB.ItemA, Index);
	AddItemBondLink(NewB.ItemB, Index);
	Newly.Add(NewB.ItemA);
	Newly.Add(NewB.ItemB);

	if (GEngine && bDevDebugBonds)
	{
		GEngine->AddOnScreenDebugMessage(-1, 0.75f, FColor::Green,
			FString::Printf(TEXT("Bond formed: %d-%d (%d)"), NewB.ItemA, NewB.ItemB, Index));
	}
}


void USimWorldHex::RemoveBondIndex(int32 Index)
{
	if (!Bonds.IsValidIndex(Index)) return;
	
	// Safety check: ensure the bond is valid before proceeding
	const FBond* BondPtr = &Bonds[Index];
	if (!BondPtr) return;
	
	const int32 LastIndex = Bonds.Num() - 1;
	const FBond Dead = *BondPtr;

	// Safety check: validate item IDs before removing links
	if (Dead.ItemA > 0 && Dead.ItemB > 0)
	{
		// Remove links for dead bond
		RemoveItemBondLink(Dead.ItemA, Index);
		RemoveItemBondLink(Dead.ItemB, Index);
	}

	// Swap removal if not last
	if (Index != LastIndex)
	{
		const FBond Moved = Bonds[LastIndex];
		Bonds.RemoveAtSwap(Index);

		// Fix moved bond indices in its items' lists
		auto FixList = [&](int32 ItemId)
			{
				if (ItemId > 0) // Safety check
				{
					if (TArray<int32>* L = ItemToBonds.Find(ItemId))
					{
						for (int32& Val : *L)
						{
							if (Val == LastIndex)
							{
								Val = Index;
								break;
							}
						}
					}
				}
			};
		FixList(Moved.ItemA);
		FixList(Moved.ItemB);
	}
	else
	{
		Bonds.RemoveAt(Index);
	}

	if (GEngine && bDevDebugBonds)
	{
		GEngine->AddOnScreenDebugMessage(-1, 0.75f, FColor::Red,
			FString::Printf(TEXT("Bond removed: %d-%d"), Dead.ItemA, Dead.ItemB));
	}
}

FVector USimWorldHex::HexCenterToWorld(const FHex& H) const
{
	const float Tile = TileSizeUU;
	const EHexOrientation Ori = GridOrientation;
	const float Sqrt3 = 1.7320508075688772f;
	const float q = (float)H.Q;
	const float r = (float)H.R;

	FVector2D Local;
	if (Ori == EHexOrientation::PointyTop)
	{
		Local.X = Tile * (Sqrt3 * q + (Sqrt3 * 0.5f) * r);
		Local.Y = Tile * (1.5f * r);
	}
	else
	{
		Local.X = Tile * (1.5f * q);
		Local.Y = Tile * (Sqrt3 * r + (Sqrt3 * 0.5f) * q);
	}

	const float Rad = FMath::DegreesToRadians(GridYawDeg);
	const float CosA = FMath::Cos(Rad);
	const float SinA = FMath::Sin(Rad);
	const float RX = Local.X * CosA - Local.Y * SinA;
	const float RY = Local.X * SinA + Local.Y * CosA;
	return WorldAnchor + FVector(RX, RY, 0.f);
}

bool USimWorldHex::GetItemVisualLocation(int32 ItemId, FVector& OutWorld) const
{
	const FHex* CurHexPtr = nullptr;
	for (const auto& KV : ItemMap)
	{
		if (KV.Value.Id == ItemId)
		{
			CurHexPtr = &KV.Key;
			break;
		}
	}
	if (!CurHexPtr) return false;

	const FHex CurHex = *CurHexPtr;
	const FVector CurCenter = HexCenterToWorld(CurHex);

	// If this item advanced exactly one belt hex this logical step, interpolate previous->current
	if (ItemsMovedLastStep.Contains(ItemId))
	{
		if (const FHex* PrevHex = PrevItemHex.Find(ItemId))
		{
			if (*PrevHex != CurHex)
			{
				const FVector PrevCenter = HexCenterToWorld(*PrevHex);
				float Alpha = FMath::Clamp(FrameAlpha, 0.f, 1.f);
				OutWorld = FMath::Lerp(PrevCenter, CurCenter, Alpha);
				return true;
			}
		}
	}

	// Standard belt in‑tile interpolation (current center -> next center)
	const FTile* Tile = TileMap.Find(CurHex);
	if (!Tile || Tile->Kind != ETileKind::Belt)
	{
		OutWorld = CurCenter;
		return true;
	}

	// Compute next hex now so we can check runtime blocking
	const uint8 Dir = (uint8)(Tile->Arg & 0xFF);
	const FHex NextHex = HexUtil::Neighbor(CurHex, (EHexDir)(Dir % 6));
	const FVector NextCenter = HexCenterToWorld(NextHex);

	// Determine whether the destination is blocked by a board item or any held orb/hand
	auto IsDestBlocked = [&](const FHex& Dest)->bool
		{
			// Board occupancy
			if (ItemMap.Contains(Dest))
				return true;

			// Held occupancy (hands and sub‑orbs)
			for (int32 ArmIdx = 0; ArmIdx < Arms.Num(); ++ArmIdx)
			{
				const FArmState& A = Arms[ArmIdx];
				if (!A.bHolding) continue;

				// Hand hex at committed sim reach/facing
				const FHex Grip = GetArmGripHex_Sim(ArmIdx);

				// Single held orb occupies the grip hex
				if (!A.Held.bIsCluster)
				{
					if (Grip == Dest) return true;
					continue;
				}

				// Cluster: each sub‑orb at rotated offsets
				const uint8 ccw = GetHeldCCW_Steps_Sim(ArmIdx);
				for (const FOrbData& Sub : A.Held.Orbs)
				{
					FHex Local(Sub.dQ, Sub.dR);
					FHex Rot = Rot60CCW(Local, ccw);
					FHex WorldHex(Grip.Q + Rot.Q, Grip.R + Rot.R);
					if (WorldHex == Dest) return true;
				}
			}
			return false;
		};

	const bool bDestBlocked = IsDestBlocked(NextHex);

	// Use belt progress for smooth visual interpolation
	const float* PLogicPtr = BeltProgress.Find(ItemId);
	float LogicalP = PLogicPtr ? *PLogicPtr : 0.f;
	const bool bBlockedEdge = LogicalP >= BeltBlockedClamp - 0.0001f;

	float VisualP = LogicalP;
	if (!bBlockedEdge)
	{
		const float Dt = FMath::Max(0.f, FixedStepSec);
		VisualP = LogicalP + FrameAlpha * Dt;

		// Never render beyond the belt's blocked edge
		VisualP = FMath::Min(VisualP, BeltBlockedClamp - 0.0001f);
	}

	// If destination is currently blocked, enforce blocked-edge clamping as an extra safety
	if (bDestBlocked)
	{
		VisualP = FMath::Min(VisualP, BeltBlockedClamp - 0.0001f);
	}

	// Clamp to [0,1) for interpolation across THIS belt segment
	VisualP = FMath::Clamp(VisualP, 0.f, 0.9999f);

	OutWorld = FMath::Lerp(CurCenter, NextCenter, VisualP);
	return true;
}
// AddBoardOrbToHeldCluster() and AddBoardOrbToHeldItem() moved to SimWorldHex_Clusters.cpp
// CheckAndRemoveDisconnectedOrbsFromHeldClusters() moved to SimWorldHex_Clusters.cpp
// CheckAndRemoveDisconnectedOrbsFromHeldClusters() moved to SimWorldHex_Clusters.cpp
void USimWorldHex::DrawBondDebugLines()
{
	// Draw bond cylinders when DebugSpheres is enabled
	if (!bDebugSpheres || Bonds.Num() == 0)
	{
		return;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	// Get driver for coordinate conversion
	AOrbSimDriver* Driver = nullptr;
	for (TActorIterator<AOrbSimDriver> It(World); It; ++It)
	{
		Driver = *It;
		break;
	}

	if (!Driver)
	{
		return;
	}

	const AActor* Anchor = Driver->GetGridAnchor();
	const FVector AnchorLoc = Anchor ? Anchor->GetActorLocation() : FVector::ZeroVector;
	const float Tile = Driver->GetTileSize();
	const EHexOrientation DriverGridOrientation = Driver->GetGridOrientation();
	const float GridYaw = Driver->GetGridYaw();

	// Helper lambda to convert hex to world position
	auto HexToWorld = [&](const FHex& H) -> FVector
		{
			const FVector2D LocalXY = HexCoord::AxialToXY(FHexAxial(H.Q, H.R), Tile, DriverGridOrientation);
			const FVector2D RotXY = HexCoord::Rotate2D(LocalXY, GridYaw);
			return AnchorLoc + FVector(RotXY.X, RotXY.Y, 0.f);
		};

	// Build lookup map for board items once
	TMap<int32, FHex> BoardItemLookup;
	BoardItemLookup.Reserve(ItemMap.Num());
	for (const auto& ItemKV : ItemMap)
	{
		if (ItemKV.Value.Id != 0)
		{
			BoardItemLookup.Add(ItemKV.Value.Id, ItemKV.Key);
		}
	}

	// Draw debug lines for all active bonds
	for (const FBond& Bond : Bonds)
	{
		if (!Bond.bActive)
		{
			continue;
		}

		// Find the actual positions of the bonded items
		FVector PosA = FVector::ZeroVector;
		FVector PosB = FVector::ZeroVector;
		bool bFoundA = false, bFoundB = false;

		// Check if items are on the board using lookup
		if (const FHex* HexA = BoardItemLookup.Find(Bond.ItemA))
		{
			PosA = HexToWorld(*HexA);
			bFoundA = true;
		}
		if (const FHex* HexB = BoardItemLookup.Find(Bond.ItemB))
		{
			PosB = HexToWorld(*HexB);
			bFoundB = true;
		}

		// Check if items are held by arms (for held clusters)
		if (!bFoundA || !bFoundB)
		{
			for (int32 ArmIndex = 0; ArmIndex < Arms.Num(); ++ArmIndex)
			{
				const FArmState& Arm = Arms[ArmIndex];
				if (!Arm.bHolding) continue;

				const FHex GripHex = GetArmGripHex_Sim(ArmIndex);
				const uint8 ccw = GetHeldCCW_Steps_Sim(ArmIndex);

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

				if (Arm.Held.bIsCluster)
				{
					for (const FOrbData& Sub : Arm.Held.Orbs)
					{
						if ((Sub.ItemId == Bond.ItemA && !bFoundA) || (Sub.ItemId == Bond.ItemB && !bFoundB))
						{
							FHex Local(Sub.dQ, Sub.dR);
							FHex Rot = Rot60(Local, ccw);
							FHex WorldHex(GripHex.Q + Rot.Q, GripHex.R + Rot.R);
							const FVector Pos = HexToWorld(WorldHex);
							if (Sub.ItemId == Bond.ItemA) { PosA = Pos; bFoundA = true; }
							else { PosB = Pos; bFoundB = true; }
						}
					}
				}
				else
				{
					if (Arm.Held.Id == Bond.ItemA && !bFoundA) { PosA = HexToWorld(GripHex); bFoundA = true; }
					if (Arm.Held.Id == Bond.ItemB && !bFoundB) { PosB = HexToWorld(GripHex); bFoundB = true; }
				}
			}
		}

		// Only draw if both positions were found
		if (!bFoundA || !bFoundB)
		{
			continue;
		}

		// Add some height offset to make bonds visible above the ground
		PosA.Z += 15.f;
		PosB.Z += 15.f;

		// Calculate bond direction and distance
		FVector BondDirection = (PosB - PosA).GetSafeNormal();
		float BondLength = FVector::Dist(PosA, PosB);
		FVector BondMidpoint = (PosA + PosB) * 0.5f;

		// Draw cylinder to represent the bond
		if (BondLength > 0.f)
		{
			// Calculate rotation to align cylinder with bond direction
			FVector Up = FVector::UpVector;
			FVector Right = FVector::CrossProduct(BondDirection, Up).GetSafeNormal();
			if (Right.IsNearlyZero())
			{
				Right = FVector::RightVector;
			}
			FVector Forward = FVector::CrossProduct(Right, BondDirection).GetSafeNormal();

			// Create rotation matrix from the three axes
			FMatrix RotationMatrix = FMatrix(Forward, Right, BondDirection, FVector::ZeroVector);
			FRotator BondRotation = FQuat(RotationMatrix).Rotator();

			// Draw the cylinder (persistent for 1 second to avoid flickering)
			DrawDebugCylinder(World, PosA, PosB, 2.f, 8, FColor::Cyan, false, 1.0f, 0, 1.f);

			// Draw small spheres at bond endpoints (persistent for 1 second)
			DrawDebugSphere(World, PosA, 4.f, 8, FColor::Cyan, false, 1.0f, 0, 1.f);
			DrawDebugSphere(World, PosB, 4.f, 8, FColor::Cyan, false, 1.0f, 0, 1.f);

			// Draw bond ID text at midpoint (persistent for 1 second)
			FVector TextPosition = BondMidpoint;
			TextPosition.Z += 10.f;
			DrawDebugString(World, TextPosition,
				FString::Printf(TEXT("Bond: %d-%d"), Bond.ItemA, Bond.ItemB),
				nullptr, FColor::Cyan, 1.0f, true, 0.8f);
		}
	}
}


FHex USimWorldHex::GetArmGripHex_Sim(int32 ArmIndex) const
{
	if (ArmIndex < 0 || ArmIndex >= Arms.Num()) return FHex(0, 0);
	const FArmState& A = Arms[ArmIndex];
	const uint8 SimFace = (SimFacingByArm.Contains(ArmIndex) ? SimFacingByArm[ArmIndex] : uint8(A.Facing));
	const int32 SimLen = (SimArmLenByArm.Contains(ArmIndex) ? SimArmLenByArm[ArmIndex] : A.ArmLen);
	FHex Grip = A.BaseHex;
	for (int i = 0; i < SimLen; ++i)
		Grip = HexUtil::Neighbor(Grip, (EHexDir)(SimFace % 6));
	return Grip;
}

uint8 USimWorldHex::GetHeldCCW_Steps_Sim(int32 ArmIndex) const
{
	if (ArmIndex < 0 || ArmIndex >= Arms.Num()) return 0;
	const FArmState& A = Arms[ArmIndex];
	const uint8 cur = (SimFacingByArm.Contains(ArmIndex) ? SimFacingByArm[ArmIndex] : uint8(A.Facing)) % 6;

	int8 simGrab = int8(-1);
	if (const int8* P = SimGrabFacingByArm.Find(ArmIndex))
	{
		simGrab = *P;
	}
	// If sim grab is invalid, fall back to the current held grab (if any)
	const int8 grab = (simGrab >= 0) ? simGrab : (A.Held.bIsCluster ? A.Held.GrabFacing : int8(-1));

	const uint8 facingDelta = (grab >= 0)
		? (uint8)(((int32)cur - (int32)(grab % 6) + 6) % 6)
		: cur;

	return (uint8)((6 - facingDelta) % 6); // CCW steps to convert local->world
}

FHex USimWorldHex::Rot60CCW(FHex D, uint8 StepsCCW)
{
	for (uint8 i = 0; i < (StepsCCW % 6); ++i)
	{
		const int32 q = D.Q;
		const int32 r = D.R;
		D.Q = -r;
		D.R = q + r;
	}
	return D;
}

FHex USimWorldHex::Rot60CW(FHex D, uint8 StepsCW)
{
	return Rot60CCW(D, (uint8)((6 - (StepsCW % 6)) % 6));
}

void USimWorldHex::ProcessPendingBondFormations(float DeltaTime)
{
	// DEBUG: Log total pending formations (commented out for production)
	// if (PendingBondFormations.Num() > 0)
	// {
	//	UE_LOG(LogAGSim, Warning, TEXT("Processing %d pending bond formations"), PendingBondFormations.Num());
	// }

	for (int32 i = PendingBondFormations.Num() - 1; i >= 0; i--)
	{
		FPendingBondFormation& Pending = PendingBondFormations[i];

		// Check if the arm is still valid and holding the item
		if (Pending.ArmIndex < 0 || Pending.ArmIndex >= Arms.Num())
		{
			PendingBondFormations.RemoveAt(i);
			continue;
		}

		const FArmState& Arm = Arms[Pending.ArmIndex];
		if (!Arm.bHolding)
		{
			// Arm is no longer holding anything, cancel the pending formation
			PendingBondFormations.RemoveAt(i);
			continue;
		}

		// Check if the board item still exists
		const FItem* BoardItem = ItemMap.Find(Pending.BoardItemHex);
		if (!BoardItem || BoardItem->Id != Pending.BoardItemId)
		{
			// Board item no longer exists, cancel the pending formation
			PendingBondFormations.RemoveAt(i);
			continue;
		}

		// Decrease time remaining
		Pending.TimeRemaining -= DeltaTime;

		if (Pending.TimeRemaining <= 0.f)
		{
			// Time to form the bond - use the current held item ID
			if (bDebugLogs && bDevDebugBonds && GEngine)
			{
				GEngine->AddOnScreenDebugMessage(-1, 1.0f, FColor::Green,
					FString::Printf(TEXT("Processing pending bond: board orb %d -> held item %d"), Pending.BoardItemId, Arm.Held.Id));
			}

			AddBoardOrbToHeldItem(Arm.Held.Id, Pending.BoardItemId, Pending.BoardItemHex);
			PendingBondFormations.RemoveAt(i);
		}
	}
}


void USimWorldHex::OnArmRotationComplete(int32 /*ArmIndex*/)
{
	// No-op for bonding/merges with sim-delayed facing.
	// Bonds and held-held merges are resolved in StepBonds using committed sim state.
}

void USimWorldHex::OnArmRotationFullyComplete(int32 ArmIndex)
{
	// Visual event only: record state for UX/other subsystems if desired.
	// Do not commit sim-facing here to avoid coupling sim to render frame timing.
	if (ArmIndex >= 0 && ArmIndex < Arms.Num())
	{
		FArmState& S = Arms[ArmIndex];
		S.bRotationCompleted = true;
	}
	
}

void USimWorldHex::CancelPendingBondFormationsForArm(int32 ArmIndex)
{
	// Cancel any pending bond formations for this arm
	for (int32 i = PendingBondFormations.Num() - 1; i >= 0; i--)
	{
		FPendingBondFormation& Pending = PendingBondFormations[i];

		if (Pending.ArmIndex == ArmIndex)
		{
			if (bDebugLogs && bDevDebugBonds && GEngine)
			{
				GEngine->AddOnScreenDebugMessage(-1, 1.0f, FColor::Yellow,
					FString::Printf(TEXT("Canceling pending bond formation for arm %d (arm dropped item)"), ArmIndex));
			}

			PendingBondFormations.RemoveAt(i);
		}
	}
}

void USimWorldHex::RebuildClusterOutputRelationships()
{
	// Blueprint-callable version with no parameters
	TMap<FHex, uint8> EmptyMap;
	RebuildClusterOutputRelationships(EmptyMap);
}

void USimWorldHex::RebuildClusterOutputRelationships(const TMap<FHex, uint8>& SavedOrbTypes)
{
	// Clear existing cluster output relationships
	TArray<FHex> BondPartnersToRemove;
	for (const auto& PartnerKV : IOBondPartners)
	{
		const FTile* Tile1 = TileMap.Find(PartnerKV.Key);
		const FTile* Tile2 = TileMap.Find(PartnerKV.Value);
		if ((Tile1 && Tile1->Kind == ETileKind::ClusterOutput) || 
		    (Tile2 && Tile2->Kind == ETileKind::ClusterOutput))
		{
			BondPartnersToRemove.Add(PartnerKV.Key);
		}
	}
	for (const FHex& H : BondPartnersToRemove)
	{
		IOBondPartners.Remove(H);
	}
	MergedClusterOutputs.Empty();
	
	UE_LOG(LogTemp, Warning, TEXT("╔════════════════════════════════════════"));
	UE_LOG(LogTemp, Warning, TEXT("║ REBUILDING CLUSTER OUTPUT RELATIONSHIPS"));
	UE_LOG(LogTemp, Warning, TEXT("╠════════════════════════════════════════"));
	UE_LOG(LogTemp, Warning, TEXT("║ %d saved orb types provided"), SavedOrbTypes.Num());
	
	// Find all unique connected clusters of ClusterOutput tiles
	TSet<FHex> ProcessedTiles;
	TArray<TArray<FHex>> ConnectedClusters;
	
	for (const auto& TileKV : TileMap)
	{
		if (TileKV.Value.Kind != ETileKind::ClusterOutput)
			continue;
			
		const FHex& StartHex = TileKV.Key;
		
		if (ProcessedTiles.Contains(StartHex))
			continue;
		
		// Find all tiles connected to this one (same template ID and adjacent)
		uint8 TemplateId = TileKV.Value.Arg;
		TArray<FHex> ClusterTiles;
		TSet<FHex> Visited;
		TArray<FHex> ToVisit;
		ToVisit.Add(StartHex);
		Visited.Add(StartHex);
		
		while (ToVisit.Num() > 0)
		{
			FHex CurrentHex = ToVisit.Pop();
			ClusterTiles.Add(CurrentHex);
			ProcessedTiles.Add(CurrentHex);
			
			// Check all 6 neighbors
			for (int32 Dir = 0; Dir < 6; Dir++)
			{
				FHex NeighborHex = HexUtil::Neighbor(CurrentHex, (EHexDir)Dir);
				const FTile* NeighborTile = TileMap.Find(NeighborHex);
				
				if (NeighborTile && 
				    NeighborTile->Kind == ETileKind::ClusterOutput && 
				    NeighborTile->Arg == TemplateId &&
				    !Visited.Contains(NeighborHex))
				{
					Visited.Add(NeighborHex);
					ToVisit.Add(NeighborHex);
				}
			}
		}
		
		if (ClusterTiles.Num() > 0)
		{
			ConnectedClusters.Add(ClusterTiles);
			UE_LOG(LogTemp, Warning, TEXT("║ Found cluster with %d tiles (Template %d)"), ClusterTiles.Num(), TemplateId);
		}
	}
	
	UE_LOG(LogTemp, Warning, TEXT("║ Total unique clusters found: %d"), ConnectedClusters.Num());
	UE_LOG(LogTemp, Warning, TEXT("║"));
	
	// For each connected cluster, rebuild bonds and templates
	int32 TotalPartnerships = 0;
	int32 TotalMergedOutputs = 0;
	
	for (const TArray<FHex>& ClusterTiles : ConnectedClusters)
	{
		if (ClusterTiles.Num() == 0)
			continue;
		
		// Get the template ID from the first tile
		const FTile* FirstTile = TileMap.Find(ClusterTiles[0]);
		if (!FirstTile)
			continue;
		
		uint8 TemplateId = FirstTile->Arg;
		
		UE_LOG(LogTemp, Warning, TEXT("║ Processing cluster: %d tiles, Template=%d"), ClusterTiles.Num(), TemplateId);
		
		// Find the BaseHex (center-most tile - geometric center of all tiles)
		// This allows clusters to be placed/rotated around their center, like Blueprint mode
		int32 SumQ = 0;
		int32 SumR = 0;
		for (const FHex& H : ClusterTiles)
		{
			SumQ += H.Q;
			SumR += H.R;
		}
		
		// Calculate average (center) position
		int32 CenterQ = FMath::RoundToInt((float)SumQ / ClusterTiles.Num());
		int32 CenterR = FMath::RoundToInt((float)SumR / ClusterTiles.Num());
		
		// Find the tile closest to the center
		FHex BaseHex = ClusterTiles[0];
		int32 MinDist = INT_MAX;
		for (const FHex& H : ClusterTiles)
		{
			int32 dQ = H.Q - CenterQ;
			int32 dR = H.R - CenterR;
			int32 dist = FMath::Abs(dQ) + FMath::Abs(dR) + FMath::Abs(dQ + dR);
			if (dist < MinDist)
			{
				MinDist = dist;
				BaseHex = H;
			}
		}
		
		UE_LOG(LogTemp, Warning, TEXT("║ Calculated BaseHex: (%d,%d) as center-most tile"), BaseHex.Q, BaseHex.R);
		
		// Build orb data and bonds for this cluster
		TArray<FOrbData> Orbs;
		TArray<TPair<int32, int32>> BondPairs;
		TMap<FHex, int32> HexToOrbIndex;
		
		// Get the existing template (if any) to extract orb types
		const FClusterTemplate* ExistingTemplate = ClusterTemplates.Find(TemplateId);
		
		for (int32 i = 0; i < ClusterTiles.Num(); i++)
		{
			const FHex& H = ClusterTiles[i];
			HexToOrbIndex.Add(H, i);
			
			FOrbData Orb;
			Orb.dQ = H.Q - BaseHex.Q;
			Orb.dR = H.R - BaseHex.R;
			
			// Get orb type from saved data, or from existing template if saved data not available
			const uint8* SavedType = SavedOrbTypes.Find(H);
			if (SavedType)
			{
				Orb.Type = *SavedType;
			}
			else if (ExistingTemplate)
			{
				// Try to find matching orb in existing template by relative position
				bool bFound = false;
				for (const FOrbData& TemplateOrb : ExistingTemplate->Orbs)
				{
					if (TemplateOrb.dQ == Orb.dQ && TemplateOrb.dR == Orb.dR)
					{
						Orb.Type = TemplateOrb.Type;
						bFound = true;
						break;
					}
				}
				
				if (!bFound)
				{
					Orb.Type = 0;
					UE_LOG(LogTemp, Warning, TEXT("║   Tile (%d,%d): No saved type or template match, using 0"), H.Q, H.R);
				}
			}
			else
			{
				Orb.Type = 0;
				UE_LOG(LogTemp, Warning, TEXT("║   Tile (%d,%d): No saved type or template, using 0"), H.Q, H.R);
			}
			
			Orbs.Add(Orb);
		}
		
		// Build bonds between adjacent tiles
		for (int32 i = 0; i < ClusterTiles.Num(); i++)
		{
			const FHex& Hex1 = ClusterTiles[i];
			
			// Check all neighbors
			for (int32 j = i + 1; j < ClusterTiles.Num(); j++)
			{
				const FHex& Hex2 = ClusterTiles[j];
				
				// Check if adjacent
				int32 dQ = Hex2.Q - Hex1.Q;
				int32 dR = Hex2.R - Hex1.R;
				int32 dist = (FMath::Abs(dQ) + FMath::Abs(dR) + FMath::Abs(dQ + dR)) / 2;
				
				if (dist == 1)
				{
					// Add partnership
					IOBondPartners.Add(Hex1, Hex2);
					TotalPartnerships++;
					
					// Add bond to template
					BondPairs.Add(TPair<int32, int32>(i, j));
				}
			}
		}
		
		// Only re-register the template if it doesn't exist or if orb data has changed
		bool bNeedToRegister = false;
		const FClusterTemplate* CurrentTemplate = ClusterTemplates.Find(TemplateId);
		if (!CurrentTemplate)
		{
			bNeedToRegister = true;
			UE_LOG(LogTemp, Warning, TEXT("║ Template %d doesn't exist, will register"), TemplateId);
		}
		else
		{
			// Check if orb data matches
			if (CurrentTemplate->Orbs.Num() != Orbs.Num())
			{
				bNeedToRegister = true;
				UE_LOG(LogTemp, Warning, TEXT("║ Template %d orb count changed (%d -> %d), will re-register"), 
					TemplateId, CurrentTemplate->Orbs.Num(), Orbs.Num());
			}
			else
			{
				// Check if all orbs match
				for (int32 i = 0; i < Orbs.Num(); i++)
				{
					bool bFound = false;
					for (const FOrbData& CurrentOrb : CurrentTemplate->Orbs)
					{
						if (CurrentOrb.dQ == Orbs[i].dQ && CurrentOrb.dR == Orbs[i].dR && CurrentOrb.Type == Orbs[i].Type)
						{
							bFound = true;
							break;
						}
					}
					if (!bFound)
					{
						bNeedToRegister = true;
						UE_LOG(LogTemp, Warning, TEXT("║ Template %d orb data changed, will re-register"), TemplateId);
						break;
					}
				}
			}
		}
		
		if (bNeedToRegister)
		{
			FString TemplateName = FString::Printf(TEXT("Reconstructed_%d_%dorbs"), TemplateId, Orbs.Num());
			RegisterClusterTemplate(TemplateId, Orbs, BondPairs, TemplateName);
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("║ Template %d is already up-to-date, skipping re-registration"), TemplateId);
		}
		
		// Create merged tile info for each adjacent pair
		for (int32 i = 0; i < ClusterTiles.Num(); i++)
		{
			const FHex& Hex1 = ClusterTiles[i];
			
			for (int32 j = i + 1; j < ClusterTiles.Num(); j++)
			{
				const FHex& Hex2 = ClusterTiles[j];
				
				// Check if adjacent
				int32 dQ = Hex2.Q - Hex1.Q;
				int32 dR = Hex2.R - Hex1.R;
				int32 dist = (FMath::Abs(dQ) + FMath::Abs(dR) + FMath::Abs(dQ + dR)) / 2;
				
				if (dist == 1)
				{
					// Create merged tile info
					FHex PrimaryHex = (Hex1.Q < Hex2.Q || (Hex1.Q == Hex2.Q && Hex1.R < Hex2.R)) ? Hex1 : Hex2;
					FHex SecondaryHex = (PrimaryHex == Hex1) ? Hex2 : Hex1;
					
					const uint8* Type1 = SavedOrbTypes.Find(Hex1);
					const uint8* Type2 = SavedOrbTypes.Find(Hex2);
					
					FMergedTileInfo Info;
					Info.Hex1 = PrimaryHex;
					Info.Hex2 = SecondaryHex;
					Info.OrbType1 = (PrimaryHex == Hex1) ? (Type1 ? *Type1 : 0) : (Type2 ? *Type2 : 0);
					Info.OrbType2 = (PrimaryHex == Hex1) ? (Type2 ? *Type2 : 0) : (Type1 ? *Type1 : 0);
					Info.TemplateId = TemplateId;
					
					MergedClusterOutputs.Add(PrimaryHex, Info);
					TotalMergedOutputs++;
				}
			}
		}
	}
	
	UE_LOG(LogTemp, Warning, TEXT("║"));
	UE_LOG(LogTemp, Warning, TEXT("║ Rebuilt %d partnerships, %d merged outputs"), TotalPartnerships, TotalMergedOutputs);
	UE_LOG(LogTemp, Warning, TEXT("╚════════════════════════════════════════"));
}

TArray<FHex> USimWorldHex::FindClusterInputChain(const FHex& StartHex, const TSet<FHex>& ExcludeHexes) const
{
	// Invalidate cache if tiles have changed
	if (LastTileRevisionForChainCache != TileRevision)
	{
		ClusterInputChainCache.Empty();
		LastTileRevisionForChainCache = TileRevision;
	}
	
	// Check cache first
	if (const TArray<FHex>* Cached = ClusterInputChainCache.Find(StartHex))
	{
		// Verify cached chain is still valid (all tiles still exist and are ClusterInput)
		bool bCacheValid = true;
		for (const FHex& Hex : *Cached)
		{
			const FTile* T = TileMap.Find(Hex);
			if (!T || T->Kind != ETileKind::ClusterInput)
			{
				bCacheValid = false;
				break;
			}
		}
		if (bCacheValid)
		{
			return *Cached;
		}
		// Cache invalid, clear it
		ClusterInputChainCache.Remove(StartHex);
	}
	
	// Perform BFS to find chain
	TArray<FHex> ClusterInputTiles;
	TSet<FHex> Visited;
	TArray<FHex> ToVisit;
	
	ToVisit.Add(StartHex);
	Visited.Add(StartHex);
	
	while (ToVisit.Num() > 0)
	{
		FHex CurrentHex = ToVisit.Pop();
		ClusterInputTiles.Add(CurrentHex);
		
		// Check all 6 neighbors
		for (uint8 Dir = 0; Dir < 6; Dir++)
		{
			FHex NeighborHex = HexUtil::Neighbor(CurrentHex, (EHexDir)Dir);
			if (!Visited.Contains(NeighborHex) && !ExcludeHexes.Contains(NeighborHex))
			{
				const FTile* NeighborTile = TileMap.Find(NeighborHex);
				if (NeighborTile && NeighborTile->Kind == ETileKind::ClusterInput)
				{
					Visited.Add(NeighborHex);
					ToVisit.Add(NeighborHex);
				}
			}
		}
	}
	
	// Cache the result for all tiles in the chain
	for (const FHex& Hex : ClusterInputTiles)
	{
		ClusterInputChainCache.Add(Hex, ClusterInputTiles);
	}
	
	return ClusterInputTiles;
}

// TriggerTile signal management
void USimWorldHex::SetBeltTriggerSignal(const FHex& TriggerHex, const FHex& BeltOwnerHex, bool bPositive)
{
	TMap<FHex, bool>& Signals = BeltTriggerSignals.FindOrAdd(BeltOwnerHex);
	Signals.Add(TriggerHex, bPositive);
}

void USimWorldHex::ClearBeltTriggerSignal(const FHex& TriggerHex, const FHex& BeltOwnerHex)
{
	if (TMap<FHex, bool>* Signals = BeltTriggerSignals.Find(BeltOwnerHex))
	{
		Signals->Remove(TriggerHex);
		if (Signals->Num() == 0)
		{
			BeltTriggerSignals.Remove(BeltOwnerHex);
		}
	}
}

bool USimWorldHex::IsBeltEnabled(const FHex& BeltHex) const
{
	const TMap<FHex, bool>* Signals = BeltTriggerSignals.Find(BeltHex);
	if (!Signals || Signals->Num() == 0)
	{
		return true; // No triggers = enabled by default
	}

	// Logical AND: All associated trigger tiles must have a positive signal
	for (const auto& KV : *Signals)
	{
		if (!KV.Value)
		{
			return false; // Any negative signal disables the belt
		}
	}

	return true; // All signals are positive
}

void USimWorldHex::SetArmTriggerSignal(const FHex& TriggerHex, const FHex& ArmBaseHex, bool bPositive)
{
	TMap<FHex, bool>& Signals = ArmTriggerSignals.FindOrAdd(ArmBaseHex);
	Signals.Add(TriggerHex, bPositive);

	if (bDebugLogs)
	{
		UE_LOG(LogTemp, Log, TEXT("Sim: Arm at (%d,%d) received signal from (%d,%d): %s. Total triggers: %d"),
			ArmBaseHex.Q, ArmBaseHex.R, TriggerHex.Q, TriggerHex.R, bPositive ? TEXT("POSITIVE") : TEXT("NEGATIVE"), Signals.Num());
	}
}

void USimWorldHex::ClearArmTriggerSignal(const FHex& TriggerHex, const FHex& ArmBaseHex)
{
	if (TMap<FHex, bool>* Signals = ArmTriggerSignals.Find(ArmBaseHex))
	{
		Signals->Remove(TriggerHex);
		if (Signals->Num() == 0)
		{
			ArmTriggerSignals.Remove(ArmBaseHex);
		}
	}
	// Don't clear activation - it persists until reset command
}

bool USimWorldHex::IsArmEnabled(const FHex& ArmBaseHex) const
{
	const TMap<FHex, bool>* Signals = ArmTriggerSignals.Find(ArmBaseHex);
	if (!Signals || Signals->Num() == 0)
	{
		return true; // No triggers = enabled by default
	}

	// Logical AND: All associated trigger tiles must have a positive signal
	for (const auto& KV : *Signals)
	{
		if (!KV.Value)
		{
			if (bDebugLogs)
			{
				UE_LOG(LogTemp, Log, TEXT("Sim: Arm at (%d,%d) DISABLED by trigger at (%d,%d)"),
					ArmBaseHex.Q, ArmBaseHex.R, KV.Key.Q, KV.Key.R);
			}
			return false; // Any negative signal disables the arm
		}
	}

	return true; // All signals are positive
}


