#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Components/SkeletalMeshComponent.h"
#include "Sim/HexTypes.h"     // FHex
#include "Sim/HexCoord.h"     // Axial<->XY helpers
#include "Sim/WalkerTypes.h"  // FWalkerInstr, EWalkerCmd
#include "BasicWalker.generated.h"

class AOrbSimDriver;
class AGameTickManager;
class UStaticMeshComponent;
class USimWorldHex;
class UOrbRenderer;

/** Event that fires when assigned tile world locations are updated */
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnAssignedTilesUpdated);

/** Visual foot state for event graph updates (includes Holding which EFootStepState lacks) */
UENUM(BlueprintType)
enum class EFootVisualState : uint8
{
	Planted  UMETA(DisplayName = "Planted"),
	Raising  UMETA(DisplayName = "Raising"),
	Moving   UMETA(DisplayName = "Moving"),
	Lowering UMETA(DisplayName = "Lowering"),
	Holding  UMETA(DisplayName = "Holding")
};

UCLASS()
class AUTOMATIONGAME_API ABasicWalker : public AActor
{
	GENERATED_BODY()

public:
	ABasicWalker();
	/** Trace from foot bone toward assigned tile and get foot goal in component space for IK */
UFUNCTION(BlueprintCallable, Category = "Walker|Foot Goals")
FVector GetFootGoalFromBoneToTile(const FName& FootBoneName, const FVector& AssignedTileWorldLocation, bool bTraceDownward = true) const;

/** Trace from foot bone toward assigned tile and get foot goal in component space (string version) */
UFUNCTION(BlueprintCallable, Category = "Walker|Foot Goals")
FVector GetFootGoalFromBoneToTileString(const FString& FootBoneName, const FVector& AssignedTileWorldLocation, bool bTraceDownward = true) const;
/** Convert a world space position to component space (for IK goals) */
UFUNCTION(BlueprintCallable, Category = "Walker")
FVector WorldToComponentSpace(const FVector& WorldLocation) const;
	// Engine
	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

	// --- Grid / placement ---
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid")
	AOrbSimDriver* Driver = nullptr;

	/** Axial tile where the walker's center body sits. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid")
	int32 Q = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid")
	int32 R = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid")
	int32 Level = 0;

	/** Convenience: set grid placement & driver in one call. */
	UFUNCTION(BlueprintCallable, Category = "Grid")
	void InitializeOnGrid(AOrbSimDriver* InDriver, int32 InQ, int32 InR, int32 InLevel);

	// --- Visuals ---
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visual")
	USkeletalMesh* WalkerSkeletalMesh = nullptr;
	
	// Animation Blueprint class for the walker
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visual")
	TSubclassOf<UAnimInstance> WalkerAnimationBlueprintClass = nullptr;
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visual")
	UStaticMesh* BodyMesh = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visual")
	float BaseZ = 10.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visual")
	FVector BaseScale = FVector(1.f);

	// --- Program control (Blueprint-friendly) ---
	/** Program editable in BP (looped if bLoopProgram). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Program")
	TArray<FWalkerInstr> Program;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Program")
	bool bLoopProgram = true;

	/** Replace Program at runtime. */
	UFUNCTION(BlueprintCallable, Category = "Program")
	void SetProgram(const TArray<FWalkerInstr>& InProgram, bool bLoop = true);

	/** Control */
	UFUNCTION(BlueprintCallable, Category = "Program")
	void Start();

	UFUNCTION(BlueprintCallable, Category = "Program")
	void Stop();

	UFUNCTION(BlueprintCallable, Category = "Program")
	void ResetProgram();
	
	UFUNCTION(BlueprintCallable, Category = "Program")
	void SyncFromSim(const FWalkerState& SimState);  // also sets SimWalkerIndex

	UFUNCTION(BlueprintCallable, Category = "Program")
	void SyncFromCurrentSim();  // sync from current sim state

	/** Check if the walker is currently moving */
	UFUNCTION(BlueprintPure, Category = "Program")
	bool IsMoving() const { return bMoving; }

	/** True when the walker is being used in CalibrateWalker */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Calibration")
	bool Calibrating = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sim")
	int32 SimWalkerIndex;

	/** Starting location for Reset command (set when placed) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid")
	FHex HomeHex;

	// --- GameManager-assigned values (public for runtime verification/editing) ---
	
	/** Step height (how high feet raise when stepping) - assigned by GameManager */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Walker|GameManager Values")
	float StepHeight = 100.f;

	/** Ticks for raising foot during step - assigned by GameManager */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Walker|GameManager Values")
	int32 StepRaiseTicks = 3;

	/** Amount of tiles the foot will traverse per GTM tick during horizontal move - assigned by GameManager */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Walker|GameManager Values")
	float TilesPerStep = 3.0f;

	/** Ticks for lowering foot during step - assigned by GameManager */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Walker|GameManager Values")
	int32 StepLowerTicks = 1;

	/** Maximum reach distance in tiles - assigned by GameManager */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Walker|GameManager Values")
	int32 ReachDistance = 7;

	/** Minimum reach distance in tiles (feet cannot be placed closer than this) - assigned by GameManager */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Walker|GameManager Values")
	float MinimumReachDistance = 2.f;

	/** Stand height (body height offset when standing) - assigned by GameManager */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Walker|GameManager Values")
	float StandHeight = 0.f;

	/** Starting foot distance (initial foot placement distance from body) - assigned by GameManager */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Walker|GameManager Values")
	float StartingFootDistance = 5.f;

	/** Rotation offset for quadrant boundaries (degrees) - assigned by GameManager */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Walker|GameManager Values")
	float QuadrantRotationOffset = 0.f;

	// --- Body Rotation Control ---
	
	/** Target rotation for the body (Z axis) - set in Event Graph or derived from foot positions */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Walker|Body Rotation")
	FRotator BodyGoalRotation = FRotator(90.f, 0.f, 90.f);
	
	/** Calculated dynamic yaw for the body (only updated if bDynamicBodyRotation is true) */
	UPROPERTY(BlueprintReadOnly, Category = "Walker|Body Rotation")
	float AvgYaw = 0.f;
	
	/** If true, QuadrantRotationOffset will automatically update based on body Z rotation */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Walker|Body Rotation")
	bool bDynamicBodyRotation = true;

	// --- Bone Names (configurable for different skeletons) ---
	
	/** Primary body bone name for center-of-mass calculations */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Walker|Bone Names")
	FName BodyBoneName = TEXT("Body1");
	
	/** Fallback body bone name if primary is not found */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Walker|Bone Names")
	FName BodyBoneNameFallback = TEXT("pelvis");
	
	/** D (South/Back) foot bone name */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Walker|Bone Names")
	FName DFootBoneName = TEXT("DFoot49");
	
	/** L (West/Left) foot bone name */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Walker|Bone Names")
	FName LFootBoneName = TEXT("LFoot25");
	
	/** R (East/Right) foot bone name */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Walker|Bone Names")
	FName RFootBoneName = TEXT("RFoot13");
	
	/** U (North/Front) foot bone name */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Walker|Bone Names")
	FName UFootBoneName = TEXT("UFoot37");

	// --- Foot Animation States ---

	UPROPERTY(BlueprintReadOnly, Category = "Walker|Animation")
	bool bDFootRaising = false;
	UPROPERTY(BlueprintReadOnly, Category = "Walker|Animation")
	bool bDFootMoving = false;
	UPROPERTY(BlueprintReadOnly, Category = "Walker|Animation")
	bool bDFootLowering = false;

	UPROPERTY(BlueprintReadOnly, Category = "Walker|Animation")
	bool bLFootRaising = false;
	UPROPERTY(BlueprintReadOnly, Category = "Walker|Animation")
	bool bLFootMoving = false;
	UPROPERTY(BlueprintReadOnly, Category = "Walker|Animation")
	bool bLFootLowering = false;

	UPROPERTY(BlueprintReadOnly, Category = "Walker|Animation")
	bool bRFootRaising = false;
	UPROPERTY(BlueprintReadOnly, Category = "Walker|Animation")
	bool bRFootMoving = false;
	UPROPERTY(BlueprintReadOnly, Category = "Walker|Animation")
	bool bRFootLowering = false;

	UPROPERTY(BlueprintReadOnly, Category = "Walker|Animation")
	bool bUFootRaising = false;
	UPROPERTY(BlueprintReadOnly, Category = "Walker|Animation")
	bool bUFootMoving = false;
	UPROPERTY(BlueprintReadOnly, Category = "Walker|Animation")
	bool bUFootLowering = false;

	// --- Foot Visual Progress (for Animation Blueprint access) ---
	// Progress values 0-1 for current animation phase (updated each tick when animating)
	
	UPROPERTY(BlueprintReadOnly, Category = "Walker|Foot Animation")
	float DFootProgress = 1.0f;
	UPROPERTY(BlueprintReadOnly, Category = "Walker|Foot Animation")
	float LFootProgress = 1.0f;
	UPROPERTY(BlueprintReadOnly, Category = "Walker|Foot Animation")
	float RFootProgress = 1.0f;
	UPROPERTY(BlueprintReadOnly, Category = "Walker|Foot Animation")
	float UFootProgress = 1.0f;
	
	// Visual state for each foot (Planted, Raising, Moving, Lowering, Holding)
	UPROPERTY(BlueprintReadOnly, Category = "Walker|Foot Animation")
	EFootVisualState DFootVisualState = EFootVisualState::Planted;
	UPROPERTY(BlueprintReadOnly, Category = "Walker|Foot Animation")
	EFootVisualState LFootVisualState = EFootVisualState::Planted;
	UPROPERTY(BlueprintReadOnly, Category = "Walker|Foot Animation")
	EFootVisualState RFootVisualState = EFootVisualState::Planted;
	UPROPERTY(BlueprintReadOnly, Category = "Walker|Foot Animation")
	EFootVisualState UFootVisualState = EFootVisualState::Planted;
	
	// Target tile world positions (where foot is moving TO)
	UPROPERTY(BlueprintReadOnly, Category = "Walker|Foot Animation")
	FVector DFootTargetWorldPosition = FVector::ZeroVector;
	UPROPERTY(BlueprintReadOnly, Category = "Walker|Foot Animation")
	FVector LFootTargetWorldPosition = FVector::ZeroVector;
	UPROPERTY(BlueprintReadOnly, Category = "Walker|Foot Animation")
	FVector RFootTargetWorldPosition = FVector::ZeroVector;
	UPROPERTY(BlueprintReadOnly, Category = "Walker|Foot Animation")
	FVector UFootTargetWorldPosition = FVector::ZeroVector;
	
	// Previous tile world positions (where foot is moving FROM)
	UPROPERTY(BlueprintReadOnly, Category = "Walker|Foot Animation")
	FVector DFootPreviousWorldPosition = FVector::ZeroVector;
	UPROPERTY(BlueprintReadOnly, Category = "Walker|Foot Animation")
	FVector LFootPreviousWorldPosition = FVector::ZeroVector;
	UPROPERTY(BlueprintReadOnly, Category = "Walker|Foot Animation")
	FVector RFootPreviousWorldPosition = FVector::ZeroVector;
	UPROPERTY(BlueprintReadOnly, Category = "Walker|Foot Animation")
	FVector UFootPreviousWorldPosition = FVector::ZeroVector;
	
	// Smoothed foot world positions (interpolated for visual smoothness)
	// These smoothly interpolate to the target position over time
	UPROPERTY(BlueprintReadOnly, Category = "Walker|Foot Animation")
	FVector DFootSmoothedWorldPosition = FVector::ZeroVector;
	UPROPERTY(BlueprintReadOnly, Category = "Walker|Foot Animation")
	FVector LFootSmoothedWorldPosition = FVector::ZeroVector;
	UPROPERTY(BlueprintReadOnly, Category = "Walker|Foot Animation")
	FVector RFootSmoothedWorldPosition = FVector::ZeroVector;
	UPROPERTY(BlueprintReadOnly, Category = "Walker|Foot Animation")
	FVector UFootSmoothedWorldPosition = FVector::ZeroVector;
	
	// Smoothed foot height offsets (interpolated for visual smoothness)
	// Use these for foot Z positioning in AnimBP for smooth lowering/raising
	UPROPERTY(BlueprintReadOnly, Category = "Walker|Foot Animation")
	float DFootSmoothedHeight = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "Walker|Foot Animation")
	float LFootSmoothedHeight = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "Walker|Foot Animation")
	float RFootSmoothedHeight = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "Walker|Foot Animation")
	float UFootSmoothedHeight = 0.f;

	// Held item states for visual representation
	UPROPERTY(BlueprintReadOnly, Category = "Walker|Held Items")
	bool bDFootHoldingItem = false;
	UPROPERTY(BlueprintReadOnly, Category = "Walker|Held Items")
	bool bLFootHoldingItem = false;
	UPROPERTY(BlueprintReadOnly, Category = "Walker|Held Items")
	bool bRFootHoldingItem = false;
	UPROPERTY(BlueprintReadOnly, Category = "Walker|Held Items")
	bool bUFootHoldingItem = false;

	UPROPERTY(BlueprintReadOnly, Category = "Walker|Held Items")
	FItem DFootHeldItem;
	UPROPERTY(BlueprintReadOnly, Category = "Walker|Held Items")
	FItem LFootHeldItem;
	UPROPERTY(BlueprintReadOnly, Category = "Walker|Held Items")
	FItem RFootHeldItem;
	UPROPERTY(BlueprintReadOnly, Category = "Walker|Held Items")
	FItem UFootHeldItem;

	USkeletalMeshComponent* GetBodyComponent() const { return BodyComp; }

	/** Get the animation blueprint instance from the body component */
	UFUNCTION(BlueprintCallable, Category = "Animation")
	UAnimInstance* GetAnimInstance() const;

	/** World locations of assigned tiles for each foot (updated from simulation) */
	UPROPERTY(BlueprintReadOnly, Category = "Foot Positions")
	FVector DFootAssignedTileWorldLocation = FVector::ZeroVector;

	UPROPERTY(BlueprintReadOnly, Category = "Foot Positions")
	FVector LFootAssignedTileWorldLocation = FVector::ZeroVector;

	UPROPERTY(BlueprintReadOnly, Category = "Foot Positions")
	FVector RFootAssignedTileWorldLocation = FVector::ZeroVector;

	UPROPERTY(BlueprintReadOnly, Category = "Foot Positions")
	FVector UFootAssignedTileWorldLocation = FVector::ZeroVector;

	/** Intended world location for the body center. Foot tiles will be calculated to center the body at this position. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Walker|Body Target")
	FVector BodyTargetWorldPosition = FVector::ZeroVector;

	/** Get bone location in world space by bone name */
	UFUNCTION(BlueprintCallable, Category = "Bone Access")
	FVector GetBoneLocationByName(const FName& BoneName, bool bWorldSpace = true) const;

	/** Get bone location in world space by bone name (string version for Blueprint convenience) */
	UFUNCTION(BlueprintCallable, Category = "Bone Access", meta = (CallInEditor = "true"))
	FVector GetBoneLocationByNameString(const FString& BoneName, bool bWorldSpace = true) const;

	/** Get bone location in component space by bone name */
	UFUNCTION(BlueprintCallable, Category = "Bone Access")
	FVector GetBoneLocationComponentSpace(const FName& BoneName) const;

	/** Line trace from a world position downward to find the grid surface */
	UFUNCTION(BlueprintCallable, Category = "Line Trace")
	bool LineTraceToGrid(const FVector& StartLocation, FVector& OutHitLocation, float TraceDistance = 1000.0f) const;

	/** Line trace from a world position in a specific direction to find the grid surface */
	UFUNCTION(BlueprintCallable, Category = "Line Trace")
	bool LineTraceToGridDirection(const FVector& StartLocation, const FVector& Direction, FVector& OutHitLocation, float TraceDistance = 1000.0f) const;

	/** Debug version that logs trace details */
	UFUNCTION(BlueprintCallable, Category = "Line Trace", meta = (CallInEditor = "true"))
	bool LineTraceToGridDebug(const FVector& StartLocation, FVector& OutHitLocation, float TraceDistance = 1000.0f, bool bDrawDebug = true) const;

public:
	void PushProgramToSim();
	TArray<FWalkerInstr> BuildProgramFromInstructions(const TArray<FWalkerInstr>& InProgram) const;
	
	UFUNCTION(BlueprintCallable, Category = "Program")
	void RebuildProgramFromInstructions();          // runtime/editor safe manual push

	/** Update public world location properties from foot hex positions (public for GameManager calibration) */
	UFUNCTION(BlueprintCallable, Category = "Walker")
	void UpdateAssignedTileWorldLocations();
	
	/** 
	 * Recalculate body rotation (AvgYaw) based on actual foot bone positions.
	 * Call this from Event Graph when you want to update body rotation.
	 * Uses foot bone locations from the skeletal mesh, so works with ManualFootGoals.
	 * Updates: AvgYaw, BodyGoalRotation, QuadrantRotationOffset
	 */
	UFUNCTION(BlueprintCallable, Category = "Walker|Body Rotation")
	void UpdateBodyRotationFromFeet();

	/** Event that fires when assigned tile world locations are updated (use in Event Graph to trigger foot goal updates) */
	UPROPERTY(BlueprintAssignable, Category = "Walker|Events")
	FOnAssignedTilesUpdated OnAssignedTilesUpdated;

	// --- Foot Visual Update Events (called when foot state changes, ProgressValue 0-1) ---
	
	/** Called each tick while D foot is animating (ProgressValue < 1). Implement in Event Graph for visual updates. */
	UFUNCTION(BlueprintImplementableEvent, Category = "Walker|Foot Events")
	void UpdateFootDVisual(float ProgressValue, EFootVisualState FootState, FVector CurrentAssignedTileWorldPosition, FVector PreviousAssignedTileWorldPosition);
	
	/** Called each tick while L foot is animating (ProgressValue < 1). Implement in Event Graph for visual updates. */
	UFUNCTION(BlueprintImplementableEvent, Category = "Walker|Foot Events")
	void UpdateFootLVisual(float ProgressValue, EFootVisualState FootState, FVector CurrentAssignedTileWorldPosition, FVector PreviousAssignedTileWorldPosition);
	
	/** Called each tick while R foot is animating (ProgressValue < 1). Implement in Event Graph for visual updates. */
	UFUNCTION(BlueprintImplementableEvent, Category = "Walker|Foot Events")
	void UpdateFootRVisual(float ProgressValue, EFootVisualState FootState, FVector CurrentAssignedTileWorldPosition, FVector PreviousAssignedTileWorldPosition);
	
	/** Called each tick while U foot is animating (ProgressValue < 1). Implement in Event Graph for visual updates. */
	UFUNCTION(BlueprintImplementableEvent, Category = "Walker|Foot Events")
	void UpdateFootUVisual(float ProgressValue, EFootVisualState FootState, FVector CurrentAssignedTileWorldPosition, FVector PreviousAssignedTileWorldPosition);

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& Event) override;
#endif

protected:
	// Fixed tick hookup
	UFUNCTION()
	void HandleFixedTickSeconds(float StepSeconds);

private:
	// Components
	UPROPERTY(Transient) USkeletalMeshComponent* BodyComp = nullptr;
	
	// Animation Blueprint instance
	UPROPERTY(Transient) UAnimInstance* WalkerAnimInstance = nullptr;

	// Sim ref
	UPROPERTY(Transient) USimWorldHex* Sim = nullptr;
	
	// Cached renderer for performance (avoids TActorIterator scans)
	UPROPERTY(Transient) UOrbRenderer* CachedOrbRenderer = nullptr;

	// State
	bool  bRunning = true;
	bool  bMoving = false;
	int32 PC = 0;                 // program counter
	int32 WaitLeft = 0;           // ticks left for a Wait

	// Movement interpolation
	FHex CurrentHex;
	FHex TargetHex;
	float MovementProgress = 0.0f;  // 0.0 to 1.0
	
	// Foot positions for visual sync
	FHex DFootHex;
	FHex LFootHex;
	FHex RFootHex;
	FHex UFootHex;
	
	// Foot height offsets from sim (raw values)
	float DFootHeightOffset = 0.f;
	float LFootHeightOffset = 0.f;
	float RFootHeightOffset = 0.f;
	float UFootHeightOffset = 0.f;
	
	// Smoothed foot height offsets (interpolated for visual smoothness) - stored here, exposed publicly
	float DFootSmoothedHeightOffset = 0.f;
	float LFootSmoothedHeightOffset = 0.f;
	float RFootSmoothedHeightOffset = 0.f;
	float UFootSmoothedHeightOffset = 0.f;
	
	// Flag to detect first-time smoothed position initialization
	bool bSmoothedPositionsInitialized = false;
	
	// Foot step states (to determine which feet are planted)
	EFootStepState DFootStepState = EFootStepState::Planted;
	EFootStepState LFootStepState = EFootStepState::Planted;
	EFootStepState RFootStepState = EFootStepState::Planted;
	EFootStepState UFootStepState = EFootStepState::Planted;
	
	// Previous step states (to detect phase transitions)
	EFootStepState DFootPrevStepState = EFootStepState::Planted;
	EFootStepState LFootPrevStepState = EFootStepState::Planted;
	EFootStepState RFootPrevStepState = EFootStepState::Planted;
	EFootStepState UFootPrevStepState = EFootStepState::Planted;
	
	// Previous step progress (to detect when sim has actually updated)
	int32 DFootPrevStepProgress = 0;
	int32 LFootPrevStepProgress = 0;
	int32 RFootPrevStepProgress = 0;
	int32 UFootPrevStepProgress = 0;
	
	// Visual start positions (captured when a new Moving phase begins)
	// These are the positions the foot is moving FROM during the Moving phase
	FVector DFootVisualStartPos = FVector::ZeroVector;
	FVector LFootVisualStartPos = FVector::ZeroVector;
	FVector RFootVisualStartPos = FVector::ZeroVector;
	FVector UFootVisualStartPos = FVector::ZeroVector;
	
	// Held item visual components (one per foot)
	UPROPERTY(Transient) UStaticMeshComponent* DFootHeldOrbComp = nullptr;
	UPROPERTY(Transient) UStaticMeshComponent* LFootHeldOrbComp = nullptr;
	UPROPERTY(Transient) UStaticMeshComponent* RFootHeldOrbComp = nullptr;
	UPROPERTY(Transient) UStaticMeshComponent* UFootHeldOrbComp = nullptr;
	
	// Cached held item IDs to detect changes and avoid recreating visuals every tick
	int32 CachedDFootHeldItemId = 0;
	int32 CachedLFootHeldItemId = 0;
	int32 CachedRFootHeldItemId = 0;
	int32 CachedUFootHeldItemId = 0;
	
	// Track whether held item visual should be shown (delayed until foot visually arrives)
	// When sim sets bHoldingItem=true, we defer showing the visual until smoothed position is close to target
	bool bDFootHeldVisualReady = false;
	bool bLFootHeldVisualReady = false;
	bool bRFootHeldVisualReady = false;
	bool bUFootHeldVisualReady = false;
	
	// Track pending drop visuals - keep showing held item until delay elapses
	bool bDFootPendingDropVisual = false;
	bool bLFootPendingDropVisual = false;
	bool bRFootPendingDropVisual = false;
	bool bUFootPendingDropVisual = false;
	
	// Sim-tick-based delay counters for pickup/drop visual timing
	// When pickup/drop occurs, set to CurrentMoveDurationTicks; decrement each sim tick; show/hide when <= 0
	int32 DFootPickupVisualDelay = 0;
	int32 LFootPickupVisualDelay = 0;
	int32 RFootPickupVisualDelay = 0;
	int32 UFootPickupVisualDelay = 0;
	
	int32 DFootDropVisualDelay = 0;
	int32 LFootDropVisualDelay = 0;
	int32 RFootDropVisualDelay = 0;
	int32 UFootDropVisualDelay = 0;
	
	// Cache held item data for rendering during drop visual transition
	FItem DFootCachedDropItem;
	FItem LFootCachedDropItem;
	FItem RFootCachedDropItem;
	FItem UFootCachedDropItem;

	// Helpers
	void EnsureComps();
	void RefreshMeshes();
	void RefreshPose(float InAlpha);                 // update transforms

	FVector HexToWorld(const FHex& H) const;         // grid → world

	void    StepProgram();                           // advance one instruction (or wait)
	void    ExpandResetAtPC();                       // turn Reset into MoveTo home location
	
	// Held item visualization (takes cached ID ref to avoid recreating visuals every tick)
	void UpdateHeldItemVisuals(UStaticMeshComponent*& HeldComp, bool bIsHolding, const FItem& HeldItem, const FName& FootSocketName, int32& CachedItemId);
	void ClearHeldItemVisuals(UStaticMeshComponent*& HeldComp);
	
	// Bond visualization helpers
	void GenerateBondedOrbMeshes(UStaticMeshComponent* HeldComp, const FItem& HeldItem);
	UStaticMesh* GetBondedOrbMesh() const;
	UMaterialInterface* GetBondedOrbMaterial() const;
	
	// Performance: cache OrbRenderer lookup
	UOrbRenderer* GetCachedOrbRenderer();
};

