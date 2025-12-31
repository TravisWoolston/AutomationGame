#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Actors/BasicArm.h"
#include "Actors/BasicWalker.h"
#include "Sim/ArmTypes.h"
#include "Sim/WalkerTypes.h"
#include "Templates/Tuple.h"
#include "Sim/SimWorldHex.h"
#include "Framework/WaveTypes.h"
#include "GameManager.generated.h"

/** Walker foot selection for calibration */
UENUM(BlueprintType)
enum class EWalkerFoot : uint8
{
	DFoot     UMETA(DisplayName = "D Foot"),
	LFoot     UMETA(DisplayName = "L Foot"),
	RFoot     UMETA(DisplayName = "R Foot"),
	UFoot     UMETA(DisplayName = "U Foot")
};

/** Grid mode selection */
UENUM(BlueprintType)
enum class EGridMode : uint8
{
	Planar      UMETA(DisplayName = "Planar Grid"),
	Goldberg    UMETA(DisplayName = "Goldberg Grid")
};

class AGameTickManager;
class UInstancedStaticMeshComponent;
class UStaticMesh;
class UMaterialInterface;
class AOrbSimDriver;
class UGoldbergGrid; // NEW
class UTexture2D;
class UTextureRenderTarget2D;
class UArmTimelineWidget;
class UPoseableMeshComponent;
class ATriggerTile;

/** Persistent walker calibration data (saved between sessions) */
USTRUCT()
struct AUTOMATIONGAME_API FWalkerCalibrationSettings
{
	GENERATED_BODY()
	
	UPROPERTY()
	float StandHeight = 0.f;
	
	UPROPERTY()
	float StartingFootDistance = 0.f;
	
	UPROPERTY()
	float StepHeight = 50.f;
	
	UPROPERTY()
	int32 StepRaiseTicks = 3;
	
	UPROPERTY()
	int32 StepMoveTicks = 3;
	
	UPROPERTY()
	int32 StepLowerTicks = 3;
	
	UPROPERTY()
	int32 ReachDistance = 2;
	
	UPROPERTY()
	float MinimumReachDistance = 0.f;
	
	UPROPERTY()
	EWalkerFoot ActiveFoot = EWalkerFoot::DFoot;
	
	// Removed OriginalFootPositions and SittingPose as they are not used for restoration
	// and TMap is not supported in Config files.
	
	UPROPERTY()
	int32 LegQuadrantOffset = 0; // Legacy: global leg quadrant offset (kept for config compatibility, no longer used directly)

	// Persistent per-quadrant leg mapping and tile rotation
	UPROPERTY()
	EWalkerFoot NQuadrantLeg = EWalkerFoot::UFoot;

	UPROPERTY()
	EWalkerFoot EQuadrantLeg = EWalkerFoot::RFoot;

	UPROPERTY()
	EWalkerFoot WQuadrantLeg = EWalkerFoot::LFoot;

	UPROPERTY()
	EWalkerFoot SQuadrantLeg = EWalkerFoot::DFoot;

	UPROPERTY()
	int32 TileQuadrantOffset = 0;

	UPROPERTY()
	float QuadrantRotationOffset = 0.f;

	UPROPERTY()
	bool bDynamicBodyRotation = false;

	UPROPERTY()
	int32 StartingFootOrientation = 0;
};

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
UCLASS(BlueprintType, Config=Game)
class AUTOMATIONGAME_API AGameManager : public AActor
{
	GENERATED_BODY()
public:
	AGameManager();
	virtual ~AGameManager();
	virtual void BeginDestroy() override;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Factory|Arms")
	TSubclassOf<ABasicArm> BasicArmClass;

	/** Optional default program to use when placing from input. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Factory|Arms")
	TArray<FArmInstr> DefaultArmProgram;

	UFUNCTION(BlueprintCallable, Category = "Factory|Arms")
	ABasicArm* SpawnBasicArmAt(const FHex& Hex, uint8 FacingDir, const TArray<FArmInstr>& Program, bool bLoop = true);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Factory|Walkers")
	TSubclassOf<ABasicWalker> BasicWalkerClass;

	/** Optional default program to use when placing walkers. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Factory|Walkers")
	TArray<FWalkerInstr> DefaultWalkerProgram;

	UFUNCTION(BlueprintCallable, Category = "Factory|Walkers")
	ABasicWalker* SpawnBasicWalkerAt(const FHex& Hex, const TArray<FWalkerInstr>& Program, bool bLoop = true);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Factory|Beacons")
	TSubclassOf<class ARequestBeacon> RequestBeaconClass;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Factory|TriggerTiles")
	TSubclassOf<class ATriggerTile> TriggerTileClass;

	UFUNCTION(CallInEditor, Category = "Walker|Calibration")
	void CalibrateWalker();
	
	UFUNCTION(CallInEditor, Category = "Walker|Calibration")
	void SetStepHeight();
	
	UFUNCTION(CallInEditor, Category = "Walker|Calibration")
	void SetStandHeightButton(); // Manually apply DesiredStandHeight
	
	UFUNCTION(CallInEditor, Category = "Walker|Calibration")
	void ApplyStandHeight(); // Apply current DesiredStandHeight manually
	
	UFUNCTION(CallInEditor, Category = "Walker|Calibration")
	void ResetStandHeight(); // Reset to sitting position (DesiredStandHeight = 0)
	
	UFUNCTION(CallInEditor, Category = "Walker|Calibration")
	void SaveWalkerCalibration();

	UFUNCTION(CallInEditor, Category = "Walker|Calibration")
	void CancelWalkerCalibration();
	
	UFUNCTION(CallInEditor, Category = "Walker|Calibration", meta = (Tooltip = "Move each foot to the furthest tile in its assigned quadrant (debug function)"))
	void FootToOuterMostTile();

	UPROPERTY(EditAnywhere, Category = "Walker|Calibration", meta = (ClampMin = "-200.0", ClampMax = "1000.0", Tooltip = "Target stand height - changes are applied automatically"))
	float DesiredStandHeight = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Walker|Calibration", meta = (Tooltip = "Current actual stand height after leg reach constraints"))
	float StandHeight = 0.f;

	UPROPERTY(VisibleAnywhere, Category = "Walker|Calibration")
	bool bWalkerStanding = false;
	
	UPROPERTY(EditAnywhere, Category = "Walker|Calibration", meta = (Tooltip = "Horizontal offset in tiles from the body-aligned tile for foot starting positions"))
	float StartingFootDistance = 0.f;
	
	UPROPERTY(EditAnywhere, Category = "Walker|Calibration", meta = (ClampMin = "0", ClampMax = "5", Tooltip = "Rotates the StartingFootDistance direction by N * 60 degrees around the body center"))
	int32 StartingFootOrientation = 0;
	
	UPROPERTY(EditAnywhere, Category = "Walker|Calibration", meta = (ClampMin = "0.0", Tooltip = "Height feet raise when stepping"))
	float StepHeight = 50.f;
	
	UPROPERTY(EditAnywhere, Category = "Walker|Calibration", meta = (Tooltip = "Ticks for raising foot"))
	int32 StepRaiseTicks = 3;
	
	UPROPERTY(EditAnywhere, Category = "Walker|Calibration", meta = (Tooltip = "Ticks for moving foot horizontally"))
	int32 StepMoveTicks = 3;
	
	UPROPERTY(EditAnywhere, Category = "Walker|Calibration", meta = (Tooltip = "Ticks for lowering foot"))
	int32 StepLowerTicks = 3;
	
	UPROPERTY(EditAnywhere, Category = "Walker|Calibration", meta = (Tooltip = "Selected foot for step height preview"))
	EWalkerFoot ActiveFoot = EWalkerFoot::DFoot;
	
	UPROPERTY(EditAnywhere, Category = "Walker|Calibration", meta = (Tooltip = "Enable active foot preview updates (applies step height to selected foot)"))
	bool bActiveFootPreview = false;
	
	UPROPERTY(EditAnywhere, Category = "Walker|Calibration", meta = (ClampMin = "1", Tooltip = "Tile range each leg can reach"))
	int32 ReachDistance = 2;

	UPROPERTY(EditAnywhere, Category = "Walker|Calibration", meta = (ClampMin = "0.0", Tooltip = "Minimum tile distance from the body for foot placement"))
	float MinimumReachDistance = 0.f;
	
	UPROPERTY(EditAnywhere, Category = "Walker|Calibration", meta = (Tooltip = "When true, foot goals are set manually via Event Graph instead of automatically by script"))
	bool bManualFootGoal = false;
	
	UPROPERTY(EditAnywhere, Category = "Walker|Calibration")
	bool bShowLegTileQuadrants = false;
	
	/** Per-quadrant leg assignment (which leg controls each spatial quadrant) */
	UPROPERTY(EditAnywhere, Category = "Walker|Calibration", meta = (Tooltip = "Leg controlling the North (R-1) quadrant"))
	EWalkerFoot NQuadrantLeg = EWalkerFoot::UFoot;

	UPROPERTY(EditAnywhere, Category = "Walker|Calibration", meta = (Tooltip = "Leg controlling the East (Q+1) quadrant"))
	EWalkerFoot EQuadrantLeg = EWalkerFoot::RFoot;

	UPROPERTY(EditAnywhere, Category = "Walker|Calibration", meta = (Tooltip = "Leg controlling the West (Q-1) quadrant"))
	EWalkerFoot WQuadrantLeg = EWalkerFoot::LFoot;

	UPROPERTY(EditAnywhere, Category = "Walker|Calibration", meta = (Tooltip = "Leg controlling the South (R+1) quadrant"))
	EWalkerFoot SQuadrantLeg = EWalkerFoot::DFoot;

	/** Global rotation for tile quadrants around the body center (does not change which leg controls which logical quadrant) */
	UPROPERTY(EditAnywhere, Category = "Walker|Calibration", meta = (ClampMin = "0", ClampMax = "3", Tooltip = "Rotate tile quadrants clockwise around the body center by this value (0-3). 0=no rotation, 1=90deg CW, 2=180deg, 3=270deg CW"))
	int32 TileQuadrantOffset = 0;

	/** Fine-tuned rotation offset in degrees for quadrant boundaries */
	UPROPERTY(EditAnywhere, Category = "Walker|Calibration", meta = (Tooltip = "Rotate the quadrant boundaries by this many degrees clockwise. Use this to center quadrants on your model's legs."))
	float QuadrantRotationOffset = 0.f;
	
	UPROPERTY(EditAnywhere, Category = "Walker|Calibration", meta = (Tooltip = "If true, QuadrantRotationOffset will automatically update based on body Z rotation"))
	bool bDynamicBodyRotation = false;
	
	UPROPERTY(EditAnywhere, Category = "Walker|Debug")
	bool bDebugWalkers = false;
	
	// Helper to get quadrant color for a leg (for runtime debug visualization)
	UFUNCTION(BlueprintCallable, Category = "Walker|Debug")
	FLinearColor GetLegQuadrantColor(const FString& OriginalQuadrant, int32 LegIndex) const;

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

	// ------------ Cluster Storage Limit ------------
	/** Maximum number of each unique cluster structure that can be held in storage */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cluster Storage", meta = (ClampMin = "1"))
	int32 StorageLimit = 10;

	/** When true, ClusterOutputs won't add a cluster structure if that structure's count has reached StorageLimit (clusters remain on output tiles until room is available) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cluster Storage")
	bool UseStorageLimit = false;

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
	UFUNCTION(BlueprintPure, Category = "Grid") FORCEINLINE float GetTileSpacing() const { return TileSizeUU; }

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

	/** Dynamic material instance for grid tiles (created from TileMaterial, used to set ripple parameters) */
	UPROPERTY()
	UMaterialInstanceDynamic* GridTileMID = nullptr;
	
	/** Cached base material used to create GridTileMID (to detect changes) */
	UPROPERTY()
	UMaterialInterface* CachedGridTileBaseMaterial = nullptr;

	/** Auto-rebuild when moved farther than this threshold. */
	UPROPERTY(EditAnywhere, Category = "Grid")
	bool bAutoExpandAtRuntime = true;

	UPROPERTY(EditAnywhere, Category = "Grid", meta = (EditCondition = "bAutoExpandAtRuntime"))
	float RecenterThresholdUU = 800.f;

	/** Scale multiplier for ClusterOutput tiles (larger than regular Output tiles). */
	UPROPERTY(EditAnywhere, Category = "Grid", meta = (ClampMin = "1.0", ClampMax = "3.0"))
	float ClusterOutputScale = 1.3f;

	/** Grid mode: Planar (flat hex grid) or Goldberg (geodesic sphere). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid")
	EGridMode GridMode = EGridMode::Planar;

	/** Subdivision frequency for Goldberg grid (>=1). Higher values = more cells. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Goldberg", meta = (EditCondition = "GridMode == EGridMode::Goldberg", ClampMin = "1", UIMin = "1"))
	int32 GoldbergFrequency = 3;

	/** Radius of the Goldberg sphere in world units. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid|Goldberg", meta = (EditCondition = "GridMode == EGridMode::Goldberg", ClampMin = "100"))
	float GoldbergRadius = 3000.f;

	/** Check if currently in Goldberg grid mode. */
	UFUNCTION(BlueprintPure, Category = "Grid")
	bool IsGoldbergMode() const { return GridMode == EGridMode::Goldberg; }

	/** Get the Goldberg grid object (only valid in Goldberg mode). */
	UFUNCTION(BlueprintPure, Category = "Grid|Goldberg")
	UGoldbergGrid* GetGoldbergGrid() const { return GoldbergGrid; }

	/** Get position for a Goldberg cell by index. */
	UFUNCTION(BlueprintCallable, Category = "Grid|Goldberg")
	FVector GetGoldbergCellPosition(int32 CellIndex) const;

	/** Get rotation for a Goldberg cell by index (surface normal as up). */
	UFUNCTION(BlueprintCallable, Category = "Grid|Goldberg")
	FRotator GetGoldbergCellRotation(int32 CellIndex) const;

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

	/** Global height offset for timelines relative to arm base */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Arm Timeline")
	float TimelineGlobalHeight = 250.0f;

	/** Minimum separation distance between timelines */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Arm Timeline")
	float TimelineSeparation = 250.0f;

	/** Strength of the repulsion force between timelines */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Arm Timeline")
	float TimelineRepulsionStrength = 2.0f;

	/** Offset for timeline widgets from arm position (Legacy/Advanced) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Arm Timeline", meta=(AdvancedDisplay))
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

	UFUNCTION(BlueprintCallable, Category = "Grid")
	FVector GetWorldForHex(const FHex& H) const;

	UFUNCTION(BlueprintCallable, Category = "Grid")
	void ToggleGridVisibility();

	// ------------ Cluster Generation ------------
	/** Generate a random cluster structure based on DifficultyValue. Returns the template ID that was registered. */
	UFUNCTION(BlueprintCallable, Category = "Cluster Generation")
	int32 GenerateRandomCluster();

	// ------------ Cluster Storage ------------
	/** Add a processed cluster to storage or increment its count if already exists (C++ only) */
	void AddClusterToStorage(const USimWorldHex::FClusterTemplate& ClusterTemplate);

	/** Get the total count of all clusters in storage (C++ only) */
	int32 GetTotalClusterCount() const;

	/** Check if storage has room for a specific cluster structure (respects UseStorageLimit and StorageLimit) (C++ only) */
	bool HasStorageRoomForCluster(const USimWorldHex::FClusterTemplate& ClusterTemplate) const;

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

	/** Find a stored cluster matching an orbs-only template (for ClusterInput matching) (C++ only) */
	bool FindStoredClusterByOrbsOnly(const USimWorldHex::FClusterTemplate& OrbsOnlyTemplate, const USimWorldHex::FClusterTemplate*& OutTemplate, int32& OutCount, FClusterCanonicalKey& OutMatchingKey) const;

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

	// ------------ Ripple/Wave Render Target ------------
	/** Render target for storing ripple and wave data (RGBA: RippleIntensity, SignalType, WaveHeight, Reserved) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ripple|RenderTarget")
	class UTextureRenderTarget2D* RippleRenderTarget;

	/** Size of the ripple render target (should be power of 2, e.g., 512, 1024, 2048) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ripple|RenderTarget", meta = (ClampMin = "256", ClampMax = "4096"))
	int32 RippleRenderTargetSize = 1024;

	/** World space size covered by the render target (in units) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ripple|RenderTarget", meta = (ClampMin = "100.0"))
	float RippleRenderTargetWorldSize = 2000.0f;

	/** 
	 * World position offset for the render target (allows RT to follow camera/viewport).
	 * The RT covers a square area centered at (RippleRTOffset.X, RippleRTOffset.Y).
	 * Set this to camera position to make ripples only visible in viewport area.
	 * Leave at (0,0) to center RT at grid anchor.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ripple|RenderTarget")
	FVector2D RippleRTOffset = FVector2D::ZeroVector;

	/** Automatically update RippleRTOffset to follow player camera position */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ripple|RenderTarget")
	bool bAutoFollowPlayerCamera = true;

	/** 
	 * Material used to draw ripples to the render target.
	 * IMPORTANT: This material MUST be configured as:
	 * - Material Domain: Post Process (or User Interface)
	 * - Blendable Location: After Tonemapping
	 * - Simple material without lighting/VSMs (causes crashes otherwise)
	 * 
	 * Expected parameters:
	 * - RippleCenter (Vector2): UV coordinates of ripple center (0-1 range)
	 * - RippleColor (Vector4): Encoded color (R=intensity, G=signal type 0/1)
	 * - RippleRadius (Scalar): Radius in RT UV space (0-1)
	 * - RippleFalloff (Scalar): Falloff distance in RT UV space (0-1)
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ripple|RenderTarget")
	class UMaterialInterface* RippleDrawMaterial;

	/** Color for positive ripple signal (green) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ripple|RenderTarget")
	FLinearColor RipplePositiveColor = FLinearColor(0.1f, 0.9f, 0.1f, 1.0f);

	/** Color for negative ripple signal (red) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ripple|RenderTarget")
	FLinearColor RippleNegativeColor = FLinearColor(0.9f, 0.1f, 0.1f, 1.0f);

	/** Initialize or recreate the ripple render target */
	UFUNCTION(BlueprintCallable, Category = "Ripple|RenderTarget")
	void InitializeRippleRenderTarget();

	/** Clear the ripple render target (call at start of each frame) */
	UFUNCTION(BlueprintCallable, Category = "Ripple|RenderTarget")
	void ClearRippleRenderTarget();

	/** Get the ripple render target (creates it if needed) */
	UFUNCTION(BlueprintPure, Category = "Ripple|RenderTarget")
	UTextureRenderTarget2D* GetRippleRenderTarget();

	/** Convert world position to render target UV coordinates */
	UFUNCTION(BlueprintPure, Category = "Ripple|RenderTarget")
	FVector2D WorldPositionToRippleRTUV(const FVector& WorldPos) const;

	/** Create or update the grid tile MID from TileMaterial (for setting ripple parameters) */
	void UpdateGridTileMID();

	// ------------ Wave Render Target (Physical Displacement) ------------
	/** Render target for storing wave height/displacement data (for World Position Offset) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wave|RenderTarget")
	class UTextureRenderTarget2D* WaveRenderTarget;

	/** Size of the wave render target (should be power of 2, e.g., 512, 1024, 2048) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wave|RenderTarget", meta = (ClampMin = "256", ClampMax = "4096"))
	int32 WaveRenderTargetSize = 2048;

	/** World space size covered by the wave render target (in units) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wave|RenderTarget", meta = (ClampMin = "100.0"))
	float WaveRenderTargetWorldSize = 10000.0f;

	/** 
	 * World position offset for the wave render target (allows RT to follow camera/viewport).
	 * The RT covers a square area centered at (WaveRTOffset.X, WaveRTOffset.Y).
	 * Set this to camera position to make waves only visible in viewport area.
	 * Leave at (0,0) to center RT at grid anchor.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wave|RenderTarget")
	FVector2D WaveRTOffset = FVector2D::ZeroVector;

	/** Automatically update WaveRTOffset to follow player camera position */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wave|RenderTarget")
	bool bAutoFollowPlayerCameraForWaves = true;

	/** 
	 * Material used to draw waves to the render target.
	 * IMPORTANT: This material MUST be configured as:
	 * - Material Domain: Surface
	 * - Blend Mode: Additive
	 * - Shading Model: Unlit
	 * 
	 * Expected parameters:
	 * - WaveCenter (Vector2): UV coordinates of wave center (0-1 range)
	 * - WaveHeight (Scalar): Wave height/displacement (Delta from 0)
	 * - WaveRadius (Scalar): Radius in RT UV space (0-1)
	 * - WaveFalloff (Scalar): Falloff distance in RT UV space (0-1)
	 * - WaveTime (Scalar): Time value for animation
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wave|RenderTarget")
	class UMaterialInterface* WaveDrawMaterial;

	/** Maximum wave height/displacement in world units */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wave|RenderTarget", meta = (ClampMin = "0.0"))
	float MaxWaveHeight = 50.0f;

	/** Wave animation speed (waves per second) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wave|RenderTarget", meta = (ClampMin = "0.0"))
	float WaveAnimationSpeed = 1.0f;

	/** 
	 * Controls how quickly waves fade out.
	 * Lower values = gradual "simmer" effect (0.25-0.5)
	 * Higher values = faster fade (1.0 = linear, 2.0 = quadratic)
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wave|RenderTarget", meta = (ClampMin = "0.1", ClampMax = "4.0"))
	float WaveFadeExponent = 0.5f;

	/** Wave height for regular Output tiles (in world units) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wave|Output", meta = (ClampMin = "0.0"))
	float OutputWaveHeight = 60.0f;

	/** Wave radius for regular Output tiles (in world units) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wave|Output", meta = (ClampMin = "1.0"))
	float OutputWaveRadius = 50.0f; // Increased start radius for visibility

	/** Wave falloff distance for regular Output tiles (in world units) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wave|Output", meta = (ClampMin = "1.0"))
	float OutputWaveFalloff = 50.0f;

	/** Wave height for ClusterOutput tiles (in world units) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wave|Output", meta = (ClampMin = "0.0"))
	float ClusterOutputWaveHeight = 35.0f;

	/** Wave radius for ClusterOutput tiles (in world units) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wave|Output", meta = (ClampMin = "1.0"))
	float ClusterOutputWaveRadius = 120.0f;

	/** Wave falloff distance for ClusterOutput tiles (in world units) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wave|Output", meta = (ClampMin = "1.0"))
	float ClusterOutputWaveFalloff = 60.0f;

	// ---- Walker Footstep Waves ----
	
	/** Wave height for Walker footsteps (splash when foot lands) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wave|Walker", meta = (ClampMin = "0.0"))
	float WalkerFootWaveHeight = 25.0f;

	/** Wave radius for Walker footsteps (in world units) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wave|Walker", meta = (ClampMin = "1.0"))
	float WalkerFootWaveRadius = 30.0f;

	/** Wave falloff distance for Walker footsteps (in world units) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wave|Walker", meta = (ClampMin = "1.0"))
	float WalkerFootWaveFalloff = 40.0f;

	/** Lifetime of Walker footstep waves (in seconds, before speed scaling) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wave|Walker", meta = (ClampMin = "0.1"))
	float WalkerFootWaveLifetime = 1.5f;

	/** Initialize or recreate the wave render target */
	UFUNCTION(BlueprintCallable, Category = "Wave|RenderTarget")
	void InitializeWaveRenderTarget();

	/** Clear the wave render target (call at start of each frame) */
	UFUNCTION(BlueprintCallable, Category = "Wave|RenderTarget")
	void ClearWaveRenderTarget();

	/** Get the wave render target (creates it if needed) */
	UFUNCTION(BlueprintPure, Category = "Wave|RenderTarget")
	UTextureRenderTarget2D* GetWaveRenderTarget();

	/** Convert world position to wave render target UV coordinates */
	UFUNCTION(BlueprintPure, Category = "Wave|RenderTarget")
	FVector2D WorldPositionToWaveRTUV(const FVector& WorldPos) const;
	
	/** Draw a wave to the render target at the specified world position (adds to active wave list) */
	UFUNCTION(BlueprintCallable, Category = "Wave|RenderTarget")
	void DrawWaveToRenderTarget(const FVector& WorldPosition, float WaveHeight, float WaveRadius, float WaveFalloff, float LifeTime = 1.5f);

	// ------------ Walker Calibration (Public for access from BasicWalker) ------------
	/** Whether walker calibration is currently active */
	UPROPERTY(BlueprintReadOnly, Category = "Walker|Calibration")
	bool bWalkerCalibrationActive = false;
	
	/** The walker currently being calibrated */
	UPROPERTY(BlueprintReadOnly, Category = "Walker|Calibration")
	TWeakObjectPtr<ABasicWalker> CalibratingWalker;

	/** Get the foot axle world location for a given hip bone name during calibration */
	UFUNCTION(BlueprintCallable, Category = "Walker|Calibration")
	FVector GetFootAxleWorldLocationForBone(const FName& HipBoneName) const;

	/** Update quadrant visualization for a runtime walker (uses same tile marking approach as CalibrateWalker) */
	void UpdateWalkerQuadrantVisualization(class ABasicWalker* Walker);
	
	/** Clear quadrant visualization for a runtime walker */
	void ClearWalkerQuadrantVisualization(class ABasicWalker* Walker);


protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void Tick(float DeltaSeconds) override;
#if WITH_EDITOR
	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

private:
	/** List of currently active waves being rendered */
	UPROPERTY(Transient)
	TArray<FGridWaveInstance> ActiveWaves;

	/** Cached material instance for drawing waves to render target */
	UPROPERTY(Transient)
	UMaterialInstanceDynamic* WaveDrawMID = nullptr;
	
	/** Cached base material used to create WaveDrawMID (to detect changes) */
	UPROPERTY(Transient)
	UMaterialInterface* CachedWaveDrawBaseMaterial = nullptr;

	/** Internal helper to draw a single wave stamp to the render target */
	void InternalDrawWaveStamp(class UCanvas* Canvas, const FVector2D& Size, const FGridWaveInstance& Wave);

	struct FWalkerLegCalibrationData
	{
		FName HipRotatorBone;
		FName HipAxleBone;
		FName KneeAxleBone;
		FName FootAxleBone;
		FName FootBone;
		FQuat HipRotatorBaseRotation = FQuat::Identity;
		FQuat HipAxleBaseRotation = FQuat::Identity;
		FQuat KneeAxleBaseRotation = FQuat::Identity;
		FQuat FootAxleBaseRotation = FQuat::Identity;
		FQuat FootBoneBaseRotation = FQuat::Identity;
		FTransform FootRelativeTransform = FTransform::Identity;
		FVector HipComponentLocation = FVector::ZeroVector;
		FVector KneeComponentLocation = FVector::ZeroVector;
		FVector FootAxleComponentLocation = FVector::ZeroVector;
		FVector FootComponentLocation = FVector::ZeroVector;
		FVector HipWorldLocation = FVector::ZeroVector;
		FVector PlaneNormalComponent = FVector::UpVector;
		FVector ThighBaseDirComponent = FVector::ForwardVector;
		FVector CalfBaseDirComponent = FVector::ForwardVector;
		FVector HipForwardDirComponent = FVector::ForwardVector;
		FVector BasePerpDirComponent = FVector::RightVector;
		FVector HipRotAxisComponent = FVector::UpVector;
		FVector HipAxleAxisComponent = FVector::ForwardVector;
		FVector KneeAxleAxisComponent = FVector::ForwardVector;
		FVector FootAxleAxisComponent = FVector::ForwardVector;
		FVector FootAxleWorldLocation = FVector::ZeroVector;
		FVector FootWorldLocation = FVector::ZeroVector;
		FVector PoleVectorLocal = FVector::ZeroVector;
		float UpperLength = 0.f;
		float LowerLength = 0.f;
		float BaseKneeAlong = 0.f;
		float BaseKneeHeight = 0.f;
	};

	FVector WalkerCalibrationBaseLocation = FVector::ZeroVector;
	TMap<FName, FQuat> WalkerSittingPose;
	TArray<FWalkerLegCalibrationData> WalkerLegCalibrations;
	float WalkerStandAlpha = 0.f;
	FVector WalkerBodyBaseWorldLocation = FVector::ZeroVector;
	FVector WalkerBodyActorOffset = FVector::ZeroVector;
	TArray<FVector> WalkerOriginalFootPositions; // Store original foot positions for offset calculation
	// Backup of foot positions when toggling FootToOuterMostTile so we can return to the previous state
	TArray<FVector> WalkerFootPositionsBeforeOuterMost;
	bool bFeetAtOuterMost = false;
	TArray<UStaticMeshComponent*> WalkerQuadrantTileMarkers; // Visual markers for reachable tiles (calibration walker)
	TMap<TWeakObjectPtr<ABasicWalker>, TArray<UStaticMeshComponent*>> RuntimeWalkerQuadrantMarkers; // Visual markers per runtime walker
	float SavedStandHeight = 0.f; // Saved height for reset operations
	
	// Persistent calibration settings (Config property allows it to persist between sessions)
	UPROPERTY(Config)
	FWalkerCalibrationSettings SavedWalkerCalibration;
	
	UPROPERTY(EditAnywhere, Category = "Walker|Calibration")
	UMaterialInterface* WalkerFootDebugMaterial = nullptr;
	UPROPERTY(EditAnywhere, Category = "Walker|Calibration")
	UMaterialInterface* WalkerKneeDebugMaterial = nullptr;
	
	void SetStandHeight(); // Internal function called automatically when DesiredStandHeight changes
	void GatherWalkerCalibrationData(class USkeletalMeshComponent* BodyComp);
	void ApplyStartingFootDistance(class USkeletalMeshComponent* BodyComp);
	void SetIKRigGoalPositions(class USkeletalMeshComponent* BodyComp, float HeightOffset);
	void UpdateLegTileQuadrantVisualization();
	void ClearLegTileQuadrantVisualization();
	void CreateWalkerDebugMeshes(class ABasicWalker* Walker, class USkeletalMeshComponent* BodyComp);
	void UpdateWalkerDebugMeshes(const class USkeletalMeshComponent* BodyComp);
	void ClearWalkerDebugMeshes();
	void LoadWalkerCalibration();
	void SaveWalkerCalibrationToConfig();
	void ApplyCalibrationValuesToAllWalkers(); // Apply all calibration values (StepHeight, ReachDistance, StartingFootDistance, etc.) to walkers in simulation
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

	// Cached orbs-only canonical keys for stored templates (for efficient ClusterInput matching)
	mutable TMap<FClusterCanonicalKey, FClusterCanonicalKey> StoredOrbsOnlyKeysCache;

	// Get orbs-only canonical key for a stored template (cached)
	FClusterCanonicalKey GetOrbsOnlyKeyForStoredTemplate(const FClusterCanonicalKey& FullKey) const;

	TArray<TWeakObjectPtr<class UStaticMeshComponent>> WalkerFootDebugMeshes;
	TArray<TWeakObjectPtr<class UStaticMeshComponent>> WalkerKneeDebugMeshes;

};
