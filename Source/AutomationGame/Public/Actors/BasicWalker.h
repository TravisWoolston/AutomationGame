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

	USkeletalMeshComponent* GetBodyComponent() const { return BodyComp; }

	/** World locations of assigned tiles for each foot (updated from simulation) */
	UPROPERTY(BlueprintReadOnly, Category = "Foot Positions")
	FVector DFootAssignedTileWorldLocation = FVector::ZeroVector;

	UPROPERTY(BlueprintReadOnly, Category = "Foot Positions")
	FVector LFootAssignedTileWorldLocation = FVector::ZeroVector;

	UPROPERTY(BlueprintReadOnly, Category = "Foot Positions")
	FVector RFootAssignedTileWorldLocation = FVector::ZeroVector;

	UPROPERTY(BlueprintReadOnly, Category = "Foot Positions")
	FVector UFootAssignedTileWorldLocation = FVector::ZeroVector;

	/** Get bone location in world space by bone name */
	UFUNCTION(BlueprintCallable, Category = "Bone Access")
	FVector GetBoneLocationByName(const FName& BoneName, bool bWorldSpace = true) const;

	/** Get bone location in world space by bone name (string version for Blueprint convenience) */
	UFUNCTION(BlueprintCallable, Category = "Bone Access", meta = (CallInEditor = "true"))
	FVector GetBoneLocationByNameString(const FString& BoneName, bool bWorldSpace = true) const;

	/** Get bone location in component space by bone name */
	UFUNCTION(BlueprintCallable, Category = "Bone Access")
	FVector GetBoneLocationComponentSpace(const FName& BoneName) const;

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
	
	// Foot positions for visual sync
	FHex DFootHex;
	FHex LFootHex;
	FHex RFootHex;
	FHex UFootHex;
	
	// Foot height offsets for visual sync
	float DFootHeightOffset = 0.f;
	float LFootHeightOffset = 0.f;
	float RFootHeightOffset = 0.f;
	float UFootHeightOffset = 0.f;
	
	// Foot step states (to determine which feet are planted)
	EFootStepState DFootStepState = EFootStepState::Planted;
	EFootStepState LFootStepState = EFootStepState::Planted;
	EFootStepState RFootStepState = EFootStepState::Planted;
	EFootStepState UFootStepState = EFootStepState::Planted;
	

	// Helpers
	void EnsureComps();
	void RefreshMeshes();
	void RefreshPose(float InAlpha);                 // update transforms

	FVector HexToWorld(const FHex& H) const;         // grid → world

	void    StepProgram();                           // advance one instruction (or wait)
	void    ExpandResetAtPC();                       // turn Reset into MoveTo home location
	
	void    UpdateAssignedTileWorldLocations();      // update public world location properties from foot hex positions
};

