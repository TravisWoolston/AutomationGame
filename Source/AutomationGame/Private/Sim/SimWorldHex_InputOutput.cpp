#include "Sim/SimWorldHex.h"
#include "Framework/GameManager.h"
#include "Actors/OrbSimDriver.h"
#include "EngineUtils.h"

// Input/Output processing functions
void USimWorldHex::SpawnInputs()
{
	if (TileMap.Num() == 0) return;

	// Track which ClusterInput hexes have already been processed as part of a chain
	TSet<FHex> ProcessedClusterInputHexes;

	for (const auto& KV : TileMap)
	{
		const FHex& H = KV.Key;
		const FTile& T = KV.Value;
		if (T.Kind != ETileKind::Input && T.Kind != ETileKind::ClusterInput) continue;
		if (SkipInputThisFrame.Contains(H)) continue;
		if (ItemMap.Contains(H)) continue;           // occupied already

		// For ClusterInput, skip if already processed as part of another chain
		if (T.Kind == ETileKind::ClusterInput && ProcessedClusterInputHexes.Contains(H)) continue;

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
			// Use cached helper function to find chain
			// Only exclude items, not processed tiles - we need to find the complete chain
			TSet<FHex> ExcludeHexes;
			for (const auto& ItemKV : ItemMap)
			{
				ExcludeHexes.Add(ItemKV.Key);
			}
			TArray<FHex> ClusterInputTiles = FindClusterInputChain(H, ExcludeHexes);
			
			// Verify this is a complete chain (not a sub-chain)
			// Check if any ClusterInput tile adjacent to the chain is not in the chain and not already processed
			bool bIsCompleteChain = true;
			TSet<FHex> ChainHexSet;
			for (const FHex& Hex : ClusterInputTiles)
			{
				ChainHexSet.Add(Hex);
			}
			
			for (const FHex& ChainHex : ClusterInputTiles)
			{
				// Check all 6 neighbors
				for (uint8 Dir = 0; Dir < 6; Dir++)
				{
					FHex NeighborHex = HexUtil::Neighbor(ChainHex, (EHexDir)Dir);
					
					// Skip if neighbor is in the chain or already processed
					if (ChainHexSet.Contains(NeighborHex) || ProcessedClusterInputHexes.Contains(NeighborHex))
					{
						continue;
					}
					
					// Check if neighbor is a ClusterInput tile
					const FTile* NeighborTile = TileMap.Find(NeighborHex);
					if (NeighborTile && NeighborTile->Kind == ETileKind::ClusterInput)
					{
						// Found an adjacent ClusterInput that's not in the chain and not processed
						// This means we have a sub-chain, not the complete chain
						bIsCompleteChain = false;
						break;
					}
				}
				if (!bIsCompleteChain) break;
			}

			// Process the chain: spawn individual orbs at each tile, then apply bonds
			// Only process if this is a complete chain (not a sub-chain)
			if (ClusterInputTiles.Num() > 0 && bIsCompleteChain)
			{
				// Use the first tile as the base hex for relative positioning
				FHex BaseHex = ClusterInputTiles[0];

				// Build cluster template from tiles to check against storage
				FClusterTemplate ChainTemplate;
				TMap<FHex, int32> HexToOrbIndex; // Map hex to index in template

				// Add orbs from each ClusterInput tile (using the tile's assigned orb type)
				for (const FHex& ClusterHex : ClusterInputTiles)
				{
					const FTile* ClusterTile = TileMap.Find(ClusterHex);
					if (ClusterTile)
					{
						FOrbData OrbData;
						OrbData.dQ = ClusterHex.Q - BaseHex.Q;
						OrbData.dR = ClusterHex.R - BaseHex.R;
						OrbData.Type = ClusterTile->Arg; // Orb type from tile's Arg
						OrbData.Color = 0; // Default color
						OrbData.ItemId = 0; // Will be assigned when spawning

						int32 OrbIndex = ChainTemplate.Orbs.Add(OrbData);
						HexToOrbIndex.Add(ClusterHex, OrbIndex);
					}
				}

				// Don't add bonds to ChainTemplate - we'll match by orb structure only
				// Bonds will be applied from the stored template when spawning

				// Check if this cluster structure exists in storage by matching orb positions/types only
				AGameManager* GameManager = nullptr;
				if (AOrbSimDriver* Driver = Cast<AOrbSimDriver>(GetOuter()))
				{
					GameManager = Driver->GM;
				}

				int32 ClusterCount = 0;
				const USimWorldHex::FClusterTemplate* StoredTemplate = nullptr;
				FClusterCanonicalKey MatchingKey;

				if (GameManager)
				{
					// Build a template with only orbs (no bonds) for matching
					USimWorldHex::FClusterTemplate OrbsOnlyTemplate;
					OrbsOnlyTemplate.Orbs = ChainTemplate.Orbs;
					// Leave bonds empty - we want to match by orb structure only

					// Use optimized method to find matching stored cluster
					const USimWorldHex::FClusterTemplate* FoundTemplate = nullptr;
					if (GameManager->FindStoredClusterByOrbsOnly(OrbsOnlyTemplate, FoundTemplate, ClusterCount, MatchingKey))
					{
						StoredTemplate = FoundTemplate;
					}
				}

				// Verify all hex positions are available (not occupied by board items)
				bool bAllHexesAvailable = true;
				for (const FHex& ClusterHex : ClusterInputTiles)
				{
					if (ItemMap.Contains(ClusterHex))
					{
						bAllHexesAvailable = false;
						break;
					}
				}

				// Check if any held cluster occupies any tile in the chain
				bool bHeldClusterOccupiesChain = false;
				if (bAllHexesAvailable)
				{
					// Helper function to rotate hex coordinates
					auto Rot60 = [](FHex D, uint8 StepsCCW)
					{
						for (uint8 i = 0; i < (StepsCCW % 6); ++i)
						{
							const int32 q = D.Q;
							const int32 r = D.R;
							D.Q = -r;
							D.R = q + r;
						}
						return D;
					};

					// Build a set of all hex positions in the chain
					TSet<FHex> HeldCheckChainHexSet;
					for (const FHex& ChainHex : ClusterInputTiles)
					{
						HeldCheckChainHexSet.Add(ChainHex);
					}

					// Check all arms for held clusters
					for (const FArmState& ArmState : Arms)
					{
						if (!ArmState.bHolding) continue;

						// Calculate the arm's grip position
						FHex GripHex = ArmState.BaseHex;
						for (int i = 0; i < ArmState.ArmLen; ++i)
						{
							GripHex = HexUtil::Neighbor(GripHex, ArmState.Facing);
						}

						// Check if this is a held cluster
						if (ArmState.Held.bIsCluster)
						{
							// Calculate rotation offset based on GrabFacing
							const uint8 cur = (uint8)ArmState.Facing % 6;
							const int8 grab = (int8)ArmState.Held.GrabFacing;
							const uint8 facingDelta = (grab >= 0)
								? (uint8)(((int32)cur - (int32)(grab % 6) + 6) % 6)
								: cur;
							const uint8 ccw = (uint8)((6 - facingDelta) % 6);

							// Check all sub-orb positions of the held cluster
							for (const FOrbData& Sub : ArmState.Held.Orbs)
							{
								FHex Local(Sub.dQ, Sub.dR);
								FHex RotLocal = Rot60(Local, ccw);
								FHex HeldPos(GripHex.Q + RotLocal.Q, GripHex.R + RotLocal.R);

								// Check if this held position overlaps with any ClusterInput tile in the chain
								if (HeldCheckChainHexSet.Contains(HeldPos))
								{
									bHeldClusterOccupiesChain = true;
									break;
								}
							}
						}
						else
						{
							// Single held orb - check if grip position is in the chain
							if (HeldCheckChainHexSet.Contains(GripHex))
				{
								bHeldClusterOccupiesChain = true;
							}
						}

						if (bHeldClusterOccupiesChain) break;
					}
				}

				// Only spawn if cluster exists in storage, all hexes are available, and no held cluster occupies the chain
				if (ClusterCount > 0 && bAllHexesAvailable && !bHeldClusterOccupiesChain && StoredTemplate)
				{
					// Spawn individual orbs at each ClusterInput tile
					TMap<FHex, int32> HexToItemId; // Map hex to item ID for bond creation

					for (const FHex& ClusterHex : ClusterInputTiles)
					{
						const FTile* ClusterTile = TileMap.Find(ClusterHex);
						if (ClusterTile)
						{
							// Spawn an orb with the tile's assigned type
							FOrb O;
							O.Type = (EOrbType)ClusterTile->Arg;
							FItem NewItem = FItem::MakeSingle(O);
							NewItem.Id = NextItemId++;

							if (TryPutItem(ClusterHex, NewItem))
							{
								HexToItemId.Add(ClusterHex, NewItem.Id);

								// CRITICAL: Mark as newly placed to prevent immediate belt movement
								NewlyPlacedItems.Add(NewItem.Id);
							}
						}
				}
				
					// Apply bonds based on adjacency between ClusterInput tiles
					// Build bonds from the actual tile layout (adjacent ClusterInput tiles get bonds)
					TMap<int32, int32> ChainTemplateIndexToItemId;

					// Map ChainTemplate indices to item IDs using HexToOrbIndex and HexToItemId
					for (const auto& HexItemPair : HexToItemId)
					{
						const FHex& Hex = HexItemPair.Key;
						int32 ItemId = HexItemPair.Value;
						
						// Find the ChainTemplate index for this hex
						if (int32* ChainTemplateIdx = HexToOrbIndex.Find(Hex))
						{
							ChainTemplateIndexToItemId.Add(*ChainTemplateIdx, ItemId);
						}
					}

					// Build bonds based on adjacency between ClusterInput tiles
					TSet<TPair<int32, int32>> AppliedBonds; // Track bonds to avoid duplicates
					int32 BondsCreated = 0; // Debug counter

					// Build bonds based on adjacency - optimized O(n) approach
					// Build a set of all ClusterInput hexes for fast lookup
					TSet<FHex> ClusterInputHexSet;
					for (const FHex& Hex : ClusterInputTiles)
					{
						ClusterInputHexSet.Add(Hex);
					}

					// Check each tile's neighbors (O(n) instead of O(n²))
					for (const FHex& HexA : ClusterInputTiles)
					{
						// Check all 6 neighbors
						for (uint8 Dir = 0; Dir < 6; Dir++)
						{
							FHex NeighborHex = HexUtil::Neighbor(HexA, (EHexDir)Dir);
							
							// Only process if neighbor is in the chain and we process each pair only once
							// Use comparison to ensure we only process each bond once
							if (ClusterInputHexSet.Contains(NeighborHex))
							{
								const FHex& HexB = NeighborHex;
								
								// Process each pair only once (use consistent ordering)
								if (HexA.Q < HexB.Q || (HexA.Q == HexB.Q && HexA.R < HexB.R))
								{
									// These are adjacent - create a bond between the orbs at these positions
									int32* ItemIdA = ChainTemplateIndexToItemId.Find(HexToOrbIndex[HexA]);
									int32* ItemIdB = ChainTemplateIndexToItemId.Find(HexToOrbIndex[HexB]);

									if (ItemIdA && ItemIdB && *ItemIdA > 0 && *ItemIdB > 0)
									{
										// Avoid duplicate bonds (order-independent check)
										int32 MinItemId = FMath::Min(*ItemIdA, *ItemIdB);
										int32 MaxItemId = FMath::Max(*ItemIdA, *ItemIdB);
										TPair<int32, int32> BondKey(MinItemId, MaxItemId);

										if (!AppliedBonds.Contains(BondKey))
										{
											AppliedBonds.Add(BondKey);

											// Create bond between these two orbs
											FBond NewBond;
											NewBond.ItemA = *ItemIdA;
											NewBond.ItemB = *ItemIdB;
											NewBond.Root = HexA;
											NewBond.dQ = HexB.Q - HexA.Q;
											NewBond.dR = HexB.R - HexA.R;
											NewBond.bActive = true;
											
											const int32 BondIndex = Bonds.Add(NewBond);
											AddItemBondLink(NewBond.ItemA, BondIndex);
											AddItemBondLink(NewBond.ItemB, BondIndex);

											BondsCreated++; // Debug counter
										}
									}
								}
							}
						}
					}

					

					// Decrement cluster count in storage
					if (GameManager)
					{
						GameManager->DecrementClusterFromStorageByCanonicalKey(MatchingKey);
					}

					// Mark all ClusterInput tiles in the chain to skip spawning
					for (const FHex& ClusterHex : ClusterInputTiles)
					{
						SkipInputThisFrame.Add(ClusterHex);
						ProcessedClusterInputHexes.Add(ClusterHex);
					}
				}
				else
				{
					// Even if cluster doesn't spawn, mark tiles as processed to avoid duplicate processing
					// (only if this was a complete chain - sub-chains are not marked so they can be part of larger chains)
					for (const FHex& ClusterHex : ClusterInputTiles)
					{
						ProcessedClusterInputHexes.Add(ClusterHex);
					}
				}
			}
			else if (ClusterInputTiles.Num() > 0 && !bIsCompleteChain)
			{
				// This is a sub-chain - don't process it, but also don't mark as processed
				// so it can be part of a larger chain when we encounter a tile that's part of the complete chain
				// Just skip it for now
			}
		}
	}
}

void USimWorldHex::SpawnInputsAfterBelts()
{
	if (TileMap.Num() == 0) return;

	// Track which ClusterInput hexes have already been processed as part of a chain
	TSet<FHex> ProcessedClusterInputHexes;

	for (const auto& KV : TileMap)
	{
		const FHex& H = KV.Key;
		const FTile& T = KV.Value;
		if (T.Kind != ETileKind::Input && T.Kind != ETileKind::ClusterInput) continue;
		if (SkipInputThisFrame.Contains(H)) continue;
		if (ItemMap.Contains(H)) continue; // still occupied after belts

		// For ClusterInput, skip if already processed as part of another chain
		if (T.Kind == ETileKind::ClusterInput && ProcessedClusterInputHexes.Contains(H)) continue;

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
			// Use cached helper function to find chain
			// Only exclude items, not processed tiles - we need to find the complete chain
			TSet<FHex> ExcludeHexes;
			for (const auto& ItemKV : ItemMap)
			{
				ExcludeHexes.Add(ItemKV.Key);
			}
			TArray<FHex> ClusterInputTiles = FindClusterInputChain(H, ExcludeHexes);
			
			// Verify this is a complete chain (not a sub-chain)
			// Check if any ClusterInput tile adjacent to the chain is not in the chain and not already processed
			bool bIsCompleteChain = true;
			TSet<FHex> ChainHexSet;
			for (const FHex& Hex : ClusterInputTiles)
			{
				ChainHexSet.Add(Hex);
			}
			
			for (const FHex& ChainHex : ClusterInputTiles)
			{
				// Check all 6 neighbors
				for (uint8 Dir = 0; Dir < 6; Dir++)
				{
					FHex NeighborHex = HexUtil::Neighbor(ChainHex, (EHexDir)Dir);
					
					// Skip if neighbor is in the chain or already processed
					if (ChainHexSet.Contains(NeighborHex) || ProcessedClusterInputHexes.Contains(NeighborHex))
					{
						continue;
					}
					
					// Check if neighbor is a ClusterInput tile
					const FTile* NeighborTile = TileMap.Find(NeighborHex);
					if (NeighborTile && NeighborTile->Kind == ETileKind::ClusterInput)
					{
						// Found an adjacent ClusterInput that's not in the chain and not processed
						// This means we have a sub-chain, not the complete chain
						bIsCompleteChain = false;
						break;
					}
				}
				if (!bIsCompleteChain) break;
			}

			// Process the chain: spawn individual orbs at each tile, then apply bonds
			// Only process if this is a complete chain (not a sub-chain)
			if (ClusterInputTiles.Num() > 0 && bIsCompleteChain)
			{
				// Use the first tile as the base hex for relative positioning
				FHex BaseHex = ClusterInputTiles[0];

				// Build cluster template from tiles to check against storage
				FClusterTemplate ChainTemplate;
				TMap<FHex, int32> HexToOrbIndex; // Map hex to index in template

				// Add orbs from each ClusterInput tile (using the tile's assigned orb type)
				for (const FHex& ClusterHex : ClusterInputTiles)
				{
					const FTile* ClusterTile = TileMap.Find(ClusterHex);
					if (ClusterTile)
					{
						FOrbData OrbData;
						OrbData.dQ = ClusterHex.Q - BaseHex.Q;
						OrbData.dR = ClusterHex.R - BaseHex.R;
						OrbData.Type = ClusterTile->Arg; // Orb type from tile's Arg
						OrbData.Color = 0; // Default color
						OrbData.ItemId = 0; // Will be assigned when spawning

						int32 OrbIndex = ChainTemplate.Orbs.Add(OrbData);
						HexToOrbIndex.Add(ClusterHex, OrbIndex);
					}
				}

				// Don't add bonds to ChainTemplate - we'll match by orb structure only
				// Bonds will be applied from the stored template when spawning

				// Check if this cluster structure exists in storage by matching orb positions/types only
				AGameManager* GameManager = nullptr;
				if (AOrbSimDriver* Driver = Cast<AOrbSimDriver>(GetOuter()))
				{
					GameManager = Driver->GM;
				}

				int32 ClusterCount = 0;
				const USimWorldHex::FClusterTemplate* StoredTemplate = nullptr;
				FClusterCanonicalKey MatchingKey;

				if (GameManager)
				{
					// Build a template with only orbs (no bonds) for matching
					USimWorldHex::FClusterTemplate OrbsOnlyTemplate;
					OrbsOnlyTemplate.Orbs = ChainTemplate.Orbs;
					// Leave bonds empty - we want to match by orb structure only

					// Use optimized method to find matching stored cluster
					const USimWorldHex::FClusterTemplate* FoundTemplate = nullptr;
					if (GameManager->FindStoredClusterByOrbsOnly(OrbsOnlyTemplate, FoundTemplate, ClusterCount, MatchingKey))
					{
						StoredTemplate = FoundTemplate;
					}
				}

				// Verify all hex positions are available (not occupied by board items)
				bool bAllHexesAvailable = true;
				for (const FHex& ClusterHex : ClusterInputTiles)
				{
					if (ItemMap.Contains(ClusterHex))
					{
						bAllHexesAvailable = false;
						break;
					}
				}

				// Check if any held cluster occupies any tile in the chain
				bool bHeldClusterOccupiesChain = false;
				if (bAllHexesAvailable)
				{
					// Helper function to rotate hex coordinates
					auto Rot60 = [](FHex D, uint8 StepsCCW)
					{
						for (uint8 i = 0; i < (StepsCCW % 6); ++i)
						{
							const int32 q = D.Q;
							const int32 r = D.R;
							D.Q = -r;
							D.R = q + r;
						}
						return D;
					};

					// Build a set of all hex positions in the chain
					TSet<FHex> HeldCheckChainHexSet;
					for (const FHex& ChainHex : ClusterInputTiles)
					{
						HeldCheckChainHexSet.Add(ChainHex);
					}

					// Check all arms for held clusters
					for (const FArmState& ArmState : Arms)
					{
						if (!ArmState.bHolding) continue;

						// Calculate the arm's grip position
						FHex GripHex = ArmState.BaseHex;
						for (int i = 0; i < ArmState.ArmLen; ++i)
						{
							GripHex = HexUtil::Neighbor(GripHex, ArmState.Facing);
						}

						// Check if this is a held cluster
						if (ArmState.Held.bIsCluster)
						{
							// Calculate rotation offset based on GrabFacing
							const uint8 cur = (uint8)ArmState.Facing % 6;
							const int8 grab = (int8)ArmState.Held.GrabFacing;
							const uint8 facingDelta = (grab >= 0)
								? (uint8)(((int32)cur - (int32)(grab % 6) + 6) % 6)
								: cur;
							const uint8 ccw = (uint8)((6 - facingDelta) % 6);

							// Check all sub-orb positions of the held cluster
							for (const FOrbData& Sub : ArmState.Held.Orbs)
							{
								FHex Local(Sub.dQ, Sub.dR);
								FHex RotLocal = Rot60(Local, ccw);
								FHex HeldPos(GripHex.Q + RotLocal.Q, GripHex.R + RotLocal.R);

								// Check if this held position overlaps with any ClusterInput tile in the chain
								if (HeldCheckChainHexSet.Contains(HeldPos))
								{
									bHeldClusterOccupiesChain = true;
									break;
								}
							}
						}
						else
						{
							// Single held orb - check if grip position is in the chain
							if (HeldCheckChainHexSet.Contains(GripHex))
				{
								bHeldClusterOccupiesChain = true;
							}
						}

						if (bHeldClusterOccupiesChain) break;
					}
				}

				// Only spawn if cluster exists in storage, all hexes are available, and no held cluster occupies the chain
				if (ClusterCount > 0 && bAllHexesAvailable && !bHeldClusterOccupiesChain && StoredTemplate)
				{
					// Spawn individual orbs at each ClusterInput tile
					TMap<FHex, int32> HexToItemId; // Map hex to item ID for bond creation

					for (const FHex& ClusterHex : ClusterInputTiles)
					{
						const FTile* ClusterTile = TileMap.Find(ClusterHex);
						if (ClusterTile)
						{
							// Spawn an orb with the tile's assigned type
							FOrb O;
							O.Type = (EOrbType)ClusterTile->Arg;
							FItem NewItem = FItem::MakeSingle(O);
							NewItem.Id = NextItemId++;

							if (TryPutItem(ClusterHex, NewItem))
							{
								HexToItemId.Add(ClusterHex, NewItem.Id);

								// CRITICAL: Mark as newly placed to prevent immediate belt movement
								NewlyPlacedItems.Add(NewItem.Id);
							}
						}
				}
				
					// Apply bonds based on adjacency between ClusterInput tiles
					// Build bonds from the actual tile layout (adjacent ClusterInput tiles get bonds)
					TMap<int32, int32> ChainTemplateIndexToItemId;

					// Map ChainTemplate indices to item IDs using HexToOrbIndex and HexToItemId
					for (const auto& HexItemPair : HexToItemId)
					{
						const FHex& Hex = HexItemPair.Key;
						int32 ItemId = HexItemPair.Value;
						
						// Find the ChainTemplate index for this hex
						if (int32* ChainTemplateIdx = HexToOrbIndex.Find(Hex))
						{
							ChainTemplateIndexToItemId.Add(*ChainTemplateIdx, ItemId);
						}
					}

					// Build bonds based on adjacency between ClusterInput tiles
					TSet<TPair<int32, int32>> AppliedBonds; // Track bonds to avoid duplicates
					int32 BondsCreated = 0; // Debug counter

					// Build bonds based on adjacency - optimized O(n) approach
					// Build a set of all ClusterInput hexes for fast lookup
					TSet<FHex> ClusterInputHexSet;
					for (const FHex& Hex : ClusterInputTiles)
					{
						ClusterInputHexSet.Add(Hex);
					}

					// Check each tile's neighbors (O(n) instead of O(n²))
					for (const FHex& HexA : ClusterInputTiles)
					{
						// Check all 6 neighbors
						for (uint8 Dir = 0; Dir < 6; Dir++)
						{
							FHex NeighborHex = HexUtil::Neighbor(HexA, (EHexDir)Dir);
							
							// Only process if neighbor is in the chain and we process each pair only once
							// Use comparison to ensure we only process each bond once
							if (ClusterInputHexSet.Contains(NeighborHex))
							{
								const FHex& HexB = NeighborHex;
								
								// Process each pair only once (use consistent ordering)
								if (HexA.Q < HexB.Q || (HexA.Q == HexB.Q && HexA.R < HexB.R))
								{
									// These are adjacent - create a bond between the orbs at these positions
									int32* ItemIdA = ChainTemplateIndexToItemId.Find(HexToOrbIndex[HexA]);
									int32* ItemIdB = ChainTemplateIndexToItemId.Find(HexToOrbIndex[HexB]);

									if (ItemIdA && ItemIdB && *ItemIdA > 0 && *ItemIdB > 0)
									{
										// Avoid duplicate bonds (order-independent check)
										int32 MinItemId = FMath::Min(*ItemIdA, *ItemIdB);
										int32 MaxItemId = FMath::Max(*ItemIdA, *ItemIdB);
										TPair<int32, int32> BondKey(MinItemId, MaxItemId);

										if (!AppliedBonds.Contains(BondKey))
										{
											AppliedBonds.Add(BondKey);

											// Create bond between these two orbs
											FBond NewBond;
											NewBond.ItemA = *ItemIdA;
											NewBond.ItemB = *ItemIdB;
											NewBond.Root = HexA;
											NewBond.dQ = HexB.Q - HexA.Q;
											NewBond.dR = HexB.R - HexA.R;
											NewBond.bActive = true;
											
											const int32 BondIndex = Bonds.Add(NewBond);
											AddItemBondLink(NewBond.ItemA, BondIndex);
											AddItemBondLink(NewBond.ItemB, BondIndex);

											BondsCreated++; // Debug counter
										}
									}
								}
							}
						}
					}


					// Decrement cluster count in storage
					if (GameManager)
					{
						GameManager->DecrementClusterFromStorageByCanonicalKey(MatchingKey);
					}

					// Mark all ClusterInput tiles in the chain to skip spawning
					for (const FHex& ClusterHex : ClusterInputTiles)
					{
						SkipInputThisFrame.Add(ClusterHex);
						ProcessedClusterInputHexes.Add(ClusterHex);
					}
				}
				else
				{
					// Even if cluster doesn't spawn, mark tiles as processed to avoid duplicate processing
					// (only if this was a complete chain - sub-chains are not marked so they can be part of larger chains)
					for (const FHex& ClusterHex : ClusterInputTiles)
					{
						ProcessedClusterInputHexes.Add(ClusterHex);
					}
				}
			}
			else if (ClusterInputTiles.Num() > 0 && !bIsCompleteChain)
			{
				// This is a sub-chain - don't process it, but also don't mark as processed
				// so it can be part of a larger chain when we encounter a tile that's part of the complete chain
				// Just skip it for now
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
		

		
		// ClusterOutputs should only process complete clusters, not single orbs
		// and only if the entire bonded cluster can be processed (all orbs on ClusterOutput tiles)

		// Must have at least 2 tiles to be a cluster (not a single orb)
		if (ClusterTiles.Num() < 2)
		{
			continue; // Skip single orb processing
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
		if (bAllTilesReady && OrbItemIds.Num() > 0)
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
				// CRITICAL: Check if the bonded cluster has any orbs that are NOT on our ClusterOutput tiles
				// This prevents processing clusters that extend beyond what can be processed
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
						
								int32 OtherItemId = (Bond.ItemA == ItemIdA) ? Bond.ItemB : Bond.ItemA;
								if (OtherItemId == ItemIdB)
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
		
		// Build cluster template for storage BEFORE removing items
		if (NewAnimations.Num() > 0)
		{
			USimWorldHex::FClusterTemplate ProcessedClusterTemplate;
			
			// Use the first tile as the base
			FHex BaseHex = NewAnimations[0].Hex;
			
			// Convert animations to canonical orb data
			TMap<int32, int32> ItemIdToIndex;
			for (const FAnimatedOrb& Anim : NewAnimations)
			{
				FOrbData OrbData;
				OrbData.Type = (uint8)Anim.Item.Single.Type;
				OrbData.Color = 0;
				OrbData.dQ = Anim.Hex.Q - BaseHex.Q;
				OrbData.dR = Anim.Hex.R - BaseHex.R;
				OrbData.ItemId = 0; // Don't store ItemId in templates
				
				int32 Index = ProcessedClusterTemplate.Orbs.Add(OrbData);
				ItemIdToIndex.Add(Anim.Item.Id, Index);
			}
			
			// Convert bonds to template bonds
			for (const FAnimatedOrb& Anim : NewAnimations)
			{
				if (const TArray<int32>* BondList = ItemToBonds.Find(Anim.Item.Id))
				{
					for (int32 BondIdx : *BondList)
					{
						if (!Bonds.IsValidIndex(BondIdx)) continue;
						const FBond& Bond = Bonds[BondIdx];
						if (!Bond.bActive) continue;
						
						int32 OtherItemId = (Bond.ItemA == Anim.Item.Id) ? Bond.ItemB : Bond.ItemA;
						if (int32* OtherIndex = ItemIdToIndex.Find(OtherItemId))
						{
							int32 ThisIndex = ItemIdToIndex[Anim.Item.Id];
							// Only add each bond once (smaller index first)
							int32 A = FMath::Min(ThisIndex, *OtherIndex);
							int32 B = FMath::Max(ThisIndex, *OtherIndex);
							
							bool bAlreadyExists = false;
							for (const TPair<int32, int32>& ExistingBond : ProcessedClusterTemplate.Bonds)
							{
								if (ExistingBond.Key == A && ExistingBond.Value == B)
								{
									bAlreadyExists = true;
									break;
								}
							}
							
							if (!bAlreadyExists)
							{
								ProcessedClusterTemplate.Bonds.Add(TPair<int32, int32>(A, B));
							}
						}
					}
				}
			}
			
			// Notify GameManager to store this cluster
			if (AOrbSimDriver* Driver = Cast<AOrbSimDriver>(GetOuter()))
			{
				if (Driver->GM)
				{
					Driver->GM->AddClusterToStorage(ProcessedClusterTemplate);
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
			// T->Arg contains the orb type (like ClusterOutput), not a template ID
			const EOrbType OrbType = (EOrbType)T->Arg;

			// Find a cluster template that matches this orb type
			uint8 MatchingTemplateId = 0;
			const FClusterTemplate* Template = nullptr;

			// First pass: find templates whose first orb matches the orb type
			for (const auto& TemplatePair : ClusterTemplates)
			{
				const FClusterTemplate& CandidateTemplate = TemplatePair.Value;
				if (CandidateTemplate.Orbs.Num() > 0 && CandidateTemplate.Orbs[0].Type == (uint8)OrbType)
				{
					MatchingTemplateId = TemplatePair.Key;
					Template = &CandidateTemplate;
					break;
				}
			}

			// Second pass: find any template containing this orb type
			if (!Template)
			{
				for (const auto& TemplatePair : ClusterTemplates)
				{
					const FClusterTemplate& CandidateTemplate = TemplatePair.Value;
					for (const auto& Orb : CandidateTemplate.Orbs)
					{
						if (Orb.Type == (uint8)OrbType)
						{
							MatchingTemplateId = TemplatePair.Key;
							Template = &CandidateTemplate;
							break;
						}
					}
					if (Template) break;
				}
			}

			if (Template)
			{
				// Check if cluster exists in storage and has count > 0
				AGameManager* GameManager = nullptr;
				if (AOrbSimDriver* Driver = Cast<AOrbSimDriver>(GetOuter()))
				{
					GameManager = Driver->GM;
				}

				int32 ClusterCount = 0;
				if (GameManager)
				{
					ClusterCount = GameManager->GetClusterCountByTemplateId(MatchingTemplateId);
				}

				// Only spawn if cluster exists in storage with count > 0
				if (ClusterCount > 0)
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
				
					// Create bonds between orbs based on template (pre-bonded)
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

					// Decrement cluster count in storage
					if (GameManager)
					{
						GameManager->DecrementClusterFromStorageByTemplateId(MatchingTemplateId);
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
			// T->Arg contains the orb type (like ClusterOutput), not a template ID
			const EOrbType OrbType = (EOrbType)T->Arg;

			// Find a cluster template that matches this orb type
			const FClusterTemplate* Template = nullptr;

			// First pass: find templates whose first orb matches the orb type
			for (const auto& TemplatePair : ClusterTemplates)
			{
				const FClusterTemplate& CandidateTemplate = TemplatePair.Value;
				if (CandidateTemplate.Orbs.Num() > 0 && CandidateTemplate.Orbs[0].Type == (uint8)OrbType)
				{
					Template = &CandidateTemplate;
					break;
				}
			}

			// Second pass: find any template containing this orb type
			if (!Template)
			{
				for (const auto& TemplatePair : ClusterTemplates)
				{
					const FClusterTemplate& CandidateTemplate = TemplatePair.Value;
					for (const auto& Orb : CandidateTemplate.Orbs)
					{
						if (Orb.Type == (uint8)OrbType)
						{
							Template = &CandidateTemplate;
							break;
						}
					}
					if (Template) break;
				}
			}

			if (Template)
			{
				// Create cluster from template for preview
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
