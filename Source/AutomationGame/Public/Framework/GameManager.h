#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Actors/BasicArm.h"
#include "Sim/ArmTypes.h"
#include "Templates/Tuple.h"
#include "Sim/SimWorldHex.h"
#include "GameManager.generated.h"

class AGameTickManager;
class UInstancedStaticMeshComponent;
class UStaticMesh;
class UMaterialInterface;
class AOrbSimDriver;
class UGoldbergGrid; // NEW
class UTexture2D;
class UArmTimelineWidget;

/** Canonical representation of a cluster structure for storage and comparison */
struct AUTOMATIONGAME_API FClusterCanonicalKey
{
	/** Normalized orb data (sorted by dQ, then dR, then Type) */
	TArray<FOrbData> CanonicalOrbs;

	/** Bond pairs (indices into CanonicalOrbs array, sorted) */
	TArray<TPair<int32, int32>> CanonicalBonds;

	/** Comparison operators for use in TMap */
	bool operator==(const FClusterCanonicalKey& Other) const;
};

FORCEINLINE uint32 GetTypeHash(const FClusterCanonicalKey& Key);
/** Lightweight coordinator that also renders a simple hex or Goldberg grid via ISM. */
UCLASS(BlueprintType)
class AUTOMATIONGAME_API AGameManager : public AActor
{
	GENERATED_BODY()
public:
	AGameManager();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Factory|Arms")
	TSubclassOf<ABasicArm> BasicArmClass;

	/** Optional default program to use when placing from input. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Factory|Arms")
	TArray<FArmInstr> DefaultArmProgram;

	UFUNCTION(BlueprintCallable, Category = "Factory|Arms")
	ABasicArm* SpawnBasicArmAt(const FHex& Hex, uint8 FacingDir, const TArray<FArmInstr>& Program, bool bLoop = true);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Refs")
	AGameTickManager* TickManager = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Refs")
	AOrbSimDriver* OrbSimDriver = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Refs")
	TSubclassOf<AOrbSimDriver> OrbSimDriverClass;

	// ------------ Startup ------------
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Startup")
	FString InitialMapToLoad;

	// ------------ Cluster Generation ------------
	/** Difficulty value for generating random target cluster structures. TotalOrbs + NumOfUniqueOrbTypes = DifficultyValue */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cluster Generation", meta = (ClampMin = "4"))
	int32 DifficultyValue = 6;
	
	/** Base difficulty required to unlock the first orb type beyond BaseA (BaseA is always available, BaseB unlocks here) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cluster Generation", meta = (ClampMin = "0"))
	int32 BaseUnlockDifficulty = 6;
	
	/** Multiplier for exponential scaling of unlock thresholds. Higher values create faster progression. Default 1.5 gives thresholds: 6, 9, 13.5≈14, 20.25≈20 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cluster Generation", meta = (ClampMin = "1.0"))
	float DifficultyScaleMult = 1.5f;

	/** Scale factor for cluster icon generation. Higher values create larger orbs and bonds. Default 1.0 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cluster Generation", meta = (ClampMin = "0.1"))
	float ClusterIconScale = 1.0f;

	// ------------ Grid Visual ------------
	/** Center-to-center spacing for hexes (was TileSizeUU). */
	UPROPERTY(EditAnywhere, Category = "Grid", meta = (ClampMin = "1.0"))
	float TileSizeUU = 120.f; // Legacy name kept for serialization

	/** Alias for TileSizeUU for clarity when working in sphere mode. */
	UFUNCTION(BlueprintPure, Category="Grid") FORCEINLINE float GetTileSpacing() const { return TileSizeUU; }

	/** PointyTop vs FlatTop orientation of the hex grid. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid")
	EHexOrientation GridOrientation = EHexOrientation::PointyTop;

	/** World yaw (deg) for the grid around the anchor. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid", meta = (ClampMin = -360.0, ClampMax = 360.0))
	float GridYawDeg = 0.f;

	/** Radius in hexes (axial distance). */
	UPROPERTY(EditAnywhere, Category = "Grid", meta = (ClampMin = "1", UIMin = "1"))
	int32 Radius = 12;

	/** Optional anchor actor for the grid; if null uses this actor. */
	UPROPERTY(EditAnywhere, Category = "Grid")
	AActor* GridAnchor = nullptr;

	UPROPERTY(EditAnywhere, Category = "Grid")
	UStaticMesh* TileMesh = nullptr;

	/** Custom mesh for Input and ClusterInput tiles. If null, uses TileMesh. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid")
	UStaticMesh* CustomInputTileMesh = nullptr;

	UPROPERTY(EditAnywhere, Category = "Grid")
	UMaterialInterface* TileMaterial = nullptr;

	/** Auto-rebuild when moved farther than this threshold. */
	UPROPERTY(EditAnywhere, Category = "Grid")
	bool bAutoExpandAtRuntime = true;

	UPROPERTY(EditAnywhere, Category = "Grid", meta = (EditCondition = "bAutoExpandAtRuntime"))
	float RecenterThresholdUU = 800.f;

	/** Scale multiplier for ClusterOutput tiles (larger than regular Output tiles). */
	UPROPERTY(EditAnywhere, Category = "Grid", meta = (ClampMin = "1.0", ClampMax = "3.0"))
	float ClusterOutputScale = 1.3f;

	/** If true, map hex grid onto a sphere surface instead of a plane. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Sphere")
	bool bSphereGrid = false;

	/** Multiplier for automatic sphere radius: AutoSphereRadius = TileSizeUU * Radius * SphereRadiusScale. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Sphere", meta=(EditCondition="bSphereGrid", ClampMin="0.1"))
	float SphereRadiusScale = 1.0f;

	/** If true, sphere radius derives from TileSpacing*Radius*SphereRadiusScale; otherwise uses SphereRadiusOverrideUU. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Sphere", meta=(EditCondition="bSphereGrid"))
	bool bAutoSphereRadius = true;

	/** Explicit sphere radius to use when bAutoSphereRadius is false. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Sphere", meta=(EditCondition="bSphereGrid && !bAutoSphereRadius", ClampMin="10"))
	float SphereRadiusOverrideUU = 3000.f;

	/** Use Goldberg (geodesic) sphere instead of projected hex disc. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Sphere", meta=(EditCondition="bSphereGrid"))
	bool bUseGoldbergSphere = false; // NEW

	/** Subdivision frequency for Goldberg sphere (>=1). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Sphere", meta=(EditCondition="bSphereGrid && bUseGoldbergSphere", ClampMin="1", UIMin="1"))
	int32 GoldbergFrequency = 3; // NEW

	/** Current effective sphere radius (auto or override). */
	UFUNCTION(BlueprintPure, Category="Grid|Sphere")
	float GetEffectiveSphereRadius() const { return bAutoSphereRadius ? (TileSizeUU * Radius * SphereRadiusScale) : SphereRadiusOverrideUU; }

	// ------------ Arm Timeline System ------------
	// NOTE: These settings control ALL arm timeline widgets globally
	// Individual arm timeline properties are private and controlled by GameManager
	
	/** Command textures for arm timeline display */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Arm Timeline")
	TMap<EArmCmd, UTexture2D*> CommandTextures;

	/** Default texture for unknown commands */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Arm Timeline")
	UTexture2D* DefaultCommandTexture = nullptr;

	/** Widget class for arm timeline display */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Arm Timeline")
	TSubclassOf<UUserWidget> ArmTimelineWidgetClass;

	/** Whether to show timeline widgets for arms */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Arm Timeline")
	bool bShowArmTimelines = false;

	/** Scale for timeline widgets */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Arm Timeline")
	float TimelineWidgetScale = 1.0f;

	/** Offset for timeline widgets from arm position */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Arm Timeline")
	FVector TimelineWidgetOffset = FVector(0.0f, 0.0f, 150.0f);

	/** Whether to rotate widgets for top-down view */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Arm Timeline")
	bool bRotateWidgetsForTopDown = true;

	/** Size of command icons in timeline widgets */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Arm Timeline")
	float TimelineIconSize = 48.0f;

	/** Background color for command icons */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Arm Timeline")
	FLinearColor TimelineIconBackgroundColor = FLinearColor(0.2f, 0.2f, 0.2f, 0.8f);

	/** Normal command color */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Arm Timeline")
	FLinearColor TimelineNormalCommandColor = FLinearColor::White;

	/** Current command color */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Arm Timeline")
	FLinearColor TimelineCurrentCommandColor = FLinearColor::Yellow;

	/** Completed command color */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Arm Timeline")
	FLinearColor TimelineCompletedCommandColor = FLinearColor::Green;

	/** Whether to use smart positioning to avoid blocking line of sight */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Arm Timeline")
	bool bUseSmartTimelinePositioning = true;

	/** Maximum number of command icons to display in timeline */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Arm Timeline")
	int32 MaxTimelineIcons = 10;

	/** Whether to use camera zoom-based scaling for timeline widgets */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Arm Timeline")
	bool bUseDistanceBasedScaling = true;

	/** Minimum camera arm length for timeline scaling (close zoom) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Arm Timeline")
	float TimelineMinDistance = 600.0f;

	/** Maximum camera arm length for timeline scaling (far zoom) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Arm Timeline")
	float TimelineMaxDistance = 6000.0f;

	/** Minimum scale when timeline is close (zoomed in) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Arm Timeline")
	float TimelineMinScale = 0.2f;

	/** Maximum scale when timeline is far (zoomed out) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Arm Timeline")
	float TimelineMaxScale = 2.0f;

	/** Whether to avoid timeline overlaps */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Arm Timeline")
	bool bAvoidTimelineOverlap = true;

	/** Lerp speed for timeline position updates (units per second) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Arm Timeline")
	float TimelineLerpSpeed = 5.0f;

	/** Adjust timeline position to avoid overlap with other timelines */
	UFUNCTION(BlueprintCallable, Category = "Arm Timeline")
	FVector AdjustTimelinePositionForOverlap(class UArmTimelineWidget* Timeline, const FVector& DesiredPosition, const FVector& ArmLocation);

	/** Set command textures for timeline display */
	UFUNCTION(BlueprintCallable, Category = "Arm Timeline")
	void SetCommandTextures(const TMap<EArmCmd, UTexture2D*>& Textures);

	/** Set the active timeline for editing */
	UFUNCTION(BlueprintCallable, Category = "Timeline Editor")
	void SetActiveTimeline(class UArmTimelineWidget* Timeline);

	/** Get the active timeline for editing */
	UFUNCTION(BlueprintCallable, Category = "Timeline Editor")
	class UArmTimelineWidget* GetActiveTimeline() const;

	/** Get the active arm being edited */
	UFUNCTION(BlueprintCallable, Category = "Timeline Editor")
	class ABasicArm* GetActiveArm() const;

	/** Active timeline widget being edited (public for direct access) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Timeline Editor", Transient)
	class UArmTimelineWidget* ActiveTimelineWidget = nullptr;

	/** Get texture for a specific command */
	UFUNCTION(BlueprintPure, Category = "Arm Timeline")
	UTexture2D* GetTextureForCommand(EArmCmd Command) const;

	/** Toggle timeline visibility for all arms */
	UFUNCTION(BlueprintCallable, Category = "Arm Timeline")
	void SetTimelineVisibility(bool bVisible);

	/** Update timeline for a specific arm */
	UFUNCTION(BlueprintCallable, Category = "Arm Timeline")
	void UpdateArmTimeline(ABasicArm* Arm, int32 CommandIndex, float Progress);

	/** Update all existing arms with the current widget class */
	UFUNCTION(BlueprintCallable, Category = "Arm Timeline")
	void UpdateAllArmTimelineWidgets();

	// ------------ Editor / Control ------------
	UFUNCTION(BlueprintCallable, Category = "Grid")
	void SetGridRadius(int32 NewRadius);

	UFUNCTION(BlueprintCallable, Category = "Grid")
	void SetTileMeshAndMaterial(UStaticMesh* Mesh, UMaterialInterface* Mat);

	UFUNCTION(CallInEditor, BlueprintCallable, Category = "Grid")
	void Rebuild();

	UFUNCTION(BlueprintCallable, Category="Grid")
	FVector GetWorldForHex(const FHex& H) const;

	// ------------ Cluster Generation ------------
	/** Generate a random cluster structure based on DifficultyValue. Returns the template ID that was registered. */
	UFUNCTION(BlueprintCallable, Category = "Cluster Generation")
	int32 GenerateRandomCluster();

	// ------------ Cluster Storage ------------
	/** Add a processed cluster to storage or increment its count if already exists (C++ only) */
	void AddClusterToStorage(const USimWorldHex::FClusterTemplate& ClusterTemplate);

	/** Get the count for a specific cluster structure (C++ only) */
	int32 GetClusterCount(const USimWorldHex::FClusterTemplate& ClusterTemplate) const;

	/** Get the count for a cluster structure by template ID (C++ only) */
	int32 GetClusterCountByTemplateId(uint8 TemplateId) const;

	/** Decrement cluster count from storage by template ID, returns true if successful (C++ only) */
	bool DecrementClusterFromStorageByTemplateId(uint8 TemplateId);

	/** Decrement cluster count from storage by canonical key, returns true if successful (C++ only) */
	bool DecrementClusterFromStorageByCanonicalKey(const FClusterCanonicalKey& Key);

	/** Get the stored cluster template by canonical key, returns nullptr if not found (C++ only) */
	const USimWorldHex::FClusterTemplate* GetStoredClusterTemplate(const FClusterCanonicalKey& Key) const;

	/** Check if a cluster structure exists in storage (C++ only) */
	bool DoesClusterExistInStorage(const USimWorldHex::FClusterTemplate& ClusterTemplate) const;

	/** Check if a cluster structure exists in storage by template ID using cached canonical key (C++ only) */
	bool DoesClusterExistInStorageByTemplateId(uint8 TemplateId) const;

	/** Cache the canonical key for a template ID (should be called when template is registered) (C++ only) */
	void CacheCanonicalKeyForTemplate(uint8 TemplateId, const USimWorldHex::FClusterTemplate& ClusterTemplate);

	/** Get all stored cluster templates and their counts (C++ only) */
	void GetStoredClusters(TArray<USimWorldHex::FClusterTemplate>& OutTemplates, TArray<int32>& OutCounts) const;
	
	/** Find or register a TemplateId for a given cluster template (C++ only) */
	uint8 FindOrRegisterTemplateId(const USimWorldHex::FClusterTemplate& ClusterTemplate);

	/** Clear all stored clusters (C++ only) */
	void ClearClusterStorage();

	/** Clear cached cluster icons (C++ only) */
	void ClearClusterIcons();

	/** Generate a canonical key for a cluster template (C++ only) */
	FClusterCanonicalKey GenerateCanonicalKey(const USimWorldHex::FClusterTemplate& ClusterTemplate) const;

	/** Generate an icon texture for a cluster template (C++ only) */
	UTexture2D* GenerateClusterIcon(const USimWorldHex::FClusterTemplate& ClusterTemplate);

	/** Get the icon texture for a stored cluster (generates if needed) (C++ only) */
	UTexture2D* GetClusterIcon(const USimWorldHex::FClusterTemplate& ClusterTemplate);

protected:
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
#if WITH_EDITOR
	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

private:
	void AutoFindRefs();
	void TryLoadInitial();
	void EnsureISM();
	void BuildGridInstances();
	void SyncOrbSimDriverParameters(); // Sync OrbSimDriver to match GameManager parameters

	// state
	UPROPERTY(Transient) UInstancedStaticMeshComponent* ISM = nullptr;
	FVector LastGridCenter = FVector::ZeroVector;
	UPROPERTY(Transient) UGoldbergGrid* GoldbergGrid = nullptr; // NEW

	// Cluster storage (NOT UPROPERTY - they contain non-USTRUCT types)
	TMap<FClusterCanonicalKey, int32> ClusterCounts;

	TMap<FClusterCanonicalKey, USimWorldHex::FClusterTemplate> StoredClusterTemplates;

	TMap<FClusterCanonicalKey, UTexture2D*> ClusterIcons;

	// Cached canonical keys by template ID (for efficient lookup without regenerating keys)
	TMap<uint8, FClusterCanonicalKey> TemplateIdToCanonicalKey;
};
