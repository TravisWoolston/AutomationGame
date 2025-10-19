#include "Sim/SimWorldHex.h"

// Simple belt system - items move every tick based on belt speed
void USimWorldHex::StepBelts()
{
	if (TileMap.Num() == 0 || ItemMap.Num() == 0) return;


	const float Speed = FMath::Max(0.f, 1.f);
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
	TSet<int32> ItemsToSetReady;

	// Structure for move operations
	struct FMoveOperation
	{
		int32 ItemId;
		FHex FromHex;
		FHex ToHex;
		TArray<int32> ConnectedItemIds; // All items that must move together as a unit
	};

	// SIMPLE BELT SYSTEM - Move items every tick based on belt speed
	// Iterative movement to allow orbs to move to vacant positions on the same tick

	// Continue moving orbs until no more movements are possible
	// Each orb can only move once per tick, but we iterate to fill vacant positions
	bool bAnyMovement = true;
	int32 MaxIterations = 10; // Safety limit to prevent infinite loops
	int32 CurrentIteration = 0;
	
	// Track which orbs have already moved this tick
	TSet<int32> OrbsMovedThisTick;
	
	while (bAnyMovement && CurrentIteration < MaxIterations)
	{
		bAnyMovement = false;
		CurrentIteration++;

		// Move items on belts - discrete tile-to-tile each tick
		// Renderer handles smooth interpolation using Alpha
		TArray<FMoveOperation> MoveOperations;
		TSet<int32> ProcessedItems;
		
		// Track destinations that will be occupied after all moves
		TMap<FHex, int32> PendingDestinations; // Maps destination hex to item ID that will move there

	// Process each belt tile independently
	for (const auto& KV : ItemMap)
	{
		const FHex& FromHex = KV.Key;
		const FItem& Item = KV.Value;
		
		if (Item.Id == 0) continue;
		if (ProcessedItems.Contains(Item.Id)) continue;
		
		// Skip orbs that have already moved this tick
		if (OrbsMovedThisTick.Contains(Item.Id)) continue;
		
		// Check if this item is on a belt
		const FTile* BeltTile = TileMap.Find(FromHex);
		if (!BeltTile || BeltTile->Kind != ETileKind::Belt) continue;
		
		// Get belt direction and destination
		const uint8 BeltDir = (uint8)(BeltTile->Arg & 0xFF);
		const FHex ToHex = HexUtil::Neighbor(FromHex, (EHexDir)(BeltDir % 6));
		
		// Check if destination is available
		// First check static obstacles (held orbs)
		bool bCanMove = !HeldOrbHexes.Contains(ToHex);
		
		// Then check if another item is at the destination
		if (bCanMove && ItemMap.Contains(ToHex))
		{
			// Destination is occupied, we're blocked for now
			// The cluster validation will handle chain movements properly
			bCanMove = false;
		}
		
		// Also check if another item already claimed this destination
		if (bCanMove && PendingDestinations.Contains(ToHex))
		{
			bCanMove = false;
		}
		
		// Don't move items that were just dropped by arms (settling period)
		if (RecentlyDroppedByArm.Contains(Item.Id))
		{
			bCanMove = false;
			if (bDebugLogs && GEngine)
			{
				GEngine->AddOnScreenDebugMessage(-1, 0.5f, FColor::Yellow,
					FString::Printf(TEXT("Belt blocked: Item %d was recently dropped by arm"), Item.Id));
			}
		}
		
		// Special case for bond/unbond tiles - can move there if empty
		const FTile* DestTile = TileMap.Find(ToHex);
		if (DestTile && (DestTile->Kind == ETileKind::Bond || DestTile->Kind == ETileKind::Unbond))
		{
			bCanMove = !ItemMap.Contains(ToHex);
		}
		
		if (!bCanMove) continue; // Blocked, skip this item
		
		// Build bonded cluster for this item
		FMoveOperation MoveOp;
		MoveOp.ItemId = Item.Id;
		MoveOp.FromHex = FromHex;
		MoveOp.ToHex = ToHex;

		// Find all bonded items that must move together
		TArray<int32> ToProcess;
		TSet<int32> InCluster;
		ToProcess.Add(Item.Id);
		InCluster.Add(Item.Id);

		while (ToProcess.Num() > 0)
		{
			int32 CurrentId = ToProcess.Pop();
			MoveOp.ConnectedItemIds.Add(CurrentId);

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

		// Validate cluster can move as a unit
		bool bCanMoveCluster = true;
		FHex ClusterMoveOffset(ToHex.Q - FromHex.Q, ToHex.R - FromHex.R);

		for (int32 ConnectedId : MoveOp.ConnectedItemIds)
		{
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

			FHex ConnectedToHex(ConnectedCurrentHex.Q + ClusterMoveOffset.Q,
				ConnectedCurrentHex.R + ClusterMoveOffset.R);

			if (HeldOrbHexes.Contains(ConnectedToHex))
			{
				bCanMoveCluster = false;
				break;
			}

			// Check if destination is available
			if (const FItem* DestItem = ItemMap.Find(ConnectedToHex))
			{
				if (!InCluster.Contains(DestItem->Id))
				{
					// Destination is blocked
					bCanMoveCluster = false;
					break;
				}
			}
			
			// Check if another pending move already claimed this destination
			if (PendingDestinations.Contains(ConnectedToHex))
			{
				bCanMoveCluster = false;
				break;
			}
		}

		if (bCanMoveCluster)
		{
			MoveOperations.Add(MoveOp);
			for (int32 ConnectedId : MoveOp.ConnectedItemIds)
			{
				ProcessedItems.Add(ConnectedId);
				
				// Track all destination hexes for this cluster
				for (const auto& BoardKV : ItemMap)
				{
					if (BoardKV.Value.Id == ConnectedId)
					{
						FHex DestHex(BoardKV.Key.Q + ClusterMoveOffset.Q, BoardKV.Key.R + ClusterMoveOffset.R);
						PendingDestinations.Add(DestHex, ConnectedId);
						break;
					}
				}
			}
		}
	}

	// Step 2.5: Iterative movement resolution
	// Use an iterative approach to resolve all possible movements
	TArray<FMoveOperation> ValidMoveOperations;
	TSet<int32> ProcessedInThisIteration;
	TMap<FHex, int32> CurrentItemPositions; // Current state of the board
	
	// Initialize current positions
	for (const auto& KV : ItemMap)
	{
		if (KV.Value.Id > 0)
		{
			CurrentItemPositions.Add(KV.Key, KV.Value.Id);
		}
	}
	
	// Keep iterating until no more moves can be validated
	bool bMadeProgress = true;
	int32 IterationCount = 0;
	MaxIterations = 100; // Safety limit
	
	while (bMadeProgress && IterationCount < MaxIterations)
	{
		bMadeProgress = false;
		IterationCount++;
		TArray<FMoveOperation> ThisIterationMoves;
		
		// Try each pending move operation
		for (const FMoveOperation& MoveOp : MoveOperations)
		{
			// Skip if already processed
			if (ProcessedInThisIteration.Contains(MoveOp.ItemId)) continue;
			
			// Check if this move can be executed given current positions
			bool bCanMove = true;
			FHex MoveOffset(MoveOp.ToHex.Q - MoveOp.FromHex.Q, MoveOp.ToHex.R - MoveOp.FromHex.R);
			
			// Check all items in the cluster
			for (int32 ItemId : MoveOp.ConnectedItemIds)
			{
				// Find current position of this item
				FHex CurrentPos;
				bool bFoundPos = false;
				for (const auto& PosKV : CurrentItemPositions)
				{
					if (PosKV.Value == ItemId)
					{
						CurrentPos = PosKV.Key;
						bFoundPos = true;
						break;
					}
				}
				
				if (!bFoundPos)
				{
					bCanMove = false;
					break;
				}
				
				// Calculate destination
				FHex DestPos(CurrentPos.Q + MoveOffset.Q, CurrentPos.R + MoveOffset.R);
				
				// Check if destination is blocked
				if (HeldOrbHexes.Contains(DestPos))
				{
					bCanMove = false;
					break;
				}
				
				// Check if another item is at destination
				if (const int32* BlockingId = CurrentItemPositions.Find(DestPos))
				{
					// Check if the blocking item is part of our cluster
					if (!MoveOp.ConnectedItemIds.Contains(*BlockingId))
					{
						bCanMove = false;
						break;
					}
				}
			}
			
			if (bCanMove)
			{
				ThisIterationMoves.Add(MoveOp);
				bMadeProgress = true;
			}
		}
		
		// Apply all moves from this iteration to update positions
		for (const FMoveOperation& MoveOp : ThisIterationMoves)
		{
			FHex MoveOffset(MoveOp.ToHex.Q - MoveOp.FromHex.Q, MoveOp.ToHex.R - MoveOp.FromHex.R);
			
			// Update positions for all items in the cluster
			TArray<TPair<FHex, int32>> Updates;
			
			// First, collect all items to move (to avoid modifying map while iterating)
			for (int32 ItemId : MoveOp.ConnectedItemIds)
			{
				for (const auto& PosKV : CurrentItemPositions)
				{
					if (PosKV.Value == ItemId)
					{
						FHex NewPos(PosKV.Key.Q + MoveOffset.Q, PosKV.Key.R + MoveOffset.R);
						Updates.Add(TPair<FHex, int32>(NewPos, ItemId));
						break;
					}
				}
			}
			
			// Remove old positions
			for (int32 ItemId : MoveOp.ConnectedItemIds)
			{
				// Find and remove all positions for this item
				TArray<FHex> KeysToRemove;
				for (const auto& PosKV : CurrentItemPositions)
				{
					if (PosKV.Value == ItemId)
					{
						KeysToRemove.Add(PosKV.Key);
					}
				}
				for (const FHex& Key : KeysToRemove)
				{
					CurrentItemPositions.Remove(Key);
				}
			}
			
			// Add new positions
			for (const auto& Update : Updates)
			{
				CurrentItemPositions.Add(Update.Key, Update.Value);
			}
			
			// Mark as processed
			ValidMoveOperations.Add(MoveOp);
			for (int32 ItemId : MoveOp.ConnectedItemIds)
			{
				ProcessedInThisIteration.Add(ItemId);
			}
		}
	}
	
	// Use validated operations
	MoveOperations = ValidMoveOperations;
	
	// Step 3: Execute all moves atomically
	for (const FMoveOperation& MoveOp : MoveOperations)
	{
		FHex MoveOffset(MoveOp.ToHex.Q - MoveOp.FromHex.Q, MoveOp.ToHex.R - MoveOp.FromHex.R);

		// Check if moving to bond/unbond tiles or their partner areas - set bIsReady to false
		const FTile* DestTile = TileMap.Find(MoveOp.ToHex);
		bool bMovingToBondTile = (DestTile && (DestTile->Kind == ETileKind::Bond || DestTile->Kind == ETileKind::Unbond));

		// Also check if moving to a partner hex of a bond/unbond glyph
		bool bMovingToPartnerArea = false;
		if (!bMovingToBondTile)
		{
			for (const auto& TileKV : TileMap)
			{
				const FTile& CheckTile = TileKV.Value;
				if (CheckTile.Kind != ETileKind::Bond && CheckTile.Kind != ETileKind::Unbond) continue;

				const uint8 CheckGlyphDir = (uint8)(CheckTile.Arg & 0xFF);
				const FHex CheckPartnerHex = HexUtil::Neighbor(TileKV.Key, (EHexDir)(CheckGlyphDir % 6));

				if (CheckPartnerHex == MoveOp.ToHex)
				{
					bMovingToPartnerArea = true;
					break;
				}
			}
		}

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

				// Reset movement progress after successful move
				BeltProgress.Remove(MovingItem->Id);

				// Check if moved to output tile
				const FTile* NewTile = TileMap.Find(NewHex);
				if (NewTile && NewTile->Kind == ETileKind::Output)
				{
					OrbsNewlyMovedToOutput.Add(NewHex);
				}
				
				// Track all orbs that moved on belts for bond timing
				OrbsNewlyMovedOnBelts.Add(ItemId);
				
				// Mark this orb as having moved this tick
				OrbsMovedThisTick.Add(ItemId);
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

					bool bBothInCluster = MoveOp.ConnectedItemIds.Contains(Bond.ItemA) &&
						MoveOp.ConnectedItemIds.Contains(Bond.ItemB);

					if (bBothInCluster)
					{
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

	// Step 4: Pull items from inputs/bond tiles onto empty belts
	// Enhanced input pulling with improved partner area logic
	auto TryPullFromHex = [&](const FHex& BeltHex, const FHex& SourceHex, const FTile* SourceTile) -> bool
		{
			if (!SourceTile) return false;

			// Handle input tiles
			if (SourceTile->Kind == ETileKind::Input)
			{
				const FItem* InputItem = ItemMap.Find(SourceHex);
				if (InputItem && InputItem->Id > 0)
				{
					FItem MovingItem = *InputItem;
					ItemMap.Remove(SourceHex);
					OrbAt.Remove(SourceHex);
				ItemMap.Add(BeltHex, MovingItem);
				OrbAt.Add(BeltHex, KindFromItem(MovingItem));
				ItemsMovedLastStep.Add(MovingItem.Id);
				
				// Mark this orb as having moved this tick
				OrbsMovedThisTick.Add(MovingItem.Id);

					// Mark this input to skip spawning this frame
					SkipInputThisFrame.Add(SourceHex);

					// Create ascend animation for the input tile
					// Animation will complete in one step, synchronized with belt interpolation
					// Use the item that was pulled to display the correct material
					FAnimatedOrb AscendAnimation;
					AscendAnimation.bActive = true;
					AscendAnimation.bIsInput = true;
					AscendAnimation.Hex = SourceHex;
					AscendAnimation.Item = MovingItem; // Use the actual item for correct material
					AscendAnimation.StartStep = StepCounter;
					AscendAnimation.CompletionStep = StepCounter + 1; // Complete at next step
					AnimatedOrbs.Add(AscendAnimation);

					return true;
				}
			}

			// Handle bond/unbond tiles with enhanced cluster logic
			if (SourceTile->Kind == ETileKind::Bond || SourceTile->Kind == ETileKind::Unbond)
			{
				FItem* BondItem = ItemMap.Find(SourceHex);
				if (BondItem && BondItem->Id > 0 &&
					!RecentlyDroppedByArm.Contains(BondItem->Id))
				{
					if (!BondItem->bIsReady)
					{
						ItemsToSetReady.Add(BondItem->Id);
						return false;
					}

					// Build cluster - same logic as main movement
					TArray<int32> ConnectedItemIds;
					TSet<int32> InCluster;
					TArray<int32> ToProcess;

					ToProcess.Add(BondItem->Id);
					InCluster.Add(BondItem->Id);

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

					// Validate cluster move
					FHex ClusterMoveOffset(BeltHex.Q - SourceHex.Q, BeltHex.R - SourceHex.R);
					TMap<int32, FHex> ClusterPositions;
					TMap<int32, FHex> ClusterDestinations;

					for (int32 ConnectedId : ConnectedItemIds)
					{
						for (const auto& BoardKV : ItemMap)
						{
							if (BoardKV.Value.Id == ConnectedId)
							{
								FHex CurrentPos = BoardKV.Key;
								FHex DestPos(CurrentPos.Q + ClusterMoveOffset.Q, CurrentPos.R + ClusterMoveOffset.R);

								ClusterPositions.Add(ConnectedId, CurrentPos);
								ClusterDestinations.Add(ConnectedId, DestPos);
								break;
							}
						}
					}

					// Check if cluster destinations are available
					bool bCanMoveCluster = true;
					for (const auto& DestKV : ClusterDestinations)
					{
						const FHex& DestPos = DestKV.Value;

						if (HeldOrbHexes.Contains(DestPos))
						{
							bCanMoveCluster = false;
							break;
						}

						if (const FItem* DestItem = ItemMap.Find(DestPos))
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
						// Execute cluster pull
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

									if (FHex* DestPos = ClusterDestinations.Find(ItemId))
									{
										NewPositions.Add(ItemId, *DestPos);
									}
								}
							}
						}

						// Remove from old positions
						for (const auto& OldPosKV : OldPositions)
						{
							ItemMap.Remove(OldPosKV.Value);
							OrbAt.Remove(OldPosKV.Value);
						}

					// Place at new positions
					for (const auto& NewPosKV : NewPositions)
					{
						int32 ItemId = NewPosKV.Key;
						FHex NewHex = NewPosKV.Value;

						if (FItem* MovingItem = MovingItems.Find(ItemId))
						{
							MovingItem->bIsReady = true;
							ItemMap.Add(NewHex, *MovingItem);
							OrbAt.Add(NewHex, KindFromItem(*MovingItem));
							ItemsMovedLastStep.Add(MovingItem->Id);
							
							// Mark this orb as having moved this tick
							OrbsMovedThisTick.Add(MovingItem->Id);
						}
					}
					
				// Set shared BeltProgress for the entire bonded cluster
				// Find representative (lowest ID)
				int32 RepresentativeId = ConnectedItemIds[0];
				for (int32 Id : ConnectedItemIds)
				{
					if (Id < RepresentativeId)
					{
						RepresentativeId = Id;
					}
				}

						// Update bonds
						for (int32 ItemId : ConnectedItemIds)
						{
							if (const TArray<int32>* BondIndices = ItemToBonds.Find(ItemId))
							{
								for (int32 BondIdx : *BondIndices)
								{
									if (!Bonds.IsValidIndex(BondIdx)) continue;
									FBond& Bond = Bonds[BondIdx];
									if (!Bond.bActive) continue;

									bool bBothInCluster = InCluster.Contains(Bond.ItemA) && InCluster.Contains(Bond.ItemB);
									if (bBothInCluster && Bond.ItemA == ItemId)
									{
										if (FHex* NewPos = NewPositions.Find(ItemId))
										{
											Bond.Root = *NewPos;
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

	// Apply input pulling to empty belt positions
	for (const auto& KV : TileMap)
	{
		const FHex& BeltHex = KV.Key;
		const FTile& BeltTile = KV.Value;

		if (BeltTile.Kind != ETileKind::Belt) continue;
		if (ItemMap.Contains(BeltHex)) continue;
		if (HeldOrbHexes.Contains(BeltHex)) continue;

		const uint8 BeltDir = (uint8)(BeltTile.Arg & 0xFF);
		const EHexDir OppositeDir = (EHexDir)(((int32)BeltDir + 3) % 6);
		const FHex InputHex = HexUtil::Neighbor(BeltHex, OppositeDir);

		const FTile* InputTile = TileMap.Find(InputHex);

		// Try pulling from primary tile
		if (InputTile)
		{
			if (TryPullFromHex(BeltHex, InputHex, InputTile))
			{
				continue;
			}
		}
		// Try pulling from partner areas
		else
		{
			bool bFoundGlyphPartner = false;
			for (const auto& TileKV : TileMap)
			{
				const FTile& CheckTile = TileKV.Value;
				if (CheckTile.Kind != ETileKind::Bond && CheckTile.Kind != ETileKind::Unbond) continue;

				const uint8 CheckGlyphDir = (uint8)(CheckTile.Arg & 0xFF);
				const FHex CheckPartnerHex = HexUtil::Neighbor(TileKV.Key, (EHexDir)(CheckGlyphDir % 6));

				if (CheckPartnerHex == InputHex)
				{
					const FItem* PartnerItem = ItemMap.Find(InputHex);
					if (PartnerItem && PartnerItem->Id > 0 &&
						!NewlyPlacedItems.Contains(PartnerItem->Id) &&
						!RecentlyDroppedByArm.Contains(PartnerItem->Id))
					{
						FTile VirtualPartnerTile = CheckTile;
						if (TryPullFromHex(BeltHex, InputHex, &VirtualPartnerTile))
						{
							bFoundGlyphPartner = true;
							break;
						}
					}
				}
			}
		}
	}

		// Check if any movements occurred in this iteration
		if (MoveOperations.Num() > 0)
		{
			bAnyMovement = true;
		}
	} // End of while loop for iterative movement
	
	// Apply deferred bIsReady changes at the end of the tick
	for (int32 ItemId : ItemsToSetReady)
	{
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
	for (int32 ItemId : NewlyPlacedItems)
	{
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