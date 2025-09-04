#include "Sim/SimWorldHex.h"
#include "Engine/Engine.h"
#include "Sim/HexCoord.h"        // <-- ADD THIS so EHexOrientation is defined

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
	if (T.Kind == ETileKind::Bond)
	{
		const FHex Partner = HexUtil::Neighbor(H, (EHexDir)(T.Arg % 6));
		if (TileMap.Contains(Partner) && TileMap[Partner].Kind != ETileKind::Empty && TileMap[Partner].Kind != ETileKind::Bond)
		{
			UE_LOG(LogTemp, Warning, TEXT("Bond placement blocked: partner hex occupied."));
			return false;
		}
	}
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
	int32 Index = Arms.Num();
	FArmState ArmCopy = Arm;
	ArmCopy.ArmIndex = Index;
	Arms.Add(ArmCopy);
	return Index;
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
	++StepCounter;               // new fixed simulation tick
	SkipInputThisFrame.Reset();
	SkipOutputThisFrame.Reset();

	StepArms();
	StepBelts();
	StepGlyphs();
}

void USimWorldHex::StepArms()
{
	for (int32 ArmIndex = 0; ArmIndex < Arms.Num(); ++ArmIndex)
	{
		FArmState& S = Arms[ArmIndex];
		UE_LOG(LogTemp, Verbose, TEXT("Sim Pre Arm%d PC=%d Holding=%d HeldId=%d TapeLen=%d"),
			ArmIndex, S.PC, S.bHolding ? 1 : 0, S.Held.Id, S.Tape.Num());

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

				if (TryArmPlaceItem(Target, S.Held))
				{
					S.bHolding = false;
					S.Held = FItem();
					SkipOutputThisFrame.Add(Target);
				}
			}
		}
		break;

		case ECmd::RotateL:
		case ECmd::RotateR:
		{
			if (S.bHolding)
			{
				EHexDir NewFacing = S.Facing;
				if (Op.Cmd == ECmd::RotateL)
					NewFacing = (EHexDir)(((uint8)S.Facing + 1) % 6);
				else
					NewFacing = (EHexDir)(((uint8)S.Facing + 5) % 6);

				FHex Target = S.BaseHex;
				for (int i = 0; i < S.ArmLen; ++i)
					Target = HexUtil::Neighbor(Target, NewFacing);

				if (ItemMap.Contains(Target))
				{
					S.WaitCounter = 1;
					goto AdvancePC;
				}
			}

			if (Op.Cmd == ECmd::RotateL)
				S.Facing = (EHexDir)(((uint8)S.Facing + 1) % 6);
			else
				S.Facing = (EHexDir)(((uint8)S.Facing + 5) % 6);
		}
		break;

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
		UE_LOG(LogTemp, Verbose, TEXT("Sim Post Op Arm%d Cmd=%d Holding=%d HeldId=%d"),
			ArmIndex, (int32)Op.Cmd, S.bHolding ? 1 : 0, S.Held.Id);
	AdvancePC:
		S.PC = (S.PC + 1) % S.Tape.Num();
	}
}

void USimWorldHex::StepBelts()
{
	// Early outs
	if (TileMap.Num() == 0 || ItemMap.Num() == 0) return;

	const auto Opp = [](EHexDir D) { return (EHexDir)(((uint8)D + 3) % 6); };
	const float Speed = FMath::Max(0.f, BeltSpeedTilesPerTick);
	const float BlockClamp = BeltBlockedClamp;

	/* -------- Maintain BeltProgress -------- */
	TSet<int32> LiveIds;
	for (const auto& KV : ItemMap)
	{
		const FHex& H = KV.Key;
		const FItem& It = KV.Value;
		const FTile* T = TileMap.Find(H);
		if (T && T->Kind == ETileKind::Belt && It.Id != 0)
		{
			LiveIds.Add(It.Id);
			if (!BeltProgress.Contains(It.Id))
				BeltProgress.Add(It.Id, 0.f);
		}
	}
	for (auto It = BeltProgress.CreateIterator(); It; ++It)
	{
		if (!LiveIds.Contains(It.Key()))
			It.RemoveCurrent();
	}

	/* -------- Snapshot occupancy & collect belt items -------- */
	TSet<FHex> Occupied;
	Occupied.Reserve(ItemMap.Num());
	for (const auto& KV : ItemMap) Occupied.Add(KV.Key);

	struct FBeltItemRef { int32 ItemId; FHex Hex; };
	TArray<FBeltItemRef> BeltItems;
	BeltItems.Reserve(BeltProgress.Num());

	for (const auto& KV : ItemMap)
	{
		const FItem& Item = KV.Value;
		if (Item.Id == 0) continue;
		if (!BeltProgress.Contains(Item.Id)) continue;
		BeltItems.Add({ Item.Id, KV.Key });
	}

	/* -------- First pass: advance progress (no scheduling yet) -------- */
	for (FBeltItemRef& Ref : BeltItems)
	{
		float& P = BeltProgress[Ref.ItemId];
		P += Speed;
	}

	/* -------- Bond pass (before singles) -------- */
	struct FForwardTry
	{
		int32 ItemId;
		FHex  From;
		FHex  To;
		uint8 DestDir;
	};
	TArray<FForwardTry> BondMoves;
	TSet<int32> BondItemsScheduled;

	if (Bonds.Num() > 0)
	{
		// Build quick item->hex map
		TMap<int32, FHex> ItemHex;
		ItemHex.Reserve(ItemMap.Num());
		for (const auto& KV : ItemMap)
			if (KV.Value.Id != 0) ItemHex.Add(KV.Value.Id, KV.Key);

		for (const FBond& B : Bonds)
		{
			const FHex* HexA = ItemHex.Find(B.ItemA);
			const FHex* HexB = ItemHex.Find(B.ItemB);
			if (!HexA || !HexB) continue;

			const FTile* TA = TileMap.Find(*HexA);
			const FTile* TB = TileMap.Find(*HexB);
			if (!(TA && TB && TA->Kind == ETileKind::Belt && TB->Kind == ETileKind::Belt)) continue;

			float* PA = BeltProgress.Find(B.ItemA);
			float* PB = BeltProgress.Find(B.ItemB);
			if (!PA || !PB) continue;

			const bool ReadyA = *PA >= 1.f;
			const bool ReadyB = *PB >= 1.f;
			if (!(ReadyA && ReadyB)) continue;

			const uint8 DirA = (uint8)(TA->Arg & 0xFF);
			const uint8 DirB = (uint8)(TB->Arg & 0xFF);
			const FHex ToA = HexUtil::Neighbor(*HexA, (EHexDir)(DirA % 6));
			const FHex ToB = HexUtil::Neighbor(*HexB, (EHexDir)(DirB % 6));

			// Maintain original relative offset after hop
			if ((ToB.Q - ToA.Q) != B.dQ || (ToB.R - ToA.R) != B.dR)
			{
				*PA = FMath::Min(*PA, BlockClamp);
				*PB = FMath::Min(*PB, BlockClamp);
				continue;
			}

			// Destination free?
			if (ItemMap.Contains(ToA) || ItemMap.Contains(ToB) ||
				Occupied.Contains(ToA) || Occupied.Contains(ToB))
			{
				*PA = FMath::Min(*PA, BlockClamp);
				*PB = FMath::Min(*PB, BlockClamp);
				continue;
			}

			// Reserve
			Occupied.Remove(*HexA); Occupied.Remove(*HexB);
			Occupied.Add(ToA);       Occupied.Add(ToB);

			// Determine next dir if still on belt
			uint8 DestDirA = DirA;
			if (const FTile* TDestA = TileMap.Find(ToA); TDestA && TDestA->Kind == ETileKind::Belt)
				DestDirA = (uint8)(TDestA->Arg & 0xFF);
			uint8 DestDirB = DirB;
			if (const FTile* TDestB = TileMap.Find(ToB); TDestB && TDestB->Kind == ETileKind::Belt)
				DestDirB = (uint8)(TDestB->Arg & 0xFF);

			BondMoves.Add({ B.ItemA, *HexA, ToA, DestDirA });
			BondMoves.Add({ B.ItemB, *HexB, ToB, DestDirB });
			BondItemsScheduled.Add(B.ItemA);
			BondItemsScheduled.Add(B.ItemB);

			*PA -= 1.f; if (*PA < 0.f) *PA = 0.f;
			*PB -= 1.f; if (*PB < 0.f) *PB = 0.f;
		}
	}

	/* -------- Single item forward scheduling (excluding bond items) -------- */
	TArray<FForwardTry> ForwardTries;
	ForwardTries.Reserve(BeltItems.Num());

	for (const FBeltItemRef& Ref : BeltItems)
	{
		if (BondItemsScheduled.Contains(Ref.ItemId)) continue;

		float& P = BeltProgress[Ref.ItemId];
		// Only attempt one hop per tick at current speed design
		if (P < 1.f) continue;

		const FTile* TFrom = TileMap.Find(Ref.Hex);
		if (!TFrom || TFrom->Kind != ETileKind::Belt) { P = 0.f; continue; }

		const uint8 Dir = (uint8)(TFrom->Arg & 0xFF);
		const FHex To = HexUtil::Neighbor(Ref.Hex, (EHexDir)(Dir % 6));

		if (Occupied.Contains(To) || ItemMap.Contains(To))
		{
			P = FMath::Min(P, BlockClamp);
			continue;
		}

		uint8 DestDir = Dir;
		if (const FTile* TDest = TileMap.Find(To))
		{
			if (TDest->Kind == ETileKind::Belt)
				DestDir = (uint8)(TDest->Arg & 0xFF);
		}

		ForwardTries.Add({ Ref.ItemId, Ref.Hex, To, DestDir });

		Occupied.Remove(Ref.Hex);
		Occupied.Add(To);
		P -= 1.f;
	}

	/* -------- Conflict resolution for single moves -------- */
	TMap<FHex, int32> DestWinnerIndex;
	for (int32 i = 0; i < ForwardTries.Num(); ++i)
	{
		const FHex& D = ForwardTries[i].To;
		if (!DestWinnerIndex.Contains(D))
		{
			DestWinnerIndex.Add(D, i);
		}
		else
		{
			const int32 Existing = DestWinnerIndex[D];
			const FForwardTry& A = ForwardTries[Existing];
			const FForwardTry& B = ForwardTries[i];
			// Deterministic tie-break: lower (Q,R)
			if (B.From.Q < A.From.Q || (B.From.Q == A.From.Q && B.From.R < A.From.R))
				DestWinnerIndex[D] = i;
		}
	}

	TSet<int32> ItemsMovedForward;
	TArray<FForwardTry> Moves;
	Moves.Reserve(DestWinnerIndex.Num() + BondMoves.Num());
	for (const auto& Pair : DestWinnerIndex)
	{
		Moves.Add(ForwardTries[Pair.Value]);
		ItemsMovedForward.Add(ForwardTries[Pair.Value].ItemId);
	}
	// Append bond moves (already non-conflicting)
	for (const FForwardTry& BM : BondMoves)
	{
		Moves.Add(BM);
		ItemsMovedForward.Add(BM.ItemId);
	}

	/* -------- Apply forward moves -------- */
	if (Moves.Num())
	{
		TMap<FHex, FItem> Original = ItemMap;
		for (const FForwardTry& M : Moves)
		{
			const FItem* Found = Original.Find(M.From);
			if (!Found || Found->Id != M.ItemId) continue;
			if (ItemMap.Contains(M.To)) continue;

			ItemMap.Remove(M.From);
			ItemMap.Add(M.To, *Found);

			OrbAt.Remove(M.From);
			OrbAt.Add(M.To, KindFromItem(*Found));

			// If destination not belt, clear progress
			if (const FTile* TD = TileMap.Find(M.To))
			{
				if (TD->Kind != ETileKind::Belt)
					BeltProgress.Remove(M.ItemId);
				else
				{
					// keep residual already set
				}

				if (TD->Kind == ETileKind::Input)
					SkipInputThisFrame.Add(M.To);
			}
			else
			{
				BeltProgress.Remove(M.ItemId);
			}
		}
	}

	/* -------- Pull pass (unchanged except bond awareness via ItemsMovedForward) -------- */
	TArray<TPair<FHex, FHex>> Pulls;
	Pulls.Reserve(32);

	for (const auto& KV : TileMap)
	{
		const FHex BeltHex = KV.Key;
		const FTile& Tile = KV.Value;
		if (Tile.Kind != ETileKind::Belt) continue;
		if (ItemMap.Contains(BeltHex)) continue;

		const uint8 Dir = (uint8)(Tile.Arg & 0xFF);
		const FHex Behind = HexUtil::Neighbor(BeltHex, Opp((EHexDir)(Dir % 6)));

		FItem* BehindItem = ItemMap.Find(Behind);
		if (!BehindItem) continue;
		if (ItemsMovedForward.Contains(BehindItem->Id)) continue;

		if (const FTile* TBehind = TileMap.Find(Behind))
		{
			if (TBehind->Kind == ETileKind::Output) continue;
			if (TBehind->Kind == ETileKind::Belt) continue; // no belt->belt pull
		}

		Pulls.Add({ Behind, BeltHex });
	}

	for (const TPair<FHex, FHex>& P : Pulls)
	{
		FItem Item;
		if (!TryTakeItem(P.Key, Item)) continue;
		ItemMap.Add(P.Value, Item);
		OrbAt.Add(P.Value, KindFromItem(Item));
		BeltProgress.Add(Item.Id, 0.f);
	}
}

void USimWorldHex::StepGlyphs()
{
	// Only skip if there are literally no tiles. (Do NOT early-out on empty ItemMap.)
	if (TileMap.Num() == 0) return;

	// Spawn on Input tiles
	for (const auto& KV : TileMap)
	{
		const FHex& H = KV.Key;
		const FTile& T = KV.Value;
		if (T.Kind != ETileKind::Input) continue;
		if (SkipInputThisFrame.Contains(H)) continue;
		if (ItemMap.Contains(H)) continue;

		FOrb O;
		O.Type = (EOrbType)((uint8)EOrbType::BaseA + (T.Arg % 4)); // simple variant
		FItem NewItem = FItem::MakeSingle(O);
		NewItem.Id = NextItemId++;
		TryPutItem(H, NewItem); // keeps OrbAt in sync
	}

	// Consume on Output tiles
	for (const auto& KV : TileMap)
	{
		const FHex& H = KV.Key;
		const FTile& T = KV.Value;
		if (T.Kind != ETileKind::Output) continue;
		if (SkipOutputThisFrame.Contains(H)) continue;

		FItem Temp;
		if (TryTakeItem(H, Temp))
		{
			++OutputCount;
		}
	}
}

// ---------- Arm <-> Item bridge with JIT Input/Output handling (restored) ----------

bool USimWorldHex::TryArmPickItem(const FHex& From, FItem& OutItem)
{
	// Direct take if item present
	if (TryTakeItem(From, OutItem))
	{
		UE_LOG(LogTemp, Verbose, TEXT("TryArmPickItem: Took item Id=%d from (%d,%d)"), OutItem.Id, From.Q, From.R);
		return true;
	}

	// JIT spawn from Input tile
	if (const FTile* T = TileMap.Find(From))
	{
		if (T->Kind == ETileKind::Input)
		{
			FOrb O; O.Type = (EOrbType)((uint8)EOrbType::BaseA + (T->Arg % 4));
			FItem NewItem = FItem::MakeSingle(O);
			NewItem.Id = NextItemId++;
			OutItem = NewItem;
			UE_LOG(LogTemp, Verbose, TEXT("TryArmPickItem: Synthesized input orb Id=%d at (%d,%d)"), OutItem.Id, From.Q, From.R);
			return true;
		}
	}

	UE_LOG(LogTemp, Verbose, TEXT("TryArmPickItem: Nothing at (%d,%d)"), From.Q, From.R);
	return false;
}

bool USimWorldHex::TryArmPlaceItem(const FHex& To, const FItem& InItem)
{
	// Output tile acts as sink
	if (const FTile* T = TileMap.Find(To))
	{
		if (T->Kind == ETileKind::Output)
		{
			// Count & ensure overlay cleared
			OrbAt.Remove(To);
			++OutputCount;
			return true; // consumed
		}
	}

	return TryPutItem(To, InItem);
}

bool USimWorldHex::PeekItemAt(const FHex& H, FItem& OutItem) const
{
	if (const FItem* Found = ItemMap.Find(H))
	{
		OutItem = *Found;
		return true;
	}
	return false;
}

// ---------- Orb overlay helpers (UI only) ----------

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
	if (const uint8* K = OrbAt.Find(Hex))
	{
		OutKind = *K;
		return true;
	}
	return false;
}
