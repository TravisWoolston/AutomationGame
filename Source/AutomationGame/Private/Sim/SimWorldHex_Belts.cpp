#include "Sim/SimWorldHex.h"

// Simple belt system - items move every tick based on belt speed
void USimWorldHex::StepBelts()
{
	if (TileMap.Num() == 0 || ItemMap.Num() == 0) return;
	
	UE_LOG(LogTemp, Warning, TEXT("StepBelts: Processing %d tiles, %d items, Speed=%.2f"), 
		TileMap.Num(), ItemMap.Num(), BeltSpeedTilesPerTick);

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
	// Items move one tile per tick when BeltSpeedTilesPerTick >= 1.0
	
	// Step 1: Update belt progress for visual interpolation
	const float Dt = FMath::Max(0.f, FixedStepSec);
	const float ProgressPerStep = Speed * Dt / FMath::Max(KINDA_SMALL_NUMBER, Dt);

	for (const auto& KV : ItemMap)
	{
		if (KV.Value.Id == 0) continue;

		const FTile* T = TileMap.Find(KV.Key);
		if (T && T->Kind == ETileKind::Belt)
		{
			float& Progress = BeltProgress.FindOrAdd(KV.Value.Id, 0.f);
			Progress += ProgressPerStep;
			// Keep progress for visual interpolation but don't cap it
		}
	}

	// Step 2: Move items on belts - simple per-item movement
	TMap<FHex, int32> BeltChainIds; // Map each belt hex to its chain ID
	TArray<TArray<FHex>> BeltChains; // Each chain is an ordered list of belt hexes
	int32 NextChainId = 0;

	// Identify belt chains by following belt directions
	for (const auto& KV : TileMap)
	{
		const FHex& BeltHex = KV.Key;
		const FTile& BeltTile = KV.Value;

		if (BeltTile.Kind != ETileKind::Belt) continue;
		if (BeltChainIds.Contains(BeltHex)) continue; // Already processed

		// Start a new chain from this belt
		TArray<FHex> Chain;
		FHex CurrentHex = BeltHex;

		// Walk backwards to find the start of the chain
		while (true)
		{
			const FTile* CurrentTile = TileMap.Find(CurrentHex);
			if (!CurrentTile || CurrentTile->Kind != ETileKind::Belt) break;

			const uint8 CurrentDir = (uint8)(CurrentTile->Arg & 0xFF);
			const EHexDir BackwardDir = (EHexDir)(((int32)CurrentDir + 3) % 6);
			const FHex PrevHex = HexUtil::Neighbor(CurrentHex, BackwardDir);
			const FTile* PrevTile = TileMap.Find(PrevHex);

			if (!PrevTile || PrevTile->Kind != ETileKind::Belt) break;

			const uint8 PrevDir = (uint8)(PrevTile->Arg & 0xFF);
			const FHex PrevDestination = HexUtil::Neighbor(PrevHex, (EHexDir)(PrevDir % 6));

			if (PrevDestination != CurrentHex) break; // Previous belt doesn't point to us

			CurrentHex = PrevHex;
		}

		// Now walk forward from the start to build the complete chain
		FHex ChainStart = CurrentHex;
		CurrentHex = ChainStart;

		while (true)
		{
			const FTile* CurrentTile = TileMap.Find(CurrentHex);
			if (!CurrentTile || CurrentTile->Kind != ETileKind::Belt) break;

			Chain.Add(CurrentHex);
			BeltChainIds.Add(CurrentHex, NextChainId);

			const uint8 CurrentDir = (uint8)(CurrentTile->Arg & 0xFF);
			const FHex NextHex = HexUtil::Neighbor(CurrentHex, (EHexDir)(CurrentDir % 6));
			const FTile* NextTile = TileMap.Find(NextHex);

			if (!NextTile || NextTile->Kind != ETileKind::Belt) break;

			const uint8 NextDir = (uint8)(NextTile->Arg & 0xFF);
			if (NextDir != CurrentDir) break; // Direction changes, end of chain

			CurrentHex = NextHex;
		}

		if (Chain.Num() > 0)
		{
			BeltChains.Add(Chain);
			NextChainId++;
		}
	}

	// Step 3: Process each belt chain for conveyor movement
	TArray<FMoveOperation> MoveOperations;
	TSet<int32> ProcessedItems;

	for (const TArray<FHex>& Chain : BeltChains)
	{
		if (Chain.Num() == 0) continue;

		// Check if the chain can advance (working backwards from output)
		TArray<bool> CanMove; // For each position in chain
		CanMove.SetNum(Chain.Num());

		// Check output end first
		const FHex& LastHex = Chain.Last();
		const FTile* LastTile = TileMap.Find(LastHex);
		const uint8 LastDir = (uint8)(LastTile->Arg & 0xFF);
		const FHex OutputHex = HexUtil::Neighbor(LastHex, (EHexDir)(LastDir % 6));

		// Can output if destination is clear
		// Note: Allow output even if there's an active descend animation (animations can overlap)
		bool bCanOutput = !HeldOrbHexes.Contains(OutputHex) && !ItemMap.Contains(OutputHex);

		// Special case for bond/unbond tiles - can move there if empty
		const FTile* OutputTile = TileMap.Find(OutputHex);
		if (OutputTile && (OutputTile->Kind == ETileKind::Bond || OutputTile->Kind == ETileKind::Unbond))
		{
			bCanOutput = !ItemMap.Contains(OutputHex);
		}

		// Work backwards through the chain
		bool bPreviousCanMove = bCanOutput;
		for (int32 i = Chain.Num() - 1; i >= 0; i--)
		{
			const FHex& ChainHex = Chain[i];
			const FItem* ChainItem = ItemMap.Find(ChainHex);
			bool bHasItem = (ChainItem && ChainItem->Id > 0);

			// Key conveyor logic: Can move if there's an item AND the next position allows movement
			CanMove[i] = bHasItem && bPreviousCanMove;

			// Next position will be available if current item moves away OR if there's no item
			bPreviousCanMove = !bHasItem || CanMove[i];
		}

		// Create move operations for items that can advance
		for (int32 i = 0; i < Chain.Num(); i++)
		{
			if (!CanMove[i]) continue;

			const FHex& FromHex = Chain[i];
			const FItem* Item = ItemMap.Find(FromHex);
			if (!Item || ProcessedItems.Contains(Item->Id)) continue;

			FHex ToHex;
			if (i == Chain.Num() - 1)
			{
				// Last item in chain - move to output
				ToHex = OutputHex;
			}
			else
			{
				// Move to next position in chain
				ToHex = Chain[i + 1];
			}

			// Build bonded cluster for this item
			FMoveOperation MoveOp;
			MoveOp.ItemId = Item->Id;
			MoveOp.FromHex = FromHex;
			MoveOp.ToHex = ToHex;

			// Find all bonded items that must move together
			TArray<int32> ToProcess;
			TSet<int32> InCluster;
			ToProcess.Add(Item->Id);
			InCluster.Add(Item->Id);

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
		}
	}

	// Step 4: Execute all moves atomically
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

				// Reset belt progress for items that moved - they start fresh at new position
				BeltProgress.Remove(MovingItem->Id);

				// Check if moved to special tiles
				const FTile* NewTile = TileMap.Find(NewHex);
				if (NewTile)
				{
					if (NewTile->Kind != ETileKind::Belt)
						BeltProgress.Remove(ItemId);
					if (NewTile->Kind == ETileKind::Output)
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
				// Start with some progress for smooth visual interpolation from input to belt
				BeltProgress.Add(MovingItem.Id, 0.1f);
				ItemsMovedLastStep.Add(MovingItem.Id);
				
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
								// Start with some progress for smooth visual interpolation
								BeltProgress.Add(MovingItem->Id, 0.1f);
								ItemsMovedLastStep.Add(MovingItem->Id);
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
				UE_LOG(LogTemp, Warning, TEXT("StepBelts: Pulled item from input (%d,%d) to belt (%d,%d)"), 
					InputHex.Q, InputHex.R, BeltHex.Q, BeltHex.R);
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
