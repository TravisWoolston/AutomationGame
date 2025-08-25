#pragma once
#include "CoreMinimal.h"

DECLARE_LOG_CATEGORY_EXTERN(LogAGDiag, Log, All);

namespace AGDiag
{
	/** Logs to Output Log and to screen (if GEngine available). */
	inline void Screen(const UObject* WorldContext, const FString& Msg, FColor Color = FColor::Yellow, float Time = 2.f)
	{
		UE_LOG(LogAGDiag, Log, TEXT("%s"), *Msg);
		if (GEngine)
		{
			GEngine->AddOnScreenDebugMessage(-1, Time, Color, Msg);
		}
	}
}