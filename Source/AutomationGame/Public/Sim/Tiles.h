#pragma once
#include "AutomationGameMinimal.h"
#include "Tiles.generated.h"

UENUM(BlueprintType)
enum class ETileKind : uint8 {
  Empty = 0,
  Input,
  Output,
  Belt,
  Bond,          // NEW: two?hex bonding glyph
  Unbond,        // NEW: two?hex unbonding glyph
  InputBond,     // NEW: bond between input tiles - orbs spawn pre-bonded
  OutputBond,    // NEW: bond between output tiles - requires bonded orbs
  ClusterInput,  // NEW: spawns pre-defined clusters based on template
  ClusterOutput, // NEW: accepts specific cluster patterns
  ClusterSend,   // NEW: sends cluster to the other grid level
  DiscoveryClusterInput,  // NEW: Spawns clusters for discovery mode (no storage
                          // required)
  DiscoveryClusterOutput, // NEW: Unlocks grid radius and removes itself once
                          // complete
  HelixOutput, // NEW: Outputs clusters into a spiraling DNA helix formation
  HelixInput,  // NEW: Receives processed clusters from a paired helix output
  Source       // NEW: Fountain helix source tile for discovery mode
};

USTRUCT(BlueprintType)
struct AUTOMATIONGAME_API FTile {
  GENERATED_BODY();
  UPROPERTY(EditAnywhere, BlueprintReadWrite) ETileKind Kind = ETileKind::Empty;
  UPROPERTY(EditAnywhere, BlueprintReadWrite) uint8 Arg = 0;
};
