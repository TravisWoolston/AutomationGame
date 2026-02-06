#pragma once
#include "CoreMinimal.h"
class USimWorldHex;

/** Helper struct for save/load operations - grants friend access */
struct FSimWorldHexSaveLoad {
  static void SaveTileMapToFile(USimWorldHex *Sim, const FString &Filename);
  static void LoadTileMapFromFile(USimWorldHex *Sim, const FString &Filename);
};

// Legacy wrappers for existing code
inline void SaveTileMapToFile(USimWorldHex *Sim, const FString &Filename) {
  FSimWorldHexSaveLoad::SaveTileMapToFile(Sim, Filename);
}
inline void LoadTileMapFromFile(USimWorldHex *Sim, const FString &Filename) {
  FSimWorldHexSaveLoad::LoadTileMapFromFile(Sim, Filename);
}
