#include "Sim/SimWorldHex.h"
#include "Engine/Engine.h"

void USimWorldHex::Initialize(int32 /*Radius*/)
{
    TileMap.Empty(); ItemMap.Empty(); Arms.Empty(); OutputCount = 0;
    TileRevision = 0;
    NextItemId = 1;
}

bool USimWorldHex::PlaceTile(const FHex& H, const FTile& T)
{
    if (T.Kind == ETileKind::Empty)
    {
        bool bHad = TileMap.Remove(H) > 0;
        if (bHad) { ItemMap.Remove(H); ++TileRevision; }
        return true;
    }
    // Overwrite/insert
    TileMap.Add(H, T);
    ++TileRevision;
    return true;
}

int32 USimWorldHex::AddArm(const FArmState& Arm)
{
    return Arms.Add(Arm);
}

bool USimWorldHex::TryPutItem(const FHex& H, const FItem& I)
{
    if (ItemMap.Contains(H)) return false;
    ItemMap.Add(H, I);
    return true;
}

bool USimWorldHex::TryTakeItem(const FHex& H, FItem& Out)
{
    if (FItem* Found = ItemMap.Find(H)) { Out = *Found; ItemMap.Remove(H); return true; }
    return false;
}

void USimWorldHex::StepSim()
{
    SkipInputThisFrame.Reset();
    SkipOutputThisFrame.Reset();

    StepArms();
    StepBelts();   // move items before glyphs fire
    StepGlyphs();
}

void USimWorldHex::StepArms()
{
    for (FArmState& S : Arms)
    {
        if (S.Tape.Num()==0) continue;
        const FOp& Op = S.Tape[S.PC];
        switch(Op.Cmd)
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
                    for (int i=0;i<S.ArmLen;++i) Target = HexUtil::Neighbor(Target, S.Facing);
                    FItem Temp; if (TryTakeItem(Target, Temp)) { S.Held = Temp; S.bHolding = true; SkipInputThisFrame.Add(Target); }
                }
            }
            break;

            case ECmd::Drop:
            {
                if (S.bHolding)
                {
                    FHex Target = S.BaseHex;
                    for (int i=0;i<S.ArmLen;++i) Target = HexUtil::Neighbor(Target, S.Facing);
                    if (TryPutItem(Target, S.Held)) { S.bHolding=false; S.Held = FItem(); SkipOutputThisFrame.Add(Target); }
                }
            }
            break;

            case ECmd::RotateL: S.Facing = (EHexDir)(((uint8)S.Facing + 5) % 6); break;
            case ECmd::RotateR: S.Facing = (EHexDir)(((uint8)S.Facing + 1) % 6); break;
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
    // Compute desired moves based on the current state (no in-place moves).
    TArray<TPair<FHex, FHex>> Moves;
    TSet<FHex> DestClaims;

    for (const TPair<FHex, FTile>& KV : TileMap)
    {
        const FHex& Src = KV.Key;
        const FTile& T  = KV.Value;
        if (T.Kind != ETileKind::Belt) continue;
        if (!ItemMap.Contains(Src)) continue;

        const EHexDir Dir = (EHexDir)(T.Arg % 6);
        const FHex Dst = HexUtil::Neighbor(Src, Dir);

        // Block if destination currently has an item, or multiple belts contest the same Dst
        if (ItemMap.Contains(Dst)) continue;
        if (DestClaims.Contains(Dst)) continue;

        Moves.Emplace(Src, Dst);
        DestClaims.Add(Dst);
    }

    // Apply moves
    for (const TPair<FHex, FHex>& M : Moves)
    {
        FItem Item;
        if (ItemMap.RemoveAndCopyValue(M.Key, Item))
        {
            ItemMap.Add(M.Value, Item);
        }
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
                    ItemMap.Add(H, NewItem);
                }
            }
            break;
            case ETileKind::Output:
            {
                if (ItemMap.Contains(H) && !SkipOutputThisFrame.Contains(H))
                {
                    ItemMap.Remove(H);
                    ++OutputCount;
                }
            }
            break;
            default: break;
        }
    }
}
