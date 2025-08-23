#include "Editor/MachineBlueprint.h"

int32 UMachineBlueprint::FindPartAtHex(FHex Hex) const
{
    for (int32 i=0;i<Parts.Num();++i)
    {
        if (Parts[i].Hex == Hex) return i;
    }
    return INDEX_NONE;
}

void UMachineBlueprint::AddOrReplacePart(const FMachinePart& Part)
{
    const int32 idx = FindPartAtHex(Part.Hex);
    if (idx != INDEX_NONE) Parts[idx] = Part;
    else Parts.Add(Part);
}

bool UMachineBlueprint::RemovePartAt(FHex Hex)
{
    const int32 idx = FindPartAtHex(Hex);
    if (idx == INDEX_NONE) return false;
    Parts.RemoveAt(idx);
    return true;
}

void UMachineBlueprint::Clear()
{
    Parts.Reset();
}
