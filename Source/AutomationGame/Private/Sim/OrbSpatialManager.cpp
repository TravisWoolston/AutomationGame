#include "Sim/OrbSpatialManager.h"
#include "Sim/SimWorldHex.h"

void UOrbSpatialManager::Initialize(USimWorldHex* InSimWorld)
{
	SimWorld = InSimWorld;
}

bool UOrbSpatialManager::IsArmWaiting(int32 ArmIndex) const
{
	if (!SimWorld.IsValid() || ArmIndex < 0 || ArmIndex >= SimWorld->Arms.Num())
		return false;
	return SimWorld->Arms[ArmIndex].WaitCounter > 0;
}

bool UOrbSpatialManager::IsArmRotating(int32 ArmIndex) const
{
	if (!SimWorld.IsValid() || ArmIndex < 0 || ArmIndex >= SimWorld->Arms.Num())
		return false;
	return SimWorld->Arms[ArmIndex].bRotationCompleted;
}