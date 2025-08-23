#include "Sim/SimWorldHex_SaveLoad.h"
#include "Sim/SimWorldHex.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Misc/FileHelper.h"

void SaveTileMapToFile(USimWorldHex* Sim, const FString& Filename)
{
	if (!Sim) return;
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Arr;
	for (const auto& KV : Sim->TileMap)
	{
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetNumberField(TEXT("Q"), KV.Key.Q);
		O->SetNumberField(TEXT("R"), KV.Key.R);
		O->SetNumberField(TEXT("Kind"), int32(KV.Value.Kind));
		O->SetNumberField(TEXT("Arg"), int32(KV.Value.Arg));
		Arr.Add(MakeShared<FJsonValueObject>(O));
	}
	Root->SetArrayField(TEXT("Tiles"), Arr);
	FString S;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&S);
	if (FJsonSerializer::Serialize(Root.ToSharedRef(), W))
	{
		FFileHelper::SaveStringToFile(S, *Filename);
	}
}

void LoadTileMapFromFile(USimWorldHex* Sim, const FString& Filename)
{
	if (!Sim) return;
	FString S;
	if (!FFileHelper::LoadFileToString(S, *Filename)) return;
	TSharedPtr<FJsonObject> Root;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(S);
	if (!FJsonSerializer::Deserialize(R, Root) || !Root.IsValid()) return;

	Sim->TileMap.Empty();
	const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
	if (Root->TryGetArrayField(TEXT("Tiles"), Arr) && Arr)
	{
		for (const TSharedPtr<FJsonValue>& V : *Arr)
		{
			const TSharedPtr<FJsonObject> O = V->AsObject();
			if (!O) continue;
			int32 Q = 0, Rr = 0, Kind = 0, Arg = 0;
			if (O->TryGetNumberField(TEXT("Q"), Q) &&
				O->TryGetNumberField(TEXT("R"), Rr) &&
				O->TryGetNumberField(TEXT("Kind"), Kind) &&
				O->TryGetNumberField(TEXT("Arg"), Arg))
			{
				Sim->TileMap.Add(FHex(Q, Rr), FTile{ ETileKind(Kind), uint8(Arg) });
			}
		}
		++Sim->TileRevision; // trigger visual rebuilds
	}
}
