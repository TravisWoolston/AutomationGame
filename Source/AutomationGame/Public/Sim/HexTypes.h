#pragma once
#include "AutomationGameMinimal.h"
#include "HexTypes.generated.h"

UENUM(BlueprintType)
enum class EViewMode : uint8 { Standard, AdvancedFilter, OrbView };

UENUM(BlueprintType)
enum class EHexDir : uint8 { D0 = 0, D1, D2, D3, D4, D5 };

/** Axial hex coordinate (q, r) and grid level. */
USTRUCT(BlueprintType)
struct AUTOMATIONGAME_API FHex {
  GENERATED_BODY();

  UPROPERTY(EditAnywhere, BlueprintReadWrite) int32 Q = 0;
  UPROPERTY(EditAnywhere, BlueprintReadWrite) int32 R = 0;
  UPROPERTY(EditAnywhere, BlueprintReadWrite) int32 Level = 0;

  FHex() {}
  FHex(int32 InQ, int32 InR, int32 InLevel = 0)
      : Q(InQ), R(InR), Level(InLevel) {}

  bool operator==(const FHex &Other) const {
    return Q == Other.Q && R == Other.R && Level == Other.Level;
  }
  bool operator!=(const FHex &Other) const { return !(*this == Other); }

  int32 Distance(const FHex &Other) const {
    const int32 dq = Q - Other.Q;
    const int32 dr = R - Other.R;
    // Distance within the same level; if different levels, technically infinite
    // or large, but usually we only check within one level
    return FMath::Max(FMath::Max(FMath::Abs(dq), FMath::Abs(dr)),
                      FMath::Abs(dq + dr));
  }

  FORCEINLINE FHex operator-(const FHex &Other) const {
    return FHex(Q - Other.Q, R - Other.R, Level);
  }
  FORCEINLINE FHex operator+(const FHex &Other) const {
    return FHex(Q + Other.Q, R + Other.R, Level);
  }

  FORCEINLINE FHex GetStepDir() const {
    // Return the unit direction closest to this offset vector
    // Using inline constants since HexUtil is defined after FHex
    static const FHex kDir[6] = {FHex(+1, 0, 0),  FHex(+1, -1, 0),
                                 FHex(0, -1, 0),  FHex(-1, 0, 0),
                                 FHex(-1, +1, 0), FHex(0, +1, 0)};
    int32 BestDist = 999;
    FHex BestDir(0, 0, 0);
    for (uint8 i = 0; i < 6; ++i) {
      // Calculate distance from this to the step direction
      int32 dq = Q - kDir[i].Q;
      int32 dr = R - kDir[i].R;
      int32 d = FMath::Max(FMath::Max(FMath::Abs(dq), FMath::Abs(dr)),
                           FMath::Abs(dq + dr));
      if (d < BestDist) {
        BestDist = d;
        BestDir = kDir[i];
      }
    }
    return BestDir;
  }

  FORCEINLINE FHex RotateCCW(int32 Steps) const {
    FHex Res = *this;
    int32 N = ((Steps % 6) + 6) % 6;
    for (int32 i = 0; i < N; ++i) {
      int32 OldQ = Res.Q;
      int32 OldR = Res.R;
      Res.Q = -OldR;
      Res.R = OldQ + OldR;
    }
    return Res;
  }
};

FORCEINLINE uint32 GetTypeHash(const FHex &H) {
  return HashCombine(HashCombine(::GetTypeHash(H.Q), ::GetTypeHash(H.R)),
                     ::GetTypeHash(H.Level));
}

namespace HexUtil {
// Axial direction vectors (pointy-top convention)

static inline FHex Step(uint8 Dir) {
  static const FHex kDir[6] = {FHex(+1, 0, 0), FHex(+1, -1, 0), FHex(0, -1, 0),
                               FHex(-1, 0, 0), FHex(-1, +1, 0), FHex(0, +1, 0)};
  return kDir[Dir % 6];
}

static inline FHex Neighbor(const FHex &H, EHexDir D) {
  const FHex s = Step(static_cast<uint8>(D));
  return FHex(H.Q + s.Q, H.R + s.R, H.Level);
}

// World conversion (top-down pointy hexes). TileSize is center-to-center
// spacing. NOTE: Z is handled by the caller or by the level offset
FORCEINLINE FVector ToWorld(const FHex &H, float TileSize) {
  const float x = TileSize * (FMath::Sqrt(3.f) * (float)H.Q +
                              (FMath::Sqrt(3.f) / 2.f) * (float)H.R);
  const float y = TileSize * (3.f / 2.f * (float)H.R);
  return FVector(x, y, 0.f);
}

FORCEINLINE int32 Dist(const FHex &A, const FHex &B) {
  const int32 dq = A.Q - B.Q;
  const int32 dr = A.R - B.R;
  return FMath::Max(FMath::Max(FMath::Abs(dq), FMath::Abs(dr)),
                    FMath::Abs(dq + dr));
}
} // namespace HexUtil

enum class ETileKind : uint8; // forward declare (optional, only if required by
                              // types here)
