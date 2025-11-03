#pragma once
#include "AutomationGameMinimal.h"
#include "Components/ActorComponent.h"
#include "Components/PoseableMeshComponent.h"
#include "Sim/HexTypes.h"  // for FHex
#include "Sim/OrbTypes.h"  // for EOrbType
#include "HexPlacementComponent.generated.h"

class USimWorldHex; class APlayerController; class AActor; class AOrbSimDriver; class ATestLevelDirector;

UENUM(BlueprintType)
enum class EPlaceKind : uint8
{
	Input,
	Output,
	Belt,
	Arm,
	Bond,          // place a Bond glyph (uses BeltDir for its direction)
	Unbond,        // place an Unbond glyph (uses BeltDir for its direction)
	ClusterOutput, // place a ClusterOutput tile (cycles through registered templates)
	ClusterInput   // place a ClusterInput tile (spawns clusters from storage)
};

/** Click-to-place; uses OrbSimDriver's GridAnchor by default (single source of truth). */
UCLASS(ClassGroup = (Custom), meta = (BlueprintSpawnableComponent))
class AUTOMATIONGAME_API UHexPlacementComponent : public UActorComponent
{
	GENERATED_BODY()
public:
	UHexPlacementComponent();
	// Set the cluster template to a specific ID if it exists
	void SetClusterTemplateToId(uint8 TemplateId);
	/** Thing to place */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Place") EPlaceKind PlaceKind = EPlaceKind::Input;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Place") uint8 BeltDir = 0;

	/** Optional anchor override to NOT use driver. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid") bool bOverrideAnchor = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid", meta = (EditCondition = "bOverrideAnchor")) AActor* AnchorOverride = nullptr;

	/** Poll mouse directly (no input mappings needed). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Input") bool bUseInputPolling = false;

	/** Debug overlay: prints and draws pick positions. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug") bool bDebug = true;

	/** Optional base material for hologram preview (should be translucent & unlit). */
	UPROPERTY(EditAnywhere, Category = "Placement Preview")
	UMaterialInterface* PreviewBaseMaterial = nullptr;

	/** Optional material specifically for skeletal mesh previews (should have skeletal mesh usage flag). */
	UPROPERTY(EditAnywhere, Category = "Placement Preview")
	UMaterialInterface* SkeletalPreviewMaterial = nullptr;

	/** Mesh used for Input/Output tiles if no board tile mesh is available. */
	UPROPERTY(EditAnywhere, Category = "Placement Preview")
	UStaticMesh* GenericTilePreviewMesh = nullptr;

	/** Optional mesh specifically for belt preview (falls back to GenericTilePreviewMesh). */
	UPROPERTY(EditAnywhere, Category = "Placement Preview")
	UStaticMesh* BeltMeshPreview = nullptr;

	/** Optional material specifically for belt preview. */
	UPROPERTY(EditAnywhere, Category = "Placement Preview")
	UMaterialInterface* BeltMaterialPreview = nullptr;

	/** Optional mesh specifically for arm base preview (falls back to GenericTilePreviewMesh). */
	UPROPERTY(EditAnywhere, Category = "Placement Preview")
	UStaticMesh* ArmBasePreviewMesh = nullptr;

	/** Use actual arm skeletal mesh from BasicArm class instead of separate preview mesh. */
	UPROPERTY(EditAnywhere, Category = "Placement Preview")
	bool bUseActualArmAssets = true;
	UPROPERTY(EditAnywhere, Category = "Placement Preview")
	UStaticMesh* BondPreviewMesh = nullptr;

	/** Optional mesh specifically for unbond preview (falls back to GenericTilePreviewMesh). */
	UPROPERTY(EditAnywhere, Category = "Placement Preview")
	UStaticMesh* UnbondPreviewMesh = nullptr;

	/** Optional material for the fused Bond preview (expects scalar params: TileRadius, Span, PadW, PadH, FlatTop, Tint). */
	UPROPERTY(EditAnywhere, Category = "Placement Preview|Bond")
	UMaterialInterface* BondPreviewMaterial = nullptr;

	/** Relative pad extents (multipliers of TileRadius) for decorative squares in Bond material. */
	UPROPERTY(EditAnywhere, Category = "Placement Preview|Bond")
	float BondPadWidthFactor = 0.22f;

	UPROPERTY(EditAnywhere, Category = "Placement Preview|Bond")
	float BondPadHeightFactor = 0.18f;

	/** Scale applied to preview (slightly smaller than real). */
	UPROPERTY(EditAnywhere, Category = "Placement Preview")
	float PreviewScale = 0.95f;

	/** Vertical Z offset so the hologram floats slightly above ground. */
	UPROPERTY(EditAnywhere, Category = "Placement Preview")
	float PreviewZOffset = 4.f;

	/** Additional yaw applied to belt preview meshes to compensate for authoring forward (+X expected). */
	UPROPERTY(EditAnywhere, Category = "Placement Preview")
	float BeltVisualYawOffsetDeg = -90.f;

	// Bond preview orientation fix (if your bond SDF always assumes pointy-top)
	UPROPERTY(EditAnywhere, Category = "Placement Preview|Bond")
	bool bBondPreviewAssumesPointyTop = true;

	UPROPERTY(EditAnywhere, Category = "Placement Preview|Bond")
	float BondOrientationFixDegPointyToFlat = 30.f;   // rotation to apply when grid is FlatTop

	UPROPERTY(EditAnywhere, Category = "Placement Preview|Bond")
	float BondOrientationFixDegFlatToPointy = -30.f;  // rotation if inverse assumption ever needed

	// Add a preview fix angle so the arm placement mesh can be rotated without changing sim logic.
	UPROPERTY(EditAnywhere, Category="Placement Preview")
	float ArmPreviewFacingFixDeg = 180.f;

	// Input API
	UFUNCTION(BlueprintCallable, Category = "Place") void HandlePlace(bool bPressed);
	UFUNCTION(BlueprintCallable, Category = "Place") void HandleRemove(bool bPressed);
	UFUNCTION(BlueprintCallable, Category = "Place") void RotateCW();
	UFUNCTION(BlueprintCallable, Category = "Place") void RotateCCW();
	UFUNCTION(BlueprintCallable, Category = "Place") void SetPlaceKind(EPlaceKind NewKind);
	
	// Type cycling (works for both Input and Output based on current placement mode)
	UFUNCTION(BlueprintCallable, Category = "Place|Type") void CycleNextType();
	UFUNCTION(BlueprintCallable, Category = "Place|Type") void CyclePreviousType();
	
	// Blueprint mode support
	UFUNCTION(BlueprintCallable, Category = "Place|Blueprint") void SetBlueprintMode(bool bInBlueprintMode) { bDisablePreview = bInBlueprintMode; }
	
	// Hover detection support
	UFUNCTION(BlueprintPure, Category = "Place") bool IsHoverValid() const { return bHoverValid; }
	UFUNCTION(BlueprintPure, Category = "Place") FHex GetHoverHex() const { return HoverHex; }
	
	/** Cycle through glyph types (Belt, Bond, Unbond) */
	UFUNCTION(BlueprintCallable, Category = "Place|Glyph") void CycleGlyphType();
	
	/** Cycle to next orb type for input placement */
	UFUNCTION(BlueprintCallable, Category = "Place|Input") void CycleInputOrbType();
	
	/** Get the current orb type for input placement */
	UFUNCTION(BlueprintPure, Category = "Place|Input") EOrbType GetCurrentInputOrbType() const;
	
	/** Get the current orb type for output placement */
	UFUNCTION(BlueprintPure, Category = "Place|Output") EOrbType GetCurrentOutputOrbType() const;
	
	/** Get the current cluster template ID for ClusterOutput placement */
	UFUNCTION(BlueprintPure, Category = "Place|ClusterOutput") uint8 GetCurrentClusterTemplateId() const;
	
	FVector HexToWorld(const FHex& H) const;
protected:
	virtual void BeginPlay() override;
	virtual void TickComponent(float DeltaTime, enum ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

private:
	AOrbSimDriver* Driver = nullptr;
	USimWorldHex* Sim = nullptr;
	APlayerController* PC = nullptr;
	bool bWasClickingL = false;
	bool bWasClickingR = false;

	// --- New preview state ---
	UPROPERTY(Transient) UStaticMeshComponent* PreviewComp = nullptr;
	UPROPERTY(Transient) UPoseableMeshComponent* SkeletalPreviewComp = nullptr;
	UPROPERTY(Transient) UMaterialInstanceDynamic* PreviewMID = nullptr;
	FHex HoverHex{ INT32_MAX, INT32_MAX };   // invalid sentinel
	bool bHoverValid = false;
	FVector LastHoverWorld = FVector::ZeroVector;

	// Input orb type management
	UPROPERTY(EditAnywhere, Category = "Input", meta = (ClampMin = "0", ClampMax = "5"))
	int32 CurrentInputOrbTypeIndex = 0;
	
	// Output orb type management
	UPROPERTY(EditAnywhere, Category = "Output", meta = (ClampMin = "0", ClampMax = "5"))
	int32 CurrentOutputOrbTypeIndex = 0;
	
	// Cluster template management
	UPROPERTY(EditAnywhere, Category = "ClusterOutput", meta = (ClampMin = "0"))
	int32 CurrentClusterTemplateIndex = 0;
	
	

	bool HitPlane(FVector& OutWorld) const;
	void DoPlaceAt(const FVector& World);
	void DoRemoveAt(const FVector& World);

	void EnsurePreviewComponent();
	void EnsureSkeletalPreviewComponent();
	void UpdateHoverHex();          // derive HoverHex from cursor each tick
	void UpdatePreviewVisual();     // position + mesh + color
	void UpdateArmPreviewBoneRotations(float Yaw);
	bool CanPlaceAt(const FHex& Hex) const;
	UStaticMesh* SelectPreviewMesh() const;
	USkeletalMesh* SelectSkeletalPreviewMesh() const;
	
	/** Get the list of orb types that have materials assigned (not using DefaultOrbMaterial) */
	TArray<EOrbType> GetValidOrbTypes() const;
	
	/** Get the list of registered cluster template IDs */
	TArray<uint8> GetValidClusterTemplateIds() const;

	FORCEINLINE AActor* GetAnchor() const;

public:
	// Public methods for accessing actual meshes from the world
	UStaticMesh* GetActualBeltMesh() const;
	UMaterialInterface* GetActualBeltMaterial() const;
	UStaticMesh* GetActualOrbMesh() const;
	UMaterialInterface* GetActualOrbMaterial() const;
	UStaticMesh* GetActualOutputOrbMesh() const;
	UMaterialInterface* GetActualOutputOrbMaterial() const;
	UPROPERTY(EditAnywhere, Category = "Scenario") bool bAutoScenarioCapture = true;
	UPROPERTY(Transient) ATestLevelDirector* CachedTLD = nullptr;
	
	// Blueprint mode flag to disable preview
	bool bDisablePreview = false;
};
