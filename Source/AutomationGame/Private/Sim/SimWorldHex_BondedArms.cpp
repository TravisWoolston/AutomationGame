#include "Sim/SimWorldHex.h"
#include "Engine/Engine.h"

// Bonded arm management functions
void USimWorldHex::CheckForNewBondedArmPairs()
{
	// Check all active bonds for held-to-held bonding
	for (int32 BondIdx = 0; BondIdx < Bonds.Num(); BondIdx++)
	{
		const FBond& Bond = Bonds[BondIdx];
		if (!Bond.bActive) continue;
		
		// Find which arms are holding these items
		int32 ArmA = -1, ArmB = -1;
		for (int32 ArmIdx = 0; ArmIdx < Arms.Num(); ArmIdx++)
		{
			const FArmState& Arm = Arms[ArmIdx];
			if (!Arm.bHolding) continue;
			
			// Check if this arm is holding either bonded item
			if (Arm.Held.bIsCluster)
			{
				for (const FOrbData& Sub : Arm.Held.Orbs)
				{
					if (Sub.ItemId == Bond.ItemA) ArmA = ArmIdx;
					if (Sub.ItemId == Bond.ItemB) ArmB = ArmIdx;
				}
			}
			else
			{
				if (Arm.Held.Id == Bond.ItemA) ArmA = ArmIdx;
				if (Arm.Held.Id == Bond.ItemB) ArmB = ArmIdx;
			}
		}
		
		// If both items are held by different arms, create a bonded arm pair
		if (ArmA >= 0 && ArmB >= 0 && ArmA != ArmB)
		{
			// Check if this bonded pair already exists
			bool bAlreadyExists = false;
			for (const FBondedArmPair& Existing : BondedArmPairs)
			{
				if (Existing.bActive && 
					((Existing.ArmIndexA == ArmA && Existing.ArmIndexB == ArmB) ||
					 (Existing.ArmIndexA == ArmB && Existing.ArmIndexB == ArmA)))
				{
					bAlreadyExists = true;
					break;
				}
			}
			
			if (!bAlreadyExists)
			{
				FBondedArmPair NewPair;
				NewPair.ArmIndexA = ArmA;
				NewPair.ArmIndexB = ArmB;
				NewPair.BondIndex = BondIdx;
				NewPair.bActive = true;
				BondedArmPairs.Add(NewPair);
				
				if (bDebugLogs && bDevDebugBonds && GEngine)
				{
					GEngine->AddOnScreenDebugMessage(-1, 2.0f, FColor::Cyan,
						FString::Printf(TEXT("NEW BONDED ARM PAIR: Arms %d and %d (Items %d-%d)"), 
							ArmA, ArmB, Bond.ItemA, Bond.ItemB));
				}
			}
		}
	}
}

void USimWorldHex::PauseBondedArms()
{
	// Pause arms that are bonded together (except for Wait and Drop commands)
	for (const FBondedArmPair& Pair : BondedArmPairs)
	{
		if (!Pair.bActive) continue;
		
		// Pause both arms in the pair
		for (int32 ArmIdx : {Pair.ArmIndexA, Pair.ArmIndexB})
		{
			if (ArmIdx < 0 || ArmIdx >= Arms.Num()) continue;
			FArmState& Arm = Arms[ArmIdx];
			if (!Arm.bHolding) continue;
			
			// Check current command - don't pause for Wait or Drop
			if (Arm.Tape.Num() > 0 && Arm.PC < Arm.Tape.Num())
			{
				const FOp& CurrentOp = Arm.Tape[Arm.PC];
				if (CurrentOp.Cmd == ECmd::Wait || CurrentOp.Cmd == ECmd::Drop)
				{
					continue; // Allow Wait and Drop commands to continue
				}
			}
			
			// Pause the arm by setting a pause flag
			Arm.bPausedForBonding = true;
		}
	}
}

void USimWorldHex::ResumeArmsWhenBondsBroken()
{
	// Check which bonds are no longer active and resume the corresponding arms
	for (int32 i = BondedArmPairs.Num() - 1; i >= 0; i--)
	{
		FBondedArmPair& Pair = BondedArmPairs[i];
		if (!Pair.bActive) continue;
		
		// Check if the bond is still active
		bool bBondStillActive = false;
		if (Pair.BondIndex >= 0 && Pair.BondIndex < Bonds.Num())
		{
			bBondStillActive = Bonds[Pair.BondIndex].bActive;
		}
		
		// Check if either arm is no longer holding the bonded items
		bool bArmAStillHolding = false, bArmBStillHolding = false;
		
		if (Pair.ArmIndexA >= 0 && Pair.ArmIndexA < Arms.Num())
		{
			const FArmState& ArmA = Arms[Pair.ArmIndexA];
			if (ArmA.bHolding && Pair.BondIndex >= 0 && Pair.BondIndex < Bonds.Num())
			{
				const FBond& Bond = Bonds[Pair.BondIndex];
				if (ArmA.Held.bIsCluster)
				{
					for (const FOrbData& Sub : ArmA.Held.Orbs)
					{
						if (Sub.ItemId == Bond.ItemA || Sub.ItemId == Bond.ItemB)
						{
							bArmAStillHolding = true;
							break;
						}
					}
				}
				else
				{
					bArmAStillHolding = (ArmA.Held.Id == Bond.ItemA || ArmA.Held.Id == Bond.ItemB);
				}
			}
		}
		
		if (Pair.ArmIndexB >= 0 && Pair.ArmIndexB < Arms.Num())
		{
			const FArmState& ArmB = Arms[Pair.ArmIndexB];
			if (ArmB.bHolding && Pair.BondIndex >= 0 && Pair.BondIndex < Bonds.Num())
			{
				const FBond& Bond = Bonds[Pair.BondIndex];
				if (ArmB.Held.bIsCluster)
				{
					for (const FOrbData& Sub : ArmB.Held.Orbs)
					{
						if (Sub.ItemId == Bond.ItemA || Sub.ItemId == Bond.ItemB)
						{
							bArmBStillHolding = true;
							break;
						}
					}
				}
				else
				{
					bArmBStillHolding = (ArmB.Held.Id == Bond.ItemA || ArmB.Held.Id == Bond.ItemB);
				}
			}
		}
		
		// If bond is broken or arms are no longer holding bonded items, resume arms
		if (!bBondStillActive || !bArmAStillHolding || !bArmBStillHolding)
		{
			// Resume both arms
			for (int32 ArmIdx : {Pair.ArmIndexA, Pair.ArmIndexB})
			{
				if (ArmIdx >= 0 && ArmIdx < Arms.Num())
				{
					Arms[ArmIdx].bPausedForBonding = false;
				}
			}
			
			if (bDebugLogs && bDevDebugBonds && GEngine)
			{
				GEngine->AddOnScreenDebugMessage(-1, 2.0f, FColor::Orange,
					FString::Printf(TEXT("BONDED ARM PAIR BROKEN: Arms %d and %d resumed"), 
						Pair.ArmIndexA, Pair.ArmIndexB));
			}
			
			// Mark this bonded pair as inactive
			Pair.bActive = false;
		}
	}
	
	// Remove inactive bonded pairs
	BondedArmPairs.RemoveAll([](const FBondedArmPair& Pair) { return !Pair.bActive; });
}

bool USimWorldHex::IsArmBonded(int32 ArmIndex) const
{
	for (const FBondedArmPair& Pair : BondedArmPairs)
	{
		if (Pair.bActive && (Pair.ArmIndexA == ArmIndex || Pair.ArmIndexB == ArmIndex))
		{
			return true;
		}
	}
	return false;
}

