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

UCLASS()
class AUTOMATIONGAME_API ABasicWalker : public AActor
{
	GENERATED_BODY()

public:
	ABasicWalker();

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

	/** Convenience: set grid placement & driver in one call. */
	UFUNCTION(BlueprintCallable, Category = "Grid")
	void InitializeOnGrid(AOrbSimDriver* InDriver, int32 InQ, int32 InR);

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

	/** Leg span - opposite feet span 8-10 tiles */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visual", meta = (ClampMin = 8, ClampMax = 10))
	int32 LegSpan = 9;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visual")
	FVector BaseScale = FVector(1.f);

	/** TileTraverseRate determines how fast it can move to its designated location (tiles per tick) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Movement", meta = (ClampMin = 0.1))
	float TileTraverseRate = 1.0f;

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

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sim")
	int32 SimWalkerIndex;

public:
	void PushProgramToSim();
	TArray<FWalkerInstr> BuildProgramFromInstructions(const TArray<FWalkerInstr>& InProgram) const;
	
	UFUNCTION(BlueprintCallable, Category = "Program")
	void RebuildProgramFromInstructions();          // runtime/editor safe manual push

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

	// State
	bool  bRunning = true;
	bool  bMoving = false;
	int32 PC = 0;                 // program counter
	int32 WaitLeft = 0;           // ticks left for a Wait

	// Movement interpolation
	FHex CurrentHex;
	FHex TargetHex;
	float MovementProgress = 0.0f;  // 0.0 to 1.0

	// Remember where Reset should point us
	FHex HomeHex;     // captured at BeginPlay

	// Helpers
	void EnsureComps();
	void RefreshMeshes();
	void RefreshPose(float InAlpha);                 // update transforms

	FVector HexToWorld(const FHex& H) const;         // grid → world

	void    StepProgram();                           // advance one instruction (or wait)
	void    ExpandResetAtPC();                       // turn Reset into MoveTo home location
};

