#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Sim/HexTypes.h"
#include "Sim/OrbTypes.h"
#include "Sim/TriggerTypes.h"
#include "TriggerTile.generated.h"

// Signal state delegate
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnTriggerSignalChanged, bool,
                                            bPositiveSignal);

// Grid visibility delegate
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnTriggerGridVisibilityChanged,
                                            bool, bIsVisible);

/**
 * TriggerTile - A placeable tile that sends signals based on orb presence.
 *
 * - Placeable on any tile
 * - Only removable when grid visibility is toggled off
 * - Placed with a selected orb type, uses that type's material for its mesh
 * - Requires an "owner" belt tile to be selected after placement
 * - Sends positive signal when an orb of the selected type occupies its tile
 * - Sends negative signal when no orb or incorrect orb type is present
 * - If placed with Void type, any orb triggers a positive signal
 */
UCLASS()
class AUTOMATIONGAME_API ATriggerTile : public AActor {
  GENERATED_BODY()

public:
  ATriggerTile();

protected:
  virtual void BeginPlay() override;
  virtual void OnConstruction(const FTransform &Transform) override;

public:
  virtual void Tick(float DeltaTime) override;
  virtual void Destroyed() override;

  // Components
  UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
  class USceneComponent *RootComp;

  UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
  class UStaticMeshComponent *MeshComp;

  /** Spline mesh component for the wire connecting to owner belt */
  UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
  class USplineMeshComponent *WireComp;

  /** Mesh component for the traveling wave pulse indicator */
  UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
  class UStaticMeshComponent *WavePulseComp;

  // Location
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TriggerTile")
  int32 Q = 0;

  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TriggerTile")
  int32 R = 0;

  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TriggerTile")
  int32 Level = 0;

  // The variant of this trigger (Positive or Negative logic)
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TriggerTile")
  ETriggerVariant Variant = ETriggerVariant::Positive;

  // The orb type this trigger responds to
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TriggerTile")
  EOrbType TriggerOrbType = EOrbType::BaseA;

  // List of owners this trigger sends signals to
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TriggerTile")
  TArray<FTriggerOwner> Owners;

  // Whether this trigger has any valid owners assigned
  UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "TriggerTile")
  bool bHasOwner = false;

  // Current signal state
  UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "TriggerTile")
  bool bCurrentSignal = false;

  // Delegates
  UPROPERTY(BlueprintAssignable, Category = "TriggerTile")
  FOnTriggerSignalChanged OnSignalChanged;

  UPROPERTY(BlueprintAssignable, Category = "TriggerTile")
  FOnTriggerGridVisibilityChanged OnGridVisibilityChanged;

  // Grid visibility getter
  UFUNCTION(BlueprintPure, Category = "TriggerTile")
  bool GetIsGridVisible() const { return bIsGridVisible; }

  // Grid visibility setter (fires delegate)
  UFUNCTION(BlueprintCallable, Category = "TriggerTile")
  void SetIsGridVisible(bool bVisible);

  // Add a new owner (belt or arm)
  UFUNCTION(BlueprintCallable, Category = "TriggerTile")
  void AddOwner(int32 InOwnerQ, int32 InOwnerR, ETriggerTileOwnerType InType);

  // Remove an owner
  UFUNCTION(BlueprintCallable, Category = "TriggerTile")
  void RemoveOwner(int32 InOwnerQ, int32 InOwnerR,
                   ETriggerTileOwnerType InType);

  // Toggles an owner (adds if missing, removes if present)
  UFUNCTION(BlueprintCallable, Category = "TriggerTile")
  void ToggleOwner(int32 InOwnerQ, int32 InOwnerR,
                   ETriggerTileOwnerType InType);

  // DEPRECATED: set the single owner belt tile (now adds to Owners array)
  UFUNCTION(BlueprintCallable, Category = "TriggerTile",
            meta = (DeprecatedFunction,
                    DeprecationMessage = "Use AddOwner instead"))
  void SetOwnerBelt(int32 InOwnerQ, int32 InOwnerR);

  // DEPRECATED: set the single owner arm tile (now adds to Owners array)
  UFUNCTION(BlueprintCallable, Category = "TriggerTile",
            meta = (DeprecatedFunction,
                    DeprecationMessage = "Use AddOwner instead"))
  void SetOwnerArm(int32 InOwnerQ, int32 InOwnerR);

  // Get the owner hex
  UFUNCTION(BlueprintPure, Category = "TriggerTile")
  // Get the first owner hex (if any)
  FHex GetOwnerHex() const {
    return Owners.Num() > 0 ? FHex(Owners[0].Q, Owners[0].R) : FHex(0, 0);
  }

  // Get the trigger hex location
  UFUNCTION(BlueprintPure, Category = "TriggerTile")
  FHex GetTriggerHex() const { return FHex(Q, R); }

  // Update visuals (material based on orb type)
  UFUNCTION(BlueprintCallable, Category = "TriggerTile")
  void UpdateVisuals();

  // Sync world location from Q, R, Level using Driver
  UFUNCTION(BlueprintCallable, Category = "Grid")
  void SyncWorldLocation();

  // Condition check (called from Tick)
  void CheckTriggerCondition();

  /** Signal handler for simulation step completion */
  UFUNCTION()
  void HandleSimStepComplete(float StepSeconds);

  // Wave ripple and pulse visuals
  void UpdateRippleVisuals();

  // Internal signal update
  void SetSignal(bool bPositive);

  // Visuals - these can be set in a Blueprint child class
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visuals")
  UStaticMesh *PositiveMesh;

  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visuals")
  UStaticMesh *NegativeMesh;

  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visuals")
  UMaterialInterface *PositiveSignalMaterial;

  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visuals")
  UMaterialInterface *NegativeSignalMaterial;

  /** Base translucent material to use for the trigger tile mesh.
   *  Should be a translucent material with "BaseColor" and "Opacity"
   * parameters. The orb type color will be applied to this material. */
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visuals")
  UMaterialInterface *TranslucentBaseMaterial;

  /** Opacity of the trigger tile mesh (0.0 = invisible, 1.0 = opaque) */
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visuals",
            meta = (ClampMin = "0.0", ClampMax = "1.0"))
  float TriggerTileOpacity = 0.5f;

  // ---- Wire Visuals ----

  /** Static mesh to use for the wire (should be a cylinder/tube oriented along
   * X axis) */
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wire")
  UStaticMesh *WireMesh;

  /** Base material for the wire (should support "BaseColor" or "Color"
   * parameter) */
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wire")
  UMaterialInterface *WireMaterial;

  /** Color for positive signal (green) */
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wire")
  FLinearColor PositiveWireColor = FLinearColor(0.1f, 0.9f, 0.1f, 1.0f);

  /** Color for negative signal (red) */
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wire")
  FLinearColor NegativeWireColor = FLinearColor(0.9f, 0.1f, 0.1f, 1.0f);

  /** Height offset for the wire above the ground */
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wire",
            meta = (ClampMin = "0.0"))
  float WireHeightOffset = 10.0f;

  /** Radius/thickness of the wire */
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wire",
            meta = (ClampMin = "0.1"))
  float WireRadius = 3.0f;

  /** How much the wire droops/sags in the middle (creates cable-like curve) */
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wire")
  float WireSagAmount = 20.0f;

  /** Update the wire visual connecting to owner belt */
  UFUNCTION(BlueprintCallable, Category = "TriggerTile")
  void UpdateWireVisual();

  // ---- Wave Pulse Indicator ----

  /** Mesh for the traveling wave pulse (e.g., small sphere or ring) */
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wave Pulse")
  UStaticMesh *WavePulseMesh;

  /** Material for the wave pulse (should support BaseColor/EmissiveColor
   * parameters) */
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wave Pulse")
  UMaterialInterface *WavePulseMaterial;

  /** Speed of the wave pulse travel in GTM ticks (1.0 = travels from trigger to
   * owner in one tick) */
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wave Pulse",
            meta = (ClampMin = "0.1"))
  float WavePulseSpeed = 1.0f;

  /** Scale of the wave pulse mesh */
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wave Pulse",
            meta = (ClampMin = "0.1"))
  float WavePulseScale = 0.3f;

  /** Height offset for the wave pulse above the wire */
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wave Pulse")
  float WavePulseHeightOffset = 5.0f;

  /** Enable/disable the traveling wave pulse effect */
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wave Pulse")
  bool bEnableWavePulse = true;

  /** Emissive intensity multiplier for the pulse glow */
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wave Pulse",
            meta = (ClampMin = "0.0"))
  float WavePulseEmissiveIntensity = 3.0f;

  /** Radius of the ripple effect on the grid material (in world units) */
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wave Pulse",
            meta = (ClampMin = "1.0"))
  float RippleRadius = 50.0f;

  /** Falloff distance for soft edge on the ripple (in world units) */
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wave Pulse",
            meta = (ClampMin = "1.0"))
  float RippleFalloff = 30.0f;

  /** Duration in GTM ticks for ripple to fade in at start */
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wave Pulse",
            meta = (ClampMin = "0.0"))
  float RippleFadeInTicks = 0.2f;

  /** Duration in GTM ticks for ripple to fade out at end */
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wave Pulse",
            meta = (ClampMin = "0.0"))
  float RippleFadeOutTicks = 0.2f;

  // Dynamic components for multiple wires and pulses
  UPROPERTY()
  TMap<FTriggerOwner, USplineMeshComponent *> WireComponents;

  UPROPERTY()
  TMap<FTriggerOwner, UStaticMeshComponent *> WavePulseComponents;

  // Helper to ensure components exist for an owner
  void EnsureComponentsForOwner(const FTriggerOwner &Owner);

  // Helper to create a wave pulse component
  UStaticMeshComponent *CreateWavePulseComponent(const FTriggerOwner &Owner);

  // Helper to create a wire component
  USplineMeshComponent *CreateWireComponent(const FTriggerOwner &Owner);

  // Grid visibility state
  bool bIsGridVisible = true;

private:
  UPROPERTY()
  class AOrbSimDriver *Driver = nullptr;

  UPROPERTY()
  class USimWorldHex *Sim = nullptr;

  UPROPERTY()
  class AGameTickManager *TickManager = nullptr;

  UPROPERTY()
  UMaterialInstanceDynamic *CachedWireMID = nullptr;

  UPROPERTY()
  UMaterialInterface *LastWireMaterialUsed = nullptr;

  UPROPERTY()
  UMaterialInstanceDynamic *RippleDrawMID = nullptr;

  UPROPERTY()
  UMaterialInstanceDynamic *WavePulseMID = nullptr;

  float WavePulseT = 0.0f;
  float RippleIntensity = 0.0f;
  bool bWireNeedsInitialUpdate = true;
  int32 WireUpdateRetryCount = 0;
};
