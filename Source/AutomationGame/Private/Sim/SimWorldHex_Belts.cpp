#include "Sim/SimWorldHex.h"

// Simplified belt processing - more reliable approach
void USimWorldHex::StepBelts()
{
	if (TileMap.Num() == 0 || ItemMap.Num() == 0) return;

	const float Speed = FMath::Max(0.f, BeltSpeedTilesPerTick);
	const float BlockClamp = BeltBlockedClamp;

	// Gather all hexes occupied by held orbs
	TSet<FHex> HeldOrbHexes;
	for (const FArmState& A : Arms)
	{
		if (!A.bHolding) continue;

		FHex GripHex = A.BaseHex;
		for (int i = 0; i < A.ArmLen; ++i)
			GripHex = HexUtil::Neighbor(GripHex, A.Facing);

		if (!A.Held.bIsCluster)
		{
			HeldOrbHexes.Add(GripHex);
		}
		else
		{
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

	// Track items that need their bIsReady flag set to true at the end of this tick
	// This prevents multiple belts from competing for the same orb in the same tick
	TSet<int32> ItemsToSetReady;

	// Simple approach: Build a comprehensive move plan first, then execute atomically
	struct FMoveOperation
	{
		int32 ItemId;
		FHex FromHex;
		FHex ToHex;
		TArray<int32> ConnectedItemIds; // All items that must move together as a unit
	};

	TArray<FMoveOperation> MoveOperations;
	TSet<int32> ProcessedItems;

	// Advance belt progress for all items
	const float Dt = FMath::Max(0.f, FixedStepSec);
	const float TilesPerSec = (Speed / FMath::Max(KINDA_SMALL_NUMBER, Dt));
	const float ProgressPerStep = TilesPerSec * Dt;

	for (const auto& KV : ItemMap)
	{
		if (KV.Value.Id == 0) continue;
		if (NewlyPlacedItems.Contains(KV.Value.Id)) continue;

		const FTile* T = TileMap.Find(KV.Key);
		if (T && T->Kind == ETileKind::Belt)
		{
			float& Progress = BeltProgress.FindOrAdd(KV.Value.Id, 0.f);
			Progress += ProgressPerStep;
		}
	}

	// Process each item that's ready to move
	for (const auto& KV : ItemMap)
	{
		if (KV.Value.Id == 0) continue;
		if (ProcessedItems.Contains(KV.Value.Id)) continue;
		if (NewlyPlacedItems.Contains(KV.Value.Id)) continue;

		const FHex& CurrentHex = KV.Key;
		const FItem& Item = KV.Value;
		const FTile* T = TileMap.Find(CurrentHex);

		if (!T || T->Kind != ETileKind::Belt) continue;

		float* Progress = BeltProgress.Find(Item.Id);
		if (!Progress || *Progress < 1.f) continue;

		// Calculate destination
		const uint8 Dir = (uint8)(T->Arg & 0xFF);
		const FHex ToHex = HexUtil::Neighbor(CurrentHex, (EHexDir)(Dir % 6));

		// Check if destination is blocked
		if (HeldOrbHexes.Contains(ToHex) ||
			(ItemMap.Contains(ToHex) && !ProcessedItems.Contains(ItemMap[ToHex].Id)))
		{
			*Progress = FMath::Min(*Progress, BlockClamp);
			continue;
		}

		// Check if destination is a bond/unbond tile - orbs MUST stop on these tiles for interaction
		// Orbs should always spend at least one tick on bond/unbond tiles before being pulled off
		const FTile* DestTile = TileMap.Find(ToHex);
		if (DestTile && (DestTile->Kind == ETileKind::Bond || DestTile->Kind == ETileKind::Unbond))
		{
			// If the destination is a bond/unbond tile that's not already occupied, 
			// the orb should stop there (not skip over it)
			if (!ItemMap.Contains(ToHex))
			{
				// Allow the orb to move onto the bond/unbond tile, but it will stop there
				// The bond/unbond interaction will happen in StepBonds() during the tick it spends there
			}
			else
			{
				// Bond/unbond tile is occupied, block the move
				*Progress = FMath::Min(*Progress, BlockClamp);
				continue;
			}
		}

		// Build connected component (all bonded items that must move together)
		FMoveOperation MoveOp;
		MoveOp.ItemId = Item.Id;
		MoveOp.FromHex = CurrentHex;
		MoveOp.ToHex = ToHex;

		// Simple flood fill to find all connected items
		TArray<int32> ToProcess;
		TSet<int32> InCluster;
		ToProcess.Add(Item.Id);
		InCluster.Add(Item.Id);

		while (ToProcess.Num() > 0)
		{
			int32 CurrentId = ToProcess.Pop();
			MoveOp.ConnectedItemIds.Add(CurrentId);

			// Find all items bonded to this one
			if (const TArray<int32>* BondList = ItemToBonds.Find(CurrentId))
			{
				for (int32 BondIdx : *BondList)
				{
					if (!Bonds.IsValidIndex(BondIdx)) continue;
					const FBond& Bond = Bonds[BondIdx];
					if (!Bond.bActive) continue;

					int32 OtherItemId = (Bond.ItemA == CurrentId) ? Bond.ItemB : Bond.ItemA;
					if (!InCluster.Contains(OtherItemId))
					{
						// Check if the other item is also on the board (not held)
						bool bFoundOnBoard = false;
						for (const auto& BoardKV : ItemMap)
						{
							if (BoardKV.Value.Id == OtherItemId)
							{
								bFoundOnBoard = true;
								break;
							}
						}

						if (bFoundOnBoard)
						{
							ToProcess.Add(OtherItemId);
							InCluster.Add(OtherItemId);
						}
					}
				}
			}
		}

		// Validate that entire cluster can move as a unit
		bool bCanMoveCluster = true;
		FHex ClusterMoveOffset(ToHex.Q - CurrentHex.Q, ToHex.R - CurrentHex.R);

		for (int32 ConnectedId : MoveOp.ConnectedItemIds)
		{
			// Find current position of this connected item
			FHex ConnectedCurrentHex;
			bool bFoundConnected = false;
			for (const auto& BoardKV : ItemMap)
			{
				if (BoardKV.Value.Id == ConnectedId)
				{
					ConnectedCurrentHex = BoardKV.Key;
					bFoundConnected = true;
					break;
				}
			}

			if (!bFoundConnected)
			{
				bCanMoveCluster = false;
				break;
			}

			// Calculate where this connected item would move to
			FHex ConnectedToHex(ConnectedCurrentHex.Q + ClusterMoveOffset.Q,
				ConnectedCurrentHex.R + ClusterMoveOffset.R);

			// Check if destination is valid
			if (HeldOrbHexes.Contains(ConnectedToHex))
			{
				bCanMoveCluster = false;
				break;
			}

			// Check if destination is occupied by something not in our cluster
			if (const FItem* DestItem = ItemMap.Find(ConnectedToHex))
			{
				if (!InCluster.Contains(DestItem->Id))
				{
					bCanMoveCluster = false;
					break;
				}
			}
		}

		if (bCanMoveCluster)
		{
			MoveOperations.Add(MoveOp);
			for (int32 ConnectedId : MoveOp.ConnectedItemIds)
			{
				ProcessedItems.Add(ConnectedId);
			}
		}
		else
		{
			*Progress = FMath::Min(*Progress, BlockClamp);
		}
	}

	// Execute all moves atomically
	for (const FMoveOperation& MoveOp : MoveOperations)
	{
		FHex MoveOffset(MoveOp.ToHex.Q - MoveOp.FromHex.Q, MoveOp.ToHex.R - MoveOp.FromHex.R);

		// Check if moving to bond/unbond tiles or their partner areas - set bIsReady to false
		const FTile* DestTile = TileMap.Find(MoveOp.ToHex);
		bool bMovingToBondTile = (DestTile && (DestTile->Kind == ETileKind::Bond || DestTile->Kind == ETileKind::Unbond));

		// Also check if moving to a partner hex of a bond/unbond glyph
		bool bMovingToPartnerArea = false;
		if (!bMovingToBondTile) // Only check for partner area if not already a primary bond/unbond tile
		{
			for (const auto& TileKV : TileMap)
			{
				const FTile& CheckTile = TileKV.Value;
				if (CheckTile.Kind != ETileKind::Bond && CheckTile.Kind != ETileKind::Unbond) continue;

				// Calculate this glyph's partner hex
				const uint8 CheckGlyphDir = (uint8)(CheckTile.Arg & 0xFF);
				const FHex CheckPartnerHex = HexUtil::Neighbor(TileKV.Key, (EHexDir)(CheckGlyphDir % 6));

				// If our destination hex is this glyph's partner hex
				if (CheckPartnerHex == MoveOp.ToHex)
				{
					bMovingToPartnerArea = true;
					break;
				}
			}
		}

		// Set flag if moving to either primary tile or partner area of a bond/unbond glyph
		bool bMovingToBondArea = bMovingToBondTile || bMovingToPartnerArea;

		// Snapshot all items that will move
		TMap<int32, FItem> MovingItems;
		TMap<int32, FHex> OldPositions;
		TMap<int32, FHex> NewPositions;

		for (int32 ItemId : MoveOp.ConnectedItemIds)
		{
			for (const auto& KV : ItemMap)
			{
				if (KV.Value.Id == ItemId)
				{
					MovingItems.Add(ItemId, KV.Value);
					OldPositions.Add(ItemId, KV.Key);
					NewPositions.Add(ItemId, FHex(KV.Key.Q + MoveOffset.Q, KV.Key.R + MoveOffset.R));
					break;
				}
			}
		}

		// Remove all items from their old positions
		for (const auto& KV : OldPositions)
		{
			ItemMap.Remove(KV.Value);
			OrbAt.Remove(KV.Value);
		}

		// Place all items at their new positions
		for (const auto& KV : NewPositions)
		{
			int32 ItemId = KV.Key;
			FHex NewHex = KV.Value;

			if (FItem* MovingItem = MovingItems.Find(ItemId))
			{
				// If moving to a bond/unbond area (primary tile or partner area), mark as not ready
				if (bMovingToBondArea)
				{
					MovingItem->bIsReady = false;
				}

				ItemMap.Add(NewHex, *MovingItem);
				OrbAt.Add(NewHex, KindFromItem(*MovingItem));

				// Update belt progress
				if (float* Progress = BeltProgress.Find(ItemId))
				{
					*Progress -= 1.f;
					if (*Progress < 0.f) *Progress = 0.f;
				}

				// Check if moved to special tiles
				if (const FTile* TD = TileMap.Find(NewHex))
				{
					if (TD->Kind != ETileKind::Belt)
						BeltProgress.Remove(ItemId);
					if (TD->Kind == ETileKind::Output)
						OrbsNewlyMovedToOutput.Add(NewHex);
				}
				else
				{
					BeltProgress.Remove(ItemId);
				}
			}
		}

		// Update bond positions to maintain cluster integrity
		for (int32 ItemId : MoveOp.ConnectedItemIds)
		{
			if (const TArray<int32>* BondIndices = ItemToBonds.Find(ItemId))
			{
				for (int32 BondIdx : *BondIndices)
				{
					if (!Bonds.IsValidIndex(BondIdx)) continue;
					FBond& Bond = Bonds[BondIdx];
					if (!Bond.bActive) continue;

					// Only update bonds between items in our moving cluster
					bool bBothInCluster = MoveOp.ConnectedItemIds.Contains(Bond.ItemA) &&
						MoveOp.ConnectedItemIds.Contains(Bond.ItemB);

					if (bBothInCluster)
					{
						// Update bond root to new position
						if (Bond.ItemA == ItemId)
						{
							if (FHex* NewPos = NewPositions.Find(ItemId))
							{
								Bond.Root = *NewPos;
							}
						}
					}
				}
			}
		}
	}

	// Record moved items for cleanup
	ItemsMovedLastStep.Empty();
	for (const FMoveOperation& MoveOp : MoveOperations)
	{
		for (int32 ItemId : MoveOp.ConnectedItemIds)
		{
			ItemsMovedLastStep.Add(ItemId);
		}
	}

	// Helper function to try pulling from a specific hex
	auto TryPullFromHex = [&](const FHex& BeltHex, const FHex& SourceHex, const FTile* SourceTile) -> bool
		{
			if (!SourceTile) return false;

			// Handle input tiles (existing logic)
			if (SourceTile->Kind == ETileKind::Input)
			{
				const FItem* InputItem = ItemMap.Find(SourceHex);
				if (InputItem && InputItem->Id > 0 &&
					!NewlyPlacedItems.Contains(InputItem->Id))
				{
					// Move the item from input to belt
					FItem MovingItem = *InputItem;

					// Remove from input
					ItemMap.Remove(SourceHex);
					OrbAt.Remove(SourceHex);

					// Add to belt
					ItemMap.Add(BeltHex, MovingItem);
					OrbAt.Add(BeltHex, KindFromItem(MovingItem));

					// Initialize belt progress for the pulled item
					BeltProgress.Add(MovingItem.Id, 0.f);

					// Record this item as moved for interpolation
					ItemsMovedLastStep.Add(MovingItem.Id);

					return true;
				}
			}

			// Handle bond/unbond tiles (enhanced logic for both sides)
			if (SourceTile->Kind == ETileKind::Bond || SourceTile->Kind == ETileKind::Unbond)
			{
				FItem* BondItem = ItemMap.Find(SourceHex);
				if (BondItem && BondItem->Id > 0 &&
					!NewlyPlacedItems.Contains(BondItem->Id) &&
					!RecentlyDroppedByArm.Contains(BondItem->Id))
				{
					// Check the IsReady flag
					if (!BondItem->bIsReady)
					{
						// Orb is not ready to be pulled yet - mark it to be set ready at the end of this tick
						// This prevents multiple belts from competing for the same orb in the same tick
						ItemsToSetReady.Add(BondItem->Id);
						return false; // Not pulled this tick, but marked for next tick
					}

					// Use the same bonded cluster logic as regular belt movement
					TArray<int32> ConnectedItemIds;
					TSet<int32> InCluster;
					TArray<int32> ToProcess;

					ToProcess.Add(BondItem->Id);
					InCluster.Add(BondItem->Id);

					// Find all bonded items using flood fill
					while (ToProcess.Num() > 0)
					{
						int32 CurrentId = ToProcess.Pop();
						ConnectedItemIds.Add(CurrentId);

						if (const TArray<int32>* BondList = ItemToBonds.Find(CurrentId))
						{
							for (int32 BondIdx : *BondList)
							{
								if (!Bonds.IsValidIndex(BondIdx)) continue;
								const FBond& Bond = Bonds[BondIdx];
								if (!Bond.bActive) continue;

								int32 OtherItemId = (Bond.ItemA == CurrentId) ? Bond.ItemB : Bond.ItemA;
								if (!InCluster.Contains(OtherItemId))
								{
									// Check if the other item is also on the board (not held)
									bool bFoundOnBoard = false;
									for (const auto& BoardKV : ItemMap)
									{
										if (BoardKV.Value.Id == OtherItemId)
										{
											bFoundOnBoard = true;
											break;
										}
									}

									if (bFoundOnBoard)
									{
										ToProcess.Add(OtherItemId);
										InCluster.Add(OtherItemId);
									}
								}
							}
						}
					}

					// Validate that entire cluster can move to the belt
					bool bCanMoveCluster = true;
					FHex ClusterMoveOffset(BeltHex.Q - SourceHex.Q, BeltHex.R - SourceHex.R);

					// Build snapshot of cluster positions
					TMap<int32, FHex> ClusterPositions;
					for (int32 ConnectedId : ConnectedItemIds)
					{
						for (const auto& BoardKV : ItemMap)
						{
							if (BoardKV.Value.Id == ConnectedId)
							{
								ClusterPositions.Add(ConnectedId, BoardKV.Key);
								break;
							}
						}
					}

					// Check if all cluster items can move to their new positions
					for (int32 ConnectedId : ConnectedItemIds)
					{
						if (FHex* CurrentPos = ClusterPositions.Find(ConnectedId))
						{
							FHex NewPos(CurrentPos->Q + ClusterMoveOffset.Q, CurrentPos->R + ClusterMoveOffset.R);

							// Check if destination is occupied by something not in our cluster
							if (const FItem* DestItem = ItemMap.Find(NewPos))
							{
								if (!InCluster.Contains(DestItem->Id))
								{
									bCanMoveCluster = false;
									break;
								}
							}

							// Check if destination is blocked by held orbs
							if (HeldOrbHexes.Contains(NewPos))
							{
								bCanMoveCluster = false;
								break;
							}
						}
						else
						{
							bCanMoveCluster = false;
							break;
						}
					}

					if (bCanMoveCluster)
					{
						// Snapshot all moving items
						TMap<int32, FItem> MovingItems;
						TMap<int32, FHex> OldPositions;
						TMap<int32, FHex> NewPositions;

						for (int32 ItemId : ConnectedItemIds)
						{
							if (FHex* CurrentPos = ClusterPositions.Find(ItemId))
							{
								if (FItem* MovingItem = ItemMap.Find(*CurrentPos))
								{
									MovingItems.Add(ItemId, *MovingItem);
									OldPositions.Add(ItemId, *CurrentPos);
									NewPositions.Add(ItemId, FHex(CurrentPos->Q + ClusterMoveOffset.Q, CurrentPos->R + ClusterMoveOffset.R));
								}
							}
						}

						// Remove all items from their old positions
						for (const auto& OldPosKV : OldPositions)
						{
							ItemMap.Remove(OldPosKV.Value);
							OrbAt.Remove(OldPosKV.Value);
						}

						// Place all items at their new positions
						for (const auto& NewPosKV : NewPositions)
						{
							int32 ItemId = NewPosKV.Key;
							FHex NewHex = NewPosKV.Value;

							if (FItem* MovingItem = MovingItems.Find(ItemId))
							{
								// Reset bIsReady when moving from bond/unbond tiles to belts
								MovingItem->bIsReady = true;

								ItemMap.Add(NewHex, *MovingItem);
								OrbAt.Add(NewHex, KindFromItem(*MovingItem));

								// Initialize belt progress for pulled items
								BeltProgress.Add(MovingItem->Id, 0.f);

								// Record this item as moved for interpolation
								ItemsMovedLastStep.Add(MovingItem->Id);
							}
						}

						// Update bond positions to maintain cluster integrity
						for (int32 ItemId : ConnectedItemIds)
						{
							if (const TArray<int32>* BondIndices = ItemToBonds.Find(ItemId))
							{
								for (int32 BondIdx : *BondIndices)
								{
									if (!Bonds.IsValidIndex(BondIdx)) continue;
									FBond& Bond = Bonds[BondIdx];
									if (!Bond.bActive) continue;

									// Only update bonds between items in our moving cluster
									bool bBothInCluster = InCluster.Contains(Bond.ItemA) && InCluster.Contains(Bond.ItemB);

									if (bBothInCluster)
									{
										// Update bond root to new position
										if (Bond.ItemA == ItemId)
										{
											if (FHex* NewPos = NewPositions.Find(ItemId))
											{
												Bond.Root = *NewPos;
											}
										}
									}
								}
							}
						}

						return true;
					}
				}
			}

			return false;
		};

	// Input Tile Pull Pass - Enhanced to handle bond/unbond glyph areas correctly
	for (const auto& KV : TileMap)
	{
		const FHex& BeltHex = KV.Key;
		const FTile& BeltTile = KV.Value;

		// Only process belt tiles
		if (BeltTile.Kind != ETileKind::Belt) continue;

		// Skip if this belt already has an item
		if (ItemMap.Contains(BeltHex)) continue;

		// Skip if a held orb is blocking this belt
		if (HeldOrbHexes.Contains(BeltHex)) continue;

		// Calculate the hex opposite to the belt's direction
		const uint8 BeltDir = (uint8)(BeltTile.Arg & 0xFF);
		const EHexDir OppositeDir = (EHexDir)(((int32)BeltDir + 3) % 6); // Opposite direction
		const FHex InputHex = HexUtil::Neighbor(BeltHex, OppositeDir);

		// Get the tile at the input hex
		const FTile* InputTile = TileMap.Find(InputHex);

		// CASE 1: Input hex has a tile - use normal logic (including bond/unbond primary hex)
		if (InputTile)
		{
			if (TryPullFromHex(BeltHex, InputHex, InputTile))
			{
				continue; // Successfully pulled, move to next belt
			}
		}
		// CASE 2: Input hex has no tile - check if it's a partner hex of a bond/unbond glyph
		else
		{
			// Check all bond/unbond tiles to see if our InputHex is their partner hex
			bool bFoundGlyphPartner = false;
			for (const auto& TileKV : TileMap)
			{
				const FTile& CheckTile = TileKV.Value;
				if (CheckTile.Kind != ETileKind::Bond && CheckTile.Kind != ETileKind::Unbond) continue;

				// Calculate this glyph's partner hex
				const uint8 CheckGlyphDir = (uint8)(CheckTile.Arg & 0xFF);
				const FHex CheckPartnerHex = HexUtil::Neighbor(TileKV.Key, (EHexDir)(CheckGlyphDir % 6));

				// If our input hex is this glyph's partner hex
				if (CheckPartnerHex == InputHex)
				{
					// Check if there's an item at the partner hex (InputHex)
					const FItem* PartnerItem = ItemMap.Find(InputHex);
					if (PartnerItem && PartnerItem->Id > 0 &&
						!NewlyPlacedItems.Contains(PartnerItem->Id) &&
						!RecentlyDroppedByArm.Contains(PartnerItem->Id))
					{
						// Create a virtual tile for the TryPullFromHex call
						// Use the same properties as the primary glyph hex
						FTile VirtualPartnerTile = CheckTile;

						if (TryPullFromHex(BeltHex, InputHex, &VirtualPartnerTile))
						{
							bFoundGlyphPartner = true;
							break; // Successfully pulled, stop checking other glyphs
						}
					}
				}
			}
		}
	}

	// Apply deferred bIsReady changes at the end of the tick
	// This ensures that multiple belts can't compete for the same orb in the same tick
	for (int32 ItemId : ItemsToSetReady)
	{
		// Find the item in the map and set it ready
		for (auto& KV : ItemMap)
		{
			if (KV.Value.Id == ItemId)
			{
				KV.Value.bIsReady = true;
				break;
			}
		}
	}
	
	// Set newly placed items to ready for next tick
	// This prevents items dropped by arms from being immediately pulled by belts
	for (int32 ItemId : NewlyPlacedItems)
	{
		// Find the item in the map and set it ready for next tick
		for (auto& KV : ItemMap)
		{
			if (KV.Value.Id == ItemId)
			{
				KV.Value.bIsReady = true;
				break;
			}
		}
	}

	// Clear newly placed items and recently dropped tracking
	NewlyPlacedItems.Empty();
	RecentlyDroppedByArm.Empty();
}
