#include "BondGlyphActor.h"
#include "Components/StaticMeshComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Kismet/KismetMathLibrary.h"

ABondGlyphActor::ABondGlyphActor()
{
	PrimaryActorTick.bCanEverTick = false;
	Quad = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("BondQuad"));
	SetRootComponent(Quad);
	Quad->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Quad->SetMobility(EComponentMobility::Movable);
}

void ABondGlyphActor::Init(const FVector& CenterMidpoint,
	float YawDeg,
	float TileRadius,
	float Span,
	bool bPointyTop,
	UStaticMesh* QuadMesh,
	UMaterialInterface* BondMat,
	const FLinearColor& Tint)
{
	// Interpret bPointyTop inverse as bFlatTop
	ApplyParams(CenterMidpoint, YawDeg, TileRadius, Span, !bPointyTop, QuadMesh, BondMat, Tint);
}

void ABondGlyphActor::InitFromHexes(const FVector& HexCenterA,
	const FVector& HexCenterB,
	float TileRadius,
	bool bFlatTop,
	UStaticMesh* QuadMesh,
	UMaterialInterface* BondMat,
	const FLinearColor& Tint)
{
	const FVector Mid = (HexCenterA + HexCenterB) * 0.5f;
	const FVector2D Delta2D(HexCenterB.X - HexCenterA.X, HexCenterB.Y - HexCenterA.Y);
	const float Span = Delta2D.Size();              // center-to-center
	const float AxisYawDeg = FMath::RadiansToDegrees(FMath::Atan2(Delta2D.Y, Delta2D.X));
	ApplyParams(Mid, AxisYawDeg, TileRadius, Span, bFlatTop, QuadMesh, BondMat, Tint);
}

void ABondGlyphActor::ApplyParams(const FVector& Mid,
	float AxisYawDeg,
	float TileRadius,
	float Span,
	bool bFlatTop,
	UStaticMesh* QuadMesh,
	UMaterialInterface* BondMat,
	const FLinearColor& Tint)
{
	SetActorLocation(Mid);
	SetActorRotation(FRotator(0.f, AxisYawDeg, 0.f));

	if (QuadMesh)
	{
		Quad->SetStaticMesh(QuadMesh);
	}

	if (BondMat)
	{
		MID = UMaterialInstanceDynamic::Create(BondMat, this);
		Quad->SetMaterial(0, MID);
		if (MID)
		{
			// Pass orientation as a 0/1 scalar (1 = flat-top)
			MID->SetScalarParameterValue(TEXT("TileRadius"), TileRadius);
			MID->SetScalarParameterValue(TEXT("Span"), Span);
			MID->SetScalarParameterValue(TEXT("PadW"), TileRadius * 0.22f);
			MID->SetScalarParameterValue(TEXT("PadH"), TileRadius * 0.18f);
			MID->SetScalarParameterValue(TEXT("FlatTop"), bFlatTop ? 1.f : 0.f);
			MID->SetVectorParameterValue(TEXT("Tint"), Tint);
		}
	}

	// Geometric extents for scaling the quad (local X = bond axis, Y = perpendicular)
	// For a single hex:
	// Flat-top: Width = 2R, Height = ?3 * R
	// Pointy-top: Width = ?3 * R, Height = 2R
	const float Sqrt3 = 1.7320508075688772f;
	float HexWidthAxis;     // along local X when the axis is horizontal
	float HexHeightPerp;    // along local Y
	if (bFlatTop)
	{
		HexWidthAxis = 2.f * TileRadius;
		HexHeightPerp = Sqrt3 * TileRadius;
	}
	else
	{
		HexWidthAxis = Sqrt3 * TileRadius;
		HexHeightPerp = 2.f * TileRadius;
	}

	// Total width along the bond axis = span between centers + width contribution of both halves.
	const float FullWidth = Span + HexWidthAxis;
	const float FullHeight = HexHeightPerp;

	// Assuming the plane mesh is 100x100 uu; adjust scale accordingly
	Quad->SetWorldScale3D(FVector(FullWidth * 0.01f, FullHeight * 0.01f, 1.f));
}