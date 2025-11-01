#include "Sim/SimWorldHex.h"

// Input/Output processing functions
void USimWorldHex::SpawnInputs()
{
	if (TileMap.Num() == 0) return;
	for (const auto& KV : TileMap)
	{
		const FHex& H = KV.Key;
		const FTile& T = KV.Value;
		if (T.Kind != ETileKind::Input && T.Kind != ETileKind::ClusterInput) continue;
		if (SkipInputThisFrame.Contains(H)) continue;
		if (ItemMap.Contains(H)) continue;           // occupied already

		// Check if any held item is currently occupying this input tile's position
		bool bHeldItemAtThisPosition = false;
		for (const FArmState& S : Arms)
		{
			if (!S.bHolding) continue;

			// Calculate the arm's grip position
			FHex GripHex = S.BaseHex;
			for (int i = 0; i < S.ArmLen; ++i)
				GripHex = HexUtil::Neighbor(GripHex, S.Facing);

			// Check if the arm's grip position is at this input tile
			if (GripHex.Q == H.Q && GripHex.R == H.R)
			{
				bHeldItemAtThisPosition = true;
				break;
			}
		}

		// Don't spawn if any held item is occupying this position
		if (bHeldItemAtThisPosition) continue;

		// Handle regular input tiles
		if (T.Kind == ETileKind::Input)
		{
			// Immediately place the item without animation
			// Animation will be handled by StepBelts when an item is pulled from this input
			FOrb O; O.Type = (EOrbType)T.Arg;
			FItem NewItem = FItem::MakeSingle(O);
			NewItem.Id = NextItemId++;

			TryPutItem(H, NewItem);
		}
		// Handle cluster input tiles
		else if (T.Kind == ETileKind::ClusterInput)
		{
			const uint8 TemplateId = T.Arg;
			const FClusterTemplate* Template = ClusterTemplates.Find(TemplateId);
			if (Template)
			{
				// Create cluster from template
				FItem ClusterItem;
				ClusterItem.bIsCluster = true;
				ClusterItem.Id = NextItemId++;
				
				// Create orbs with unique IDs
				for (const FOrbData& TemplateOrb : Template->Orbs)
				{
					FOrbData NewOrb = TemplateOrb;
					NewOrb.ItemId = NextItemId++;
					ClusterItem.Orbs.Add(NewOrb);
				}
				
				// Place the cluster
				if (TryPutItem(H, ClusterItem))
				{
					// Create bonds between orbs based on template
					for (const TPair<int32, int32>& BondPair : Template->Bonds)
					{
						if (BondPair.Key < ClusterItem.Orbs.Num() && BondPair.Value < ClusterItem.Orbs.Num())
						{
							int32 ItemA = ClusterItem.Orbs[BondPair.Key].ItemId;
							int32 ItemB = ClusterItem.Orbs[BondPair.Value].ItemId;
							
							FBond NewBond;
							NewBond.ItemA = ItemA;
							NewBond.ItemB = ItemB;
							NewBond.Root = H;
							NewBond.dQ = ClusterItem.Orbs[BondPair.Value].dQ - ClusterItem.Orbs[BondPair.Key].dQ;
							NewBond.dR = ClusterItem.Orbs[BondPair.Value].dR - ClusterItem.Orbs[BondPair.Key].dR;
							NewBond.bActive = true;
							
							const int32 BondIndex = Bonds.Add(NewBond);
							AddItemBondLink(NewBond.ItemA, BondIndex);
							AddItemBondLink(NewBond.ItemB, BondIndex);
						}
					}
				}
			}
		}
	}
}

void USimWorldHex::SpawnInputsAfterBelts()
{
	if (TileMap.Num() == 0) return;
	for (const auto& KV : TileMap)
	{
		const FHex& H = KV.Key;
		const FTile& T = KV.Value;
		if (T.Kind != ETileKind::Input && T.Kind != ETileKind::ClusterInput) continue;
		if (SkipInputThisFrame.Contains(H)) continue;
		if (ItemMap.Contains(H)) continue; // still occupied after belts

		// Check if any held item is currently occupying this input tile's position
		bool bHeldItemAtThisPosition = false;
		for (const FArmState& S : Arms)
		{
			if (!S.bHolding) continue;

			// Calculate the arm's grip position
			FHex GripHex = S.BaseHex;
			for (int i = 0; i < S.ArmLen; ++i)
				GripHex = HexUtil::Neighbor(GripHex, S.Facing);

			// Check if the arm's grip position is at this input tile
			if (GripHex.Q == H.Q && GripHex.R == H.R)
			{
				bHeldItemAtThisPosition = true;
				break;
			}
		}

		// Don't spawn if any held item is occupying this position
		if (bHeldItemAtThisPosition) continue;

		// Handle regular input tiles
		if (T.Kind == ETileKind::Input)
		{
			// Immediately place the item without animation
			// Animation will be handled by StepBelts when an item is pulled from this input
			FOrb O; O.Type = (EOrbType)T.Arg;
			FItem NewItem = FItem::MakeSingle(O);
			NewItem.Id = NextItemId++;

			TryPutItem(H, NewItem);
		}
		// Handle cluster input tiles
		else if (T.Kind == ETileKind::ClusterInput)
		{
			const uint8 TemplateId = T.Arg;
			const FClusterTemplate* Template = ClusterTemplates.Find(TemplateId);
			if (Template)
			{
				// Create cluster from template
				FItem ClusterItem;
				ClusterItem.bIsCluster = true;
				ClusterItem.Id = NextItemId++;
				
				// Create orbs with unique IDs
				for (const FOrbData& TemplateOrb : Template->Orbs)
				{
					FOrbData NewOrb = TemplateOrb;
					NewOrb.ItemId = NextItemId++;
					ClusterItem.Orbs.Add(NewOrb);
				}
				
				// Place the cluster
				if (TryPutItem(H, ClusterItem))
				{
					// Create bonds between orbs based on template
					for (const TPair<int32, int32>& BondPair : Template->Bonds)
					{
						if (BondPair.Key < ClusterItem.Orbs.Num() && BondPair.Value < ClusterItem.Orbs.Num())
						{
							int32 ItemA = ClusterItem.Orbs[BondPair.Key].ItemId;
							int32 ItemB = ClusterItem.Orbs[BondPair.Value].ItemId;
							
							FBond NewBond;
							NewBond.ItemA = ItemA;
							NewBond.ItemB = ItemB;
							NewBond.Root = H;
							NewBond.dQ = ClusterItem.Orbs[BondPair.Value].dQ - ClusterItem.Orbs[BondPair.Key].dQ;
							NewBond.dR = ClusterItem.Orbs[BondPair.Value].dR - ClusterItem.Orbs[BondPair.Key].dR;
							NewBond.bActive = true;
							
							const int32 BondIndex = Bonds.Add(NewBond);
							AddItemBondLink(NewBond.ItemA, BondIndex);
							AddItemBondLink(NewBond.ItemB, BondIndex);
						}
					}
				}
			}
		}
	}
}

bool USimWorldHex::DoesItemMatchOutputType(const FItem& Item, const FTile& OutputTile) const
{
	EOrbType ExpectedType = (EOrbType)OutputTile.Arg;

	if (Item.bIsCluster)
	{
		// For clusters, check if any sub-orb matches the expected type
		for (const FOrbData& Sub : Item.Orbs)
		{
			if ((EOrbType)Sub.Type == ExpectedType)
			{
				return true;
			}
		}
		return false;
	}
	else
	{
		// For single orbs, check the main orb type
		return Item.Single.Type == ExpectedType;
	}
}

void USimWorldHex::ProcessOutputs()
{
	if (TileMap.Num() == 0) return;
	
	// Track which outputs we've already processed (for bonded outputs)
	TSet<FHex> ProcessedOutputs;
	
	for (const auto& KV : TileMap)
	{
		const FHex& H = KV.Key;
		const FTile& T = KV.Value;
		if (T.Kind != ETileKind::Output && T.Kind != ETileKind::ClusterOutput) continue;
		if (OrbsNewlyMovedToOutput.Contains(H)) continue; // Skip orbs just moved by belts this tick
		if (ProcessedOutputs.Contains(H)) continue; // Skip if already processed as part of a bonded pair

		// Process orbs that have been sitting on output tiles for at least one tick

		// Check if there's an item on this output tile
		const FItem* ItemPtr = ItemMap.Find(H);
		if (!ItemPtr) continue;

		// Handle regular output tiles
		if (T.Kind == ETileKind::Output)
		{
			// Check if the orb type matches the output's expected type
			if (DoesItemMatchOutputType(*ItemPtr, T))
		{
			// Check if this output tile has an OutputBond partner
			bool bRequiresBond = false;
			FHex PartnerOutputHex;
			const FItem* PartnerItem = nullptr;
			const FTile* PartnerTile = nullptr;
			
			if (const FHex* PartnerHex = IOBondPartners.Find(H))
			{
				bRequiresBond = true;
				PartnerOutputHex = *PartnerHex;
				PartnerItem = ItemMap.Find(PartnerOutputHex);
				PartnerTile = TileMap.Find(PartnerOutputHex);
			}
			
			// If bonding is required, verify that the items are bonded to each other
			bool bCanOutput = true;
			if (bRequiresBond)
			{
				// Both items must exist, be bonded to each other, and match their respective output types
				bCanOutput = false;
				
				if (PartnerItem && PartnerItem->Id != 0 && PartnerTile)
				{
					// Check that partner orb matches its output type
					if (!DoesItemMatchOutputType(*PartnerItem, *PartnerTile))
					{
						// Partner doesn't match its output type - can't output
						bCanOutput = false;
					}
					else
					{
						// Check if the two items/clusters are bonded to each other
						// For clusters, check if any orb in one cluster is bonded to any orb in the other
						TSet<int32> ItemAIds;
						TSet<int32> ItemBIds;
						
						// Gather all IDs for item A (current item)
						if (ItemPtr->bIsCluster)
						{
							for (const FOrbData& Sub : ItemPtr->Orbs)
							{
								if (Sub.ItemId > 0)
								{
									ItemAIds.Add(Sub.ItemId);
								}
							}
						}
						else
						{
							ItemAIds.Add(ItemPtr->Id);
						}
						
						// Gather all IDs for item B (partner item)
						if (PartnerItem->bIsCluster)
						{
							for (const FOrbData& Sub : PartnerItem->Orbs)
							{
								if (Sub.ItemId > 0)
								{
									ItemBIds.Add(Sub.ItemId);
								}
							}
						}
						else
						{
							ItemBIds.Add(PartnerItem->Id);
						}
						
						// Check if any ID from item A is bonded to any ID from item B
						for (int32 IdA : ItemAIds)
						{
							if (const TArray<int32>* BondList = ItemToBonds.Find(IdA))
							{
								for (int32 BondIdx : *BondList)
								{
									if (!Bonds.IsValidIndex(BondIdx)) continue;
									const FBond& Bond = Bonds[BondIdx];
									if (!Bond.bActive) continue;
									
									int32 OtherId = (Bond.ItemA == IdA) ? Bond.ItemB : Bond.ItemA;
									if (ItemBIds.Contains(OtherId))
									{
										bCanOutput = true;
										break;
									}
								}
								if (bCanOutput) break;
							}
						}
					}
				}
			}
			
			if (bCanOutput)
			{
				// Type matches and bond requirement satisfied - remove from simulation and create animated orbs
				FItem Temp = *ItemPtr;
				ItemMap.Remove(H);
				OrbAt.Remove(H);

				// Create animated orb for output (descending) - synchronized to step counter
				FAnimatedOrb AnimatedOrb;
				AnimatedOrb.Item = Temp;
				AnimatedOrb.Hex = H;
				AnimatedOrb.StartStep = StepCounter; // Start immediately this step
				AnimatedOrb.CompletionStep = StepCounter + 3;
				AnimatedOrb.bIsInput = false;
				AnimatedOrb.bActive = true;
				
				// If this was a bonded output, also process the partner and mark both animations
				if (bRequiresBond && PartnerItem)
				{
					AnimatedOrb.bIsBonded = true;
					AnimatedOrb.PartnerHex = PartnerOutputHex;
					AnimatedOrb.PartnerItemId = PartnerItem->Id;
					
					// Remove partner item and create its animation
					FItem PartnerTemp = *PartnerItem;
					ItemMap.Remove(PartnerOutputHex);
					OrbAt.Remove(PartnerOutputHex);
					
					FAnimatedOrb PartnerAnimatedOrb;
					PartnerAnimatedOrb.Item = PartnerTemp;
					PartnerAnimatedOrb.Hex = PartnerOutputHex;
					PartnerAnimatedOrb.StartStep = StepCounter;
					PartnerAnimatedOrb.CompletionStep = StepCounter + 3;
					PartnerAnimatedOrb.bIsInput = false;
					PartnerAnimatedOrb.bActive = true;
					PartnerAnimatedOrb.bIsBonded = true;
					PartnerAnimatedOrb.PartnerHex = H;
					PartnerAnimatedOrb.PartnerItemId = Temp.Id;
					
					AnimatedOrbs.Add(PartnerAnimatedOrb);
					
					// Mark partner as processed
					ProcessedOutputs.Add(PartnerOutputHex);
					
					++OutputCount; // Count partner too
				}
				
				AnimatedOrbs.Add(AnimatedOrb);
				++OutputCount;
			}
		}
		// If type doesn't match or bond requirement not met, leave the orb on the tile (no action needed)
		}
	// Handle cluster output tiles
	else if (T.Kind == ETileKind::ClusterOutput)
	{
		// Find all tiles that are part of this cluster output (connected adjacent ClusterOutput tiles)
		TArray<FHex> ClusterTiles;
		TArray<uint8> ExpectedTypes;
		
		// Expected type is stored directly in Tile.Arg
		uint8 CurrentExpectedType = T.Arg;
		
		// Add the current tile
		ClusterTiles.Add(H);
		ExpectedTypes.Add(CurrentExpectedType);
		
		// Find all adjacent ClusterOutput tiles recursively (forming a connected cluster)
		TSet<FHex> VisitedTiles;
		VisitedTiles.Add(H);
		TArray<FHex> ToVisit;
		ToVisit.Add(H);
		
		while (ToVisit.Num() > 0)
		{
			FHex CurrentHex = ToVisit.Pop();
			
			// Check all 6 neighbors
			for (int32 Dir = 0; Dir < 6; Dir++)
			{
				FHex NeighborHex = HexUtil::Neighbor(CurrentHex, (EHexDir)Dir);
				
				// Skip if already visited
				if (VisitedTiles.Contains(NeighborHex))
					continue;
				
				// Check if neighbor is a ClusterOutput tile
				const FTile* NeighborTile = TileMap.Find(NeighborHex);
				if (NeighborTile && NeighborTile->Kind == ETileKind::ClusterOutput)
				{
					VisitedTiles.Add(NeighborHex);
					ClusterTiles.Add(NeighborHex);
					ToVisit.Add(NeighborHex);
					
					// Expected type for this neighbor is in its Arg
					uint8 NeighborExpectedType = NeighborTile->Arg;
					ExpectedTypes.Add(NeighborExpectedType);

				}
			}
		}
		

		
		// Check if ALL tiles in the cluster have orbs of the correct type AND are bonded together
		bool bAllTilesReady = true;
		TArray<int32> OrbItemIds; // Store item IDs to check bonds later
		
		for (int32 i = 0; i < ClusterTiles.Num(); i++)
		{
			const FHex& TileHex = ClusterTiles[i];
			const uint8 ExpectedType = ExpectedTypes[i];
			
			const FItem* TileItem = ItemMap.Find(TileHex);
			if (!TileItem)
			{

				bAllTilesReady = false;
				break;
			}
			
			// ClusterOutput tiles expect single orbs, not clusters
			if (TileItem->bIsCluster)
			{

				bAllTilesReady = false;
				break;
			}
			
			// Single orb - check type from the Single field
			uint8 ActualType = (uint8)TileItem->Single.Type;
			if (ActualType != ExpectedType)
			{
				bAllTilesReady = false;
				break;
			}
			
			// Add the item ID to the list for bond checking
			if (TileItem->Id > 0)
			{
				OrbItemIds.Add(TileItem->Id);
			}

		}
		
		// Check if all orbs belong to the same cluster (are bonded together)
		// AND that the cluster doesn't have any orbs on non-ClusterOutput tiles
		if (bAllTilesReady && ClusterTiles.Num() > 1 && OrbItemIds.Num() > 0)
		{
			// Build a connectivity graph of ALL bonded orbs (including those outside the cluster)
			TSet<int32> AllConnectedOrbs;
			TArray<int32> ToVisitItems;
			
			// Start with the first orb
			AllConnectedOrbs.Add(OrbItemIds[0]);
			ToVisitItems.Add(OrbItemIds[0]);
			
			while (ToVisitItems.Num() > 0)
			{
				int32 CurrentItemId = ToVisitItems.Pop();
				
				// Check all bonds involving this orb
				if (const TArray<int32>* BondList = ItemToBonds.Find(CurrentItemId))
				{
					for (int32 BondIdx : *BondList)
					{
						if (!Bonds.IsValidIndex(BondIdx)) continue;
						const FBond& Bond = Bonds[BondIdx];
						if (!Bond.bActive) continue;
						
						// Find the other orb in this bond
						int32 OtherItemId = (Bond.ItemA == CurrentItemId) ? Bond.ItemB : Bond.ItemA;
						
						// Add to connected set if not already visited
						if (!AllConnectedOrbs.Contains(OtherItemId))
						{
							AllConnectedOrbs.Add(OtherItemId);
							ToVisitItems.Add(OtherItemId);
						}
					}
				}
			}
			
			// Check if all orbs on our ClusterOutput tiles are connected
			TSet<int32> OrbsOnClusterTiles;
			for (int32 Id : OrbItemIds)
			{
				OrbsOnClusterTiles.Add(Id);
			}
			
			for (int32 Id : OrbItemIds)
			{
				if (!AllConnectedOrbs.Contains(Id))
				{
					
					bAllTilesReady = false;
					break;
				}
			}
			
			if (bAllTilesReady)
			{
				// Check if the bonded cluster has any orbs that are NOT on our ClusterOutput tiles
				for (int32 ConnectedId : AllConnectedOrbs)
				{
					if (!OrbsOnClusterTiles.Contains(ConnectedId))
					{
						// This bonded orb is somewhere else (not on our ClusterOutput tiles)

						bAllTilesReady = false;
						break;
					}
				}

			}
		}
		

		
	// If all tiles are ready, create descent animations and remove items
	if (bAllTilesReady)
	{
		// First pass: create all animations
		TArray<FAnimatedOrb> NewAnimations;
		for (const FHex& TileHex : ClusterTiles)
		{
			const FItem* TileItem = ItemMap.Find(TileHex);
			if (TileItem)
			{
				// Create descending animation for this orb
				FAnimatedOrb AnimatedOrb;
				AnimatedOrb.Item = *TileItem;
				AnimatedOrb.Hex = TileHex;
				AnimatedOrb.StartStep = StepCounter;
				AnimatedOrb.CompletionStep = StepCounter + 3;
				AnimatedOrb.bIsInput = false;
				AnimatedOrb.bActive = true;
				AnimatedOrb.bIsBonded = false; // Will be set if bonded to another orb in cluster
				
				NewAnimations.Add(AnimatedOrb);
			}
		}
		
		// Second pass: check for bonds between orbs in the cluster
		for (int32 i = 0; i < NewAnimations.Num(); i++)
		{
			FAnimatedOrb& AnimA = NewAnimations[i];
			int32 ItemIdA = AnimA.Item.Id;
			
			// Check if this orb is bonded to any other orb in the cluster
			for (int32 j = i + 1; j < NewAnimations.Num(); j++)
			{
				FAnimatedOrb& AnimB = NewAnimations[j];
				int32 ItemIdB = AnimB.Item.Id;
				
				// Check if these two orbs are bonded to each other
				if (const TArray<int32>* BondList = ItemToBonds.Find(ItemIdA))
				{
					for (int32 BondIdx : *BondList)
					{
						if (!Bonds.IsValidIndex(BondIdx)) continue;
						const FBond& Bond = Bonds[BondIdx];
						if (!Bond.bActive) continue;
						
						int32 OtherId = (Bond.ItemA == ItemIdA) ? Bond.ItemB : Bond.ItemA;
						if (OtherId == ItemIdB)
						{
							// These orbs are bonded - mark both animations
							AnimA.bIsBonded = true;
							AnimA.PartnerHex = AnimB.Hex;
							AnimA.PartnerItemId = ItemIdB;
							
							AnimB.bIsBonded = true;
							AnimB.PartnerHex = AnimA.Hex;
							AnimB.PartnerItemId = ItemIdA;
							break;
						}
					}
				}
			}
		}
		
		// Add all animations and remove items
		for (const FAnimatedOrb& Anim : NewAnimations)
		{
			AnimatedOrbs.Add(Anim);
			
			// Remove the item from the board
			ItemMap.Remove(Anim.Hex);
			OrbAt.Remove(Anim.Hex);
			
			// Mark as processed
			ProcessedOutputs.Add(Anim.Hex);
			
			++OutputCount;
		}
		

	}
	// If not all tiles are ready, leave all orbs on their tiles
}
	}
}

bool USimWorldHex::TryArmPickItem(const FHex& From, FItem& OutItem)
{
	if (TryTakeItem(From, OutItem))
	{
		if (OutItem.Id != 0)
			HeldItemLastHex.Add(OutItem.Id, From);
		return true;
	}
	if (const FTile* T = TileMap.Find(From))
	{
		if (T->Kind == ETileKind::Input)
		{
			FOrb O; O.Type = (EOrbType)T->Arg;
			FItem NewItem = FItem::MakeSingle(O);
			NewItem.Id = NextItemId++;
			OutItem = NewItem;
			
			// Create ascending animation for the input tile
			// This ensures the next orb spawns after the animation completes
			FAnimatedOrb AscendAnimation;
			AscendAnimation.bActive = true;
			AscendAnimation.bIsInput = true;
			AscendAnimation.Hex = From;
			AscendAnimation.Item = OutItem;
			AscendAnimation.StartStep = StepCounter;
			AscendAnimation.CompletionStep = StepCounter + 1; // Complete at next step
			
			// Check if this input tile has an InputBond partner
			if (const FHex* PartnerHex = IOBondPartners.Find(From))
			{
				// Check if partner also has an item (both inputs should spawn bonded)
				if (const FItem* PartnerItem = ItemMap.Find(*PartnerHex))
				{
					AscendAnimation.bIsBonded = true;
					AscendAnimation.PartnerHex = *PartnerHex;
					AscendAnimation.PartnerItemId = PartnerItem->Id;
				}
			}
			
			AnimatedOrbs.Add(AscendAnimation);
			
			return true;
		}
		else if (T->Kind == ETileKind::ClusterInput)
		{
			const uint8 TemplateId = T->Arg;
			const FClusterTemplate* Template = ClusterTemplates.Find(TemplateId);
			if (Template)
			{
				// Create cluster from template
				FItem ClusterItem;
				ClusterItem.bIsCluster = true;
				ClusterItem.Id = NextItemId++;
				
				// Create orbs with unique IDs
				for (const FOrbData& TemplateOrb : Template->Orbs)
				{
					FOrbData NewOrb = TemplateOrb;
					NewOrb.ItemId = NextItemId++;
					ClusterItem.Orbs.Add(NewOrb);
				}
				
				OutItem = ClusterItem;
				
				// Create bonds between orbs based on template
				for (const TPair<int32, int32>& BondPair : Template->Bonds)
				{
					if (BondPair.Key < ClusterItem.Orbs.Num() && BondPair.Value < ClusterItem.Orbs.Num())
					{
						int32 ItemA = ClusterItem.Orbs[BondPair.Key].ItemId;
						int32 ItemB = ClusterItem.Orbs[BondPair.Value].ItemId;
						
						FBond NewBond;
						NewBond.ItemA = ItemA;
						NewBond.ItemB = ItemB;
						NewBond.Root = From;
						NewBond.dQ = ClusterItem.Orbs[BondPair.Value].dQ - ClusterItem.Orbs[BondPair.Key].dQ;
						NewBond.dR = ClusterItem.Orbs[BondPair.Value].dR - ClusterItem.Orbs[BondPair.Key].dR;
						NewBond.bActive = true;
						
						const int32 BondIndex = Bonds.Add(NewBond);
						AddItemBondLink(NewBond.ItemA, BondIndex);
						AddItemBondLink(NewBond.ItemB, BondIndex);
					}
				}
				
				// Create ascending animation for the cluster input tile
				FAnimatedOrb AscendAnimation;
				AscendAnimation.bActive = true;
				AscendAnimation.bIsInput = true;
				AscendAnimation.Hex = From;
				AscendAnimation.Item = ClusterItem;
				AscendAnimation.StartStep = StepCounter;
				AscendAnimation.CompletionStep = StepCounter + 1; // Complete at next step
				
				AnimatedOrbs.Add(AscendAnimation);
				
				return true;
			}
		}
	}
	return false;
}

bool USimWorldHex::TryArmPlaceItem(const FHex& To, const FItem& InItem)
{
	// Always use the normal placement logic, even for output tiles
	// This allows ProcessOutputs() to handle type checking and animation properly
	const bool bPlaced = TryPutItem(To, InItem);
	if (bPlaced)
		HeldItemLastHex.Remove(InItem.Id);
	return bPlaced;
}

bool USimWorldHex::PeekItemAt(const FHex& H, FItem& OutItem) const
{
	if (const FItem* Item = ItemMap.Find(H))
	{
		OutItem = *Item;
		return true;
	}
	if (const FTile* T = TileMap.Find(H))
	{
		if (T->Kind == ETileKind::Input)
		{
			FOrb O; O.Type = (EOrbType)T->Arg;
			FItem NewItem = FItem::MakeSingle(O);
			NewItem.Id = 0; // Not yet assigned
			OutItem = NewItem;
			return true;
		}
		else if (T->Kind == ETileKind::ClusterInput)
		{
			const uint8 TemplateId = T->Arg;
			const FClusterTemplate* Template = ClusterTemplates.Find(TemplateId);
			if (Template)
			{
				// Create cluster from template
				FItem ClusterItem;
				ClusterItem.bIsCluster = true;
				ClusterItem.Id = 0; // Not yet assigned
				
				// Create orbs without IDs for peek
				for (const FOrbData& TemplateOrb : Template->Orbs)
				{
					FOrbData NewOrb = TemplateOrb;
					NewOrb.ItemId = 0; // Not yet assigned
					ClusterItem.Orbs.Add(NewOrb);
				}
				
				OutItem = ClusterItem;
				return true;
			}
		}
	}
	return false;
}

bool USimWorldHex::TryPlaceOrbAt(const FHex& Hex, uint8 Kind)
{
	if (ItemMap.Contains(Hex)) return false;
	FOrb O; O.Type = (EOrbType)Kind;
	FItem I = FItem::MakeSingle(O);
	I.Id = NextItemId++;
	return TryPutItem(Hex, I);
}

bool USimWorldHex::TryPickupOrbAt(const FHex& Hex, uint8& OutKind)
{
	const FItem* Item = ItemMap.Find(Hex);
	if (!Item) return false;
	OutKind = KindFromItem(*Item);
	FItem TempItem;
	return TryTakeItem(Hex, TempItem);
}

bool USimWorldHex::HasOrbAt(const FHex& H) const
{
	return ItemMap.Contains(H);
}

bool USimWorldHex::GetOrbKindAt(const FHex& Hex, uint8& OutKind) const
{
	if (const FItem* Item = ItemMap.Find(Hex))
	{
		OutKind = KindFromItem(*Item);
		return true;
	}
	return false;
}

void USimWorldHex::UpdateAnimatedOrbs(float DeltaTime)
{
	// Animations complete based on step counter, not time
	const int32 CurrentStep = GetStepCounter();

	for (int32 i = AnimatedOrbs.Num() - 1; i >= 0; i--)
	{
		FAnimatedOrb& AnimatedOrb = AnimatedOrbs[i];
		if (!AnimatedOrb.bActive) continue;

		// Check if animation has reached its completion step
		if (CurrentStep >= AnimatedOrb.CompletionStep)
		{
			// Animation complete
			if (AnimatedOrb.bIsInput)
			{
				// Input animation complete - spawn a new orb
				if (!ItemMap.Contains(AnimatedOrb.Hex))
				{
					// Get the input tile to determine orb type
					const FTile* InputTile = TileMap.Find(AnimatedOrb.Hex);
					if (InputTile && InputTile->Kind == ETileKind::Input)
					{
						FOrb O;
						O.Type = (EOrbType)InputTile->Arg;
						FItem NewItem = FItem::MakeSingle(O);
						NewItem.Id = NextItemId++;

						TryPutItem(AnimatedOrb.Hex, NewItem);
						
						// If this was a bonded animation, also check if partner needs a bond formed
						if (AnimatedOrb.bIsBonded && AnimatedOrb.PartnerItemId != 0)
						{
							// Find the partner item on the board
							if (const FItem* PartnerItem = ItemMap.Find(AnimatedOrb.PartnerHex))
							{
								if (PartnerItem->Id == AnimatedOrb.PartnerItemId)
								{
									// Form bond between the two items
									FBond NewBond;
									NewBond.ItemA = NewItem.Id;
									NewBond.ItemB = PartnerItem->Id;
									NewBond.Root = AnimatedOrb.Hex;
									NewBond.dQ = (int8)(AnimatedOrb.PartnerHex.Q - AnimatedOrb.Hex.Q);
									NewBond.dR = (int8)(AnimatedOrb.PartnerHex.R - AnimatedOrb.Hex.R);
									NewBond.bActive = true;

									const int32 BondIndex = Bonds.Add(NewBond);
									AddItemBondLink(NewBond.ItemA, BondIndex);
									AddItemBondLink(NewBond.ItemB, BondIndex);
								}
							}
						}
					}
				}
			}
			else
			{
			}

			// Remove the completed animation
			AnimatedOrbs.RemoveAt(i);
		}
	}
}
