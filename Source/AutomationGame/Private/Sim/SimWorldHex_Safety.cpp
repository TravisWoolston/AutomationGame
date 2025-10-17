#include "Sim/SimWorldHex.h"
#include "AGLogChannels.h"

// Safety and validation methods
bool USimWorldHex::ValidateDataIntegrity() const
{
	bool bIsValid = true;
	
	// Check ItemToBonds integrity
	for (const auto& ItemBondPair : ItemToBonds)
	{
		int32 ItemId = ItemBondPair.Key;
		const TArray<int32>& BondIndices = ItemBondPair.Value;
		
		// Validate item ID
		if (ItemId <= 0)
		{
			UE_LOG(LogTemp, Error, TEXT("Invalid ItemId in ItemToBonds: %d"), ItemId);
			bIsValid = false;
			continue;
		}
		
		// Validate bond indices
		for (int32 BondIndex : BondIndices)
		{
			if (!Bonds.IsValidIndex(BondIndex))
			{
				UE_LOG(LogTemp, Error, TEXT("Invalid BondIndex %d for ItemId %d"), BondIndex, ItemId);
				bIsValid = false;
			}
			else
			{
				const FBond& Bond = Bonds[BondIndex];
				if (Bond.ItemA != ItemId && Bond.ItemB != ItemId)
				{
					UE_LOG(LogTemp, Error, TEXT("Bond %d doesn't reference ItemId %d"), BondIndex, ItemId);
					bIsValid = false;
				}
			}
		}
	}
	
	// Check Bonds array integrity
	for (int32 i = 0; i < Bonds.Num(); ++i)
	{
		const FBond& Bond = Bonds[i];
		if (Bond.ItemA <= 0 || Bond.ItemB <= 0)
		{
			UE_LOG(LogTemp, Error, TEXT("Invalid bond at index %d: ItemA=%d, ItemB=%d"), i, Bond.ItemA, Bond.ItemB);
			bIsValid = false;
		}
	}
	
	return bIsValid;
}

void USimWorldHex::CleanupCorruptedData()
{
	UE_LOG(LogTemp, Warning, TEXT("Cleaning up corrupted data in SimWorldHex"));
	
	// Remove invalid entries from ItemToBonds
	TArray<int32> KeysToRemove;
	for (const auto& ItemBondPair : ItemToBonds)
	{
		int32 ItemId = ItemBondPair.Key;
		const TArray<int32>& BondIndices = ItemBondPair.Value;
		
		if (ItemId <= 0)
		{
			KeysToRemove.Add(ItemId);
			continue;
		}
		
		// Remove invalid bond indices
		TArray<int32> ValidBondIndices;
		for (int32 BondIndex : BondIndices)
		{
			if (Bonds.IsValidIndex(BondIndex))
			{
				const FBond& Bond = Bonds[BondIndex];
				if (Bond.ItemA == ItemId || Bond.ItemB == ItemId)
				{
					ValidBondIndices.Add(BondIndex);
				}
			}
		}
		
		if (ValidBondIndices.Num() != BondIndices.Num())
		{
			ItemToBonds[ItemId] = ValidBondIndices;
		}
	}
	
	// Remove invalid keys
	for (int32 Key : KeysToRemove)
	{
		ItemToBonds.Remove(Key);
	}
	
	// Remove invalid bonds
	for (int32 i = Bonds.Num() - 1; i >= 0; --i)
	{
		const FBond& Bond = Bonds[i];
		if (Bond.ItemA <= 0 || Bond.ItemB <= 0)
		{
			RemoveBondIndex(i);
		}
	}
	
	UE_LOG(LogTemp, Warning, TEXT("Data cleanup completed"));
}
