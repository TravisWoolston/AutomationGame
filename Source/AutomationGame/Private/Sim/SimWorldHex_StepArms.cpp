#include "Sim/SimWorldHex.h"
#include "Actors/BasicArm.h"
#include "Rendering/OrbRenderer.h"
#include "EngineUtils.h"
#include "Engine/Engine.h"


void USimWorldHex::StepArms()
{
	// Apply pending sim values produced by last tick’s Rotate/Extend/Retract
	if (PendingFacingByArm.Num() || PendingArmLenByArm.Num() || PendingGrabFacingByArm.Num())
	{
		for (const auto& KV : PendingFacingByArm)     SimFacingByArm.Add(KV.Key, KV.Value);
		for (const auto& KV : PendingArmLenByArm)     SimArmLenByArm.Add(KV.Key, KV.Value);
		for (const auto& KV : PendingGrabFacingByArm) SimGrabFacingByArm.Add(KV.Key, KV.Value);
		PendingFacingByArm.Empty();
		PendingArmLenByArm.Empty();
		PendingGrabFacingByArm.Empty();
	}

	// Build reverse lookup (item id -> hex) once for this arm step (excludes held items)
	TMap<int32, FHex> ItemIdToHex;
	ItemIdToHex.Reserve(ItemMap.Num());
	for (const auto& KV : ItemMap)
	{
		if (KV.Value.Id != 0)
			ItemIdToHex.Add(KV.Value.Id, KV.Key);
	}

	// Note: We don't reset bRotationCompleted here anymore
	// It stays true until a new rotation starts, allowing grab to check
	// if the previous rotation completed

	// --- Local helpers (can access private FBond type through this) ---
	auto GatherBondedClusterFn = [&](int32 RootId, TSet<int32>& OutIds)
		{
			if (RootId == 0 || OutIds.Contains(RootId)) return;
			TArray<int32> Stack;
			Stack.Add(RootId);
			OutIds.Add(RootId);
			while (Stack.Num())
			{
				int32 Cur = Stack.Pop(EAllowShrinking::No);
				if (const TArray<int32>* BondList = ItemToBonds.Find(Cur))
				{
					for (int32 BondIdx : *BondList)
					{
						if (!Bonds.IsValidIndex(BondIdx)) continue;
						const FBond& B = Bonds[BondIdx];
						const int32 Other = (B.ItemA == Cur) ? B.ItemB : B.ItemA;
						if (Other != 0 && !OutIds.Contains(Other))
						{
							OutIds.Add(Other);
							Stack.Add(Other);
						}
					}
				}
			}
		};

	auto RotateAxial60Fn = [](const FHex& D, int32 StepsCCW)->FHex
		{
			int32 n = ((StepsCCW % 6) + 6) % 6;
			FHex r = D;
			for (int32 i = 0; i < n; ++i)
			{
				const int32 q = r.Q;
				const int32 rr = r.R;
				r.Q = -rr;
				r.R = q + rr;
			}
			return r;
		};

	auto ApproxXYToAxialFn = [&](const FVector2D& Offset)->FHex
		{
			// Small brute force – cluster sizes are tiny.
			float Best = FLT_MAX;
			int32 BestQ = 0, BestR = 0;
			for (int q = -6; q <= 6; ++q)
			{
				for (int r = -6; r <= 6; ++r)
				{
					const FVector2D XY = HexCoord::AxialToXY(FHexAxial(q, r), 1.f, GridOrientation);
					const float DistSq = (XY - Offset).SizeSquared();
					if (DistSq < Best)
					{
						Best = DistSq;
						BestQ = q;
						BestR = r;
					}
				}
			}
			return FHex(BestQ, BestR);
		};
	// ---------------------------------------------------------------

	for (int32 ArmIndex = 0; ArmIndex < Arms.Num(); ArmIndex++)
	{
		FArmState& S = Arms[ArmIndex];
		if (S.Tape.Num() == 0) continue;


		// Allow multiple operations per tick for immediate execution
		int32 OpsExecutedThisTick = 0;
		const int32 MaxOpsPerTick = 3; // Safety limit to prevent infinite loops
		bool bContinueProcessing = true;

		while (bContinueProcessing && OpsExecutedThisTick < MaxOpsPerTick)
		{
			bContinueProcessing = false; // Default to stopping after one op
			OpsExecutedThisTick++;


			const FOp& Op = S.Tape[S.PC];
			bool bResetCommandExpanded = false; // Track if Reset expanded the tape

			// Check if the previous command was a Grab that we need to execute now
			if (S.PC > 0 && !S.bHolding)
			{
				int32 PrevPC = S.PC - 1;
				const FOp& PrevOp = S.Tape[PrevPC];

				if (PrevOp.Cmd == ECmd::Grab)
				{
					//// Execute the deferred grab now
					//if (bDebugLogs && GEngine)
					//{
					//	GEngine->AddOnScreenDebugMessage(-1, 1.0f, FColor::Cyan,
					//		FString::Printf(TEXT("Arm %d: Executing deferred grab from PC=%d"), ArmIndex, PrevPC));
					//}

					// Execute grab logic here (copied from original grab case)
					bool bGrabSucceeded = false;
					FHex Target = S.BaseHex;
					for (int i = 0; i < S.ArmLen; ++i)
						Target = HexUtil::Neighbor(Target, S.Facing);

					FItem* Found = ItemMap.Find(Target);

					// Check if another arm is already trying to grab this cluster
					bool bAnotherArmGrabbing = false;
					if (Found)
					{
						for (const FPendingGrab& PendingGrab : PendingGrabs)
						{
							if (PendingGrab.ArmIndex != ArmIndex &&
								PendingGrab.TargetHex == Target &&
								PendingGrab.ClusterId == Found->Id)
							{
								bAnotherArmGrabbing = true;
								break;
							}
						}
					}

					if (!bAnotherArmGrabbing)
					{
						if (!Found)
						{
							FItem Temp;
							if (TryArmPickItem(Target, Temp))
							{
								S.Held = Temp;
								S.bHolding = true;
								S.PrevHeldOrbCount = 1;
								SkipInputThisFrame.Add(Target);
								bGrabSucceeded = true;
							}
						}
						else
						{
							// Handle cluster grab (simplified for brevity)
							TSet<int32> ClusterIds;
							if (Found->Id != 0 && ItemToBonds.Contains(Found->Id))
								GatherBondedClusterFn(Found->Id, ClusterIds);

							if (ClusterIds.Num() <= 1)
							{
								FItem SingleOut;
								if (TryArmPickItem(Target, SingleOut))
								{
									S.Held = SingleOut;
									S.bHolding = true;
									S.PrevHeldOrbCount = 1;
									SkipInputThisFrame.Add(Target);
									bGrabSucceeded = true;
								}
							}
							// Note: Full cluster grab logic would go here but omitted for brevity
						}
					}
				}
			}

			// Clean up any stale pending grabs for this arm (if it's not trying to grab anymore)
			if (Op.Cmd != ECmd::Grab)
			{
				for (int32 i = PendingGrabs.Num() - 1; i >= 0; i--)
				{
					if (PendingGrabs[i].ArmIndex == ArmIndex)
					{
						PendingGrabs.RemoveAt(i);
					}
				}
			}

			switch (Op.Cmd)
			{
			case ECmd::Wait:
			{
				if (S.WaitCounter == 0)
				{
					S.WaitCounter = (Op.Arg == 0 ? 1 : Op.Arg);
					if (bDebugLogs && GEngine)
					{
						GEngine->AddOnScreenDebugMessage(-1, 1.0f, FColor::Yellow,
							FString::Printf(TEXT("Arm %d: Starting wait for %d ticks"), ArmIndex, S.WaitCounter));
					}
				}
				--S.WaitCounter;
				if (S.WaitCounter > 0)
				{
					// Still waiting - don't advance PC, just continue to next arm
					if (bDebugLogs && GEngine)
					{
						GEngine->AddOnScreenDebugMessage(-1, 0.1f, FColor::Cyan,
							FString::Printf(TEXT("Arm %d: Waiting... %d ticks left"), ArmIndex, S.WaitCounter));
					}
					break; // Exit switch without advancing PC
				}
				S.WaitCounter = 0;
				if (bDebugLogs && GEngine)
				{
					GEngine->AddOnScreenDebugMessage(-1, 1.0f, FColor::Green,
						FString::Printf(TEXT("Arm %d: Wait complete, advancing to next instruction"), ArmIndex));
				}
			} break;

			case ECmd::Grab:
			{
				if (Op.Cmd != ECmd::Grab)
				{
					for (int32 i = PendingGrabs.Num() - 1; i >= 0; i--)
					{
						if (PendingGrabs[i].ArmIndex == ArmIndex)
						{
							PendingGrabs.RemoveAt(i);
						}
					}
				}
				// If already holding, just advance
				if (S.bHolding)
				{
					bContinueProcessing = true; // allow next op this tick
					break;
				}

				// Compute grip/target hex (visual-facing has already been applied to S.Facing here)
				const FHex Target = GetArmGripHex_Sim(ArmIndex);


				FItem* Found = ItemMap.Find(Target);

				// Try single grab first
				if (!Found)
				{
					FItem Temp;
					if (TryArmPickItem(Target, Temp))
					{
						// Don't allow re-grab of items we dropped ourselves this tick
						if (const int32* DroppedByArm = RecentlyDroppedByArm.Find(Temp.Id))
						{
							if (*DroppedByArm == ArmIndex)
							{
								// Put it back and keep waiting on this op
								ItemMap.Add(Target, Temp);
								bContinueProcessing = false;
								break;
							}
						}

						S.Held = Temp;
						S.bHolding = true;
						S.PrevHeldOrbCount = 1;
						SkipInputThisFrame.Add(Target);
						// single item has no held-rotation
						SimGrabFacingByArm.Add(ArmIndex, int8(-1));

						// Advance immediately
						bContinueProcessing = true;
					}
					else
					{
						// No item to pick up this tick; stay on Grab
						bContinueProcessing = false;
					}
					break;
				}

				// We have something at target; decide if it's a bonded cluster
				TSet<int32> ClusterIds;
				if (Found->Id != 0 && ItemToBonds.Contains(Found->Id))
				{
					// Gather bonded ids
					TArray<int32> Stack;
					Stack.Add(Found->Id);
					ClusterIds.Add(Found->Id);

					while (Stack.Num() > 0)
					{
						int32 CurrentId = Stack.Pop();
						if (const TArray<int32>* BondList = ItemToBonds.Find(CurrentId))
						{
							for (int32 BondIdx : *BondList)
							{
								if (!Bonds.IsValidIndex(BondIdx)) continue;
								const FBond& Bond = Bonds[BondIdx];
								if (!Bond.bActive) continue;

								int32 OtherId = (Bond.ItemA == CurrentId) ? Bond.ItemB : Bond.ItemA;
								if (OtherId > 0 && !ClusterIds.Contains(OtherId))
								{
									ClusterIds.Add(OtherId);
									Stack.Add(OtherId);
								}
							}
						}
					}
				}

				// If not actually a multi-item cluster, do a single grab fast-path
				if (ClusterIds.Num() <= 1)
				{
					FItem SingleOut;
					if (TryArmPickItem(Target, SingleOut))
					{
						if (const int32* DroppedByArm = RecentlyDroppedByArm.Find(SingleOut.Id))
						{
							if (*DroppedByArm == ArmIndex)
							{
								// Put it back; do not advance PC
								ItemMap.Add(Target, SingleOut);
								bContinueProcessing = false;
								break;
							}
						}

						S.Held = SingleOut;
						S.bHolding = true;
						S.PrevHeldOrbCount = 1;
						SkipInputThisFrame.Add(Target);
						SimGrabFacingByArm.Add(ArmIndex, int8(-1));

						// Advance immediately
						bContinueProcessing = true;
					}
					else
					{
						bContinueProcessing = false;
					}
					break;
				}

				// Multi-item cluster: assemble immediately
				FItem Cluster;
				Cluster.bIsCluster = true;
				Cluster.Id = Found->Id;
				Cluster.GrabFacing = (int8)S.Facing; // seed with current facing (visual already committed to S.Facing)

				bool bAllFound = true;
				for (int32 ItemId : ClusterIds)
				{
					FHex ItemHex;
					bool bItemFound = false;

					for (const auto& KV2 : ItemMap)
					{
						if (KV2.Value.Id == ItemId)
						{
							ItemHex = KV2.Key;
							bItemFound = true;
							break;
						}
					}

					if (!bItemFound)
					{
						bAllFound = false;
						break;
					}

					const FHex D(ItemHex.Q - Target.Q, ItemHex.R - Target.R);

					FOrbData Orb;
					const FItem* It = ItemMap.Find(ItemHex);
					if (It && !It->bIsCluster)
					{
						// Disallow re-grab of items we dropped ourselves this tick
						if (const int32* DroppedByArm = RecentlyDroppedByArm.Find(It->Id))
						{
							if (*DroppedByArm == ArmIndex)
							{
								bAllFound = false; // treat as not ready yet
								break;
							}
						}

						Orb.Type = (uint8)It->Single.Type;
						Orb.Color = (uint8)It->Single.Color;
						Orb.ItemId = It->Id;
						Orb.dQ = (int8)D.Q;
						Orb.dR = (int8)D.R;
						Orb.Offset = FVector2D::ZeroVector;
						Cluster.Orbs.Add(Orb);
					}
				}

				if (bAllFound && Cluster.Orbs.Num() > 0)
				{
					// Remove all cluster members from the board
					for (const FOrbData& Sub : Cluster.Orbs)
					{
						for (auto It = ItemMap.CreateIterator(); It; ++It)
						{
							if (It.Value().Id == Sub.ItemId)
							{
								SkipInputThisFrame.Add(It.Key());
								HeldItemLastHex.Add(Sub.ItemId, It.Key());
								It.RemoveCurrent();
								break;
							}
						}
					}

					S.Held = Cluster;
					S.bHolding = true;
					S.PrevHeldOrbCount = Cluster.Orbs.Num();
					// seed sim-held orientation immediately
					SimGrabFacingByArm.Add(ArmIndex, Cluster.GrabFacing);

					// Advance immediately
					bContinueProcessing = true;
				}
				else
				{
					// Not ready to grab the cluster now; stay on Grab
					bContinueProcessing = false;
				}
			}
			break;

			case ECmd::Drop:
				if (S.bHolding)
				{
					FHex GripTarget = S.BaseHex;
					for (int i = 0; i < S.ArmLen; ++i)
						GripTarget = HexUtil::Neighbor(GripTarget, S.Facing);

					if (!S.Held.bIsCluster)
					{
						// Clear any ghost tracking for this single orb before placing it
						HeldItemLastHex.Remove(S.Held.Id);
						NewlyPlacedItems.Remove(S.Held.Id);
						// Don't remove from RecentlyDroppedByArm - we need to add it after placement

						// Also remove any ghost orb from ItemMap that might be causing collision issues
						for (auto It = ItemMap.CreateIterator(); It; ++It)
						{
							if (It.Value().Id == S.Held.Id)
							{
								It.RemoveCurrent();
								break;
							}
						}

						if (TryArmPlaceItem(GripTarget, S.Held))
						{
							// Add to recently dropped tracking for settling period
							RecentlyDroppedByArm.Add(S.Held.Id, ArmIndex);
							
							S.bHolding = false;
							S.Held = FItem();
							S.bWaitingForBond = false;
							S.WaitForBondInitialCount = 0;
							S.PrevHeldOrbCount = 0;
							// NEW: clear sim held-rotation immediately (not only pending)
							SimGrabFacingByArm.Add(ArmIndex, int8(-1));
							PendingGrabFacingByArm.Add(ArmIndex, int8(-1));
							SkipOutputThisFrame.Add(GripTarget);

							// Enable immediate execution of next command after successful drop
							bContinueProcessing = true;
						}
						else
						{
							// Drop failed - location occupied
						}
					}
					else
					{
						// Compute rotation delta relative to grab, not absolute facing.
						const uint8 CurFacingSteps = (uint8)S.Facing % 6;
						const int8 StoredGrabFacing = (int8)(S.Held.GrabFacing);
						const uint8 FacingDelta = (StoredGrabFacing >= 0)
							? (uint8)(((int32)CurFacingSteps - (int32)(StoredGrabFacing % 6) + 6) % 6)
							: CurFacingSteps; // fallback for legacy items

						bool bBlocked = false;
						TArray<FHex> TargetHexes;
						TargetHexes.Reserve(S.Held.Orbs.Num());

						for (const FOrbData& Sub : S.Held.Orbs)
						{
							FHex Local;
							if (Sub.dQ != 0 || Sub.dR != 0 || Sub.Offset.IsNearlyZero())
								Local = FHex(Sub.dQ, Sub.dR);
							else
								Local = ApproxXYToAxialFn(Sub.Offset); // legacy fallback

							// FacingDelta was computed as (CurFacing - GrabFacing) in a system where
							// +1 is a clockwise 60° step (see DirToYaw). RotateAxial60Fn expects
							// positive steps = CCW, so we invert the direction.
							const uint8 CCWSteps = (uint8)((6 - FacingDelta) % 6);
							FHex RotLocal = RotateAxial60Fn(Local, CCWSteps);

							FHex WorldHex(GripTarget.Q + RotLocal.Q, GripTarget.R + RotLocal.R);
							if (ItemMap.Contains(WorldHex)) { bBlocked = true; break; }
							TargetHexes.Add(WorldHex);
						}

						if (bBlocked)
						{
							if (S.WaitCounter == 0) S.WaitCounter = 1;
							break;
						}

						// Clear any ghost tracking for this arm before placing real orbs
						// This prevents teleporting behavior from stale HeldItemLastHex entries
						for (const FOrbData& Sub : S.Held.Orbs)
						{
							HeldItemLastHex.Remove(Sub.ItemId);
							NewlyPlacedItems.Remove(Sub.ItemId);
							// Don't remove from RecentlyDroppedByArm - we need to add after placement

							// Also remove any ghost orbs from ItemMap that might be causing collision issues
							for (auto It = ItemMap.CreateIterator(); It; ++It)
							{
								if (It.Value().Id == Sub.ItemId)
								{
									It.RemoveCurrent();
									break;
								}
							}
						}

						// Place each sub‑orb back with its preserved ItemId (or assign if missing)
						for (int32 idx = 0; idx < S.Held.Orbs.Num(); ++idx)
						{
							const FOrbData& Sub = S.Held.Orbs[idx];
							FOrb O; O.Type = (EOrbType)Sub.Type; O.Color = (EOrbColor)Sub.Color;

							FItem NewItem = FItem::MakeSingle(O);
							if (Sub.ItemId > 0)
							{
								NewItem.Id = Sub.ItemId;          // reuse original Id
								// Defensive: keep NextItemId ahead
								NextItemId = FMath::Max(NextItemId, NewItem.Id + 1);
							}
							else
							{
								NewItem.Id = NextItemId++;        // legacy fallback
							}

							if (TryPutItem(TargetHexes[idx], NewItem))
							{
								SkipOutputThisFrame.Add(TargetHexes[idx]);
								HeldItemLastHex.Remove(NewItem.Id);
								// Add to recently dropped tracking for settling period
								RecentlyDroppedByArm.Add(NewItem.Id, ArmIndex);
							}
						}

						// Bonds referencing these Ids were never removed; offsets will self‑heal in CleanupBrokenBonds next tick.
						S.bHolding = false;
						S.Held = FItem();
						S.bWaitingForBond = false;
						S.WaitForBondInitialCount = 0;
						S.PrevHeldOrbCount = 0;
						// NEW: clear sim held-rotation immediately (not only pending)
						SimGrabFacingByArm.Add(ArmIndex, int8(-1));
						PendingGrabFacingByArm.Add(ArmIndex, int8(-1));

						// Enable immediate execution of next command after successful drop
						bContinueProcessing = true;
					}

					// Clean up any pending grabs for this arm
					for (int32 i = PendingGrabs.Num() - 1; i >= 0; i--)
					{
						if (PendingGrabs[i].ArmIndex == ArmIndex)
						{
							PendingGrabs.RemoveAt(i);
						}
					}
				}
				break;

			case ECmd::RotateL:
			case ECmd::RotateR:
			{
				// Mark that rotation is starting
				S.bRotationCompleted = false;

				// If we were told to wait (from a previously blocked rotate) count it down and retry next tick.
				if (S.WaitCounter > 0)
				{
					--S.WaitCounter;
					// Do NOT advance PC; retry this same rotate instruction.
					continue;
				}

				EHexDir NewFacing = (Op.Cmd == ECmd::RotateL)
					? (EHexDir)(((uint8)S.Facing + 1) % 6)
					: (EHexDir)(((uint8)S.Facing + 5) % 6);

				// Hand positions before/after rotation
				FHex OldHand = S.BaseHex;
				for (int i = 0; i < S.ArmLen; ++i)
					OldHand = HexUtil::Neighbor(OldHand, S.Facing);

				FHex NewHand = S.BaseHex;
				for (int i = 0; i < S.ArmLen; ++i)
					NewHand = HexUtil::Neighbor(NewHand, NewFacing);

				const int32 dQ = NewHand.Q - OldHand.Q;
				const int32 dR = NewHand.R - OldHand.R;

				// Occupancy: world items
				TSet<FHex> WorldOcc;
				for (const auto& KV : ItemMap)
					WorldOcc.Add(KV.Key);

				// Helper: compute hand hex for a given arm/facing
				auto ComputeGripHex = [](const FArmState& A, EHexDir Facing)->FHex
					{
						FHex Grip = A.BaseHex;
						for (int i = 0; i < A.ArmLen; ++i)
							Grip = HexUtil::Neighbor(Grip, Facing);
						return Grip;
					};

				// Helper: rotate an axial coordinate by StepsCCW 60° steps
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

				// Build occupancy for other arms' held orbs (and their hands)
				TSet<FHex> OtherHeldOcc;
				for (int32 OtherIdx = 0; OtherIdx < Arms.Num(); ++OtherIdx)
				{
					if (OtherIdx == ArmIndex) continue;
					const FArmState& O = Arms[OtherIdx];
					if (!O.bHolding) continue;

					// Always include their hand hex
					const FHex OHand = ComputeGripHex(O, O.Facing);
					OtherHeldOcc.Add(OHand);

					if (O.Held.bIsCluster)
					{
						// Virtual positions of their sub-orbs at current facing
						const uint8 cur = (uint8)O.Facing % 6;
						const int8 grab = (int8)O.Held.GrabFacing;
						const uint8 facingDelta = (grab >= 0)
							? (uint8)(((int32)cur - (int32)(grab % 6) + 6) % 6)
							: cur;
						const uint8 ccw = (uint8)((6 - facingDelta) % 6);

						for (const FOrbData& Sub : O.Held.Orbs)
						{
							FHex Local(Sub.dQ, Sub.dR);
							FHex RotLocal = Rot60(Local, ccw);
							OtherHeldOcc.Add(FHex(OHand.Q + RotLocal.Q, OHand.R + RotLocal.R));
						}
					}
					else
					{
						// Single held orb occupies the hand
						OtherHeldOcc.Add(OHand);
					}
				}

				// Self held-orb positions at start and end, plus two “mid” phases
				auto BuildLocals = [&](EHexDir Facing, TArray<FHex>& OutLocals)->void
					{
						OutLocals.Reset();
						if (!S.bHolding)
							return;

						const FHex Grip = ComputeGripHex(S, Facing);
						if (!S.Held.bIsCluster)
						{
							OutLocals.Add(Grip);
							return;
						}

						const uint8 cur = (uint8)Facing % 6;
						const int8 grab = (int8)S.Held.GrabFacing;
						const uint8 facingDelta = (grab >= 0)
							? (uint8)(((int32)cur - (int32)(grab % 6) + 6) % 6)
							: cur;
						const uint8 ccw = (uint8)((6 - facingDelta) % 6);

						for (const FOrbData& Sub : S.Held.Orbs)
						{
							FHex Local(Sub.dQ, Sub.dR);
							FHex RotLocal = Rot60(Local, ccw);
							OutLocals.Add(FHex(Grip.Q + RotLocal.Q, Grip.R + RotLocal.R));
						}
					};

				TArray<FHex> StartLocals, EndLocals, MidTranslateLocals, MidRotateLocals;
				BuildLocals(S.Facing, StartLocals);
				BuildLocals(NewFacing, EndLocals);

				// Mid A: translate hand first (Old orientation at New hand)
				if (S.bHolding)
				{
					if (!S.Held.bIsCluster)
					{
						MidTranslateLocals.Add(NewHand);
					}
					else
					{
						// Old orientation at New hand
						const uint8 curOld = (uint8)S.Facing % 6;
						const int8 grab = (int8)S.Held.GrabFacing;
						const uint8 facingDeltaOld = (grab >= 0)
							? (uint8)(((int32)curOld - (int32)(grab % 6) + 6) % 6)
							: curOld;
						const uint8 ccwOld = (uint8)((6 - facingDeltaOld) % 6);
						for (const FOrbData& Sub : S.Held.Orbs)
						{
							FHex Local(Sub.dQ, Sub.dR);
							FHex RotLocal = Rot60(Local, ccwOld);
							MidTranslateLocals.Add(FHex(NewHand.Q + RotLocal.Q, NewHand.R + RotLocal.R));
						}
					}
				}

				// Mid B: rotate first (New orientation at Old hand)
				if (S.bHolding)
				{
					if (!S.Held.bIsCluster)
					{
						MidRotateLocals.Add(OldHand);
					}
					else
					{
						const uint8 curNew = (uint8)NewFacing % 6;
						const int8 grab = (int8)S.Held.GrabFacing;
						const uint8 facingDeltaNew = (grab >= 0)
							? (uint8)(((int32)curNew - (int32)(grab % 6) + 6) % 6)
							: curNew;
						const uint8 ccwNew = (uint8)((6 - facingDeltaNew) % 6);
						for (const FOrbData& Sub : S.Held.Orbs)
						{
							FHex Local(Sub.dQ, Sub.dR);
							FHex RotLocal = Rot60(Local, ccwNew);
							MidRotateLocals.Add(FHex(OldHand.Q + RotLocal.Q, OldHand.R + RotLocal.R));
						}
					}
				}

				// NEW: Get predicted bond formations for Opus Magnum-style logic
				TMap<FHex, int32> PredictedBonds = GetPredictedBondFormations();
				
				auto OverlapsAny = [&](const FHex& H)->bool
					{
						// Opus Magnum logic: don't consider board orbs as obstructions if they will bond to held clusters
						if (PredictedBonds.Contains(H))
						{
							return false; // This position will have a bond formed, so it's not an obstruction
						}
						return WorldOcc.Contains(H) || OtherHeldOcc.Contains(H);
					};

				bool bBlocked = false;

				// 1) Destination hand cannot contain foreign item/held orb (covers single held-orb too)
				// Only check this if the arm is holding something
				if (S.bHolding && OverlapsAny(NewHand))
				{
					bBlocked = true;
				}

				// 2) End positions of all sub-orbs
				if (!bBlocked)
				{
					for (const FHex& H : EndLocals)
					{
						if (OverlapsAny(H))
						{
							bBlocked = true;
							break;
						}
					}
				}

				// 3) Mid A: translate first (hand moves; orientation unchanged)
				if (!bBlocked)
				{
					for (const FHex& H : MidTranslateLocals)
						if (OverlapsAny(H)) { bBlocked = true; break; }
				}

				// 4) Mid B: rotate first (orientation changes; hand stays)
				if (!bBlocked)
				{
					for (const FHex& H : MidRotateLocals)
						if (OverlapsAny(H)) { bBlocked = true; break; }
				}

				// 5) Full sweep: check if the rotation path would intersect with any orbs
				if (!bBlocked && S.bHolding)
				{
					// For held clusters, we need to check if any sub-orb would sweep through occupied space
					// during the rotation from start to end orientation
					if (S.Held.bIsCluster)
					{
						// Check each sub-orb's sweep path
						for (const FOrbData& Sub : S.Held.Orbs)
						{
							FHex Local(Sub.dQ, Sub.dR);
							
							// Calculate the start and end positions of this sub-orb
							const uint8 curStart = (uint8)S.Facing % 6;
							const int8 grab = (int8)S.Held.GrabFacing;
							const uint8 facingDeltaStart = (grab >= 0)
								? (uint8)(((int32)curStart - (int32)(grab % 6) + 6) % 6)
								: curStart;
							const uint8 ccwStart = (uint8)((6 - facingDeltaStart) % 6);
							FHex RotLocalStart = Rot60(Local, ccwStart);
							FHex StartPos(OldHand.Q + RotLocalStart.Q, OldHand.R + RotLocalStart.R);
							
							const uint8 curEnd = (uint8)NewFacing % 6;
							const uint8 facingDeltaEnd = (grab >= 0)
								? (uint8)(((int32)curEnd - (int32)(grab % 6) + 6) % 6)
								: curEnd;
							const uint8 ccwEnd = (uint8)((6 - facingDeltaEnd) % 6);
							FHex RotLocalEnd = Rot60(Local, ccwEnd);
							FHex EndPos(NewHand.Q + RotLocalEnd.Q, NewHand.R + RotLocalEnd.R);
							
							// Check if any hex along the path from StartPos to EndPos is occupied
							// For now, we'll check a few intermediate points along the path
							const int32 subOrbDq = EndPos.Q - StartPos.Q;
							const int32 subOrbDr = EndPos.R - StartPos.R;
							
							// Check intermediate positions along the sweep path
							for (int32 i = 1; i <= 3; ++i) // Check 3 intermediate points
							{
								const float t = float(i) / 4.0f; // 0.25, 0.5, 0.75
								FHex IntermediatePos(
									StartPos.Q + FMath::RoundToInt(subOrbDq * t),
									StartPos.R + FMath::RoundToInt(subOrbDr * t)
								);
								
								if (OverlapsAny(IntermediatePos))
								{
									bBlocked = true;
									break;
								}
							}
							
							if (bBlocked) break;
						}
					}
					else
					{
						// For single held orbs, check if the hand position sweep would intersect anything
						const int32 handDq = NewHand.Q - OldHand.Q;
						const int32 handDr = NewHand.R - OldHand.R;
						
						// Check intermediate positions along the hand's sweep path
						for (int32 i = 1; i <= 3; ++i) // Check 3 intermediate points
						{
							const float t = float(i) / 4.0f; // 0.25, 0.5, 0.75
							FHex IntermediatePos(
								OldHand.Q + FMath::RoundToInt(handDq * t),
								OldHand.R + FMath::RoundToInt(handDr * t)
							);
							
							if (OverlapsAny(IntermediatePos))
							{
								bBlocked = true;
								break;
							}
						}
					}
				}

				if (bBlocked)
				{
					// Arm cannot rotate this tick; set a 1‑tick wait and retry SAME instruction.
					if (S.WaitCounter == 0)
					{
						S.WaitCounter = 1;
					}
					// Do NOT advance PC; keep pointing at this rotate op.
					continue;
				}

				// Commit visual/current arm rotation immediately
				S.Facing = NewFacing;

				// Defer simulation-facing to next fixed tick
				PendingFacingByArm.Add(ArmIndex, static_cast<uint8>(S.Facing));
				// Note: we do not touch SimFacingByArm here, it updates next tick
			}
			break;

			case ECmd::Extend:
			{
				const int32 Delta = (Op.Arg == 0 ? 1 : Op.Arg);
				const int32 OldLen = S.ArmLen;
				S.ArmLen = FMath::Clamp<int32>(S.ArmLen + Delta, 1, MAX_ARM_LEN);
				// Defer simulation reach to next fixed tick
				PendingArmLenByArm.Add(ArmIndex, S.ArmLen);

				if (bDevDebugArms && GEngine && S.ArmLen != OldLen)
				{
					FHex Hand = S.BaseHex;
					for (int i = 0; i < S.ArmLen; ++i)
						Hand = HexUtil::Neighbor(Hand, S.Facing);
					GEngine->AddOnScreenDebugMessage(
						-1, 1.5f, FColor::Green,
						FString::Printf(TEXT("[Arm %d] EXTEND (Δ=%d) Len=%d Hand=(%d,%d)"),
							ArmIndex, Delta, S.ArmLen, Hand.Q, Hand.R));
				}
			}
			break;

			case ECmd::Retract:
			{
				const int32 Delta = (Op.Arg == 0 ? 1 : Op.Arg);
				const int32 OldLen = S.ArmLen;
				S.ArmLen = FMath::Clamp<int32>(S.ArmLen - Delta, 1, MAX_ARM_LEN);
				// Defer simulation reach to next fixed tick
				PendingArmLenByArm.Add(ArmIndex, S.ArmLen);

				if (bDevDebugArms && GEngine && S.ArmLen != OldLen)
				{
					FHex Hand = S.BaseHex;
					for (int i = 0; i < S.ArmLen; ++i)
						Hand = HexUtil::Neighbor(Hand, S.Facing);
					GEngine->AddOnScreenDebugMessage(
						-1, 1.5f, FColor::Orange,
						FString::Printf(TEXT("[Arm %d] RETRACT (Δ=%d) Len=%d Hand=(%d,%d)"),
							ArmIndex, Delta, S.ArmLen, Hand.Q, Hand.R));
				}
			}
			break;
			case ECmd::MovePos: S.TrackIdx = FMath::Clamp<int32>(S.TrackIdx + 1, -4, 4); break;
			case ECmd::MoveNeg: S.TrackIdx = FMath::Clamp<int32>(S.TrackIdx - 1, -4, 4); break;
			case ECmd::PivotL:  break;
			case ECmd::PivotR:  break;
			case ECmd::Reset:
			{
				// Reset command implementation: Generate commands to drop and return to home position
				// Calculate movements needed to return to home position
				int32 CurrentReach = S.ArmLen;  // Not const, as we may update it after extend/drop
				const int32 TargetReach = S.HomeArmLen;
				const uint8 CurrentRotation = static_cast<uint8>(S.Facing);
				const uint8 TargetRotation = static_cast<uint8>(S.HomeFacing);


				// Create a temporary program to return to home
				TArray<FOp> ResetOps;

				// Handle drop if holding
				if (S.bHolding)
				{
					ResetOps.Add({ ECmd::Drop, 0 });
				}

				// Add reach adjustments
				const int32 ReachDiff = TargetReach - CurrentReach;
				if (ReachDiff > 0)
				{
					// Need to extend
					for (int32 i = 0; i < ReachDiff; ++i)
					{
						ResetOps.Add({ ECmd::Extend, 1 });
					}
				}
				else if (ReachDiff < 0)
				{
					// Need to retract
					for (int32 i = 0; i < -ReachDiff; ++i)
					{
						ResetOps.Add({ ECmd::Retract, 1 });
					}
				}

				// Add rotation adjustments - find shortest path
				// In hex coordinates, RotateL increases the facing value (0->1->2->3->4->5->0)
				// and RotateR decreases it (0->5->4->3->2->1->0)
				// So to go from current to target:
				// - If we need to increase facing, we use RotateL
				// - If we need to decrease facing, we use RotateR

				if (CurrentRotation != TargetRotation)
				{
					// Calculate steps needed in each direction
					// Steps using RotateL (counter-clockwise, increases facing)
					const int32 StepsCounterClockwise = (TargetRotation - CurrentRotation + 6) % 6;
					// Steps using RotateR (clockwise, decreases facing)
					const int32 StepsClockwise = (CurrentRotation - TargetRotation + 6) % 6;

					// Choose the shorter path
					if (StepsClockwise <= StepsCounterClockwise)
					{
						// Use clockwise rotation (RotateR)
						for (int32 i = 0; i < StepsClockwise; ++i)
						{
							ResetOps.Add({ ECmd::RotateR, 1 });
						}
					}
					else
					{
						// Use counter-clockwise rotation (RotateL)
						for (int32 i = 0; i < StepsCounterClockwise; ++i)
						{
							ResetOps.Add({ ECmd::RotateL, 1 });
						}
					}
				}

				// If we generated any reset operations, insert them into the tape
				if (ResetOps.Num() > 0)
				{
					// Insert the reset operations after the current PC
					S.Tape.Insert(ResetOps, S.PC + 1);
					bResetCommandExpanded = true;

					// Enable immediate execution of the first reset operation
					bContinueProcessing = true;
				}
				else
				{
					// Already at home position
				}
			}
			break;
			case ECmd::Repeat:  break;
			case ECmd::RotateHeldL:
				if (S.bHolding && S.Held.bIsCluster)
				{
					S.bRotationCompleted = false;
					S.Held.GrabFacing = (S.Held.GrabFacing + 5) % 6;
					// Defer simulation-held orientation to next fixed tick
					PendingGrabFacingByArm.Add(ArmIndex, S.Held.GrabFacing);
				}
				break;
			case ECmd::RotateHeldR:
				if (S.bHolding && S.Held.bIsCluster)
				{
					S.bRotationCompleted = false;
					S.Held.GrabFacing = (S.Held.GrabFacing + 1) % 6;
					// Defer simulation-held orientation to next fixed tick
					PendingGrabFacingByArm.Add(ArmIndex, S.Held.GrabFacing);
				}
				break;
			case ECmd::WaitForBond:
				if (S.bHolding)
				{
					const int32 CurrentCountNow = S.Held.bIsCluster ? S.Held.Orbs.Num() : 1;

					// Initialize the wait state on first entry using the CURRENT count
					if (!S.bWaitingForBond)
					{
						S.bWaitingForBond = true;
						S.WaitForBondInitialCount = CurrentCountNow;

						// If a bond already exists (eg. assimilation earlier this tick), proceed immediately
						if (CurrentCountNow > S.WaitForBondInitialCount)
						{
							S.bWaitingForBond = false;
							S.WaitForBondInitialCount = 0;
							bContinueProcessing = true;
							break;
						}
					}

					// Progress check
					if (CurrentCountNow > S.WaitForBondInitialCount)
					{
						S.bWaitingForBond = false;
						S.WaitForBondInitialCount = 0;
						bContinueProcessing = true;
					}
					else
					{
						break; // keep waiting
					}
				}
				break;
			}

			// Handle PC advancement based on command execution result
			// Some commands (like failed grabs or waiting) shouldn't advance PC
			bool bShouldAdvancePC = true;
			switch (Op.Cmd)
			{
			case ECmd::Wait:
				if (S.WaitCounter > 0) bShouldAdvancePC = false;
				break;
			case ECmd::Grab:
				// Check if we're still trying to grab (failed grab)
				// In the grab case above, if grab fails we break without setting bContinueProcessing
				// So if we get here and bContinueProcessing is false, it means grab failed
				if (!bContinueProcessing) bShouldAdvancePC = false;
				break;
			case ECmd::WaitForBond:
				if (S.bWaitingForBond) bShouldAdvancePC = false;
				break;
			default:
				break;
			}

			if (bShouldAdvancePC)
			{
				// Special handling for Reset command: don't advance PC if we just expanded the tape
				// The PC is already pointing to where it should be after insertion
				if (bResetCommandExpanded)
				{
					// PC stays where it is - it's now pointing at the Reset command
					// We need to advance it to the first inserted operation
					S.PC = (S.PC + 1) % S.Tape.Num();
				}
				else
				{
					int32 OldPC = S.PC;
					S.PC = (S.PC + 1) % S.Tape.Num();

					// Check if we wrapped around (looped)
					if (S.PC == 0 && OldPC > 0)
					{
						// We've looped! Check if the tape has been expanded by Reset commands
						// If so, we need to rebuild it from the original program

						// Find the corresponding actor and rebuild the tape
						ABasicArm* ArmActor = nullptr;
						if (UWorld* World = GetWorld())
						{
							for (TActorIterator<ABasicArm> It(World); It; ++It)
							{
								ABasicArm* TestArm = *It;
								if (TestArm && TestArm->SimArmIndex == ArmIndex)
								{
									ArmActor = TestArm;
									break;
								}
							}
						}

						if (ArmActor)
						{
							// Rebuild the tape from the original program
							TArray<FOp> OriginalTape = ArmActor->BuildOpsFromProgram(ArmActor->Program);
							if (S.Tape.Num() != OriginalTape.Num())
							{
								// Restore original tape to remove temporary Reset operations
								S.Tape = OriginalTape;
							}
						}
					}

				}
			}
		} // End of while loop for immediate execution

	}

	// Update previous orb counts for next tick's WaitForBond checks
	for (FArmState& S : Arms)
	{
		S.PrevHeldOrbCount = S.bHolding ? (S.Held.bIsCluster ? S.Held.Orbs.Num() : 1) : 0;
	}
}