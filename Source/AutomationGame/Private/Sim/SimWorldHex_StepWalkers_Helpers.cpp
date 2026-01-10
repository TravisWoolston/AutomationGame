#include "Sim/SimWorldHex.h"
#include "Sim/HexTypes.h"
#include "Sim/WalkerTypes.h"
#include "Sim/HexCoord.h"
#include "Actors/RequestBeacon.h"

bool USimWorldHex::IsHexOccupied(const FHex& H, const TSet<FHex>& HeldOrbHexes, const TSet<FHex>& ArmBaseHexes, const TSet<FHex>& HoldingFootHexes) const
{
	// Check for items (orbs on ground or belts)
	if (ItemMap.Contains(H))
	{
		return true;
	}
	
	// Check for held orbs (by arms)
	if (HeldOrbHexes.Contains(H))
	{
		return true;
	}
	
	// Check for arms
	if (ArmBaseHexes.Contains(H))
	{
		return true;
	}
	
	// Holding feet are NOT obstacles (they're raised and carrying items to be placed elsewhere)
	if (HoldingFootHexes.Contains(H))
	{
		return false;
	}
	
	return false;
}

TArray<FFootState*> USimWorldHex::GetAllFeet(FWalkerState& W) const
{
	return { &W.DFoot, &W.LFoot, &W.RFoot, &W.UFoot };
}

bool USimWorldHex::AreFeetAdjacent(int32 FootIndexA, int32 FootIndexB) const
{
	if (FootIndexA == FootIndexB) return false;

	auto GetRingIndex = [](int32 FootIndex) -> int32
	{
		switch (FootIndex)
		{
		case 0: return 2; // D
		case 1: return 3; // L
		case 2: return 1; // R
		case 3: return 0; // U
		default: return -1;
		}
	};

	const int32 RingA = GetRingIndex(FootIndexA);
	const int32 RingB = GetRingIndex(FootIndexB);
	if (RingA < 0 || RingB < 0) return false;

	const int32 Diff = FMath::Abs(RingA - RingB);
	return (Diff == 1 || Diff == 3);
}

FString USimWorldHex::GetFixedQuadrantFromHex(const FHex& BodyHex, const FHex& TestHex, float RotationOffsetDeg) const
{
	const int32 q = TestHex.Q - BodyHex.Q;
	const int32 r = TestHex.R - BodyHex.R;

	const float x = 1.7320508f * (float)q + 0.8660254f * (float)r;
	const float y = 1.5f * (float)r;

	float Angle = FMath::Atan2(y, x);
	Angle = FMath::RadiansToDegrees(Angle);
	Angle -= RotationOffsetDeg;
	while (Angle <= -180.f) Angle += 360.f;
	while (Angle > 180.f) Angle -= 360.f;

	if (Angle >= -45.f && Angle < 45.f) return TEXT("E");
	if (Angle >= 45.f && Angle < 135.f) return TEXT("S");
	if (Angle >= -135.f && Angle < -45.f) return TEXT("N");
	return TEXT("W");
}

float USimWorldHex::CalculateSimWalkerRotation(const FWalkerState& W) const
{
	float TotalWeightedSin = 0.f;
	float TotalWeightedCos = 0.f;
	float TotalWeight = 0.f;

	// Calculate movement lead to compensate
	float MoveDQ = 0.f, MoveDR = 0.f;
	if (W.bProcessingRequest && W.BaseHex != W.CurrentTargetHex)
	{
		float MDQ = (float)(W.CurrentTargetHex.Q - W.BaseHex.Q);
		float MDR = (float)(W.CurrentTargetHex.R - W.BaseHex.R);
		float MLen = FMath::Sqrt(MDQ*MDQ + MDR*MDR + (MDQ+MDR)*(MDQ+MDR));
		if (MLen > 0.001f) { MoveDQ = MDQ / MLen; MoveDR = MDR / MLen; }
	}
	float LeadDist = (float)W.ReachDistance * 0.4f;

	TArray<FFootState*> AllFeet = GetAllFeet(const_cast<FWalkerState&>(W));
	for (int32 i = 0; i < AllFeet.Num(); ++i)
	{
		FFootState* Foot = AllFeet[i];
		if (Foot->StepState != EFootStepState::Planted) continue;

		// Compensate for movement lead: check deviation from (Center + Lead)
		float DQ = (float)(Foot->CurrentTile.Q - W.BaseHex.Q) - MoveDQ * LeadDist;
		float DR = (float)(Foot->CurrentTile.R - W.BaseHex.R) - MoveDR * LeadDist;
		
		float X = 1.7320508f * DQ + 0.8660254f * DR;
		float Y = 1.5f * DR;
		float Angle = FMath::Atan2(Y, X);

		float IdealAngle = 0.f;
		switch (i)
		{
		case 3: IdealAngle = -PI * 0.5f; break; // U/North
		case 2: IdealAngle = 0.f; break;         // R/East
		case 0: IdealAngle = PI * 0.5f; break;  // D/South
		case 1: IdealAngle = PI; break;          // L/West
		}

		float Diff = FRotator::NormalizeAxis(FMath::RadiansToDegrees(Angle - IdealAngle));
		float DiffRad = FMath::DegreesToRadians(Diff);

		TotalWeightedSin += FMath::Sin(DiffRad);
		TotalWeightedCos += FMath::Cos(DiffRad);
		TotalWeight += 1.0f;
	}

	if (TotalWeight > 0.1f)
	{
		float AvgAngle = FMath::Atan2(TotalWeightedSin, TotalWeightedCos);
		float NewRot = FRotator::NormalizeAxis(FMath::RadiansToDegrees(AvgAngle));
		NewRot = FMath::RoundToFloat(NewRot * 10.f) / 10.f;

		if (FMath::Abs(NewRot - W.QuadrantRotationOffset) > 2.0f)
		{
			return NewRot;
		}
	}

	return W.QuadrantRotationOffset;
}

FHex USimWorldHex::GetQuadrantCenterHex(const FHex& BodyHex, int32 FootIndex, int32 ReachDistance, float MinReach, float RotationOffsetDeg) const
{
	float TargetAngleRad = 0.f;
	switch (FootIndex)
	{
	case 3: TargetAngleRad = -PI * 0.5f; break; // U/North
	case 2: TargetAngleRad = 0.f; break;         // R/East
	case 0: TargetAngleRad = PI * 0.5f; break;  // D/South
	case 1: TargetAngleRad = PI; break;          // L/West
	}
	
	const float TotalAngleRad = TargetAngleRad + FMath::DegreesToRadians(RotationOffsetDeg);
	const float C = FMath::Cos(TotalAngleRad);
	const float S = FMath::Sin(TotalAngleRad);
	
	// Cartesian to Axial (Pointy Top Inverse)
	// q = x/sqrt(3) - y/3
	// r = y * 2/3
	const float QuadDQ = (0.5773503f * C - 0.3333333f * S);
	const float QuadDR = (0.6666667f * S);

	float CenterDistanceHex = (MinReach > 0.1f) ? (MinReach + (float)ReachDistance) * 0.5f : (float)ReachDistance * 0.5f;
	CenterDistanceHex = FMath::Clamp(CenterDistanceHex, MinReach + 0.1f, (float)ReachDistance);
	
	// Convert Hex Distance to Cartesian Space (sqrt(3) factor)
	float CenterDistanceCartesian = CenterDistanceHex * 1.7320508f;
	
	float IdealQ = (float)BodyHex.Q + QuadDQ * CenterDistanceCartesian;
	float IdealR = (float)BodyHex.R + QuadDR * CenterDistanceCartesian;
	
	return FHex(FMath::RoundToInt(IdealQ), FMath::RoundToInt(IdealR), BodyHex.Level);
}
