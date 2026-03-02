#pragma once

#include "AutomationGameMinimal.h"
#include "Sim/HexTypes.h"
// .generate must be last
#include "OrbTypes.generated.h"

static FORCEINLINE int32 MakeSubOrbId(int32 ItemId, int32 SubIdx) {
  return (ItemId << 8) | (SubIdx & 0xFF);
}

UENUM(BlueprintType)
enum class EOrbType : uint8 {
  None = 0,
  BaseA,
  BaseB,
  BaseC,
  MetalA,
  CatalystX,
  Void
};

UENUM(BlueprintType)
enum class EOrbColor : uint8 {
  None = 0,
  White,
  Red,
  Green,
  Blue,
  Yellow,
  Cyan,
  Magenta
};

/** Single orb (atom-like piece). */
USTRUCT(BlueprintType)
struct FOrb {
  GENERATED_BODY();
  UPROPERTY(EditAnywhere, BlueprintReadWrite) EOrbType Type = EOrbType::None;
  UPROPERTY(EditAnywhere, BlueprintReadWrite)
  EOrbColor Color = EOrbColor::White;
  UPROPERTY(EditAnywhere, BlueprintReadWrite) uint8 Kind = 0;
  UPROPERTY(EditAnywhere, BlueprintReadWrite,
            meta = (ClampMin = 0, ClampMax = 5))
  uint8 Rot60 = 0;

  /** Remaining stability in GTM steps. If <= 0, becomes Void. */
  UPROPERTY(EditAnywhere, BlueprintReadWrite)
  int32 Stability = 1000;
};

USTRUCT(BlueprintType)
struct FOrbBond {
  GENERATED_BODY()
  UPROPERTY(EditAnywhere, BlueprintReadWrite) int32 OrbA = 0;
  UPROPERTY(EditAnywhere, BlueprintReadWrite) int32 OrbB = 0;
  UPROPERTY(EditAnywhere, BlueprintReadWrite) uint8 BondType = 0;
};

/** Data for an orb inside a grabbed cluster (includes integer axial delta). */
USTRUCT(BlueprintType)
struct FOrbData {
  GENERATED_BODY()

  UPROPERTY(EditAnywhere, BlueprintReadWrite) uint8 Type = 0;
  UPROPERTY(EditAnywhere, BlueprintReadWrite) uint8 Color = 0;
  UPROPERTY(EditAnywhere, BlueprintReadWrite)
  FVector2D Offset = FVector2D::ZeroVector;
  UPROPERTY(EditAnywhere, BlueprintReadWrite) int32 dQ = 0;
  UPROPERTY(EditAnywhere, BlueprintReadWrite) int32 dR = 0;
  // NEW: original item Id for this orb (preserved while held so bonds stay
  // valid)
  UPROPERTY(VisibleAnywhere, BlueprintReadOnly) int32 ItemId = 0;

  // NEW: Previous board hex location (for smooth transition animation into
  // helix)
  UPROPERTY(Transient)
  FHex PreviousBoardHex = FHex(0, 0, 0);

  /** Remaining stability in GTM steps. If <= 0, becomes Void. */
  UPROPERTY(EditAnywhere, BlueprintReadWrite)
  int32 Stability = 1000;
};

/** Item on a tile: either a single orb or a bonded cluster. */
USTRUCT(BlueprintType)
struct AUTOMATIONGAME_API FItem {
  GENERATED_BODY();
  UPROPERTY(VisibleAnywhere, BlueprintReadOnly) int32 Id = 0;
  UPROPERTY(EditAnywhere, BlueprintReadWrite) bool bIsCluster = false;
  UPROPERTY(EditAnywhere, BlueprintReadWrite) FOrb Single;
  UPROPERTY(EditAnywhere, BlueprintReadWrite) TArray<FOrbData> Orbs;
  UPROPERTY(EditAnywhere, BlueprintReadWrite) TArray<FOrbBond> Bonds;

  // NEW: facing direction (0..5) of the arm at the moment this cluster was
  // grabbed. Singles ignore this (value stays -1). Used so drop rotates
  // relative, not absolute.
  UPROPERTY()
  int8 GrabFacing = -1;

  // NEW: flag to track if this item is "anchored" or "locked" (cannot be moved
  // by hands/belts)
  UPROPERTY()
  bool bIsLocked = false;

  // NEW: flag to track if this item has been moved by belts this tick (prevents
  UPROPERTY()
  bool LocationUpdatedThisTick = false;

  // NEW: flag to track if this item is ready to be pulled from bond/unbond
  // tiles Set to false when an orb is pushed onto a bond/unbond tile by a belt
  // Set to true when a belt tries to pull but the orb isn't ready (allowing one
  // tick on the tile)
  UPROPERTY()
  bool bIsReady = true;

  static FItem MakeSingle(const FOrb &O) {
    FItem I;
    I.bIsCluster = false;
    I.Single = O;
    return I;
  }
};

/** A single rung in the helix - represents one processed cluster */
USTRUCT(BlueprintType)
struct AUTOMATIONGAME_API FHelixRung {
  GENERATED_BODY();

  UPROPERTY(EditAnywhere, BlueprintReadWrite)
  TArray<FOrbData> Orbs; // The orbs in this cluster (with relative positions)

  // Bonds (not exposed to BP due to TPair)
  TArray<TPair<int32, int32>> Bonds;
  TArray<TPair<int32, int32>> BondsToPrevious;
  TArray<TPair<int32, int32>> BondsToBoard;

  UPROPERTY(EditAnywhere, BlueprintReadWrite)
  float SpawnTime = 0.f;
  UPROPERTY(EditAnywhere, BlueprintReadWrite)
  int32 SpawnTick = 0;
  UPROPERTY(EditAnywhere, BlueprintReadWrite)
  float ZOffset = 0.f;
  UPROPERTY(EditAnywhere, BlueprintReadWrite)
  float TargetZOffset = 0.f;
  UPROPERTY(EditAnywhere, BlueprintReadWrite)
  float RotationDegrees = 0.f;

  /** Direction (0-5) items were pushed INTO the helix - used for portal exit
   * direction */
  UPROPERTY(EditAnywhere, BlueprintReadWrite)
  uint8 EntryDirection = 0;
};

/** A group of adjacent HelixOutput tiles that form a single helix column */
USTRUCT(BlueprintType)
struct AUTOMATIONGAME_API FHelixOutputGroup {
  GENERATED_BODY();

  UPROPERTY(EditAnywhere, BlueprintReadWrite)
  TArray<FHex> MemberTiles;
  UPROPERTY(EditAnywhere, BlueprintReadWrite)
  FHex CenterHex = FHex(0, 0, 0);
  UPROPERTY(EditAnywhere, BlueprintReadWrite)
  FVector CenterWorld = FVector::ZeroVector;
  UPROPERTY(EditAnywhere, BlueprintReadWrite)
  TArray<FHelixRung> Rungs;
  UPROPERTY(EditAnywhere, BlueprintReadWrite)
  int64 TotalClustersProcessed = 0;
  UPROPERTY(EditAnywhere, BlueprintReadWrite)
  float HelixRotationOffset = 0.f;

  UPROPERTY(EditAnywhere, BlueprintReadWrite)
  TArray<FHex> PairedInputTiles;
  UPROPERTY(EditAnywhere, BlueprintReadWrite)
  FHex PairedInputCenter = FHex(0, 0, 0);
  UPROPERTY(EditAnywhere, BlueprintReadWrite)
  FVector PairedInputCenterWorld = FVector::ZeroVector;
  UPROPERTY(EditAnywhere, BlueprintReadWrite)
  bool bHasPairedInput = false;

  UPROPERTY(EditAnywhere, BlueprintReadWrite)
  float DegreesPerRung = 36.f;
  UPROPERTY(EditAnywhere, BlueprintReadWrite)
  float RungSpacing = 100.f;
  UPROPERTY(EditAnywhere, BlueprintReadWrite)
  int32 MaxVisibleRungs = 20;
  UPROPERTY(EditAnywhere, BlueprintReadWrite)
  int32 LastProcessedTick = -1;

  UPROPERTY(EditAnywhere, BlueprintReadWrite)
  int32 LastEnteredItemId = 0;
  UPROPERTY(EditAnywhere, BlueprintReadWrite)
  int32 LastEnteredItemTick = -1;
  UPROPERTY(EditAnywhere, BlueprintReadWrite)
  int32 RungTransitTime = 20;

  static constexpr int32 MinPairDistance = 3;
};
