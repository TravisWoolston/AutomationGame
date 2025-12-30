#include "Actors/TriggerTile.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SplineMeshComponent.h"
#include "Actors/OrbSimDriver.h"
#include "Sim/SimWorldHex.h"
#include "Sim/HexTypes.h"
#include "Sim/Tiles.h"
#include "Framework/GameManager.h"
#include "Data/OrbMaterialManager.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "EngineUtils.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Framework/GameTickManager.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "Engine/TextureRenderTarget2D.h"

ATriggerTile::ATriggerTile()
{
	PrimaryActorTick.bCanEverTick = true;

	RootComp = CreateDefaultSubobject<USceneComponent>(TEXT("RootComp"));
	RootComponent = RootComp;

	MeshComp = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("MeshComp"));
	MeshComp->SetupAttachment(RootComp);
	MeshComp->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	// Create wire component for visual connection to owner
	WireComp = CreateDefaultSubobject<USplineMeshComponent>(TEXT("WireComp"));
	WireComp->SetupAttachment(RootComp);
	WireComp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	WireComp->SetMobility(EComponentMobility::Movable);
	// Use Z-axis as forward since engine cylinder is oriented along Z
	WireComp->SetForwardAxis(ESplineMeshAxis::Z);
	WireComp->SetVisibility(false); // Hidden until owner is set

	// Create wave pulse indicator component
	WavePulseComp = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("WavePulseComp"));
	WavePulseComp->SetupAttachment(RootComp);
	WavePulseComp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	WavePulseComp->SetMobility(EComponentMobility::Movable);
	WavePulseComp->SetVisibility(false); // Hidden until owner is set
	WavePulseComp->SetCastShadow(false);
}

void ATriggerTile::BeginPlay()
{
	Super::BeginPlay();

	// Find Driver and Sim
	if (UWorld* World = GetWorld())
	{
		Driver = AOrbSimDriver::Find(World);
		if (Driver)
		{
			Sim = Driver->GetSim();
		}

		// Initialize IsGridVisible from GameManager's current state and get mesh if not set
		for (TActorIterator<AGameManager> It(World); It; ++It)
		{
			AGameManager* GM = *It;
			if (GM)
			{
				// If mesh isn't already set (e.g., from Blueprint), use GameManager's TileMesh
				if (MeshComp && !MeshComp->GetStaticMesh() && GM->TileMesh)
				{
					MeshComp->SetStaticMesh(GM->TileMesh);
				}
				
				if (UInstancedStaticMeshComponent* ISM = GM->FindComponentByClass<UInstancedStaticMeshComponent>())
				{
					SetIsGridVisible(ISM->IsVisible());
				}
				break;
			}
		}

		// Find GameTickManager for tick rate reference
		TickManager = AGameTickManager::Get(this);
	}

	UpdateVisuals();
	UpdateWireVisual();
}

void ATriggerTile::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	// Force wire update for first few frames to catch late Blueprint property initialization
	if (bWireNeedsInitialUpdate && WireUpdateRetryCount < 5)
	{
		WireUpdateRetryCount++;
		
		// Check if WireMaterial is now set (from Blueprint)
		if (WireMaterial && WireMaterial != LastWireMaterialUsed)
		{
			UE_LOG(LogTemp, Log, TEXT("TriggerTile: Late wire material detected (%s), updating wire"), *WireMaterial->GetName());
			CachedWireMID = nullptr; // Force recreation
			UpdateWireVisual();
			bWireNeedsInitialUpdate = false;
		}
		else if (WireUpdateRetryCount >= 5)
		{
			// Stop retrying after 5 frames
			bWireNeedsInitialUpdate = false;
		}
	}

	// Only check trigger condition if we have an owner
	if (bHasOwner)
	{
		CheckTriggerCondition();
	}

	// Animate ripple based on GTM tick rate (updates every engine tick)
	if (bEnableWavePulse && bHasOwner && Driver && TickManager)
	{
		// Get GTM's current ticks per second
		float GTMTicksPerSecond = TickManager->TicksPerSecond;
		
		// Advance ripple position based on GTM tick rate
		// WavePulseSpeed = 1.0 means it takes 1 GTM tick to travel from start to end
		// So we advance by: (DeltaTime * TicksPerSecond) / WavePulseSpeed
		float AdvanceRate = (DeltaTime * GTMTicksPerSecond) / FMath::Max(0.001f, WavePulseSpeed);
		WavePulseT += AdvanceRate;
		
		if (WavePulseT >= 1.0f)
		{
			WavePulseT -= FMath::Floor(WavePulseT); // Wrap around for continuous loop
			RippleIntensity = 0.0f; // Reset intensity when starting new cycle
		}

		// Calculate fade in/out based on position
		// Convert fade ticks to normalized position (0-1)
		float TotalTravelTicks = WavePulseSpeed;
		float FadeInEnd = (RippleFadeInTicks / TotalTravelTicks);
		float FadeOutStart = 1.0f - (RippleFadeOutTicks / TotalTravelTicks);
		
		if (WavePulseT < FadeInEnd)
		{
			// Fading in
			RippleIntensity = (FadeInEnd > 0.0f) ? FMath::Clamp(WavePulseT / FadeInEnd, 0.0f, 1.0f) : 1.0f;
		}
		else if (WavePulseT > FadeOutStart)
		{
			// Fading out
			float FadeOutRange = (RippleFadeOutTicks / TotalTravelTicks);
			RippleIntensity = (FadeOutRange > 0.0f) ? FMath::Clamp((1.0f - WavePulseT) / FadeOutRange, 0.0f, 1.0f) : 1.0f;
		}
		else
		{
			// Full intensity in the middle
			RippleIntensity = 1.0f;
		}
	}

	// Update visuals every frame
	UpdateRippleVisuals();
}

void ATriggerTile::UpdateRippleVisuals()
{
	if (!bEnableWavePulse || !bHasOwner || !Driver)
	{
		// Disable effects when wave pulse is disabled or no owner
		if (WavePulseComp)
		{
			WavePulseComp->SetVisibility(false);
		}
		return;
	}

	// Calculate start and end positions (at grid level)
	FVector StartPos = GetActorLocation();
	FHex OwnerHex(OwnerQ, OwnerR);
	FVector EndPos = Driver->MapHexToWorld(OwnerHex);
	
	// Calculate current ripple position at grid level
	FVector RipplePos = FMath::Lerp(StartPos, EndPos, WavePulseT);
	
	// Draw ripple to render target
	if (UWorld* World = GetWorld())
	{
		AGameManager* GM = nullptr;
		for (TActorIterator<AGameManager> It(World); It; ++It)
		{
			GM = *It;
			break;
		}
		
		if (GM)
		{
			UTextureRenderTarget2D* RT = GM->GetRippleRenderTarget();
			UMaterialInterface* DrawMaterial = GM->RippleDrawMaterial;
			
			// Skip if draw material isn't set (prevents crash)
			if (!DrawMaterial)
			{
				return;
			}
			
			if (RT && RippleIntensity > 0.0f && RT->IsValidLowLevel() && DrawMaterial->IsValidLowLevel())
			{
				// Create or get material instance for drawing
				if (!RippleDrawMID)
				{
					UMaterialInterface* BaseMat = DrawMaterial;
					if (UMaterialInstanceDynamic* ExistingMID = Cast<UMaterialInstanceDynamic>(BaseMat))
					{
						BaseMat = ExistingMID->Parent;
					}
					
					if (BaseMat && BaseMat->IsValidLowLevel())
					{
						RippleDrawMID = UMaterialInstanceDynamic::Create(BaseMat, this);
					}
				}
				
				if (RippleDrawMID && RippleDrawMID->IsValidLowLevel())
				{
					// Convert world position to RT UV coordinates
					FVector2D RTUV = GM->WorldPositionToRippleRTUV(RipplePos);
					
					// Encode data: R = intensity, G = signal type (0=red, 1=green)
					FLinearColor EncodedColor(
						RippleIntensity,  // R channel: intensity
						bCurrentSignal ? 1.0f : 0.0f,  // G channel: signal type
						0.0f,  // B channel: reserved for waves
						1.0f   // A channel: unused
					);
					
					// Set material parameters for ripple drawing
					// Parameters expected by the draw material:
					// - RippleCenter: UV coordinates in RT (0-1 range)
					// - RippleColor: Encoded color (R=intensity, G=signal type)
					// - RippleRadius: Radius in RT UV space (0-1)
					// - RippleFalloff: Falloff distance in RT UV space (0-1)
					float RTWorldSize = GM->RippleRenderTargetWorldSize;
					if (RTWorldSize > 0.0f)
					{
						RippleDrawMID->SetVectorParameterValue(FName("RippleCenter"), FLinearColor(RTUV.X, RTUV.Y, 0.0f, 1.0f));
						RippleDrawMID->SetVectorParameterValue(FName("RippleColor"), EncodedColor);
						RippleDrawMID->SetScalarParameterValue(FName("RippleRadius"), RippleRadius / RTWorldSize);
						RippleDrawMID->SetScalarParameterValue(FName("RippleFalloff"), RippleFalloff / RTWorldSize);
						
						// Draw to render target (additive blend mode in material)
						// Only draw if we're in game world (not editor preview)
						if (World->WorldType == EWorldType::Game || World->WorldType == EWorldType::PIE)
						{
							UKismetRenderingLibrary::DrawMaterialToRenderTarget(World, RT, RippleDrawMID);
						}
					}
				}
			}
		}
	}
	
	// Optional: Update visible pulse mesh indicator (if mesh is set up)
	if (WavePulseComp && WavePulseMesh)
	{
		// Add height offset for visible mesh
		FVector MeshPos = RipplePos;
		MeshPos.Z += WireHeightOffset + WavePulseHeightOffset;
		
		WavePulseComp->SetWorldLocation(MeshPos);
		WavePulseComp->SetWorldScale3D(FVector(WavePulseScale));
		
		// Setup mesh if needed
		if (!WavePulseComp->GetStaticMesh())
		{
			WavePulseComp->SetStaticMesh(WavePulseMesh);
		}
		
		// Setup material if needed
		if (!WavePulseMID && WavePulseComp->GetStaticMesh())
		{
			UMaterialInterface* BaseMat = WavePulseMaterial;
			if (!BaseMat)
			{
				BaseMat = WavePulseComp->GetStaticMesh()->GetMaterial(0);
			}
			
			if (BaseMat)
			{
				if (UMaterialInstanceDynamic* ExistingMID = Cast<UMaterialInstanceDynamic>(BaseMat))
				{
					BaseMat = ExistingMID->Parent;
				}
				
				if (BaseMat)
				{
					WavePulseMID = UMaterialInstanceDynamic::Create(BaseMat, this);
					WavePulseComp->SetMaterial(0, WavePulseMID);
				}
			}
		}
		
		// Update color with intensity
		if (WavePulseMID)
		{
			FLinearColor PulseColor = bCurrentSignal ? PositiveWireColor : NegativeWireColor;
			FLinearColor EmissiveColor = PulseColor * WavePulseEmissiveIntensity * RippleIntensity;
			
			WavePulseMID->SetVectorParameterValue(FName("BaseColor"), PulseColor * RippleIntensity);
			WavePulseMID->SetVectorParameterValue(FName("Color"), PulseColor * RippleIntensity);
			WavePulseMID->SetVectorParameterValue(FName("Tint"), PulseColor * RippleIntensity);
			WavePulseMID->SetVectorParameterValue(FName("EmissiveColor"), EmissiveColor);
		}
		
		WavePulseComp->SetVisibility(RippleIntensity > 0.0f);
	}
	else if (WavePulseComp)
	{
		WavePulseComp->SetVisibility(false);
	}
}

void ATriggerTile::CheckTriggerCondition()
{
	// Ensure we have Sim reference
	if (!Sim && !Driver)
	{
		if (UWorld* World = GetWorld())
		{
			Driver = AOrbSimDriver::Find(World);
			if (Driver)
			{
				Sim = Driver->GetSim();
			}
		}
	}

	if (!Sim) return;

	// Check tile content at Q, R
	FHex Location(Q, R);
	bool bMatchingOrb = false;

	// Check Sim Grid for ItemId
	if (Sim->ItemMap.Contains(Location))
	{
		const FItem* Item = Sim->ItemMap.Find(Location);
		if (Item && Item->Id != 0)
		{
			// There's an orb here - check if it matches our trigger type
			if (TriggerOrbType == EOrbType::Void)
			{
				// Void type: any orb triggers positive signal
				bMatchingOrb = true;
			}
			else if (!Item->bIsCluster)
			{
				// Single orb - check if type matches
				bMatchingOrb = (Item->Single.Type == TriggerOrbType);
			}
			else
			{
				// Cluster - check if any orb in the cluster matches
				// For now, check the first orb in the cluster at this position
				for (const FOrbData& OrbData : Item->Orbs)
				{
					if (OrbData.dQ == 0 && OrbData.dR == 0)
					{
						// This is the orb at the cluster's root position
						bMatchingOrb = ((EOrbType)OrbData.Type == TriggerOrbType);
						break;
					}
				}
			}
		}
	}

	// Update signal state if changed
	SetSignal(bMatchingOrb);
}

void ATriggerTile::SetSignal(bool bNewSignal)
{
	if (bCurrentSignal != bNewSignal)
	{
		bCurrentSignal = bNewSignal;
		OnSignalChanged.Broadcast(bCurrentSignal);

		// Update the simulation's belt trigger signal
		if (bHasOwner && Sim)
		{
			FHex OwnerHex(OwnerQ, OwnerR);
			Sim->SetBeltTriggerSignal(OwnerHex, bCurrentSignal);
			
			UE_LOG(LogTemp, Log, TEXT("TriggerTile at (%d,%d) signal changed to %s - belt at (%d,%d) now %s"),
				Q, R, bCurrentSignal ? TEXT("POSITIVE") : TEXT("NEGATIVE"),
				OwnerQ, OwnerR, bCurrentSignal ? TEXT("ENABLED") : TEXT("DISABLED"));
		}

		// Update visuals to reflect signal state
		UpdateVisuals();
		UpdateWireVisual();
	}
}

void ATriggerTile::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	
	// Try to set mesh from GameManager if not already set
	if (MeshComp && !MeshComp->GetStaticMesh())
	{
		if (UWorld* World = GetWorld())
		{
			for (TActorIterator<AGameManager> It(World); It; ++It)
			{
				AGameManager* GM = *It;
				if (GM && GM->TileMesh)
				{
					MeshComp->SetStaticMesh(GM->TileMesh);
					break;
				}
			}
		}
	}
	
	UpdateVisuals();
	UpdateWireVisual();
}

void ATriggerTile::SetIsGridVisible(bool bVisible)
{
	if (bIsGridVisible != bVisible)
	{
		bIsGridVisible = bVisible;
		OnGridVisibilityChanged.Broadcast(bVisible);
	}
}

void ATriggerTile::SetOwnerBelt(int32 InOwnerQ, int32 InOwnerR)
{
	OwnerQ = InOwnerQ;
	OwnerR = InOwnerR;
	bHasOwner = true;

	UE_LOG(LogTemp, Log, TEXT("TriggerTile at (%d,%d) now owned by belt at (%d,%d)"), Q, R, OwnerQ, OwnerR);
	
	// Initialize the simulation with the current signal state
	// Start with negative signal (no orb present yet) so belt starts disabled
	if (Sim)
	{
		FHex OwnerHex(OwnerQ, OwnerR);
		Sim->SetBeltTriggerSignal(OwnerHex, bCurrentSignal);
	}

	// Update wire visual to show connection
	UpdateWireVisual();
}

void ATriggerTile::UpdateVisuals()
{
	if (!MeshComp) return;

	// Ensure mesh is set before applying materials
	if (!MeshComp->GetStaticMesh())
	{
		if (UWorld* World = GetWorld())
		{
			for (TActorIterator<AGameManager> It(World); It; ++It)
			{
				AGameManager* GM = *It;
				if (GM && GM->TileMesh)
				{
					MeshComp->SetStaticMesh(GM->TileMesh);
					break;
				}
			}
		}
	}

	// Priority 1: Use explicit signal state materials if provided
	if (bCurrentSignal && PositiveSignalMaterial)
	{
		MeshComp->SetMaterial(0, PositiveSignalMaterial);
		return;
	}
	else if (!bCurrentSignal && NegativeSignalMaterial)
	{
		MeshComp->SetMaterial(0, NegativeSignalMaterial);
		return;
	}

	// Priority 2: Use TranslucentBaseMaterial with orb type color tint
	if (TranslucentBaseMaterial)
	{
		UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(TranslucentBaseMaterial, this);
		if (MID)
		{
			// Get the orb type color from the material manager
			FLinearColor OrbColor = FLinearColor::White;
			if (UWorld* World = GetWorld())
			{
				UOrbMaterialManager* MaterialManager = UOrbMaterialManager::FindInWorld(this);
				if (MaterialManager)
				{
					// Try to get color from the orb type material
					UMaterialInterface* TypeMaterial = MaterialManager->GetMaterialForOrbType(TriggerOrbType);
					if (TypeMaterial)
					{
						// Try to extract base color from the material
						// Use material-specific colors based on orb type
						switch (TriggerOrbType)
						{
						case EOrbType::BaseA:   OrbColor = FLinearColor(0.8f, 0.2f, 0.2f); break; // Red
						case EOrbType::BaseB:   OrbColor = FLinearColor(0.2f, 0.8f, 0.2f); break; // Green
						case EOrbType::BaseC:   OrbColor = FLinearColor(0.2f, 0.2f, 0.8f); break; // Blue
						case EOrbType::MetalA:  OrbColor = FLinearColor(0.7f, 0.7f, 0.7f); break; // Silver
						case EOrbType::CatalystX: OrbColor = FLinearColor(0.8f, 0.6f, 0.2f); break; // Gold
						case EOrbType::Void:    OrbColor = FLinearColor(0.5f, 0.0f, 0.8f); break; // Purple (any orb)
						default:                OrbColor = FLinearColor::White; break;
						}
					}
				}
			}

			// Apply color and opacity to the translucent material
			MID->SetVectorParameterValue(FName("BaseColor"), OrbColor);
			MID->SetVectorParameterValue(FName("Color"), OrbColor); // Alternative parameter name
			MID->SetVectorParameterValue(FName("Tint"), OrbColor); // Another common name
			MID->SetScalarParameterValue(FName("Opacity"), TriggerTileOpacity);
			
			MeshComp->SetMaterial(0, MID);
			return;
		}
	}

	// Priority 3: Fallback - create translucent version of orb material
	if (UWorld* World = GetWorld())
	{
		UOrbMaterialManager* MaterialManager = UOrbMaterialManager::FindInWorld(this);
		if (MaterialManager)
		{
			UMaterialInterface* TypeMaterial = MaterialManager->GetMaterialForOrbType(TriggerOrbType);
			if (TypeMaterial)
			{
				UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(TypeMaterial, this);
				if (MID)
				{
					// Try to set opacity (material must support this parameter)
					MID->SetScalarParameterValue(FName("Opacity"), TriggerTileOpacity);
					MeshComp->SetMaterial(0, MID);
					return;
				}
			}
		}
	}
}

void ATriggerTile::UpdateWireVisual()
{
	if (!WireComp) return;

	// Hide wire if no owner
	if (!bHasOwner)
	{
		WireComp->SetVisibility(false);
		return;
	}

	// Get world positions for start and end points
	FVector StartPos = GetActorLocation();
	FVector EndPos = StartPos; // Default to same position

	// Calculate owner world position
	if (Driver)
	{
		FHex OwnerHex(OwnerQ, OwnerR);
		EndPos = Driver->MapHexToWorld(OwnerHex);
	}

	// Apply height offset
	StartPos.Z += WireHeightOffset;
	EndPos.Z += WireHeightOffset;

	// Convert to local space (relative to actor)
	FVector LocalStart = GetActorTransform().InverseTransformPosition(StartPos);
	FVector LocalEnd = GetActorTransform().InverseTransformPosition(EndPos);

	// Calculate distance and direction
	FVector ToEnd = LocalEnd - LocalStart;
	float Distance = ToEnd.Size();
	FVector HorizontalDir = ToEnd.GetSafeNormal();

	// Create tangents that produce a drooping cable effect
	// The tangents point horizontally at start/end, but the spline will sag in the middle
	// due to how Bezier curves work with these tangent directions
	float TangentLength = Distance * 0.5f;
	
	// Start tangent: point toward the end, but also slightly downward to create sag
	FVector StartTangent = HorizontalDir * TangentLength;
	StartTangent.Z -= WireSagAmount; // Add downward component for sag
	
	// End tangent: point from start direction, also with downward component
	FVector EndTangent = HorizontalDir * TangentLength;
	EndTangent.Z -= WireSagAmount; // Add downward component for sag

	// Set spline mesh geometry
	WireComp->SetStartAndEnd(LocalStart, StartTangent, LocalEnd, EndTangent);

	// Set start/end scale based on wire radius
	// SplineMesh scale is applied to the mesh's original size
	// Assuming a unit cylinder, scale to desired radius
	FVector2D WireScale(WireRadius, WireRadius);
	WireComp->SetStartScale(WireScale);
	WireComp->SetEndScale(WireScale);

	// Try to set the wire mesh if not already set
	if (!WireComp->GetStaticMesh() && WireMesh)
	{
		WireComp->SetStaticMesh(WireMesh);
	}

	// If still no mesh, try to load a default cylinder from engine
	if (!WireComp->GetStaticMesh())
	{
		UStaticMesh* DefaultCylinder = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
		if (DefaultCylinder)
		{
			WireComp->SetStaticMesh(DefaultCylinder);
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("TriggerTile: Could not load default cylinder mesh for wire. Assign WireMesh property."));
		}
	}

	// Apply material with signal-based color
	FLinearColor WireColor = bCurrentSignal ? PositiveWireColor : NegativeWireColor;

	// Determine the base material to use - prioritize explicitly set WireMaterial
	UMaterialInterface* DesiredBaseMaterial = nullptr;
	
	if (WireMaterial)
	{
		DesiredBaseMaterial = WireMaterial;
		UE_LOG(LogTemp, Verbose, TEXT("TriggerTile: Using assigned WireMaterial: %s"), *WireMaterial->GetName());
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("TriggerTile: WireMaterial is not set! Assign a material in the Blueprint."));
		if (WireComp->GetStaticMesh())
		{
			// Fallback to mesh's default material
			DesiredBaseMaterial = WireComp->GetStaticMesh()->GetMaterial(0);
			if (DesiredBaseMaterial)
			{
				UE_LOG(LogTemp, Log, TEXT("TriggerTile: Falling back to mesh default material: %s"), *DesiredBaseMaterial->GetName());
			}
		}
	}

	// Check if we need to (re)create the cached MID
	// Recreate if: no cache exists, OR the source material changed, OR WireMaterial was set but we used mesh default
	bool bNeedNewMID = !CachedWireMID || 
	                   (WireMaterial && LastWireMaterialUsed != WireMaterial) ||
	                   (!WireMaterial && DesiredBaseMaterial != LastWireMaterialUsed);

	if (bNeedNewMID && DesiredBaseMaterial)
	{
		UMaterialInterface* BaseMaterial = DesiredBaseMaterial;
		
		// Make sure we're not creating a MID from another MID
		if (UMaterialInstanceDynamic* ExistingMID = Cast<UMaterialInstanceDynamic>(BaseMaterial))
		{
			// Get the parent material instead
			BaseMaterial = ExistingMID->Parent;
		}
		
		if (BaseMaterial)
		{
			CachedWireMID = UMaterialInstanceDynamic::Create(BaseMaterial, this);
			LastWireMaterialUsed = DesiredBaseMaterial;
			
			UE_LOG(LogTemp, Log, TEXT("TriggerTile: Created wire MID from material %s"), 
				*BaseMaterial->GetName());
		}
	}

	// Update the color on the cached MID
	if (CachedWireMID)
	{
		CachedWireMID->SetVectorParameterValue(FName("BaseColor"), WireColor);
		CachedWireMID->SetVectorParameterValue(FName("Color"), WireColor);
		CachedWireMID->SetVectorParameterValue(FName("Tint"), WireColor);
		CachedWireMID->SetVectorParameterValue(FName("EmissiveColor"), WireColor);
		WireComp->SetMaterial(0, CachedWireMID);
	}
	else if (DesiredBaseMaterial)
	{
		// Fallback: apply material directly without dynamic instance
		WireComp->SetMaterial(0, DesiredBaseMaterial);
		UE_LOG(LogTemp, Warning, TEXT("TriggerTile: Could not create MID, applying material directly"));
	}

	// Show the wire
	WireComp->SetVisibility(true);
}

void ATriggerTile::Destroyed()
{
	Super::Destroyed();

	// Clean up the belt trigger signal in simulation
	if (bHasOwner && Sim)
	{
		FHex OwnerHex(OwnerQ, OwnerR);
		Sim->ClearBeltTriggerSignal(OwnerHex);
		UE_LOG(LogTemp, Log, TEXT("TriggerTile at (%d,%d) destroyed - belt at (%d,%d) re-enabled"), Q, R, OwnerQ, OwnerR);
	}
	else
	{
		UE_LOG(LogTemp, Log, TEXT("TriggerTile at (%d,%d) destroyed"), Q, R);
	}
}


void ATriggerTile::SetOwnerArm(int32 InOwnerQ, int32 InOwnerR)
{
	OwnerQ = InOwnerQ;
	OwnerR = InOwnerR;
	bHasOwner = true;
	OwnerType = ETriggerTileOwnerType::Arm;

	UE_LOG(LogTemp, Log, TEXT("TriggerTile at (%d,%d) now owned by arm at (%d,%d)"), Q, R, OwnerQ, OwnerR);
	
	// Implementation note: Arms typically poll their trigger tile or receive signals via other means
	// If the Sim supports Arm signals, we would update it here similar to SetOwnerBelt

	// Update wire visual to show connection
	UpdateWireVisual();
}
