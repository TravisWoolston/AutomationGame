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

		FOrb O; O.Type = (EOrbType)T.Arg;
		FItem NewItem = FItem::MakeSingle(O);
		NewItem.Id = NextItemId++;

		// Create animated orb for input (ascending)
		FAnimatedOrb AnimatedOrb;
		AnimatedOrb.Item = NewItem;
		AnimatedOrb.Hex = H;
		AnimatedOrb.AnimationTime = 0.f;
		AnimatedOrb.AnimationDuration = 0.5f; // 0.5 seconds to ascend
		AnimatedOrb.bIsInput = true;
		AnimatedOrb.bActive = true;
		AnimatedOrbs.Add(AnimatedOrb);
		
		// Don't immediately place the item - it will be placed when animation completes
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

		FOrb O; O.Type = (EOrbType)T.Arg;
		FItem NewItem = FItem::MakeSingle(O);
		NewItem.Id = NextItemId++;

		// Create animated orb for input (ascending)
		FAnimatedOrb AnimatedOrb;
		AnimatedOrb.Item = NewItem;
		AnimatedOrb.Hex = H;
		AnimatedOrb.AnimationTime = 0.f;
		AnimatedOrb.AnimationDuration = 0.5f; // 0.5 seconds to ascend
		AnimatedOrb.bIsInput = true;
		AnimatedOrb.bActive = true;
		AnimatedOrbs.Add(AnimatedOrb);
		
		// Don't immediately place the item - it will be placed when animation completes
		// Mark so a second (unexpected) pass this frame wouldn't double spawn
		SkipInputThisFrame.Add(H);
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
		if (SkipOutputThisFrame.Contains(H)) continue;
		
		// Skip orbs that were just moved to this output tile this tick
		if (OrbsNewlyMovedToOutput.Contains(H)) continue;

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
			
			// Create animated orb for output (descending)
			FAnimatedOrb AnimatedOrb;
			AnimatedOrb.Item = Temp;
			AnimatedOrb.Hex = H;
			AnimatedOrb.AnimationTime = 0.f;
			AnimatedOrb.AnimationDuration = 1.0f; // 1 second to descend
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
	for (int32 i = AnimatedOrbs.Num() - 1; i >= 0; i--)
	{
		FAnimatedOrb& AnimatedOrb = AnimatedOrbs[i];
		if (!AnimatedOrb.bActive) continue;

		AnimatedOrb.AnimationTime += DeltaTime;
		
		if (AnimatedOrb.AnimationTime >= AnimatedOrb.AnimationDuration)
		{
			// Animation complete
			if (AnimatedOrb.bIsInput)
			{
				// Input animation complete - place the item
				TryPutItem(AnimatedOrb.Hex, AnimatedOrb.Item);
			}
			// For outputs, the item was already removed from simulation when animation started
			
			// Remove the completed animation
			AnimatedOrbs.RemoveAt(i);
		}
	}
}
