#pragma once

#include "CoreMinimal.h"
#include "TriggerTypes.generated.h"

// Owner type enum
UENUM(BlueprintType)
enum class ETriggerTileOwnerType : uint8 { Belt, Arm };

// Trigger tile variant
UENUM(BlueprintType)
enum class ETriggerVariant : uint8 { Positive, Negative };

/** Struct representing an owner (host) for a TriggerTile signal connection */
USTRUCT(BlueprintType)
struct FTriggerOwner {
  GENERATED_BODY()

  UPROPERTY(EditAnywhere, BlueprintReadWrite)
  int32 Q = 0;

  UPROPERTY(EditAnywhere, BlueprintReadWrite)
  int32 R = 0;

  UPROPERTY(EditAnywhere, BlueprintReadWrite)
  ETriggerTileOwnerType Type = ETriggerTileOwnerType::Belt;

  bool operator==(const FTriggerOwner &Other) const {
    return Q == Other.Q && R == Other.R && Type == Other.Type;
  }
  bool operator!=(const FTriggerOwner &Other) const {
    return !(*this == Other);
  }
};

FORCEINLINE uint32 GetTypeHash(const FTriggerOwner &O) {
  return HashCombine(HashCombine(::GetTypeHash(O.Q), ::GetTypeHash(O.R)),
                     ::GetTypeHash((uint8)O.Type));
}
