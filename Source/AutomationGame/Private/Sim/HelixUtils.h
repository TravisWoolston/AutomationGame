#include "EngineUtils.h"
#include "Framework/GameManager.h"
#include "Sim/SimWorldHex.h"

// Helper to find the center-most hex deterministically
static FHex FindCenterHexRobust(USimWorldHex *Sim, const TArray<FHex> &Tiles) {
  if (Tiles.Num() == 0)
    return FHex(0, 0, 0);
  if (Tiles.Num() == 1)
    return Tiles[0];

  // Calculate average world position
  FVector AvgPos = FVector::ZeroVector;
  for (const FHex &H : Tiles) {
    AvgPos += Sim->HexCenterToWorld(H);
  }
  AvgPos /= Tiles.Num();

  // Find tile closest to average with deterministic tie-breaking
  FHex BestHex = Tiles[0];
  float BestDist = FLT_MAX;

  for (const FHex &H : Tiles) {
    float Dist = FVector::DistSquared(Sim->HexCenterToWorld(H), AvgPos);

    // Use a small tolerance for distance comparison
    if (Dist < BestDist - 0.1f) {
      BestDist = Dist;
      BestHex = H;
    } else if (FMath::Abs(Dist - BestDist) < 0.1f) {
      // Tie-breaker: prioritize lower Q, then lower R
      if (H.Q < BestHex.Q || (H.Q == BestHex.Q && H.R < BestHex.R)) {
        BestDist = Dist;
        BestHex = H;
      }
    }
  }
  return BestHex;
}
