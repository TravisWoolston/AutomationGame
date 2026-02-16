#pragma once
#include "AutomationGameMinimal.h"
#include "Sim/HexTypes.h"
#include "Sim/OrbTypes.h"
#include "WalkerTypes.generated.h"

/** Walker stance state for sitting/standing transitions */
UENUM(BlueprintType)
enum class EWalkerStance : uint8 {
  Sitting UMETA(DisplayName = "Sitting"),
  StandingUp UMETA(DisplayName = "Standing Up"),
  Standing UMETA(DisplayName = "Standing"),
  SittingDown UMETA(DisplayName = "Sitting Down")
};

/** Foot step state for multi-step walking animation */
UENUM(BlueprintType)
enum class EFootStepState : uint8 {
  Planted UMETA(DisplayName = "Planted"),
  Raising UMETA(DisplayName = "Raising"),
  Moving UMETA(DisplayName = "Moving"),
  Lowering UMETA(DisplayName = "Lowering")
};

/** Walker movement request */
USTRUCT(BlueprintType)
struct AUTOMATIONGAME_API FWalkerRequest {
  GENERATED_BODY()

  UPROPERTY(EditAnywhere, BlueprintReadWrite)
  FHex TargetHex;

  UPROPERTY(EditAnywhere, BlueprintReadWrite)
  int32 Priority = 0; // Higher priority requests processed first

  // Add attempt counter to requests to track timeouts
  UPROPERTY(EditAnywhere, BlueprintReadWrite)
  int32 AttemptTicks = 0;

  bool operator==(const FWalkerRequest &Other) const {
    return TargetHex == Other.TargetHex && Priority == Other.Priority;
  }
  bool operator!=(const FWalkerRequest &Other) const {
    return !(*this == Other);
  }
};

/** Foot state for individual leg tracking */
USTRUCT(BlueprintType)
struct AUTOMATIONGAME_API FFootState {
  GENERATED_BODY()

  UPROPERTY(EditAnywhere, BlueprintReadWrite)
  FHex CurrentTile;

  UPROPERTY(EditAnywhere, BlueprintReadWrite)
  FHex TargetTile;

  UPROPERTY(EditAnywhere, BlueprintReadWrite)
  EFootStepState StepState = EFootStepState::Planted;

  UPROPERTY(EditAnywhere, BlueprintReadWrite)
  int32 StepProgress = 0; // 0-3 for raising, 0-3 for moving, 0-3 for lowering

  UPROPERTY(EditAnywhere, BlueprintReadWrite)
  float HeightOffset = 0.f; // Visual height offset for raised foot

  UPROPERTY(EditAnywhere, BlueprintReadWrite)
  int32 LastPlantDuration =
      0; // How long the foot has been planted/static (or time since last move)

  // NEW: Held item state
  UPROPERTY(EditAnywhere, BlueprintReadWrite)
  bool bHoldingItem = false;

  UPROPERTY(EditAnywhere, BlueprintReadWrite)
  FItem HeldItem;

  // Gather/Scatter: multiple individual orbs held as singles
  UPROPERTY(EditAnywhere, BlueprintReadWrite)
  bool bGathering = false; // True when in Gather/Scatter mode (vs Relocate)

  UPROPERTY(EditAnywhere, BlueprintReadWrite)
  TArray<FItem> GatheredOrbs; // Individual unbonded orbs picked up for scatter

  // Step-by-step gather: list of orb hexes to visit and pick up one at a time
  UPROPERTY(EditAnywhere, BlueprintReadWrite)
  TArray<FHex> GatherTargetHexes;

  // Step-by-step scatter: list of empty hexes to visit and place one orb at a
  // time
  UPROPERTY(EditAnywhere, BlueprintReadWrite)
  TArray<FHex> ScatterTargetHexes;

  // True while the foot is actively gathering (stepping to orb tiles)
  UPROPERTY(EditAnywhere, BlueprintReadWrite)
  bool bGatherPhaseActive = false;

  // True while the foot is actively scattering (stepping to empty tiles)
  UPROPERTY(EditAnywhere, BlueprintReadWrite)
  bool bScatterPhaseActive = false;

  UPROPERTY(EditAnywhere, BlueprintReadWrite)
  int32 ScatterDroppedCount = 0; // How many orbs have been scattered so far

  // The designated drop-off hex for the held item (set when picking up from a
  // paired beacon)
  UPROPERTY(EditAnywhere, BlueprintReadWrite)
  FHex DesignatedDropOffHex;

  // Whether the held item has a designated drop-off location
  UPROPERTY(EditAnywhere, BlueprintReadWrite)
  bool bHasDesignatedDropOff = false;

  // The source pickup beacon hex (to keep it InProgress until delivery)
  UPROPERTY(EditAnywhere, BlueprintReadWrite)
  FHex SourcePickUpHex;

  // Whether there's a source pickup beacon to track
  UPROPERTY(EditAnywhere, BlueprintReadWrite)
  bool bHasSourcePickUp = false;

  // NEW: Dynamic move duration based on TilesPerStep
  UPROPERTY(EditAnywhere, BlueprintReadWrite)
  float CurrentMoveDurationTicks = 0.f;
};

// Legacy instruction system (kept for backward compatibility with scenario
// saving)
UENUM(BlueprintType)
enum class EWalkerCmd : uint8 {
  NoOp UMETA(DisplayName = "NoOp"),
  MoveTo UMETA(DisplayName = "MoveTo"),
  Reset UMETA(DisplayName = "Reset to Start (macro)"),
  Wait UMETA(DisplayName = "Wait (ticks = Arg)")
};

/** One instruction + small integer argument (target hex Q/R, wait ticks, etc.)
 */
USTRUCT(BlueprintType)
struct AUTOMATIONGAME_API FWalkerInstr {
  GENERATED_BODY()

  UPROPERTY(EditAnywhere, BlueprintReadWrite)
  EWalkerCmd Cmd = EWalkerCmd::NoOp;

  /** For MoveTo: Q coordinate. For Wait: ticks. Ignored otherwise. */
  UPROPERTY(EditAnywhere, BlueprintReadWrite)
  int32 Arg1 = 0;

  /** For MoveTo: R coordinate. Ignored otherwise. */
  UPROPERTY(EditAnywhere, BlueprintReadWrite)
  int32 Arg2 = 0;

  // Added equality operators so TArray<FWalkerInstr>::operator== works
  bool operator==(const FWalkerInstr &Other) const {
    return Cmd == Other.Cmd && Arg1 == Other.Arg1 && Arg2 == Other.Arg2;
  }
  bool operator!=(const FWalkerInstr &Other) const { return !(*this == Other); }
};

USTRUCT(BlueprintType)
struct AUTOMATIONGAME_API FWalkerState {
  GENERATED_BODY()

  // --- Core state ---
  UPROPERTY(EditAnywhere, BlueprintReadWrite) FHex BaseHex; // Body position
  UPROPERTY(EditAnywhere, BlueprintReadWrite)
  FHex HomeHex; // Starting location for Reset

  // --- Stance and transitions ---
  UPROPERTY(EditAnywhere, BlueprintReadWrite)
  EWalkerStance Stance = EWalkerStance::Sitting;
  UPROPERTY(EditAnywhere, BlueprintReadWrite)
  int32 StanceTransitionProgress = 0; // 0-3 for sit/stand transitions

  // --- Request queue ---
  UPROPERTY(EditAnywhere, BlueprintReadWrite) TArray<FWalkerRequest> Requests;
  UPROPERTY(EditAnywhere, BlueprintReadWrite)
  FHex CurrentTargetHex; // Current request target
  UPROPERTY(EditAnywhere, BlueprintReadWrite) bool bProcessingRequest = false;

  // --- Timeout tracking ---
  UPROPERTY(EditAnywhere, BlueprintReadWrite)
  int32 TicksSinceLastFootTargetChange =
      0; // Tracks when feet last updated their target tiles
  UPROPERTY(EditAnywhere, BlueprintReadWrite)
  int32 RequestCooldownTicks =
      0; // Cooldown period after canceling a request before accepting new ones

  // --- Foot states (D, L, R, U order) ---
  UPROPERTY(EditAnywhere, BlueprintReadWrite) FFootState DFoot;
  UPROPERTY(EditAnywhere, BlueprintReadWrite) FFootState LFoot;
  UPROPERTY(EditAnywhere, BlueprintReadWrite) FFootState RFoot;
  UPROPERTY(EditAnywhere, BlueprintReadWrite) FFootState UFoot;

  // --- Walking parameters ---
  UPROPERTY(EditAnywhere, BlueprintReadWrite) float StepHeight = 50.f;
  UPROPERTY(EditAnywhere, BlueprintReadWrite) int32 StepRaiseTicks = 3;
  UPROPERTY(EditAnywhere, BlueprintReadWrite) float TilesPerStep = 0.5f;
  UPROPERTY(EditAnywhere, BlueprintReadWrite) int32 StepLowerTicks = 3;
  UPROPERTY(EditAnywhere, BlueprintReadWrite)
  int32 ReachDistance = 2; // Tiles each leg can reach
  UPROPERTY(EditAnywhere, BlueprintReadWrite)
  float MinimumReachDistance =
      0.f; // Minimum tile distance from body for foot placement
  UPROPERTY(EditAnywhere, BlueprintReadWrite)
  float StandHeight = 0.f; // Body height offset when standing
  UPROPERTY(EditAnywhere, BlueprintReadWrite)
  float QuadrantRotationOffset =
      0.f; // Rotation offset for quadrant boundaries (degrees)
  UPROPERTY(EditAnywhere, BlueprintReadWrite)
  float StartingFootDistance = 0.f; // Starting foot distance in tiles from body

  // --- Legacy program support (kept for scenario compatibility) ---
  UPROPERTY(EditAnywhere, BlueprintReadWrite) TArray<FWalkerInstr> Program;
  UPROPERTY(EditAnywhere, BlueprintReadWrite) int32 PC = 0;
  UPROPERTY(EditAnywhere, BlueprintReadWrite) int32 WaitCounter = 0;
  UPROPERTY(EditAnywhere, BlueprintReadWrite) bool bMoving = false;
  UPROPERTY(EditAnywhere, BlueprintReadWrite) float MovementProgress = 0.0f;
  UPROPERTY(EditAnywhere, BlueprintReadWrite) FHex TargetHex;

  // Added equality operator so TArray<FWalkerState>::IndexOfByKey works
  bool operator==(const FWalkerState &Other) const {
    return BaseHex == Other.BaseHex && HomeHex == Other.HomeHex;
  }
  bool operator!=(const FWalkerState &Other) const { return !(*this == Other); }
};
