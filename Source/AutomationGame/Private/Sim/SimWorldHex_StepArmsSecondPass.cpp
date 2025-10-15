#include "Sim/SimWorldHex.h"
#include "Actors/BasicArm.h"
#include "Rendering/OrbRenderer.h"
#include "EngineUtils.h"
#include "Engine/Engine.h"

void USimWorldHex::StepArmsSecondPass()
{
	// Second pass - only process arms that are trying to grab and don't have anything
	// This allows immediate grabbing of orbs that were just unbonded

	for (int32 ArmIndex = 0; ArmIndex < Arms.Num(); ArmIndex++)
	{
		FArmState& S = Arms[ArmIndex];
		if (S.Tape.Num() == 0) continue;

		// Only process arms that are trying to grab and failed in the first pass
		const FOp& Op = S.Tape[S.PC];
		if (Op.Cmd != ECmd::Grab || S.bHolding) continue;

		// Gate on committed sim state (delayed facing/length)
		const uint8 SimFace = SimFacingByArm.Contains(ArmIndex)
			? SimFacingByArm[ArmIndex]
			: uint8(S.Facing);
		const int32 SimLen = SimArmLenByArm.Contains(ArmIndex)
			? SimArmLenByArm[ArmIndex]
			: S.ArmLen;

		if (SimFace != uint8(S.Facing) || SimLen != S.ArmLen)
		{
			// Still in-flight rotation/extension; wait for next tick
			continue;
		}
		// Calculate target position
		const FHex Target = GetArmGripHex_Sim(ArmIndex);

		// Try to grab again
		FItem* Found = ItemMap.Find(Target);
		if (!Found)
		{
			FItem Temp;
			if (TryArmPickItem(Target, Temp))
			{
				if (const int32* DroppedByArm = RecentlyDroppedByArm.Find(Temp.Id))
				{
					if (*DroppedByArm == ArmIndex) continue;
				}

				S.Held = Temp;
				S.bHolding = true;
				S.PrevHeldOrbCount = 1;
				SimGrabFacingByArm.Add(ArmIndex, int8(-1));
				S.PC = (S.PC + 1) % S.Tape.Num();
			}
		}
		else
		{
			// Check for bonded cluster
			TSet<int32> ClusterIds;
			if (Found->Id != 0 && ItemToBonds.Contains(Found->Id))
			{
				// Gather all bonded items
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

			if (ClusterIds.Num() <= 1)
			{
				// Single item
				FItem SingleOut;
				if (TryArmPickItem(Target, SingleOut))
				{
					if (const int32* DroppedByArm = RecentlyDroppedByArm.Find(SingleOut.Id))
					{
						if (*DroppedByArm == ArmIndex)
						{
							continue;
						}
					}

					S.Held = SingleOut;
					S.bHolding = true;
					S.PrevHeldOrbCount = 1;
					// NEW: single item has no held-rotation
					SimGrabFacingByArm.Add(ArmIndex, int8(-1));

					// Advance PC since grab succeeded
					S.PC = (S.PC + 1) % S.Tape.Num();
				}
			}
			else
			{
				// Multi-item cluster - grab the whole thing
				FItem Cluster;
				Cluster.bIsCluster = true;
				Cluster.Id = Found->Id;
				Cluster.GrabFacing = (int8)S.Facing;

				// Gather all cluster members
				bool bAllFound = true;
				for (int32 ItemId : ClusterIds)
				{
					FHex ItemHex;
					bool bItemFound = false;

					for (const auto& KV : ItemMap)
					{
						if (KV.Value.Id == ItemId)
						{
							ItemHex = KV.Key;
							bItemFound = true;
							break;
						}
					}

					if (!bItemFound)
					{
						bAllFound = false;
						break;
					}

					// Calculate relative position
					FHex D(ItemHex.Q - Target.Q, ItemHex.R - Target.R);

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
					// Remove all items from board
					for (const FOrbData& Sub : Cluster.Orbs)
					{
						for (auto It = ItemMap.CreateIterator(); It; ++It)
						{
							if (It.Value().Id == Sub.ItemId)
							{
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

					// Advance PC
					S.PC = (S.PC + 1) % S.Tape.Num();
					S.bReadyForGrab = false;
				}
				else
				{
					S.bReadyForGrab = true;
				}
			}
		}
	}

	// Helper to execute one fast op (Drop/Grab/Rotate/Extend/RotateHeld) with proper checks
	auto ExecFastOpOnce = [&](int32 ArmIndex)->bool
		{
			if (ArmIndex < 0 || ArmIndex >= Arms.Num()) return false;
			FArmState& S = Arms[ArmIndex];
			if (S.Tape.Num() == 0) return false;

			// Local helpers
			auto RotateAxial60Fn = [](const FHex& D, uint8 StepsCCW)->FHex
				{
					FHex r = D;
					for (uint8 i = 0; i < (StepsCCW % 6); ++i)
					{
						const int32 q = r.Q, rr = r.R;
						r.Q = -rr; r.R = q + rr;
					}
					return r;
				};
			auto ComputeGripHex = [](const FArmState& A)->FHex
				{
					FHex Grip = A.BaseHex;
					for (int i = 0; i < A.ArmLen; ++i)
						Grip = HexUtil::Neighbor(Grip, A.Facing);
					return Grip;
				};

			const FOp& Op = S.Tape[S.PC];

			switch (Op.Cmd)
			{
			case ECmd::Drop:
			{
				if (!S.bHolding) { S.PC = (S.PC + 1) % S.Tape.Num(); return true; }
				FHex GripTarget = ComputeGripHex(S);

				if (!S.Held.bIsCluster)
				{
					HeldItemLastHex.Remove(S.Held.Id);
					NewlyPlacedItems.Remove(S.Held.Id);
					RecentlyDroppedByArm.Remove(S.Held.Id);

					// Remove any ghost
					for (auto It = ItemMap.CreateIterator(); It; ++It)
					{
						if (It.Value().Id == S.Held.Id) { It.RemoveCurrent(); break; }
					}

					if (TryArmPlaceItem(GripTarget, S.Held))
					{
						S.bHolding = false;
						S.Held = FItem();
						S.bWaitingForBond = false;
						S.WaitForBondInitialCount = 0;
						S.PrevHeldOrbCount = 0;
						SimGrabFacingByArm.Add(ArmIndex, int8(-1));
						PendingGrabFacingByArm.Add(ArmIndex, int8(-1));
						SkipOutputThisFrame.Add(GripTarget);

						S.PC = (S.PC + 1) % S.Tape.Num();
						return true;
					}
					return false;
				}
				else
				{
					const uint8 CurFacingSteps = (uint8)S.Facing % 6;
					const int8 StoredGrabFacing = (int8)(S.Held.GrabFacing);
					const uint8 FacingDelta = (StoredGrabFacing >= 0)
						? (uint8)(((int32)CurFacingSteps - (int32)(StoredGrabFacing % 6) + 6) % 6)
						: CurFacingSteps;

					bool bBlocked = false;
					TArray<FHex> TargetHexes;
					TargetHexes.Reserve(S.Held.Orbs.Num());

					for (const FOrbData& Sub : S.Held.Orbs)
					{
						FHex Local(Sub.dQ, Sub.dR);
						const uint8 CCWSteps = (uint8)((6 - FacingDelta) % 6);
						FHex RotLocal = RotateAxial60Fn(Local, CCWSteps);

						FHex WorldHex(GripTarget.Q + RotLocal.Q, GripTarget.R + RotLocal.R);
						if (ItemMap.Contains(WorldHex)) { bBlocked = true; break; }
						TargetHexes.Add(WorldHex);
					}

					if (bBlocked) return false;

					for (const FOrbData& Sub : S.Held.Orbs)
					{
						HeldItemLastHex.Remove(Sub.ItemId);
						NewlyPlacedItems.Remove(Sub.ItemId);
						RecentlyDroppedByArm.Remove(Sub.ItemId);
						for (auto It = ItemMap.CreateIterator(); It; ++It)
						{
							if (It.Value().Id == Sub.ItemId) { It.RemoveCurrent(); break; }
						}
					}

					for (int32 idx = 0; idx < S.Held.Orbs.Num(); ++idx)
					{
						const FOrbData& Sub = S.Held.Orbs[idx];
						FOrb O; O.Type = (EOrbType)Sub.Type; O.Color = (EOrbColor)Sub.Color;

						FItem NewItem = FItem::MakeSingle(O);
						if (Sub.ItemId > 0) { NewItem.Id = Sub.ItemId; NextItemId = FMath::Max(NextItemId, NewItem.Id + 1); }
						else { NewItem.Id = NextItemId++; }

						if (TryPutItem(TargetHexes[idx], NewItem))
						{
							SkipOutputThisFrame.Add(TargetHexes[idx]);
							HeldItemLastHex.Remove(NewItem.Id);
						}
					}

					S.bHolding = false;
					S.Held = FItem();
					S.bWaitingForBond = false;
					S.WaitForBondInitialCount = 0;
					S.PrevHeldOrbCount = 0;
					SimGrabFacingByArm.Add(ArmIndex, int8(-1));
					PendingGrabFacingByArm.Add(ArmIndex, int8(-1));

					S.PC = (S.PC + 1) % S.Tape.Num();
					return true;
				}
			}

			case ECmd::Grab:
			{
				if (S.bHolding) { S.PC = (S.PC + 1) % S.Tape.Num(); return true; }

				FHex Target = ComputeGripHex(S);
				FItem* Found = ItemMap.Find(Target);

				// Single first
				if (!Found)
				{
					FItem Temp;
					if (TryArmPickItem(Target, Temp))
					{
						if (const int32* DroppedByArm = RecentlyDroppedByArm.Find(Temp.Id)) { if (*DroppedByArm == ArmIndex) return false; }
						S.Held = Temp; S.bHolding = true; S.PrevHeldOrbCount = 1; SimGrabFacingByArm.Add(ArmIndex, int8(-1));
						S.PC = (S.PC + 1) % S.Tape.Num();
						return true;
					}
					return false;
				}

				// Cluster BFS
				TSet<int32> ClusterIds;
				if (Found->Id != 0 && ItemToBonds.Contains(Found->Id))
				{
					TArray<int32> Stack; Stack.Add(Found->Id); ClusterIds.Add(Found->Id);
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
								if (OtherId > 0 && !ClusterIds.Contains(OtherId)) { ClusterIds.Add(OtherId); Stack.Add(OtherId); }
							}
						}
					}
				}

				if (ClusterIds.Num() <= 1)
				{
					FItem SingleOut;
					if (TryArmPickItem(Target, SingleOut))
					{
						if (const int32* DroppedByArm = RecentlyDroppedByArm.Find(SingleOut.Id)) { if (*DroppedByArm == ArmIndex) return false; }
						S.Held = SingleOut; S.bHolding = true; S.PrevHeldOrbCount = 1; SimGrabFacingByArm.Add(ArmIndex, int8(-1));
						S.PC = (S.PC + 1) % S.Tape.Num();
						return true;
					}
					return false;
				}
				else
				{
					FItem Cluster; Cluster.bIsCluster = true; Cluster.Id = Found->Id; Cluster.GrabFacing = (int8)S.Facing;
					bool bAllFound = true;
					for (int32 ItemId : ClusterIds)
					{
						FHex ItemHex; bool bItemFound = false;
						for (const auto& KV : ItemMap) { if (KV.Value.Id == ItemId) { ItemHex = KV.Key; bItemFound = true; break; } }
						if (!bItemFound) { bAllFound = false; break; }
						const FItem* It = ItemMap.Find(ItemHex);
						if (It && !It->bIsCluster)
						{
							if (const int32* DroppedByArm = RecentlyDroppedByArm.Find(It->Id)) { if (*DroppedByArm == ArmIndex) { bAllFound = false; break; } }
							const FHex D(ItemHex.Q - Target.Q, ItemHex.R - Target.R);
							FOrbData Orb; Orb.Type = (uint8)It->Single.Type; Orb.Color = (uint8)It->Single.Color; Orb.ItemId = It->Id; Orb.dQ = (int8)D.Q; Orb.dR = (int8)D.R;
							Cluster.Orbs.Add(Orb);
						}
					}
					if (!bAllFound || Cluster.Orbs.Num() == 0) return false;

					for (const FOrbData& Sub : Cluster.Orbs)
					{
						for (auto It = ItemMap.CreateIterator(); It; ++It)
						{
							if (It.Value().Id == Sub.ItemId) { HeldItemLastHex.Add(Sub.ItemId, It.Key()); It.RemoveCurrent(); break; }
						}
					}
					S.Held = Cluster; S.bHolding = true; S.PrevHeldOrbCount = Cluster.Orbs.Num(); SimGrabFacingByArm.Add(ArmIndex, Cluster.GrabFacing);
					S.PC = (S.PC + 1) % S.Tape.Num();
					return true;
				}
			}

			case ECmd::RotateHeldL:
				if (S.bHolding && S.Held.bIsCluster)
				{
					S.bRotationCompleted = false;
					S.Held.GrabFacing = (S.Held.GrabFacing + 5) % 6;
					PendingGrabFacingByArm.Add(ArmIndex, S.Held.GrabFacing);
				}
				S.PC = (S.PC + 1) % S.Tape.Num();
				return true;

			case ECmd::RotateHeldR:
				if (S.bHolding && S.Held.bIsCluster)
				{
					S.bRotationCompleted = false;
					S.Held.GrabFacing = (S.Held.GrabFacing + 1) % 6;
					PendingGrabFacingByArm.Add(ArmIndex, S.Held.GrabFacing);
				}
				S.PC = (S.PC + 1) % S.Tape.Num();
				return true;

			case ECmd::Extend:
			{
				const int32 Delta = (Op.Arg == 0 ? 1 : Op.Arg);
				const int32 OldLen = S.ArmLen;
				S.ArmLen = FMath::Clamp<int32>(S.ArmLen + Delta, 1, MAX_ARM_LEN);
				PendingArmLenByArm.Add(ArmIndex, S.ArmLen);
				if (bDevDebugArms && GEngine && S.ArmLen != OldLen)
				{
					FHex Hand = ComputeGripHex(S);
					GEngine->AddOnScreenDebugMessage(-1, 1.0f, FColor::Green,
						FString::Printf(TEXT("[Arm %d] EXTEND (Δ=%d) Len=%d Hand=(%d,%d)"), ArmIndex, Delta, S.ArmLen, Hand.Q, Hand.R));
				}
				S.PC = (S.PC + 1) % S.Tape.Num();
				return true;
			}

			case ECmd::Retract:
			{
				const int32 Delta = (Op.Arg == 0 ? 1 : Op.Arg);
				const int32 OldLen = S.ArmLen;
				S.ArmLen = FMath::Clamp<int32>(S.ArmLen - Delta, 1, MAX_ARM_LEN);
				PendingArmLenByArm.Add(ArmIndex, S.ArmLen);
				if (bDevDebugArms && GEngine && S.ArmLen != OldLen)
				{
					FHex Hand = ComputeGripHex(S);
					GEngine->AddOnScreenDebugMessage(-1, 1.0f, FColor::Orange,
						FString::Printf(TEXT("[Arm %d] RETRACT (Δ=%d) Len=%d Hand=(%d,%d)"), ArmIndex, Delta, S.ArmLen, Hand.Q, Hand.R));
				}
				S.PC = (S.PC + 1) % S.Tape.Num();
				return true;
			}

			case ECmd::RotateL:
			case ECmd::RotateR:
			{
				// Collision-aware rotate (same checks as StepArms)
				S.bRotationCompleted = false;

				// If previously blocked and waiting, consume the wait and retry next tick
				if (S.WaitCounter > 0)
				{
					--S.WaitCounter;
					return false; // don't advance PC here
				}

				EHexDir NewFacing = (Op.Cmd == ECmd::RotateL)
					? (EHexDir)(((uint8)S.Facing + 1) % 6)
					: (EHexDir)(((uint8)S.Facing + 5) % 6);

				// Hand positions before/after
				FHex OldHand = ComputeGripHex(S);
				EHexDir OldFacing = S.Facing;
				S.Facing = NewFacing;
				FHex NewHand = ComputeGripHex(S);
				S.Facing = OldFacing; // restore for now

				// Occupancy: world items
				TSet<FHex> WorldOcc;
				for (const auto& KV : ItemMap)
					WorldOcc.Add(KV.Key);

				// Other arms held occupancy (hands + virtual orbs)
				TSet<FHex> OtherHeldOcc;
				for (int32 OtherIdx = 0; OtherIdx < Arms.Num(); ++OtherIdx)
				{
					if (OtherIdx == ArmIndex) continue;
					const FArmState& O = Arms[OtherIdx];
					if (!O.bHolding) continue;

					const FHex OHand = ComputeGripHex(O);
					OtherHeldOcc.Add(OHand);

					if (O.Held.bIsCluster)
					{
						const uint8 cur = (uint8)O.Facing % 6;
						const int8 grab = (int8)O.Held.GrabFacing;
						const uint8 facingDelta = (grab >= 0)
							? (uint8)(((int32)cur - (int32)(grab % 6) + 6) % 6)
							: cur;
						const uint8 ccw = (uint8)((6 - facingDelta) % 6);
						for (const FOrbData& Sub : O.Held.Orbs)
						{
							FHex Local(Sub.dQ, Sub.dR);
							FHex RotLocal = RotateAxial60Fn(Local, ccw);
							OtherHeldOcc.Add(FHex(OHand.Q + RotLocal.Q, OHand.R + RotLocal.R));
						}
					}
					else
					{
						OtherHeldOcc.Add(OHand);
					}
				}

				// Build self locals for start and end, plus mid phases
				auto BuildLocals = [&](EHexDir Facing, TArray<FHex>& OutLocals)->void
					{
						OutLocals.Reset();
						if (!S.bHolding) return;

						// Grip at this facing
						const EHexDir Saved = S.Facing;
						S.Facing = Facing;
						FHex Grip = ComputeGripHex(S);
						S.Facing = Saved;

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
							FHex RotLocal = RotateAxial60Fn(Local, ccw);
							OutLocals.Add(FHex(Grip.Q + RotLocal.Q, Grip.R + RotLocal.R));
						}
					};

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

				TArray<FHex> StartLocals, EndLocals, MidTranslateLocals, MidRotateLocals;
				BuildLocals(S.Facing, StartLocals);
				BuildLocals(NewFacing, EndLocals);

				// Mid A: translate first (old orientation at new hand)
				if (S.bHolding)
				{
					if (!S.Held.bIsCluster) { MidTranslateLocals.Add(NewHand); }
					else
					{
						const uint8 curOld = (uint8)S.Facing % 6;
						const int8 grab = (int8)S.Held.GrabFacing;
						const uint8 facingDeltaOld = (grab >= 0)
							? (uint8)(((int32)curOld - (int32)(grab % 6) + 6) % 6)
							: curOld;
						const uint8 ccwOld = (uint8)((6 - facingDeltaOld) % 6);
						for (const FOrbData& Sub : S.Held.Orbs)
						{
							FHex Local(Sub.dQ, Sub.dR);
							FHex RotLocal = RotateAxial60Fn(Local, ccwOld);
							MidTranslateLocals.Add(FHex(NewHand.Q + RotLocal.Q, NewHand.R + RotLocal.R));
						}
					}
				}

				// Mid B: rotate first (new orientation at old hand)
				if (S.bHolding)
				{
					if (!S.Held.bIsCluster) { MidRotateLocals.Add(OldHand); }
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
							FHex RotLocal = RotateAxial60Fn(Local, ccwNew);
							MidRotateLocals.Add(FHex(OldHand.Q + RotLocal.Q, OldHand.R + RotLocal.R));
						}
					}
				}

				bool bBlocked = false;

				// 1) Destination hand cannot contain foreign item/held orb if holding
				if (S.bHolding && OverlapsAny(NewHand)) bBlocked = true;

				// 2) End locals
				if (!bBlocked)
				{
					for (const FHex& H : EndLocals) { if (OverlapsAny(H)) { bBlocked = true; break; } }
				}

				// 3) Mid translate
				if (!bBlocked)
				{
					for (const FHex& H : MidTranslateLocals) { if (OverlapsAny(H)) { bBlocked = true; break; } }
				}

				// 4) Mid rotate
				if (!bBlocked)
				{
					for (const FHex& H : MidRotateLocals) { if (OverlapsAny(H)) { bBlocked = true; break; } }
				}

				if (bBlocked)
				{
					if (S.WaitCounter == 0) S.WaitCounter = 1;
					return false; // keep PC (retry next tick)
				}

				// Commit visual/current rotation immediately; sim-facing updates next tick
				S.Facing = NewFacing;
				PendingFacingByArm.Add(ArmIndex, static_cast<uint8>(S.Facing));
				S.PC = (S.PC + 1) % S.Tape.Num();
				return true;
			}

			default:
				return false;
			}
		};

	// NEW: finalize WaitForBond in the same tick (after StepBonds ran) and execute next op(s) immediately
	for (int32 ArmIndex = 0; ArmIndex < Arms.Num(); ArmIndex++)
	{
		FArmState& S = Arms[ArmIndex];
		if (S.Tape.Num() == 0) continue;

		if (S.PC < 0 || S.PC >= S.Tape.Num()) continue;
		const FOp& Op = S.Tape[S.PC];
		if (Op.Cmd != ECmd::WaitForBond) continue;
		if (!S.bHolding) continue;

		const int32 CurrentCount = S.Held.bIsCluster ? S.Held.Orbs.Num() : 1;

		// If bonds increased count this tick, clear wait and advance now
		if (S.bWaitingForBond && CurrentCount > S.WaitForBondInitialCount)
		{
			S.bWaitingForBond = false;
			S.WaitForBondInitialCount = 0;

			// Advance past WaitForBond
			S.PC = (S.PC + 1) % S.Tape.Num();

			// Execute up to 3 fast ops immediately (Rotate/Extend/RotateHeld/Grab/Drop)
			int executed = 0;
			const int kMaxFast = 1;
			while (executed < kMaxFast)
			{
				const int32 PrevPC = S.PC;
				if (!ExecFastOpOnce(ArmIndex)) break;
				++executed;
				// Safety: stop if PC didn't advance
				if (S.PC == PrevPC) break;
			}
		}
	}
}