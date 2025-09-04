#pragma once
#include "GameFramework/Actor.h"
#include "BondGlyphActor.generated.h"

class UStaticMeshComponent;
class UMaterialInterface;
class UMaterialInstanceDynamic;

UCLASS()
class AUTOMATIONGAME_API ABondGlyphActor : public AActor
{
	GENERATED_BODY()
public:
	ABondGlyphActor();

	// Original explicit init (kept)
	void Init(const FVector& CenterMidpoint,
		float YawDeg,
		float TileRadius,
		float Span,
		bool bPointyTop,
		UStaticMesh* QuadMesh,
		UMaterialInterface* BondMat,
		const FLinearColor& Tint);

	// NEW: build from two hex centers (preferred, orientation flag is flatTop)
	void InitFromHexes(const FVector& HexCenterA,
		const FVector& HexCenterB,
		float TileRadius,
		bool bFlatTop,
		UStaticMesh* QuadMesh,
		UMaterialInterface* BondMat,
		const FLinearColor& Tint);

protected:
	UPROPERTY() UStaticMeshComponent* Quad = nullptr;
	UPROPERTY() UMaterialInstanceDynamic* MID = nullptr;

	// Shared internal setup
	void ApplyParams(const FVector& Mid,
		float AxisYawDeg,
		float TileRadius,
		float Span,
		bool bFlatTop,
		UStaticMesh* QuadMesh,
		UMaterialInterface* BondMat,
		const FLinearColor& Tint);
};