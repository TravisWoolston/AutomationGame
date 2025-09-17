#include "Rendering/OrbRenderer.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Actors/OrbSimDriver.h"
#include "Sim/SimWorldHex.h"
#include "Sim/HexTypes.h"
#include "Sim/HexCoord.h"
#include "Engine/World.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "UObject/ConstructorHelpers.h"
#include "Engine/Engine.h"
#include "GameFramework/Actor.h"
#include "Data/OrbMaterialManager.h"

static FORCEINLINE int32 MakeSubOrbId(int32 ItemId, int32 SubIdx)
{
	return (ItemId << 8) | (SubIdx & 0xFF);
}

// Optional utility to rotate a raw FVector2D (not hex axial) by yaw degrees.
static FORCEINLINE FVector2D RotateXY(const FVector2D& V, float YawDeg)
{
	const float Rad = FMath::DegreesToRadians(YawDeg);
	const float C = FMath::Cos(Rad);
	const float S = FMath::Sin(Rad);
	return FVector2D(C * V.X - S * V.Y, S * V.X + C * V.Y);
}

// Helper for shortest yaw interpolation
static float LerpAngleDeg(float A, float B, float T)
{
	float Delta = FMath::UnwindDegrees(B - A);
	// Shortest path
	if (Delta > 180.f) Delta -= 360.f;
	if (Delta < -180.f) Delta += 360.f;
	return FMath::UnwindDegrees(A + Delta * T);
}

// Compute arm base world position (without iterating arm length)
static FVector ComputeArmBaseWorld(const FArmState& S, const AActor* Anchor, float Tile, float GridYaw, EHexOrientation Ori)
{
	const FVector AnchorV = Anchor ? Anchor->GetActorLocation() : FVector::ZeroVector;
	const FHexAxial A(S.BaseHex.Q, S.BaseHex.R);
	const FVector2D LocalXY = HexCoord::AxialToXY(A, Tile, Ori);
	const FVector2D RotXY = HexCoord::Rotate2D(LocalXY, GridYaw);
	return AnchorV + FVector(RotXY.X, RotXY.Y, 0.f);
}

// Returns grip world center (end effector) at a given yaw (continuous) for pure rotation (no extension animation yet)
FVector UOrbRenderer::ComputeArmGripAtYaw(const FArmState& S, float YawDeg, const AActor* Anchor, float Tile, float GridYaw, EHexOrientation Ori) const
{
	const FVector Base = ComputeArmBaseWorld(S, Anchor, Tile, GridYaw, Ori);
	// Direction unit in local (grid-rotated) space
	const float Rad = FMath::DegreesToRadians(YawDeg);
	const FVector2D Dir(FMath::Cos(Rad), FMath::Sin(Rad));
	const float Reach = float(S.ArmLen) * Tile;
	return Base + FVector(Dir.X * Reach, Dir.Y * Reach, 0.f);
}

UOrbRenderer::UOrbRenderer()
{
	PrimaryComponentTick.bCanEverTick = true;

	// Initialize bonded orb mesh to a small sphere by default
	static ConstructorHelpers::FObjectFinder<UStaticMesh> SphereMesh(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> BasicMat(TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	if (!BondedOrbMesh && SphereMesh.Succeeded()) BondedOrbMesh = SphereMesh.Object;
	if (!BondedOrbMaterial && BasicMat.Succeeded()) BondedOrbMaterial = BasicMat.Object;
}

void UOrbRenderer::OnRegister()
{
	Super::OnRegister();
	EnsureISM();
}

void UOrbRenderer::BeginPlay()
{
	Super::BeginPlay();
	EnsureISM();

	if (UWorld* W = GetWorld())
	{
		Driver = AOrbSimDriver::Find(W);
		if (Driver) Sim = Driver->GetSim();
	}

	RebuildCurrPos();
	PrevPos = CurrPos;
	PrevRot = CurrRot;                      // <-- add this
	PrevAlpha = Driver ? Driver->GetStepAlpha() : 0.f; // optional, keeps first frame clean
}


void UOrbRenderer::EnsureISM()
{
	AActor* Owner = GetOwner();
	if (!Owner) return;

	// Get material manager
	UOrbMaterialManager* MaterialManager = UOrbMaterialManager::FindInWorld(this);
	if (!MaterialManager) return;

	// Get the base mesh from material manager
	UStaticMesh* BaseMesh = MaterialManager->GetOrbMesh();
	if (!BaseMesh) return;

	// Create ISM components for each orb type
	TArray<EOrbType> AllOrbTypes = {
		EOrbType::None,
		EOrbType::BaseA,
		EOrbType::BaseB,
		EOrbType::BaseC,
		EOrbType::MetalA,
		EOrbType::CatalystX
	};

	for (EOrbType OrbType : AllOrbTypes)
	{
		// Skip if already created
		if (OrbTypeISMs.Contains(OrbType)) continue;

		// Create ISM component for this orb type
		FString ComponentName = FString::Printf(TEXT("OrbRenderer_ISM_%d"), (int32)OrbType);
		UInstancedStaticMeshComponent* ISM = NewObject<UInstancedStaticMeshComponent>(Owner, *ComponentName);
		ISM->RegisterComponent();

		if (Owner->GetRootComponent())
		{
			ISM->AttachToComponent(Owner->GetRootComponent(), FAttachmentTransformRules::KeepRelativeTransform);
		}

		// Set mesh and material for this orb type
		ISM->SetStaticMesh(BaseMesh);
		
		UMaterialInterface* TypeMaterial = MaterialManager->GetMaterialForOrbType(OrbType);
		if (TypeMaterial)
		{
			ISM->SetMaterial(0, TypeMaterial);
			ISM->SetRenderCustomDepth(false);
		}
		
		ISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		
		// Store in map
		OrbTypeISMs.Add(OrbType, ISM);
	}

	// Create bonded orb ISM
	BondedOrbISM = NewObject<UInstancedStaticMeshComponent>(Owner, TEXT("BondedOrbRenderer_ISM"));
	BondedOrbISM->RegisterComponent();

	if (Owner->GetRootComponent())
	{
		BondedOrbISM->AttachToComponent(Owner->GetRootComponent(), FAttachmentTransformRules::KeepRelativeTransform);
	}

	if (BondedOrbMesh) BondedOrbISM->SetStaticMesh(BondedOrbMesh);
	if (BondedOrbMaterial) 
	{
		BondedOrbISM->SetMaterial(0, BondedOrbMaterial);
		// Ensure the material renders as opaque (not transparent)
		BondedOrbISM->SetRenderCustomDepth(false);
	}
	BondedOrbISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
}

AActor* UOrbRenderer::GetAnchor() const
{
	if (bOverrideAnchor && AnchorOverride) return AnchorOverride;
	return Driver ? Driver->GetGridAnchor() : nullptr;
}

void UOrbRenderer::TickComponent(float DeltaTime, enum ELevelTick, FActorComponentTickFunction*)
{
	if ((!Driver || !Sim) && bAutoReacquire)
	{
		if (UWorld* W = GetWorld())
		{
			if (!Driver) Driver = AOrbSimDriver::Find(W);
			if (Driver && !Sim) Sim = Driver->GetSim();
			if (Sim) { RebuildCurrPos(); PrevPos = CurrPos; PrevRot = CurrRot; }
		}
	}
	if (!Driver || !Sim) return;

	const float Alpha = Driver->GetStepAlpha();
	const int32 Step = Sim->GetStepCounter();

	// Feed frame alpha early
	if (bUseSimVisualInterpolation)
	{
		Sim->SetFrameAlpha(Alpha);
	}

	// Detect new fixed step via counter instead of Alpha wrap
	if (Step != LastSeenStepCounter)
	{
		LastSeenStepCounter = Step;

		// Capture previous arm facings before rebuild
		PrevArmFacing = CurrArmFacing;
		CurrArmFacing.Reset();

		const TMap<int32, FVector>  OldPos = CurrPos;
		const TMap<int32, FRotator> OldRot = CurrRot;
		RebuildCurrPos();
		PrevPos = OldPos;
		PrevRot = OldRot;

		// Fill CurrArmFacing after new sim snapshot
		for (int32 ArmIdx = 0; ArmIdx < Sim->Arms.Num(); ++ArmIdx)
		{
			CurrArmFacing.Add(ArmIdx, (uint8)Sim->Arms[ArmIdx].Facing);
			// Ensure Prev has entry (for first step or newly added arms)
			if (!PrevArmFacing.Contains(ArmIdx))
				PrevArmFacing.Add(ArmIdx, CurrArmFacing[ArmIdx]);
		}

		if (bUseSimVisualInterpolation)
		{
			Sim->SetFrameAlpha(0.f);
		}
	}

	PrevAlpha = Alpha;
	
	// Update animated orbs
	Sim->UpdateAnimatedOrbs(DeltaTime);
	
	// Process pending bond formations
	Sim->ProcessPendingBondFormations(DeltaTime);
	
	RebuildInstances();
	RebuildBondedOrbs();
}


void UOrbRenderer::RebuildCurrPos()
{
	CurrPos.Reset();
	CurrRot.Reset();
	if (!Sim) return;

	const AActor* Anchor = GetAnchor();
	const FVector AnchorV = Anchor ? Anchor->GetActorLocation() : FVector::ZeroVector;
	const float Tile = Driver ? Driver->GetTileSize() : Sim->TileSizeUU;
	const float Yaw = Driver ? Driver->GetGridYaw() : 0.f;
	const EHexOrientation Ori = Driver ? Driver->GetGridOrientation() : EHexOrientation::PointyTop;

	// Board items (not held)
	for (const auto& KV : Sim->ItemMap)
	{
		const FItem& Item = KV.Value;
		const int32 ItemId = Item.Id;
		if (ItemId == 0) continue;

		const FHexAxial A(KV.Key.Q, KV.Key.R);
		const FVector2D LocalXY = HexCoord::AxialToXY(A, Tile, Ori);
		const FVector2D RotXY = HexCoord::Rotate2D(LocalXY, Yaw);
		const FVector TileWorld = AnchorV + FVector(RotXY.X, RotXY.Y, 0.f);

		if (!Item.bIsCluster)
		{
			CurrPos.Add(ItemId, TileWorld);
		}
		else
		{
			for (int32 SubIdx = 0; SubIdx < Item.Orbs.Num(); ++SubIdx)
			{
				const FOrbData& Sub = Item.Orbs[SubIdx];
				// Treat offsets as tile-based; scale by Tile
				const FVector2D OffRot = RotateXY(Sub.Offset * Tile, Yaw);
				const FVector SubWorld = TileWorld + FVector(OffRot.X, OffRot.Y, 0.f);
				const int32 SubId = MakeSubOrbId(ItemId, SubIdx);
				CurrPos.Add(SubId, SubWorld);
			}
		}
	}

	// Held items (per arm)
	for (const FArmState& S : Sim->Arms)
	{
		if (!S.bHolding) continue;
		const FItem& Held = S.Held;
		if (Held.Id == 0) continue;

		FHex Grip = S.BaseHex;
		for (int i = 0; i < S.ArmLen; ++i)
			Grip = HexUtil::Neighbor(Grip, S.Facing);

		const FHexAxial GA(Grip.Q, Grip.R);
		const FVector2D LocalXY = HexCoord::AxialToXY(GA, Tile, Ori);
		const FVector2D RotXY = HexCoord::Rotate2D(LocalXY, Yaw);
		const FVector GripWorld = AnchorV + FVector(RotXY.X, RotXY.Y, 0.f);

		const float OrbYaw = DirToYaw(static_cast<uint8>(S.Facing));
		const FRotator BaseRot(0.f, OrbYaw, 0.f);

		if (!Held.bIsCluster)
		{
			CurrPos.Add(Held.Id, GripWorld);
			CurrRot.Add(Held.Id, BaseRot);
		}
		else
		{
			//for (int32 SubIdx = 0; SubIdx < Held.Orbs.Num(); ++SubIdx)
			//{
			//	const FOrbData& Sub = Held.Orbs[SubIdx];
			//	// Rotate + scale offsets (tile units -> UU)
			//	const FVector2D OffRot = RotateXY(Sub.Offset * Tile, OrbYaw + Yaw);
			//	const FVector SubWorld = GripWorld + FVector(OffRot.X, OffRot.Y, 0.f);
			//	const int32 SubId = MakeSubOrbId(Held.Id, SubIdx);
			//	CurrPos.Add(SubId, SubWorld);
			//	CurrRot.Add(SubId, BaseRot);
			//}
		}
	}
}

void UOrbRenderer::RebuildInstances()
{
	EnsureISM();
	if (!Driver || !Sim) return;

	const float Alpha = Driver->GetStepAlpha();
	
	// Clear all ISM instances
	for (auto& KV : OrbTypeISMs)
	{
		if (KV.Value)
		{
			KV.Value->ClearInstances();
		}
	}

	// Build set of held item ids so we don't double render
	TSet<int32> HeldIds;
	for (const FArmState& S : Sim->Arms)
	{
		if (S.bHolding && S.Held.Id != 0)
		{
			HeldIds.Add(S.Held.Id);
			// Also add sub-orb IDs for clusters
			if (S.Held.bIsCluster)
			{
				for (const FOrbData& Sub : S.Held.Orbs)
				{
					if (Sub.ItemId > 0)
					{
						HeldIds.Add(Sub.ItemId);
					}
				}
			}
		}
	}

	if (bUseSimVisualInterpolation)
	{
		// Direct per-item query (belt interpolation already applied in sim)
		for (const auto& KV : Sim->ItemMap)
		{
			const int32 Id = KV.Value.Id;
			if (Id == 0) continue;
			if (HeldIds.Contains(Id)) continue;

			FVector Pos;
			if (!Sim->GetItemVisualLocation(Id, Pos))
			{
				// Fallback: try current map hex center (already stored in CurrPos if present)
				if (const FVector* CP = CurrPos.Find(Id))
					Pos = *CP;
				else
					continue;
			}

			// Get the orb type and find the appropriate ISM
			EOrbType OrbType = EOrbType::BaseA; // Default
			if (KV.Value.bIsCluster && KV.Value.Orbs.Num() > 0)
			{
				OrbType = (EOrbType)KV.Value.Orbs[0].Type;
			}
			else if (!KV.Value.bIsCluster)
			{
				OrbType = KV.Value.Single.Type;
			}

			// Add to the appropriate ISM
			if (UInstancedStaticMeshComponent** ISMPtr = OrbTypeISMs.Find(OrbType))
			{
				if (UInstancedStaticMeshComponent* ISM = *ISMPtr)
				{
					ISM->AddInstance(FTransform(FRotator::ZeroRotator, Pos, FVector(OrbScale)));
				}
			}
		}
		
		// Render animated orbs (inputs ascending, outputs descending)
		RenderAnimatedOrbs();
		
		return; // done
	}

	// Legacy prev->curr interpolation path
	TSet<int32> AllIds;
	PrevPos.GetKeys(AllIds);
	for (const auto& K : CurrPos) AllIds.Add(K.Key);

	for (int32 Id : AllIds)
	{
		if (HeldIds.Contains(Id)) continue;

		const FVector* A = PrevPos.Find(Id);
		const FVector* B = CurrPos.Find(Id);
		const FRotator* RotA = PrevRot.Find(Id);
		const FRotator* RotB = CurrRot.Find(Id);

		const FVector From = A ? *A : (B ? *B : FVector::ZeroVector);
		const FVector To = B ? *B : From;

		FVector P;
		// Check if this orb is currently held by an arm
		bool bHeldByArm = false;
		//FVector ArmBase;
		for (const FArmState& S : Sim->Arms)
		{
			if (S.bHolding && S.Held.Id != 0)
			{
				HeldIds.Add(S.Held.Id);
				if (S.Held.bIsCluster)
				{
					for (int32 i = 0; i < S.Held.Orbs.Num(); ++i)
					{
						HeldIds.Add(MakeSubOrbId(S.Held.Id, i)); // Skip sub‑orb ISM instances while held
					}
				}
			}
		}

		//if (bHeldByArm)
		//{
		//	// Interpolate along arc from From to To, centered at ArmBase
		//	P = InterpArc(ArmBase, From, To, Alpha);
		//}
	//else
		// Default straight interpolation
		P = FMath::Lerp(From, To, Alpha);

		// Arm rotation arc (only if held and position changed by rotation)
		if (bHeldByArm)
		{
			// Find arm record
			const AActor* Anchor = GetAnchor();
			const float Tile = Driver ? Driver->GetTileSize() : Sim->TileSizeUU;
			const float GridYaw = Driver ? Driver->GetGridYaw() : 0.f;
			const EHexOrientation Ori = Driver ? Driver->GetGridOrientation() : EHexOrientation::PointyTop;

			for (int32 ArmIdx = 0; ArmIdx < Sim->Arms.Num(); ++ArmIdx)
			{
				const FArmState& S = Sim->Arms[ArmIdx];
				if (S.bHolding && S.Held.Id == Id)
				{
					const uint8* PrevF = PrevArmFacing.Find(ArmIdx);
					const uint8* CurrF = CurrArmFacing.Find(ArmIdx);
					if (PrevF && CurrF)
					{
						float YawA = DirToYaw(*PrevF);
						float YawB = DirToYaw(*CurrF);
						float YawI = LerpAngleDeg(YawA, YawB, Alpha);
						P = ComputeArmGripAtYaw(S, YawI, Anchor, Tile, GridYaw, Ori);
					}
					break;
				}
			}
		}

		FRotator RA = RotA ? *RotA : FRotator::ZeroRotator;
		FRotator RB = RotB ? *RotB : RA;
		FRotator R = FMath::Lerp(RA, RB, Alpha);

		// Get the orb type for this item
		EOrbType OrbType = EOrbType::BaseA; // Default
		for (const auto& KV : Sim->ItemMap)
		{
			if (KV.Value.Id == Id)
			{
				if (KV.Value.bIsCluster && KV.Value.Orbs.Num() > 0)
				{
					OrbType = (EOrbType)KV.Value.Orbs[0].Type;
				}
				else if (!KV.Value.bIsCluster)
				{
					OrbType = KV.Value.Single.Type;
				}
				break;
			}
		}

		// Add to the appropriate ISM
		if (UInstancedStaticMeshComponent** ISMPtr = OrbTypeISMs.Find(OrbType))
		{
			if (UInstancedStaticMeshComponent* ISM = *ISMPtr)
			{
				ISM->AddInstance(FTransform(R, P, FVector(OrbScale)));
			}
		}
	}
}

float UOrbRenderer::DirToYaw(uint8 Dir) const
{
	const float GridYaw = Driver ? Driver->GetGridYaw() : 0.f;
	return FMath::UnwindDegrees(GridYaw + 60.f * float(Dir % 6));
}

void UOrbRenderer::RebuildBondedOrbs()
{
	// Bonded orb rendering is now handled by BasicArm for held clusters
	// This function is kept for potential future use with board-only bonds
	EnsureISM();
	if (!BondedOrbISM || !Driver || !Sim) return;

	BondedOrbISM->ClearInstances();
	if (!BondedOrbMesh) return;

	// For now, only render bonds between board items (not held items)
	// Held cluster bonds are handled by BasicArm
	const float Alpha = Driver->GetStepAlpha();
	const AActor* Anchor = GetAnchor();
	const FVector AnchorV = Anchor ? Anchor->GetActorLocation() : FVector::ZeroVector;
	const float Tile = Driver ? Driver->GetTileSize() : Sim->TileSizeUU;
	const float Yaw = Driver ? Driver->GetGridYaw() : 0.f;
	const EHexOrientation Ori = Driver ? Driver->GetGridOrientation() : EHexOrientation::PointyTop;

	// Render bonded orb connections for board items only
	for (const auto& Bond : Sim->GetBonds())
	{
		if (!Bond.bActive) continue;

		// Get positions of both bonded items
		FVector PosA, PosB;
		bool bFoundA = false, bFoundB = false;

		// Helper function to get interpolated position for board items only
		auto GetInterpolatedPosition = [&](int32 ItemId, FVector& OutPos) -> bool
		{
			// Only check position maps for board items
			const FVector* CurrPosItem = CurrPos.Find(ItemId);
			const FVector* PrevPosItem = PrevPos.Find(ItemId);
			
			if (CurrPosItem)
			{
				const FVector From = PrevPosItem ? *PrevPosItem : *CurrPosItem;
				OutPos = FMath::Lerp(From, *CurrPosItem, Alpha);
				return true;
			}
			return false;
		};

		// Get positions for both bonded items
		bFoundA = GetInterpolatedPosition(Bond.ItemA, PosA);
		bFoundB = GetInterpolatedPosition(Bond.ItemB, PosB);

		// If we found both positions, create a connection mesh
		if (bFoundA && bFoundB)
		{
			// Calculate midpoint and direction
			FVector Midpoint = (PosA + PosB) * 0.5f;
			FVector Direction = (PosB - PosA).GetSafeNormal();
			float Distance = FVector::Dist(PosA, PosB);

			// For a cylinder that's standing up by default, we need to rotate it to align with the bond direction
			// The cylinder's default "up" axis is Z, so we need to rotate it so its Z-axis aligns with the bond direction
			FVector Up = Direction; // The bond direction becomes the new "up" for the cylinder
			FVector Right = FVector::CrossProduct(Up, FVector::UpVector).GetSafeNormal();
			if (Right.IsNearlyZero())
			{
				// If the bond is vertical, use a different approach
				Right = FVector::RightVector;
			}
			FVector Forward = FVector::CrossProduct(Right, Up).GetSafeNormal();
			
			// Create rotation matrix from the three axes
			FMatrix RotationMatrix = FMatrix(Forward, Right, Up, FVector::ZeroVector);
			FRotator Rotation = FQuat(RotationMatrix).Rotator();

			// Scale the cylinder to fit the bond length
			// The cylinder's length is along its Z-axis, so we scale Z by the distance
			FVector Scale = FVector(BondedOrbScale, BondedOrbScale, 1.f);

			// Add the bonded orb instance
			BondedOrbISM->AddInstance(FTransform(Rotation, Midpoint, Scale));
		}
	}
}

void UOrbRenderer::RenderAnimatedOrbs()
{
	if (!Sim) return;
	
	const TArray<USimWorldHex::FAnimatedOrb>& AnimatedOrbs = Sim->GetAnimatedOrbs();
	if (AnimatedOrbs.Num() == 0) return;
	
	const AActor* Anchor = GetAnchor();
	const FVector AnchorV = Anchor ? Anchor->GetActorLocation() : FVector::ZeroVector;
	const float Tile = Driver ? Driver->GetTileSize() : Sim->TileSizeUU;
	const float Yaw = Driver ? Driver->GetGridYaw() : 0.f;
	const EHexOrientation Ori = Driver ? Driver->GetGridOrientation() : EHexOrientation::PointyTop;
	
	for (const USimWorldHex::FAnimatedOrb& AnimatedOrb : AnimatedOrbs)
	{
		if (!AnimatedOrb.bActive) continue;
		
		// Calculate base position at the hex center
		const FHexAxial A(AnimatedOrb.Hex.Q, AnimatedOrb.Hex.R);
		const FVector2D LocalXY = HexCoord::AxialToXY(A, Tile, Ori);
		const FVector2D RotXY = HexCoord::Rotate2D(LocalXY, Yaw);
		const FVector BasePos = AnchorV + FVector(RotXY.X, RotXY.Y, 0.f);
		
		// Calculate animation progress (0.0 to 1.0)
		const float Progress = FMath::Clamp(AnimatedOrb.AnimationTime / AnimatedOrb.AnimationDuration, 0.f, 1.f);
		
		// Calculate Z offset based on animation type and progress
		float ZOffset = 0.f;
		if (AnimatedOrb.bIsInput)
		{
			// Input: start below ground, ascend to tile level
			ZOffset = FMath::Lerp(-50.f, 0.f, Progress);
		}
		else
		{
			// Output: start at tile level, descend below ground
			ZOffset = FMath::Lerp(0.f, -50.f, Progress);
		}
		
		const FVector FinalPos = BasePos + FVector(0.f, 0.f, ZOffset);
		
		// Get the orb type
		EOrbType OrbType = EOrbType::BaseA; // Default
		if (AnimatedOrb.Item.bIsCluster && AnimatedOrb.Item.Orbs.Num() > 0)
		{
			OrbType = (EOrbType)AnimatedOrb.Item.Orbs[0].Type;
		}
		else if (!AnimatedOrb.Item.bIsCluster)
		{
			OrbType = AnimatedOrb.Item.Single.Type;
		}
		
		// Add to the appropriate ISM
		if (UInstancedStaticMeshComponent** ISMPtr = OrbTypeISMs.Find(OrbType))
		{
			if (UInstancedStaticMeshComponent* ISM = *ISMPtr)
			{
				ISM->AddInstance(FTransform(FRotator::ZeroRotator, FinalPos, FVector(OrbScale)));
			}
		}
	}
}

