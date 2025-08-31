#include "Sim/SimWorldHex.h"
#include "Engine/Engine.h"

void USimWorldHex::Initialize(int32 /*Radius*/)
{
	TileMap.Empty();
	ItemMap.Empty();
	Arms.Empty();
	OutputCount = 0;
	TileRevision = 0;
	NextItemId = 1;
	OrbAt.Empty();
}

bool USimWorldHex::PlaceTile(const FHex& H, const FTile& T)
{
	if (T.Kind == ETileKind::Empty)
	{
		const bool bHad = TileMap.Remove(H) > 0;
		if (bHad)
		{
			ItemMap.Remove(H);
			OrbAt.Remove(H);
			++TileRevision;
		}
		return true;
	}
	TileMap.Add(H, T);
	++TileRevision;
	return true;
}

int32 USimWorldHex::AddArm(const FArmState& Arm)
{
	return Arms.Add(Arm);
}

// ---- Item helpers that also keep the Orb overlay in sync ----

bool USimWorldHex::TryPutItem(const FHex& H, const FItem& I)
{
	if (ItemMap.Contains(H)) return false;
	ItemMap.Add(H, I);
	OrbAt.Add(H, KindFromItem(I));
	return true;
}

bool USimWorldHex::TryTakeItem(const FHex& H, FItem& Out)
{
	if (FItem* Found = ItemMap.Find(H))
	{
		Out = *Found;
		ItemMap.Remove(H);
		OrbAt.Remove(H);
		return true;
	}
	return false;
}

// ---- Sim tick ----

void USimWorldHex::StepSim()
{
	UE_LOG(LogTemp, Warning, TEXT("StepSim called"));
	SkipInputThisFrame.Reset();
	SkipOutputThisFrame.Reset();

	StepArms();
	StepBelts();
	StepGlyphs();
}

void USimWorldHex::StepArms()
{
	//UE_LOG(LogTemp, Warning, TEXT("StepArms called, NumArms=%d"), Arms.Num());
	for (int32 ArmIndex = 0; ArmIndex < Arms.Num(); ++ArmIndex)
	{
		FArmState& S = Arms[ArmIndex];
		//UE_LOG(LogTemp, Warning, TEXT("Arm %d Tape Length: %d"), ArmIndex, S.Tape.Num());
		//for (int32 i = 0; i < S.Tape.Num(); ++i)
		//{
		//	UE_LOG(LogTemp, Warning, TEXT("  Tape[%d] Cmd=%d Arg=%d"), i, (int32)S.Tape[i].Cmd, S.Tape[i].Arg);
		//}

		if (S.Tape.Num() == 0) continue;
		const FOp& Op = S.Tape[S.PC];
		switch (Op.Cmd)
		{
		case ECmd::Wait:
		{
			if (S.WaitCounter == 0) { S.WaitCounter = (Op.Arg == 0 ? 1 : Op.Arg); }
			S.WaitCounter--;
			if (S.WaitCounter > 0) { goto AdvancePC; }
			S.WaitCounter = 0;
		}
		break;

		case ECmd::Grab:
		{
			UE_LOG(LogTemp, Log, TEXT("Grab"));
			if (!S.bHolding)
			{
				FHex Target = S.BaseHex;
				for (int i = 0; i < S.ArmLen; ++i) Target = HexUtil::Neighbor(Target, S.Facing);

				FItem Temp;
				bool bItemPresent = ItemMap.Contains(Target);
				UE_LOG(LogTemp, Log, TEXT("Arm %d attempting grab at (%d,%d). Item present: %s"), ArmIndex, Target.Q, Target.R, bItemPresent ? TEXT("YES") : TEXT("NO"));
				if (TryArmPickItem(Target, Temp))
				{
					S.Held = Temp;
					S.bHolding = true;
					SkipInputThisFrame.Add(Target);
					UE_LOG(LogTemp, Log, TEXT("Arm %d grabbed orb Id=%d at (%d,%d)"), ArmIndex, Temp.Id, Target.Q, Target.R);
				}
			}
		}
		break;

		case ECmd::Drop:
		{
			if (S.bHolding)
			{
				FHex Target = S.BaseHex;
				for (int i = 0; i < S.ArmLen; ++i) Target = HexUtil::Neighbor(Target, S.Facing);

				// Arm bridge (consumes on Output; places otherwise) + overlay sync
				if (TryArmPlaceItem(Target, S.Held))
				{
					S.bHolding = false;
					S.Held = FItem();
					SkipOutputThisFrame.Add(Target);
				}
			}
		}
		break;

		case ECmd::RotateL: S.Facing = (EHexDir)(((uint8)S.Facing + 1) % 6); break; // CCW
		case ECmd::RotateR: S.Facing = (EHexDir)(((uint8)S.Facing + 5) % 6); break; // CW
		case ECmd::Extend:  S.ArmLen = FMath::Clamp<int32>(S.ArmLen + 1, 1, 3); break;
		case ECmd::Retract: S.ArmLen = FMath::Clamp<int32>(S.ArmLen - 1, 1, 3); break;
		case ECmd::MovePos: S.TrackIdx = FMath::Clamp<int32>(S.TrackIdx + 1, -4, 4); break;
		case ECmd::MoveNeg: S.TrackIdx = FMath::Clamp<int32>(S.TrackIdx - 1, -4, 4); break;
		case ECmd::PivotL:  break;
		case ECmd::PivotR:  break;
		case ECmd::Reset:
			S.Facing = S.HomeFacing; S.ArmLen = S.HomeArmLen; S.TrackIdx = S.HomeTrackIdx; break;
		case ECmd::Repeat:  break;
		}
	AdvancePC:
		S.PC = (S.PC + 1) % S.Tape.Num();
	}
}

void USimWorldHex::StepBelts()
{
	TArray<TPair<FHex, FHex>> Moves;      // from -> to
	TSet<FHex> DestReserved;             // destinations already claimed (e.g., pulls)
	auto Opp = [](EHexDir D) { return (EHexDir)(((uint8)D + 3) % 6); };

	// A) NEW: belts can pull from an Input that sits "behind" them IF they face the opposite of the Input's facing.
	for (const auto& KV : TileMap)
	{
		const FHex H = KV.Key;
		const FTile& T = KV.Value;
		if (T.Kind != ETileKind::Belt) continue;

		const EHexDir BeltDir = (EHexDir)(T.Arg & 0xFF);
		const FHex Behind = HexUtil::Neighbor(H, Opp(BeltDir));

		if (const FTile* TBehind = TileMap.Find(Behind))
		{
			if (TBehind->Kind == ETileKind::Input)
			{
				const EHexDir InputDir = (EHexDir)(TBehind->Arg & 0xFF);
				// Only pull if the belt is going in the opposite direction of the Input tile
				if (Opp(BeltDir) == InputDir)
				{
					const bool bInputHasItem = ItemMap.Contains(Behind);
					const bool bBeltCellEmpty = !ItemMap.Contains(H);
					const bool bDestFree = !DestReserved.Contains(H);

					if (bInputHasItem && bBeltCellEmpty && bDestFree)
					{
						Moves.Add(TPair<FHex, FHex>(Behind, H)); // move: Input -> Belt
						DestReserved.Add(H);

						// NOTE: Do NOT mark SkipInputThisFrame for 'Behind' here.
						// StepGlyphs() runs after belts and should be allowed to spawn
						// a fresh orb on the now-empty Input next frame.
					}
				}
			}
		}
	}

	// B) Existing: forward pushes (belt tile -> beltDir). Respect any destinations already reserved by pulls.
	for (const auto& KV : TileMap)
	{
		const FHex H = KV.Key;
		const FTile& T = KV.Value;
		if (T.Kind != ETileKind::Belt) continue;

		const EHexDir BeltDir = (EHexDir)(T.Arg & 0xFF);
		const FHex To = HexUtil::Neighbor(H, BeltDir);

		if (!ItemMap.Contains(H))        continue;         // nothing to move
		if (ItemMap.Contains(To))        continue;         // target occupied
		if (DestReserved.Contains(To))   continue;         // already taken by a pull this step

		Moves.Add(TPair<FHex, FHex>(H, To));                // move: Belt -> Forward

		// If we push into an Input, prevent that Input from also spawning this frame.
		if (const FTile* TTo = TileMap.Find(To))
		{
			if (TTo->Kind == ETileKind::Input)
			{
				SkipInputThisFrame.Add(To);
			}
		}
	}

	// C) Resolve conflicts (unique destinations), then apply
	TSet<FHex> DestUsed = DestReserved; // pulls already claimed these
	TArray<TPair<FHex, FHex>> Winners;
	Winners.Reserve(Moves.Num());

	for (const TPair<FHex, FHex>& M : Moves)
	{
		const FHex& From = M.Key;
		const FHex& To = M.Value;
		if (!DestUsed.Contains(To) && !ItemMap.Contains(To))
		{
			Winners.Add(M);
			DestUsed.Add(To);
		}
	}

	for (const TPair<FHex, FHex>& M : Winners)
	{
		FItem Item = ItemMap.FindRef(M.Key);
		ItemMap.Remove(M.Key);
		ItemMap.Add(M.Value, Item);
	}
}


void USimWorldHex::StepGlyphs()
{
	for (const auto& KV : TileMap)
	{
		const FHex& H = KV.Key;
		const FTile& T = KV.Value;
		switch (T.Kind)
		{
		case ETileKind::Input:
		{
			if (!ItemMap.Contains(H) && !SkipInputThisFrame.Contains(H))
			{
				FOrb O; O.Type = EOrbType::BaseA;
				FItem NewItem = FItem::MakeSingle(O);
				NewItem.Id = NextItemId++;
				// Use helper so OrbAt stays synced
				TryPutItem(H, NewItem);
			}
		}
		break;
		case ETileKind::Output:
		{
			if (ItemMap.Contains(H) && !SkipOutputThisFrame.Contains(H))
			{
				FItem Discard;
				TryTakeItem(H, Discard); // also clears OrbAt
				++OutputCount;
			}
		}
		break;
		default: break;
		}
	}
}

// ---------- Arm <-> Item bridge with JIT Input/Output handling ----------

bool USimWorldHex::TryArmPickItem(const FHex& From, FItem& OutItem)
{
	if (TryTakeItem(From, OutItem))
	{
		UE_LOG(LogTemp, Log, TEXT("TryArmPickItem: Took item Id=%d from (%d,%d)"), OutItem.Id, From.Q, From.R);
		return true;
	}
	if (const FTile* T = TileMap.Find(From))
	{
		if (T->Kind == ETileKind::Input)
		{
			FOrb O; O.Type = EOrbType::BaseA;
			FItem NewItem = FItem::MakeSingle(O);
			NewItem.Id = NextItemId++;
			OutItem = NewItem;
			UE_LOG(LogTemp, Log, TEXT("TryArmPickItem: Synthesized input orb Id=%d at (%d,%d)"), OutItem.Id, From.Q, From.R);
			return true;
		}
	}
	UE_LOG(LogTemp, Log, TEXT("TryArmPickItem: Nothing to grab at (%d,%d)"), From.Q, From.R);
	return false;
}

bool USimWorldHex::TryArmPlaceItem(const FHex& To, const FItem& InItem)
{
	// 1) If this is an OUTPUT tile, consume immediately (no placement; ensure overlay is clear)
	if (const FTile* T = TileMap.Find(To))
	{
		if (T->Kind == ETileKind::Output)
		{
			OrbAt.Remove(To);
			++OutputCount;
			return true; // consumed
		}
	}

	// 2) Otherwise attempt to place if empty (adds OrbAt on success)
	return TryPutItem(To, InItem);
}

// ---------- Small read-only helpers ----------

bool USimWorldHex::PeekItemAt(const FHex& H, FItem& OutItem) const
{
	if (const FItem* Found = ItemMap.Find(H)) { OutItem = *Found; return true; }
	return false;
}

// ---------- Legacy/simple Orb presence helpers (used by UI) ----------

bool USimWorldHex::TryPlaceOrbAt(const FHex& Hex, uint8 Kind)
{
	if (OrbAt.Contains(Hex)) return false;
	OrbAt.Add(Hex, Kind);
	return true;
}

bool USimWorldHex::TryPickupOrbAt(const FHex& Hex, uint8& OutKind)
{
	if (uint8* K = OrbAt.Find(Hex))
	{
		OutKind = *K;
		OrbAt.Remove(Hex);
		return true;
	}
	return false;
}

bool USimWorldHex::HasOrbAt(const FHex& Hex) const
{
	return OrbAt.Contains(Hex);
}

bool USimWorldHex::GetOrbKindAt(const FHex& Hex, uint8& OutKind) const
{
	if (const uint8* K = OrbAt.Find(Hex)) { OutKind = *K; return true; }
	return false;
}
