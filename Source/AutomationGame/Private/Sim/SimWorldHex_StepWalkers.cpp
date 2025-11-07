#include "Sim/SimWorldHex.h"
#include "Sim/HexTypes.h"
#include "Sim/WalkerTypes.h"
#include "Sim/HexCoord.h"

void USimWorldHex::StepWalkers()
{
	for (int32 WalkerIndex = 0; WalkerIndex < Walkers.Num(); ++WalkerIndex)
	{
		FWalkerState& W = Walkers[WalkerIndex];
		
		// Skip if no program
		if (W.Program.Num() == 0)
		{
			continue;
		}
		
		// Handle wait counter
		if (W.WaitCounter > 0)
		{
			--W.WaitCounter;
			continue;
		}
		
		// If currently moving, advance movement
		if (W.bMoving)
		{
			// Calculate distance to target (hex distance)
			int32 dQ = W.TargetHex.Q - W.BaseHex.Q;
			int32 dR = W.TargetHex.R - W.BaseHex.R;
			// Hex distance approximation
			float Distance = FMath::Max(FMath::Abs(float(dQ)), FMath::Max(FMath::Abs(float(dR)), FMath::Abs(float(dQ + dR))));
			
			// Advance movement progress
			W.MovementProgress += W.TileTraverseRate;
			
			// Check if we've reached the target
			if (W.MovementProgress >= Distance || (W.BaseHex.Q == W.TargetHex.Q && W.BaseHex.R == W.TargetHex.R))
			{
				W.BaseHex = W.TargetHex;
				W.bMoving = false;
				W.MovementProgress = 0.0f;
				// Advance to next instruction
				W.PC = (W.PC + 1) % W.Program.Num();
			}
			else
			{
				// Still moving - move one hex step at a time based on progress
				// For now, just move directly when progress exceeds 1.0
				if (W.MovementProgress >= 1.0f)
				{
					// Move one step closer to target
					if (W.BaseHex.Q != W.TargetHex.Q)
					{
						W.BaseHex.Q += (W.TargetHex.Q > W.BaseHex.Q) ? 1 : -1;
					}
					if (W.BaseHex.R != W.TargetHex.R)
					{
						W.BaseHex.R += (W.TargetHex.R > W.BaseHex.R) ? 1 : -1;
					}
					W.MovementProgress -= 1.0f;
				}
			}
			continue;
		}
		
		// Execute current instruction
		if (W.PC >= W.Program.Num())
		{
			W.PC = 0; // Loop back
		}
		
		const FWalkerInstr& Instr = W.Program[W.PC];
		
		switch (Instr.Cmd)
		{
		case EWalkerCmd::NoOp:
			W.PC = (W.PC + 1) % W.Program.Num();
			break;
			
		case EWalkerCmd::Wait:
			W.WaitCounter = FMath::Max(1, Instr.Arg1);
			break;
			
		case EWalkerCmd::MoveTo:
		{
			FHex TargetHex(Instr.Arg1, Instr.Arg2);
			W.TargetHex = TargetHex;
			W.bMoving = true;
			W.MovementProgress = 0.0f;
			// Don't advance PC yet - wait until movement completes
			break;
		}
		
		case EWalkerCmd::Reset:
		{
			// Reset should have been expanded to MoveTo in PushProgramToSim
			// But handle it here as a fallback
			W.TargetHex = W.HomeHex;
			W.bMoving = true;
			W.MovementProgress = 0.0f;
			W.PC = (W.PC + 1) % W.Program.Num();
			break;
		}
		
		default:
			W.PC = (W.PC + 1) % W.Program.Num();
			break;
		}
	}
}

