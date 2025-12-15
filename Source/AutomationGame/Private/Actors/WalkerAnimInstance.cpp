#include "Actors/WalkerAnimInstance.h"
#include "Actors/BasicWalker.h"

ABasicWalker* UWalkerAnimInstance::GetOwningWalker() const
{
	AActor* Owner = GetOwningActor();
	if (Owner)
	{
		return Cast<ABasicWalker>(Owner);
	}
	return nullptr;
}
