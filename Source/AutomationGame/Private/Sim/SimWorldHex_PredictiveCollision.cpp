#include "Sim/SimWorldHex.h"
#include "Actors/BasicArm.h"
#include "EngineUtils.h"
#include "Engine/Engine.h"

bool USimWorldHex::WouldCauseHeldClusterOverlap()
{
	// Clear previous debug data
	PredictiveCollisionDebugData.Empty();
	
	// Only check if we have arms
	if (Arms.Num() < 2)
	{
		return false;
	}
	
	// Simulate what would happen if all arms executed their current commands (one tick ahead only)
	TArray<FPredictiveArmState> PredictedStates = SimulateAllArmsExecution();
	
	// Check all pairs of arms for potential held cluster overlap
	for (int32 i = 0; i < PredictedStates.Num(); ++i)
	{
		if (!PredictedStates[i].bHolding) continue;
		
		for (int32 j = i + 1; j < PredictedStates.Num(); ++j)
		{
			if (!PredictedStates[j].bHolding) continue;
			
			// Check if these two arms' held clusters would overlap during their movements
			if (WouldHeldClustersOverlap(PredictedStates[i], PredictedStates[j], Arms[i], Arms[j]))
			{
				// Record debug info for visualization
				FPredictiveCollisionInfo CollisionInfo;
				CollisionInfo.ArmIndexA = i;
				CollisionInfo.ArmIndexB = j;
				CollisionInfo.Reason = FString::Printf(TEXT("Predictive collision between arms %d and %d"), i, j);
				
				// For now, just use the first arm's predicted position as the collision hex
				TArray<FHex> PositionsA = GetHeldClusterPositions(PredictedStates[i]);
				if (PositionsA.Num() > 0)
				{
					CollisionInfo.CollisionHex = PositionsA[0];
				}
				
				PredictiveCollisionDebugData.Add(CollisionInfo);
				return true;
			}
		}
	}
	
	return false;
}

TArray<USimWorldHex::FPredictiveArmState> USimWorldHex::SimulateAllArmsExecution()
{
	TArray<FPredictiveArmState> PredictedStates;
	PredictedStates.Reserve(Arms.Num());
	
	// Start with current arm states
	for (int32 ArmIndex = 0; ArmIndex < Arms.Num(); ++ArmIndex)
	{
		PredictedStates.Add(FPredictiveArmState(Arms[ArmIndex]));
	}
	
	// Apply pending sim values that will be committed next tick
	for (const auto& KV : PendingFacingByArm)
	{
		if (KV.Key >= 0 && KV.Key < PredictedStates.Num())
		{
			PredictedStates[KV.Key].Facing = KV.Value;
		}
	}
	
	// Note: We don't apply PendingArmLenByArm here because extend/retract commands
	// will use the current arm length from CurrentState, and we want to predict what
	// happens AFTER the extend/retract command executes
	
	// Now simulate what each arm would do if it executed its current command (ONE TICK AHEAD ONLY)
	for (int32 ArmIndex = 0; ArmIndex < PredictedStates.Num(); ++ArmIndex)
	{
		FPredictiveArmState& PredictedState = PredictedStates[ArmIndex];
		const FArmState& CurrentState = Arms[ArmIndex];
		
		// Skip if arm is waiting or has no current command
		if (PredictedState.WaitCounter > 0 || CurrentState.Tape.Num() == 0)
		{
			continue;
		}
		
		// Get the current command from the tape
		if (PredictedState.PC >= CurrentState.Tape.Num())
		{
			continue; // Invalid PC
		}
		
		const FOp& CurrentOp = CurrentState.Tape[PredictedState.PC];
		
		// Only simulate movement commands that could cause held cluster collision
		// Don't advance PC - we only want to see what would happen in the next tick
		switch (CurrentOp.Cmd)
		{
		case ECmd::RotateL:
		case ECmd::RotateR:
		{
			// Simulate rotation
			EHexDir NewFacing = (CurrentOp.Cmd == ECmd::RotateL)
				? (EHexDir)(((uint8)PredictedState.Facing + 1) % 6)
				: (EHexDir)(((uint8)PredictedState.Facing + 5) % 6);
			
			PredictedState.Facing = static_cast<uint8>(NewFacing);
			// Don't advance PC - this is just for collision prediction
			break;
		}
		case ECmd::Extend:
		{
			// Simulate extension - use current state's actual arm length
			int32 NewArmLen = CurrentState.ArmLen + CurrentOp.Arg;
			NewArmLen = FMath::Max(1, FMath::Min(NewArmLen, 10)); // Clamp to reasonable range
			PredictedState.ArmLen = NewArmLen;
			// Don't advance PC - this is just for collision prediction
			break;
		}
		case ECmd::Retract:
		{
			// Simulate retraction - use current state's actual arm length
			int32 NewArmLen = CurrentState.ArmLen - CurrentOp.Arg;
			NewArmLen = FMath::Max(1, FMath::Min(NewArmLen, 10)); // Clamp to reasonable range
			PredictedState.ArmLen = NewArmLen;
			// Don't advance PC - this is just for collision prediction
			break;
		}
		case ECmd::Grab:
		case ECmd::Drop:
		case ECmd::WaitForBond:
		case ECmd::Wait:
		default:
			// These commands don't change the arm's position or held cluster position
			// So we don't need to simulate them for collision detection
			break;
		}
	}
	
	return PredictedStates;
}

bool USimWorldHex::WouldHeldClustersOverlap(const FPredictiveArmState& ArmA, const FPredictiveArmState& ArmB,
	const FArmState& CurrentA, const FArmState& CurrentB)
{
	// Check if either arm is performing a rotation
	bool ArmARotating = (CurrentA.PC < CurrentA.Tape.Num() && 
		(CurrentA.Tape[CurrentA.PC].Cmd == ECmd::RotateL || CurrentA.Tape[CurrentA.PC].Cmd == ECmd::RotateR) &&
		CurrentA.WaitCounter == 0);
	bool ArmBRotating = (CurrentB.PC < CurrentB.Tape.Num() && 
		(CurrentB.Tape[CurrentB.PC].Cmd == ECmd::RotateL || CurrentB.Tape[CurrentB.PC].Cmd == ECmd::RotateR) &&
		CurrentB.WaitCounter == 0);
	
	// Check if either arm is performing an extend or retract
	bool ArmAExtending = (CurrentA.PC < CurrentA.Tape.Num() && 
		(CurrentA.Tape[CurrentA.PC].Cmd == ECmd::Extend || CurrentA.Tape[CurrentA.PC].Cmd == ECmd::Retract) &&
		CurrentA.WaitCounter == 0);
	bool ArmBExtending = (CurrentB.PC < CurrentB.Tape.Num() && 
		(CurrentB.Tape[CurrentB.PC].Cmd == ECmd::Extend || CurrentB.Tape[CurrentB.PC].Cmd == ECmd::Retract) &&
		CurrentB.WaitCounter == 0);
		
	// If both arms are rotating, check their sweeps
	if (ArmARotating && ArmBRotating)
	{
		return WouldRotationSweepsOverlap(CurrentA, CurrentB, ArmA, ArmB);
	}
	// If only one arm is rotating, check its sweep against the other's static position
	else if (ArmARotating)
	{
		// Check A's sweep against B's current position
		return WouldRotationSweepOverlapStatic(CurrentA, ArmA, CurrentB);
	}
	else if (ArmBRotating)
	{
		// Check B's sweep against A's current position
		return WouldRotationSweepOverlapStatic(CurrentB, ArmB, CurrentA);
	}
	// If both arms are extending/retracting or one is and other is not rotating, check positions
	else if (ArmAExtending || ArmBExtending)
	{
		// Check final positions after extend/retract
		TArray<FHex> PositionsA = GetHeldClusterPositions(ArmA);
		TArray<FHex> PositionsB = GetHeldClusterPositions(ArmB);
		
		for (const FHex& PosA : PositionsA)
		{
			for (const FHex& PosB : PositionsB)
			{
				if (PosA.Q == PosB.Q && PosA.R == PosB.R)
				{
					return true;
				}
			}
		}
	}
	else
	{
		// Neither is rotating or extending, just check final positions
		TArray<FHex> PositionsA = GetHeldClusterPositions(ArmA);
		TArray<FHex> PositionsB = GetHeldClusterPositions(ArmB);
		
		for (const FHex& PosA : PositionsA)
		{
			for (const FHex& PosB : PositionsB)
			{
				if (PosA.Q == PosB.Q && PosA.R == PosB.R)
				{
					return true;
				}
			}
		}
	}
	
	return false;
}

TArray<FHex> USimWorldHex::GetHeldClusterPositions(const FPredictiveArmState& ArmState)
{
	TArray<FHex> Positions;
	
	if (!ArmState.bHolding)
	{
		return Positions;
	}
	
	// Calculate the grip position
	FHex GripHex = ArmState.BaseHex;
	if (ArmState.ArmLen > 1)
	{
		// Extend in the facing direction
		FHex DirectionOffset = HexUtil::Neighbor(FHex(0, 0), (EHexDir)ArmState.Facing);
		GripHex.Q += DirectionOffset.Q * (ArmState.ArmLen - 1);
		GripHex.R += DirectionOffset.R * (ArmState.ArmLen - 1);
	}
	
	if (!ArmState.Held.bIsCluster)
	{
		// Single held orb - just add the grip position
		Positions.Add(GripHex);
	}
	else
	{
		// Held cluster - add all sub-orb positions
		const uint8 ccw = (ArmState.Held.GrabFacing >= 0) 
			? (uint8)(((int32)ArmState.Facing - (int32)(ArmState.Held.GrabFacing % 6) + 6) % 6)
			: (uint8)ArmState.Facing; // fallback for legacy items
		
		for (const FOrbData& Sub : ArmState.Held.Orbs)
		{
			FHex Local(Sub.dQ, Sub.dR);
			FHex RotLocal = Rot60CCW(Local, ccw);
			Positions.Add(FHex(GripHex.Q + RotLocal.Q, GripHex.R + RotLocal.R));
		}
	}
	
	return Positions;
}

bool USimWorldHex::WouldRotationSweepsOverlap(const FArmState& ArmA, const FArmState& ArmB,
	const FPredictiveArmState& PredictedA, const FPredictiveArmState& PredictedB)
{
	// Helper to compute grip hex
	auto ComputeGripHex = [](const FHex& BaseHex, uint8 Facing, int32 ArmLen) -> FHex
	{
		FHex Grip = BaseHex;
		for (int i = 0; i < ArmLen; ++i)
			Grip = HexUtil::Neighbor(Grip, (EHexDir)Facing);
		return Grip;
	};

	// Helper to build held cluster positions at a given facing
	auto BuildHeldPositions = [&](const FHex& BaseHex, uint8 Facing, int32 ArmLen, const FItem& Held) -> TArray<FHex>
	{
		TArray<FHex> Positions;
		if (!Held.bIsCluster)
		{
			Positions.Add(ComputeGripHex(BaseHex, Facing, ArmLen));
			return Positions;
		}

		const FHex Grip = ComputeGripHex(BaseHex, Facing, ArmLen);
		const uint8 cur = Facing % 6;
		const int8 grab = (int8)Held.GrabFacing;
		const uint8 facingDelta = (grab >= 0)
			? (uint8)(((int32)cur - (int32)(grab % 6) + 6) % 6)
			: cur;
		const uint8 ccw = (uint8)((6 - facingDelta) % 6);

		for (const FOrbData& Sub : Held.Orbs)
		{
			FHex Local(Sub.dQ, Sub.dR);
			FHex RotLocal = Rot60CCW(Local, ccw);
			Positions.Add(FHex(Grip.Q + RotLocal.Q, Grip.R + RotLocal.R));
		}
		return Positions;
	};

	// For each arm, check the sweep path (start, mid-translate, mid-rotate, end)
	// Arm A positions during rotation
	TArray<FHex> A_Start = BuildHeldPositions(ArmA.BaseHex, (uint8)ArmA.Facing, ArmA.ArmLen, ArmA.Held);
	TArray<FHex> A_End = BuildHeldPositions(PredictedA.BaseHex, PredictedA.Facing, PredictedA.ArmLen, PredictedA.Held);
	
	// Mid positions for A
	FHex A_OldGrip = ComputeGripHex(ArmA.BaseHex, (uint8)ArmA.Facing, ArmA.ArmLen);
	FHex A_NewGrip = ComputeGripHex(PredictedA.BaseHex, PredictedA.Facing, PredictedA.ArmLen);
	const int32 dQA = A_NewGrip.Q - A_OldGrip.Q;
	const int32 dRA = A_NewGrip.R - A_OldGrip.R;
	
	TArray<FHex> A_MidTranslate;
	for (const FHex& Pos : A_Start)
	{
		A_MidTranslate.Add(FHex(Pos.Q + dQA, Pos.R + dRA));
	}
	TArray<FHex> A_MidRotate = BuildHeldPositions(ArmA.BaseHex, PredictedA.Facing, ArmA.ArmLen, ArmA.Held);

	// Arm B positions during rotation
	TArray<FHex> B_Start = BuildHeldPositions(ArmB.BaseHex, (uint8)ArmB.Facing, ArmB.ArmLen, ArmB.Held);
	TArray<FHex> B_End = BuildHeldPositions(PredictedB.BaseHex, PredictedB.Facing, PredictedB.ArmLen, PredictedB.Held);
	
	// Mid positions for B
	FHex B_OldGrip = ComputeGripHex(ArmB.BaseHex, (uint8)ArmB.Facing, ArmB.ArmLen);
	FHex B_NewGrip = ComputeGripHex(PredictedB.BaseHex, PredictedB.Facing, PredictedB.ArmLen);
	const int32 dQB = B_NewGrip.Q - B_OldGrip.Q;
	const int32 dRB = B_NewGrip.R - B_OldGrip.R;
	
	TArray<FHex> B_MidTranslate;
	for (const FHex& Pos : B_Start)
	{
		B_MidTranslate.Add(FHex(Pos.Q + dQB, Pos.R + dRB));
	}
	TArray<FHex> B_MidRotate = BuildHeldPositions(ArmB.BaseHex, PredictedB.Facing, ArmB.ArmLen, ArmB.Held);

	// Check all combinations of sweep positions
	TArray<TArray<FHex>*> A_Phases = { &A_Start, &A_MidTranslate, &A_MidRotate, &A_End };
	TArray<TArray<FHex>*> B_Phases = { &B_Start, &B_MidTranslate, &B_MidRotate, &B_End };

	for (TArray<FHex>* A_Phase : A_Phases)
	{
		for (TArray<FHex>* B_Phase : B_Phases)
		{
			for (const FHex& PosA : *A_Phase)
			{
				for (const FHex& PosB : *B_Phase)
				{
					if (PosA.Q == PosB.Q && PosA.R == PosB.R)
					{
						return true; // Collision detected during sweep
					}
				}
			}
		}
	}

	return false;
}

// Helper function to check rotation sweep against static arm
bool USimWorldHex::WouldRotationSweepOverlapStatic(const FArmState& RotatingArm, const FPredictiveArmState& PredictedRotating,
	const FArmState& StaticArm)
{
	// Helper to compute grip hex
	auto ComputeGripHex = [](const FHex& BaseHex, uint8 Facing, int32 ArmLen) -> FHex
	{
		FHex Grip = BaseHex;
		for (int i = 0; i < ArmLen; ++i)
			Grip = HexUtil::Neighbor(Grip, (EHexDir)Facing);
		return Grip;
	};

	// Helper to build held cluster positions at a given facing
	auto BuildHeldPositions = [&](const FHex& BaseHex, uint8 Facing, int32 ArmLen, const FItem& Held) -> TArray<FHex>
	{
		TArray<FHex> Positions;
		if (!Held.bIsCluster)
		{
			Positions.Add(ComputeGripHex(BaseHex, Facing, ArmLen));
			return Positions;
		}

		const FHex Grip = ComputeGripHex(BaseHex, Facing, ArmLen);
		const uint8 cur = Facing % 6;
		const int8 grab = (int8)Held.GrabFacing;
		const uint8 facingDelta = (grab >= 0)
			? (uint8)(((int32)cur - (int32)(grab % 6) + 6) % 6)
			: cur;
		const uint8 ccw = (uint8)((6 - facingDelta) % 6);

		for (const FOrbData& Sub : Held.Orbs)
		{
			FHex Local(Sub.dQ, Sub.dR);
			FHex RotLocal = Rot60CCW(Local, ccw);
			Positions.Add(FHex(Grip.Q + RotLocal.Q, Grip.R + RotLocal.R));
		}
		return Positions;
	};

	// Static arm's held positions (current state)
	TArray<FHex> StaticPositions = BuildHeldPositions(StaticArm.BaseHex, (uint8)StaticArm.Facing, StaticArm.ArmLen, StaticArm.Held);

	// Rotating arm's sweep positions
	TArray<FHex> Rot_Start = BuildHeldPositions(RotatingArm.BaseHex, (uint8)RotatingArm.Facing, RotatingArm.ArmLen, RotatingArm.Held);
	TArray<FHex> Rot_End = BuildHeldPositions(PredictedRotating.BaseHex, PredictedRotating.Facing, PredictedRotating.ArmLen, PredictedRotating.Held);
	
	// Mid positions for rotating arm
	FHex Rot_OldGrip = ComputeGripHex(RotatingArm.BaseHex, (uint8)RotatingArm.Facing, RotatingArm.ArmLen);
	FHex Rot_NewGrip = ComputeGripHex(PredictedRotating.BaseHex, PredictedRotating.Facing, PredictedRotating.ArmLen);
	const int32 dQ = Rot_NewGrip.Q - Rot_OldGrip.Q;
	const int32 dR = Rot_NewGrip.R - Rot_OldGrip.R;
	
	TArray<FHex> Rot_MidTranslate;
	for (const FHex& Pos : Rot_Start)
	{
		Rot_MidTranslate.Add(FHex(Pos.Q + dQ, Pos.R + dR));
	}
	TArray<FHex> Rot_MidRotate = BuildHeldPositions(RotatingArm.BaseHex, PredictedRotating.Facing, RotatingArm.ArmLen, RotatingArm.Held);

	// Check all phases of rotating arm against static arm
	TArray<TArray<FHex>*> Rot_Phases = { &Rot_Start, &Rot_MidTranslate, &Rot_MidRotate, &Rot_End };

	for (TArray<FHex>* Rot_Phase : Rot_Phases)
	{
		for (const FHex& PosRot : *Rot_Phase)
		{
			for (const FHex& PosStatic : StaticPositions)
			{
				if (PosRot.Q == PosStatic.Q && PosRot.R == PosStatic.R)
				{
					return true; // Collision detected during sweep
				}
			}
		}
	}

	return false;
}
