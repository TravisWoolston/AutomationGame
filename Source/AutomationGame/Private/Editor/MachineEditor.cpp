#include "Editor/MachineEditor.h"
#include "Framework/GameTickManager.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "DrawDebugHelpers.h"
#include "Kismet/GameplayStatics.h"
#include "Editor/MachineBlueprintSave.h"

namespace { static inline float DirToYaw(uint8 Dir) { return 60.f * float(Dir % 6); } }

AMachineEditor::AMachineEditor()
{
	PrimaryActorTick.bCanEverTick = true;

	USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);

	GridAnchor = CreateDefaultSubobject<USceneComponent>(TEXT("GridAnchor"));
	GridAnchor->SetupAttachment(Root);

	TileISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("TileISM"));
	TileISM->SetupAttachment(Root);
	TileISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	TileISM->SetCastShadow(false);

	BeltISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("BeltISM"));
	BeltISM->SetupAttachment(Root);
	BeltISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	BeltISM->SetCastShadow(false);
}

void AMachineEditor::EnsureComponents()
{
	if (!TileISM)
	{
		TileISM = NewObject<UInstancedStaticMeshComponent>(this, TEXT("TileISM_Runtime"));
		TileISM->SetupAttachment(GetRootComponent());
		TileISM->RegisterComponent();
		TileISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		TileISM->SetCastShadow(false);
	}
	if (!BeltISM)
	{
		BeltISM = NewObject<UInstancedStaticMeshComponent>(this, TEXT("BeltISM_Runtime"));
		BeltISM->SetupAttachment(GetRootComponent());
		BeltISM->RegisterComponent();
		BeltISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		BeltISM->SetCastShadow(false);
	}
}

void AMachineEditor::BeginPlay()
{
	Super::BeginPlay();
	EnsureComponents();

	// Try to hook into a shared tick manager (preferred)
	TickState.Manager = AGameTickManager::Get(this);
	if (TickState.Manager)
	{
		// Bind to the float-based delegate
		TickState.Manager->OnFixedTick.AddDynamic(this, &AMachineEditor::HandleFixedTickSeconds);
	}

	if (bGenerateOnBeginPlay && Terrain.Num() == 0 && InitialRingRadius >= 0)
	{
		GenerateFilledRadius(InitialRingRadius);
	}

	NewBlueprint(FText::FromString(TEXT("New Machine")));
	RebuildInternal();
}

void AMachineEditor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (TickState.Manager)
	{
		TickState.Manager->OnFixedTick.RemoveDynamic(this, &AMachineEditor::HandleFixedTickSeconds);
	}
	Super::EndPlay(EndPlayReason);
}

void AMachineEditor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	// If no manager in the level, fall back to local accumulator play
	if (!TickState.Manager && TickState.bLocalPlaying)
	{
		TickState.LocalAccum += double(DeltaSeconds) * double(LocalTicksPerSecond);
		const int32 Steps = FMath::FloorToInt(TickState.LocalAccum);
		if (Steps > 0)
		{
			TickState.LocalAccum -= Steps;
			HandleFixedTick(Steps);
		}
	}

	const bool bNeedsDraw = (BoardRevision != LastDrawnBoardRevision) ||
		(BlueprintVersionCounter != LastDrawnBlueprintVersion);
	if (bNeedsDraw) RebuildInternal();
}

void AMachineEditor::HandleFixedTickSeconds(float StepSeconds)
{
	// If each broadcast = one step, forward 1.
	// Adjust if you later vary step time.
	HandleFixedTick(1);
}

void AMachineEditor::HandleFixedTick(int32 Steps)
{
	// In future, drive an internal test sim here. For now, just bump counter.
	BlueprintVersionCounter += FMath::Max(1, Steps);
}

FVector AMachineEditor::HexToWorld(const FHex& H) const
{
	const FVector Anchor = GridAnchor ? GridAnchor->GetComponentLocation() : FVector::ZeroVector;
	const FVector2D XY = HexCoord::AxialToXY(FHexAxial(H.Q, H.R), TileSizeUU, GridOrientation);
	const FVector2D RotXY = HexCoord::Rotate2D(XY, GridYawDeg);
	return Anchor + FVector(RotXY.X, RotXY.Y, 0.f);
}

void AMachineEditor::AddHex(FHex H)
{
	if (!Terrain.Contains(H))
	{
		Terrain.Add(H);
		++BoardRevision;
	}
}

void AMachineEditor::RemoveHex(FHex H)
{
	if (Terrain.Remove(H) > 0)
	{
		++BoardRevision;
	}
}

void AMachineEditor::GenerateFilledRadius(int32 Radius)
{
	for (int32 q = -Radius; q <= Radius; ++q)
	{
		for (int32 r = -Radius; r <= Radius; r++)
		{
			const int32 s = -q - r;
			if (FMath::Max3(FMath::Abs(q), FMath::Abs(r), FMath::Abs(s)) <= Radius)
			{
				Terrain.Add(FHex(q, r));
			}
		}
	}
	++BoardRevision;
}

void AMachineEditor::Rebuild()
{
	RebuildInternal();
}

void AMachineEditor::RebuildInternal()
{
	EnsureComponents();

	LastDrawnBoardRevision = BoardRevision;
	LastDrawnBlueprintVersion = BlueprintVersionCounter;

	if (TileMesh) TileISM->SetStaticMesh(TileMesh);
	if (TileMaterial) TileISM->SetMaterial(0, TileMaterial);
	if (BeltMesh) BeltISM->SetStaticMesh(BeltMesh);
	if (BeltMaterial) BeltISM->SetMaterial(0, BeltMaterial);

	TileISM->ClearInstances();
	BeltISM->ClearInstances();

	RebuildTiles();
	RebuildBelts();
}

void AMachineEditor::RebuildTiles()
{
	for (const FHex& H : Terrain)
	{
		const FVector Loc = HexToWorld(H);
		if (TileMesh)
		{
			TileISM->AddInstance(FTransform(FRotator::ZeroRotator, Loc, FVector(1)));
		}
		else
		{
			DrawDebugPoint(GetWorld(), Loc + FVector(0, 0, 2), 6.f, FColor::Cyan, false, 1.5f);
		}
	}
}

void AMachineEditor::DrawBeltArrow(const FVector& Center, float TileRadiusUU, float YawDeg)
{
	if (!bAlwaysDrawBeltArrows) return;
	const float Len = TileRadiusUU * DebugArrowLenScale;
	const FVector Dir = FRotator(0.f, YawDeg, 0.f).RotateVector(FVector(1, 0, 0));
	const FVector A = Center + FVector(0, 0, BeltZOffset + 2.f);
	const FVector B = A + Dir * Len;
	DrawDebugDirectionalArrow(GetWorld(), A, B, 18.f, FColor::Yellow, false, 1.0f, 0, DebugArrowThickness);
}

void AMachineEditor::RebuildBelts()
{
	if (!ActiveBlueprint) return;

	for (const FMachinePart& P : ActiveBlueprint->Parts)
	{
		if (!Terrain.Contains(P.Hex)) continue;

		const FVector Loc = HexToWorld(P.Hex) + FVector(0, 0, BeltZOffset);

		if (P.Kind == EMachinePartKind::Belt)
		{
			const float Yaw = DirToYaw(P.Dir) + GridYawDeg;
			if (BeltMesh)
			{
				BeltISM->AddInstance(FTransform(FRotator(0.f, Yaw, 0.f), Loc, BeltScale));
				DrawBeltArrow(Loc, TileSizeUU, Yaw);
			}
			else
			{
				DrawBeltArrow(Loc, TileSizeUU, Yaw);
			}
		}
		else
		{
			DrawBeltArrow(Loc, TileSizeUU, 0.f);
		}
	}
}

void AMachineEditor::MarkBlueprintDirty()
{
	++BlueprintVersionCounter;
}

void AMachineEditor::NewBlueprint(const FText& Name)
{
	ActiveBlueprint = NewObject<UMachineBlueprint>(this, UMachineBlueprint::StaticClass());
	ActiveBlueprint->DisplayName = Name;
	MarkBlueprintDirty();
	RebuildInternal();
}

void AMachineEditor::ClearBlueprint()
{
	if (ActiveBlueprint)
	{
		ActiveBlueprint->Clear();
		MarkBlueprintDirty();
	}
}

void AMachineEditor::PlacePart(FHex Hex, EMachinePartKind Kind, uint8 Dir, int32 ParamI, float ParamF)
{
	if (!HasHex(Hex)) return;
	if (!ActiveBlueprint) NewBlueprint(FText::FromString(TEXT("New Machine")));
	FMachinePart P;
	P.Hex = Hex; P.Kind = Kind; P.Dir = Dir; P.ParamI = ParamI; P.ParamF = ParamF;
	ActiveBlueprint->AddOrReplacePart(P);
	MarkBlueprintDirty();
}

bool AMachineEditor::RemovePartAt(FHex Hex)
{
	if (!ActiveBlueprint) return false;
	const bool b = ActiveBlueprint->RemovePartAt(Hex);
	if (b) MarkBlueprintDirty();
	return b;
}

void AMachineEditor::GetAllParts(TArray<FMachinePart>& OutParts) const
{
	OutParts.Reset();
	if (ActiveBlueprint) OutParts.Append(ActiveBlueprint->Parts);
}

void AMachineEditor::RotatePartAt(FHex Hex, bool bCW)
{
	if (!ActiveBlueprint) return;
	const int32 idx = ActiveBlueprint->FindPartAtHex(Hex);
	if (idx == INDEX_NONE) return;
	uint8& D = ActiveBlueprint->Parts[idx].Dir;
	D = uint8((int32(D) + (bCW ? 5 : 1)) % 6);
	MarkBlueprintDirty();
}

void AMachineEditor::Play()
{
	if (TickState.Manager) { TickState.Manager->Play(); return; }
	TickState.bLocalPlaying = true;
}

void AMachineEditor::Pause()
{
	if (TickState.Manager) { TickState.Manager->Pause(); return; }
	TickState.bLocalPlaying = false;
}

void AMachineEditor::Stop()
{
	if (TickState.Manager) { TickState.Manager->Stop(); return; }
	TickState.bLocalPlaying = false;
	TickState.LocalAccum = 0.0;
}

void AMachineEditor::StepTicks(int32 NumTicks)
{
	if (TickState.Manager) { TickState.Manager->StepTicks(NumTicks); return; }
	HandleFixedTick(FMath::Max(1, NumTicks));
}
