#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Sim/HexCoord.h"
#include "Sim/HexTypes.h"
#include "Editor/MachineBlueprint.h"
#include "Editor/MachineEditor_TickHook.h"
#include "MachineEditor.generated.h"

class UInstancedStaticMeshComponent;
class USceneComponent;
class UStaticMesh;
class UMaterialInterface;
class USimWorldHex;
class AGameTickManager;

UCLASS(Blueprintable)
class AUTOMATIONGAME_API AMachineEditor : public AActor
{
	GENERATED_BODY()
public:
	AMachineEditor();

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	// ---- Grid ----
	UPROPERTY(EditAnywhere, Category = "Grid")
	float TileSizeUU = 300.f;

	UPROPERTY(EditAnywhere, Category = "Grid")
	EHexOrientation GridOrientation = EHexOrientation::PointyTop;

	UPROPERTY(EditAnywhere, Category = "Grid", meta = (ClampMin = -360.0, ClampMax = 360.0))
	float GridYawDeg = 0.f;

	UPROPERTY(VisibleAnywhere, Category = "Grid")
	USceneComponent* GridAnchor = nullptr;

	UFUNCTION(BlueprintCallable, Category = "Grid")
	FVector HexToWorld(const FHex& H) const;

	// ---- Board / Terrain (local to editor) ----
	UPROPERTY(EditAnywhere, Category = "Board")
	int32 InitialRingRadius = 6;

	UPROPERTY(EditAnywhere, Category = "Board")
	bool bGenerateOnBeginPlay = true;

	UFUNCTION(BlueprintCallable, Category = "Board")
	bool HasHex(FHex H) const { return Terrain.Contains(H); }

	UFUNCTION(BlueprintCallable, Category = "Board")
	void AddHex(FHex H);

	UFUNCTION(BlueprintCallable, Category = "Board")
	void RemoveHex(FHex H);

	// ---- Visuals ----
	UPROPERTY(EditAnywhere, Category = "Visual|Tiles")
	UStaticMesh* TileMesh = nullptr;

	UPROPERTY(EditAnywhere, Category = "Visual|Tiles")
	UMaterialInterface* TileMaterial = nullptr;

	UPROPERTY(EditAnywhere, Category = "Visual|Belts")
	UStaticMesh* BeltMesh = nullptr;

	UPROPERTY(EditAnywhere, Category = "Visual|Belts")
	UMaterialInterface* BeltMaterial = nullptr;

	UPROPERTY(EditAnywhere, Category = "Visual|Belts")
	float BeltZOffset = 10.f;

	UPROPERTY(EditAnywhere, Category = "Visual|Belts")
	FVector BeltScale = FVector(1.f, 1.f, 1.f);

	UPROPERTY(EditAnywhere, Category = "Debug")
	bool bAlwaysDrawBeltArrows = true;

	UPROPERTY(EditAnywhere, Category = "Debug")
	float DebugArrowLenScale = 0.6f;

	UPROPERTY(EditAnywhere, Category = "Debug")
	float DebugArrowThickness = 6.f;

	UFUNCTION(CallInEditor, BlueprintCallable, Category = "Debug")
	void Rebuild();

	// ---- Active Blueprint ----
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Blueprint")
	UMachineBlueprint* ActiveBlueprint = nullptr;

	UFUNCTION(BlueprintCallable, Category = "Blueprint")
	void NewBlueprint(const FText& Name);

	UFUNCTION(BlueprintCallable, Category = "Blueprint")
	void ClearBlueprint();

	UFUNCTION(BlueprintCallable, Category = "Blueprint")
	void PlacePart(FHex Hex, EMachinePartKind Kind, uint8 Dir, int32 ParamI = 0, float ParamF = 0.f);

	UFUNCTION(BlueprintCallable, Category = "Blueprint")
	bool RemovePartAt(FHex Hex);

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Blueprint")
	void GetAllParts(TArray<FMachinePart>& OutParts) const;

	UFUNCTION(BlueprintCallable, Category = "Blueprint")
	void RotatePartAt(FHex Hex, bool bCW);

	// ---- Play controls (delegates to GameTickManager if present) ----
	UPROPERTY(EditAnywhere, Category = "Play", meta = (ClampMin = 0.1, ClampMax = 60.0))
	float LocalTicksPerSecond = 2.f;

	UFUNCTION(BlueprintCallable, Category = "Play")
	void Play();

	UFUNCTION(BlueprintCallable, Category = "Play")
	void Pause();

	UFUNCTION(BlueprintCallable, Category = "Play")
	void Stop();

	UFUNCTION(BlueprintCallable, Category = "Play")
	void StepTicks(int32 NumTicks = 1);

private:
	// Rendering
	UPROPERTY(Transient) UInstancedStaticMeshComponent* TileISM = nullptr;
	UPROPERTY(Transient) UInstancedStaticMeshComponent* BeltISM = nullptr;

	// Local editor board
	TSet<FHex> Terrain;
	int32 BoardRevision = 0;
	int32 LastDrawnBoardRevision = -1;
	int32 LastDrawnBlueprintVersion = -1;
	int32 BlueprintVersionCounter = 0;

	// tick hookup
	UPROPERTY(Transient) FMachineEditorTickState TickState;

	// impl
	void EnsureComponents();
	void GenerateFilledRadius(int32 Radius);
	void RebuildInternal();
	void RebuildTiles();
	void RebuildBelts();
	void DrawBeltArrow(const FVector& Center, float TileRadiusUU, float YawDeg);
	void MarkBlueprintDirty();

	// Delegate handlers (need UFUNCTION for AddDynamic; kept private)
	UFUNCTION()
	void HandleFixedTickSeconds(float StepSeconds);

	UFUNCTION()
	void HandleFixedTick(int32 Steps);
};
