#include "Sim/SimWorldHex.h"

// Input/Output processing functions
void USimWorldHex::SpawnInputs()
{
	if (TileMap.Num() == 0) return;
	for (const auto& KV : TileMap)
	{
		const FHex& H = KV.Key;
		const FTile& T = KV.Value;
		if (T.Kind != ETileKind::Input) continue;
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

		// Immediately place the item without animation
		// Animation will be handled by StepBelts when an item is pulled from this input
		FOrb O; O.Type = (EOrbType)T.Arg;
		FItem NewItem = FItem::MakeSingle(O);
		NewItem.Id = NextItemId++;

		TryPutItem(H, NewItem);
	}
}

void USimWorldHex::SpawnInputsAfterBelts()
{
	if (TileMap.Num() == 0) return;
	for (const auto& KV : TileMap)
	{
		const FHex& H = KV.Key;
		const FTile& T = KV.Value;
		if (T.Kind != ETileKind::Input) continue;
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

		// Immediately place the item without animation
		// Animation will be handled by StepBelts when an item is pulled from this input
		FOrb O; O.Type = (EOrbType)T.Arg;
		FItem NewItem = FItem::MakeSingle(O);
		NewItem.Id = NextItemId++;

		TryPutItem(H, NewItem);
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
	for (const auto& KV : TileMap)
	{
		const FHex& H = KV.Key;
		const FTile& T = KV.Value;
		if (T.Kind != ETileKind::Output) continue;
		if (OrbsNewlyMovedToOutput.Contains(H)) continue; // Skip orbs just moved by belts this tick

		// Process orbs that have been sitting on output tiles for at least one tick

		// Check if there's an item on this output tile
		const FItem* ItemPtr = ItemMap.Find(H);
		if (!ItemPtr) continue;


		// Check if the orb type matches the output's expected type
		if (DoesItemMatchOutputType(*ItemPtr, T))
		{
			// Type matches - remove from simulation and create animated orb
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
			AnimatedOrbs.Add(AnimatedOrb);



			++OutputCount;
		}
		// If type doesn't match, leave the orb on the tile (no action needed)
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
			return true;
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
