#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Sim/HexTypes.h"     // FHex
#include "Sim/HexCoord.h"     // Axial<->XY helpers
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
	Reset     UMETA(DisplayName = "Reset to Start (macro)")
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
};

UCLASS(Blueprintable)
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
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visual", AdvancedDisplay)
	float HandReachUU = 0.6f;

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

	/** For debugging: execute current instruction immediately (ignores Wait). */
	UFUNCTION(BlueprintCallable, Category = "Program")
	void ExecuteCurrent();
	// BasicArm.h
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sim")
	bool bAffectSim = false;   // default false: sim is authoritative


protected:
	// Fixed tick hookup
	UFUNCTION()
	void HandleFixedTickSeconds(float StepSeconds);

private:
	// Components
	UPROPERTY(Transient) UStaticMeshComponent* BaseComp = nullptr;
	UPROPERTY(Transient) UStaticMeshComponent* HandComp = nullptr;
	UPROPERTY(Transient) UStaticMeshComponent* HeldOrbComp = nullptr;

	// Sim ref
	UPROPERTY(Transient) USimWorldHex* Sim = nullptr;

	// State
	bool  bRunning = true;
	bool  bHolding = false;       // holding an orb from Sim
	uint8 HeldKind = 0;           // simple “orb kind” carried while holding
	int32 PC = 0;                 // program counter
	int32 WaitLeft = 0;           // ticks left for a Wait

	// Smooth yaw animation across fixed steps
	float StepSecondsCur = 0.f;
	float StepAlpha = 0.f;        // 0..1 inside current fixed step (local)
	float LastYawDeg = 0.f;       // snapshot at step start
	float CurrYawDeg = 0.f;       // target yaw derived from FacingDir

	// Remember where Reset should point us
	uint8 StartFacingDir = 0;     // captured at BeginPlay

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
};
