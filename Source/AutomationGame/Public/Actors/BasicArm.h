#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Sim/HexTypes.h"     // FHex
#include "Sim/HexCoord.h"     // Axial<->XY helpers
#include "Sim/OrbTypes.h"    // FOrb
#include "BasicArm.generated.h"

class AOrbSimDriver;
class AGameTickManager;
class UStaticMeshComponent;
class USimWorldHex;

/** Discrete arm instructions (Opus Magnum–style, kept simple to start) */
UENUM(BlueprintType)
enum class EArmCmd : uint8
{
	NoOp      UMETA(DisplayName = "NoOp"),
	Grab      UMETA(DisplayName = "Grab"),
	Drop      UMETA(DisplayName = "Drop"),
	RotateCW  UMETA(DisplayName = "Rotate CW (60° * Arg)"),
	RotateCCW UMETA(DisplayName = "Rotate CCW (60° * Arg)"),
	Wait      UMETA(DisplayName = "Wait (ticks = Arg)"),
	Reset     UMETA(DisplayName = "Reset to Start (macro)"),
	Extend    UMETA(DisplayName = "Extend (+Arg, default 1)"),   // NEW
	Retract   UMETA(DisplayName = "Retract (-Arg, default 1)")   // NEW
};

/** One instruction + small integer argument (dir steps, wait ticks, etc.) */
USTRUCT(BlueprintType)
struct FArmInstr
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	EArmCmd Cmd = EArmCmd::NoOp;

	/** For Rotate/Wait, meaning depends on Cmd. Ignored otherwise. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = 0, UIMin = 0))
	int32 Arg = 0;

	// Added equality operators so TArray<FArmInstr>::operator== works
	bool operator==(const FArmInstr& Other) const
	{
		return Cmd == Other.Cmd && Arg == Other.Arg;
	}
	bool operator!=(const FArmInstr& Other) const
	{
		return !(*this == Other);
	}
};



UCLASS()
class AUTOMATIONGAME_API ABasicArm : public AActor
{
	GENERATED_BODY()

public:
	ABasicArm();

	// Engine
	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

	// --- Grid / placement ---
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid")
	AOrbSimDriver* Driver = nullptr;

	/** Axial tile where the arm’s pivot sits. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid")
	int32 Q = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid")
	int32 R = 0;

	/** Facing dir (0..5), where +1 means +60° CW. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid", meta = (ClampMin = 0, ClampMax = 5))
	uint8 FacingDir = 0;

	/** Convenience: set grid placement & driver in one call. */
	UFUNCTION(BlueprintCallable, Category = "Grid")
	void InitializeOnGrid(AOrbSimDriver* InDriver, int32 InQ, int32 InR, uint8 InFacing);

	// --- Visuals ---
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visual")
	UStaticMesh* BaseMesh = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visual")
	UStaticMesh* HandMesh = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visual")
	float BaseZ = 10.f;

	/** Tile-based reach: 1 = adjacent target tile center. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visual", meta = (ClampMin = 1))
	int32 ReachTiles = 1;

	/** (Legacy) scalar reach fraction — kept for compatibility with old placements. */
	// UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visual", AdvancedDisplay)
	// float HandReachUU = 0.6f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visual")
	FVector BaseScale = FVector(1.f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visual")
	FVector HandScale = FVector(0.4f);

	/** Additive yaw (degrees) applied to the base mesh only, after grid yaw + facing. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visual")
	float BaseYawOffsetDeg = 0.f;

	// Optional held-orb preview (purely visual)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visual")
	UStaticMesh* HeldOrbMesh = nullptr;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visual")
	UStaticMesh* MeshType1 = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visual")
	UStaticMesh* MeshType2 = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visual")
	UMaterialInterface* MaterialColor1 = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visual")
	UMaterialInterface* MaterialColor2 = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visual")
	UMaterialInterface* DefaultOrbMaterial = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visual")
	FVector HeldOrbScale = FVector(0.2f);

	// --- Program control (Blueprint-friendly) ---
	/** Program editable in BP (looped if bLoopProgram). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Program")
	TArray<FArmInstr> Program;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Program")
	bool bLoopProgram = true;

	/** Replace Program at runtime. */
	UFUNCTION(BlueprintCallable, Category = "Program")
	void SetProgram(const TArray<FArmInstr>& InProgram, bool bLoop = true);

	/** Control */
	UFUNCTION(BlueprintCallable, Category = "Program")
	void Start();

	UFUNCTION(BlueprintCallable, Category = "Program")
	void Stop();

	UFUNCTION(BlueprintCallable, Category = "Program")
	void ResetProgram();
	UFUNCTION(BlueprintCallable, Category = "Program")
	void SyncFromSim(const FArmState& SimState);  // also sets SimArmIndex

	UFUNCTION(BlueprintCallable, Category = "Program")
	void SyncFromCurrentSim();  // sync from current sim state

	UFUNCTION(BlueprintCallable, Category = "Program")
	void UpdateHeldClusterOnly();  // update only the held cluster visual	

	/** Check if the arm is currently animating (rotating, etc.) */
	UFUNCTION(BlueprintPure, Category = "Program")
	bool IsAnimating() const { return StepAlpha < 1.0f; }
	
	/** Get the current step alpha for debugging */
	UFUNCTION(BlueprintPure, Category = "Program")
	float GetStepAlpha() const { return StepAlpha; }

	/** For debugging: execute current instruction immediately (ignores Wait). */
	UFUNCTION(BlueprintCallable, Category = "Program")
	void ExecuteCurrent();

	/** Sync debug settings from the OrbSimDriver */
	UFUNCTION(BlueprintCallable, Category = "Debug")
	void SyncDebugSettingsFromDriver();
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sim")
	int32 SimArmIndex;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sim")
	bool bAffectSim = false;   // default false: sim is authoritative
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Orb Visuals")
	TMap<uint8, UStaticMesh*> OrbTypeToMesh;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Orb Visuals")
	TMap<uint8, UMaterialInterface*> OrbColorToMaterial;

public:
	void PushProgramToSim();
	void PushRuntimePropertiesToSim();   // NEW
	/** If true, cluster sub-orb offsets are multiplied by TileSize (grid units -> UU). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Orb Visuals")
	bool bScaleClusterOffsetsByTile = true;

	/** Extra scalar applied after (optional) tile scaling. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Orb Visuals", meta = (ClampMin = 0.01))
	float ClusterOffsetScale = 1.f;

	/** Show the container/static mesh for a cluster (sphere) in addition to sub-orbs. Usually false. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Orb Visuals")
	bool bShowClusterContainerMesh = false;
	UFUNCTION(BlueprintCallable, Category = "Program")
	void RebuildTapeFromProgram();          // runtime/editor safe manual push

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& Event) override;
#endif

protected:
	// Fixed tick hookup
	UFUNCTION()
	void HandleFixedTickSeconds(float StepSeconds);

private:
	// Components
	UPROPERTY(Transient) UStaticMeshComponent* BaseComp = nullptr;
	UPROPERTY(Transient) UStaticMeshComponent* HandComp = nullptr;
	UPROPERTY(Transient) UStaticMeshComponent* HeldOrbComp = nullptr;

	// --- Held cluster visual support ---
	UPROPERTY(Transient)
	TArray<UStaticMeshComponent*> HeldClusterComps;          // One per sub-orb
	TArray<FVector2D> HeldClusterLocalOffsets;               // Raw (unrotated) 2D offsets from FOrbData
	void RebuildHeldClusterVisual();                         // Build from HeldOrb
	void ClearHeldClusterVisual(bool bKeepParent = true);
	void UpdateHeldClusterTransforms(float CurrentYawDeg);   // Called each tick after RefreshPose

	// Sim ref
	UPROPERTY(Transient) USimWorldHex* Sim = nullptr;

	// State
	bool  bRunning = true;
	bool  bHolding = false;       // holding an orb from Sim
	FItem HeldOrb; // Holds the actual item (single orb or cluster)
	int32 PC = 0;                 // program counter
	int32 WaitLeft = 0;           // ticks left for a Wait

	// Smooth yaw animation across fixed steps
	float StepSecondsCur = 0.f;
	float StepAlpha = 0.f;        // 0..1 inside current fixed step (local)
	float LastYawDeg = 0.f;       // snapshot at step start
	float CurrYawDeg = 0.f;       // target yaw derived from FacingDir

	// Previous hand position for smooth interpolation
	FVector LastHandWorld = FVector::ZeroVector;
	FHex LastTargetHex = FHex(0, 0);

	// Remember where Reset should point us
	uint8 StartFacingDir = 0;     // captured at BeginPlay

	// --- Orb mesh/material mapping ---


	// Helpers
	void EnsureComps();
	void RefreshMeshes();
	void RefreshPose(float InAlpha);                 // update transforms
	FVector HexToWorld(const FHex& H) const;         // grid → world
	float   DirToYaw(uint8 Dir) const;               // grid yaw + 60° steps

	FHex    DirStep(uint8 Dir) const;
	FHex    ComputeTargetHex() const;

	void    StepProgram();                           // advance one instruction (or wait)
	void    ApplyRotateSteps(int32 StepsSigned);     // +CW, -CCW
	void    ExpandResetAtPC();                       // turn Reset into Drop(if holding)+Rotate

	// Visual held-orb toggle (purely cosmetic)
	void ShowHeldOrb(bool bShow);
	void AttachHeldOrb();
	void DetachHeldOrb();
	UStaticMesh* GetMeshForOrbType(uint8 OrbType) const;
	UMaterialInterface* GetMaterialForOrbColor(uint8 OrbColor) const;

	// NEW: track previous held item to know when to rebuild visual
	int32 LastHeldItemId = 0;
	int32 LastHeldOrbCount = 0;

	// NEW helper: update (attach/detach) held orb visual when state changes
	void UpdateHeldOrbVisual();

	UStaticMesh* ResolveOrbMesh(const FItem& Item) const;      // NEW
	UMaterialInterface* ResolveOrbMaterial(const FItem& Item) const; // NEW

	// Bonded orb mesh generation
	void GenerateBondedOrbMeshes();
	
	// Helper to get bonded orb properties from OrbRenderer
	UStaticMesh* GetBondedOrbMesh() const;
	UMaterialInterface* GetBondedOrbMaterial() const;
	float GetBondedOrbScale() const;


public:
	// If > 0 overrides ReachTiles * TileSize for grip distance (UU)
	UPROPERTY(EditAnywhere, Category = "Arm|Grip")
	float GripDistanceOverride = 0.f;

	// Vertical (Z) offset for the grip pivot relative to base component
	UPROPERTY(EditAnywhere, Category = "Arm|Grip")
	float GripZOffset = 0.f;

	// Extra lateral (Y) offset if your mesh forward axis is not centered
	UPROPERTY(EditAnywhere, Category = "Arm|Grip")
	float GripSideOffset = 0.f;

	// Additional offset for fine-tuning grip position (in world space)
	UPROPERTY(EditAnywhere, Category = "Arm|Grip")
	FVector GripLocationOffset = FVector::ZeroVector;

	// If true we still try to use a socket; if false we force procedural pivot usage
	UPROPERTY(EditAnywhere, Category = "Arm|Grip")
	bool bUseGripSocket = false;

	/** If the authored arm mesh forward is flipped, invert cluster visual layout (does not affect logic). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Orb Visuals")
	bool bInvertClusterVisualFacing = true;                // RESTORED

	/** Local rotation applied to the procedural grip pivot after its location update. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Arm|Grip")
	FRotator GripPivotLocalRotation = FRotator::ZeroRotator; // RESTORED

protected:
	UPROPERTY(Transient)
	USceneComponent* GripPivotComp = nullptr;

	FVector ComputeGripLocalOffset() const;

public:
	UPROPERTY(EditAnywhere, Category = "Debug")
	bool bDebugHandLocation = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	bool bDebugLogs = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	bool bDebugSpheres = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	bool bDebugLines = false;

	// Bonded orb mesh properties (get from OrbRenderer)
	UPROPERTY(EditAnywhere, Category = "Bonded Orbs")
	bool bShowBondedOrbs = true;
};
