#pragma once
#include "CoreMinimal.h"
class USimWorldHex;

void SaveTileMapToFile(USimWorldHex* Sim, const FString& Filename);
void LoadTileMapFromFile(USimWorldHex* Sim, const FString& Filename);
