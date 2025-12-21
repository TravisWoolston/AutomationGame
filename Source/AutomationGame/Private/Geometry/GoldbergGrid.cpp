#include "Geometry/GoldbergGrid.h"

namespace
{
	// Helper to create a canonical edge key (smaller index first)
	TPair<int32, int32> MakeEdgeKey(int32 A, int32 B)
	{
		return A < B ? TPair<int32, int32>(A, B) : TPair<int32, int32>(B, A);
	}
	
	uint64 EdgeKeyHash(int32 A, int32 B)
	{
		if (A > B) Swap(A, B);
		return (uint64(A) << 32) | uint64(B);
	}
}

void UGoldbergGrid::Build(int32 Frequency, float Radius, const FVector& Origin)
{
	Cells.Reset();
	CachedFrequency = Frequency;
	CachedRadius = Radius;
	CachedOrigin = Origin;
	Frequency = FMath::Max(1, Frequency);
	
	// Base icosahedron vertices
	const float Phi = (1.f + FMath::Sqrt(5.f)) * 0.5f;
	TArray<FVector> IcoVerts = {
		FVector(-1, Phi, 0), FVector(1, Phi, 0), FVector(-1, -Phi, 0), FVector(1, -Phi, 0),
		FVector(0, -1, Phi), FVector(0, 1, Phi), FVector(0, -1, -Phi), FVector(0, 1, -Phi),
		FVector(Phi, 0, -1), FVector(Phi, 0, 1), FVector(-Phi, 0, -1), FVector(-Phi, 0, 1)
	};
	for (FVector& V : IcoVerts) V.Normalize();
	
	// Base icosahedron faces (20 triangles)
	struct FTri { int32 A, B, C; };
	TArray<FTri> IcoFaces = {
		{0,11,5}, {0,5,1}, {0,1,7}, {0,7,10}, {0,10,11},
		{1,5,9}, {5,11,4}, {11,10,2}, {10,7,6}, {7,1,8},
		{3,9,4}, {3,4,2}, {3,2,6}, {3,6,8}, {3,8,9},
		{4,9,5}, {2,4,11}, {6,2,10}, {8,6,7}, {9,8,1}
	};
	
	// Step 1: Subdivide each icosahedron face into smaller triangles
	// and collect all subdivision vertices
	TArray<FVector> AllVerts;
	AllVerts.Reserve(12 + 20 * Frequency * Frequency);
	TMap<uint64, int32> VertCache; // Hash -> vertex index for deduplication
	
	auto GetOrAddVert = [&](const FVector& V) -> int32
	{
		FVector Normalized = V.GetSafeNormal();
		// Use a spatial hash for deduplication
		int32 HashX = FMath::RoundToInt(Normalized.X * 10000);
		int32 HashY = FMath::RoundToInt(Normalized.Y * 10000);
		int32 HashZ = FMath::RoundToInt(Normalized.Z * 10000);
		uint64 Hash = (uint64(HashX + 50000) << 40) | (uint64(HashY + 50000) << 20) | uint64(HashZ + 50000);
		
		if (int32* Found = VertCache.Find(Hash))
		{
			return *Found;
		}
		int32 Idx = AllVerts.Add(Normalized);
		VertCache.Add(Hash, Idx);
		return Idx;
	};
	
	// Add base icosahedron vertices
	for (const FVector& V : IcoVerts)
	{
		GetOrAddVert(V);
	}
	
	// Subdivide each face and collect triangles
	struct FSubTri { int32 V0, V1, V2; };
	TArray<FSubTri> AllTris;
	AllTris.Reserve(20 * Frequency * Frequency);
	
	for (const FTri& Face : IcoFaces)
	{
		FVector A = IcoVerts[Face.A];
		FVector B = IcoVerts[Face.B];
		FVector C = IcoVerts[Face.C];
		
		// Create a grid of vertices on this face
		// For frequency f, we have (f+1)(f+2)/2 vertices per face
		TArray<TArray<int32>> Grid;
		Grid.SetNum(Frequency + 1);
		
		for (int32 i = 0; i <= Frequency; ++i)
		{
			Grid[i].SetNum(Frequency + 1 - i);
			for (int32 j = 0; j <= Frequency - i; ++j)
			{
				// Barycentric interpolation
				float u = float(i) / Frequency;
				float v = float(j) / Frequency;
				float w = 1.f - u - v;
				FVector P = A * w + B * v + C * u;
				P.Normalize();
				Grid[i][j] = GetOrAddVert(P);
			}
		}
		
		// Create triangles from the grid
		for (int32 i = 0; i < Frequency; ++i)
		{
			for (int32 j = 0; j < Frequency - i; ++j)
			{
				// Upward-pointing triangle
				AllTris.Add({ Grid[i][j], Grid[i][j + 1], Grid[i + 1][j] });
				
				// Downward-pointing triangle (if not on edge)
				if (j < Frequency - i - 1)
				{
					AllTris.Add({ Grid[i][j + 1], Grid[i + 1][j + 1], Grid[i + 1][j] });
				}
			}
		}
	}
	
	// Step 2: Build the DUAL - each vertex becomes a cell, cells sharing an edge are neighbors
	// In the dual of a triangulated sphere:
	// - Each original VERTEX becomes a CELL (centered at that vertex)
	// - Cells are neighbors if they share a triangle (original vertices connected by edge)
	
	// Count triangles per vertex to determine if pentagon (5) or hexagon (6)
	TArray<TSet<int32>> VertexNeighbors;
	VertexNeighbors.SetNum(AllVerts.Num());
	
	// For each triangle, its three vertices are all neighbors of each other in the dual
	for (const FSubTri& Tri : AllTris)
	{
		VertexNeighbors[Tri.V0].Add(Tri.V1);
		VertexNeighbors[Tri.V0].Add(Tri.V2);
		VertexNeighbors[Tri.V1].Add(Tri.V0);
		VertexNeighbors[Tri.V1].Add(Tri.V2);
		VertexNeighbors[Tri.V2].Add(Tri.V0);
		VertexNeighbors[Tri.V2].Add(Tri.V1);
	}
	
	// Create cells from vertices
	Cells.Reserve(AllVerts.Num());
	CachedValidCellCount = 0;
	
	for (int32 i = 0; i < AllVerts.Num(); ++i)
	{
		FGoldbergCell Cell;
		Cell.Position = Origin + AllVerts[i] * Radius;
		Cell.Neighbors = VertexNeighbors[i].Array();
		Cell.Neighbors.Sort();
		// Original icosahedron vertices have 5 neighbors, all others have 6
		Cell.bPentagon = (Cell.Neighbors.Num() == 5);
		Cells.Add(Cell);
		
		if (!Cell.bPentagon)
		{
			CachedValidCellCount++;
		}
	}
	
	UE_LOG(LogTemp, Log, TEXT("GoldbergGrid: Built %d cells (%d hexagons, %d pentagons) at frequency %d"),
		Cells.Num(), CachedValidCellCount, Cells.Num() - CachedValidCellCount, Frequency);
	
	// Build the hex mapping after cells are created
	BuildHexMapping();
}

void UGoldbergGrid::BuildHexMapping()
{
	HexToCellMap.Empty();
	CellToHexMap.Empty();
	
	if (Cells.Num() == 0)
	{
		return;
	}
	
	// Use BFS from a starting cell to assign hex coordinates based on actual neighbor relationships.
	// This ensures adjacent cells get adjacent hex coordinates for tight packing.
	
	// Find a good starting cell (non-pentagon, near "top" of sphere for consistency)
	int32 StartCell = INDEX_NONE;
	float BestZ = -FLT_MAX;
	for (int32 i = 0; i < Cells.Num(); ++i)
	{
		if (Cells[i].bPentagon) continue;
		FVector Dir = (Cells[i].Position - CachedOrigin).GetSafeNormal();
		if (Dir.Z > BestZ)
		{
			BestZ = Dir.Z;
			StartCell = i;
		}
	}
	
	if (StartCell == INDEX_NONE)
	{
		UE_LOG(LogTemp, Warning, TEXT("GoldbergGrid: No valid starting cell found for hex mapping"));
		return;
	}
	
	// Hex direction offsets (axial coordinates)
	// Dir 0 = E (+1, 0), Dir 1 = NE (+1, -1), Dir 2 = NW (0, -1)
	// Dir 3 = W (-1, 0), Dir 4 = SW (-1, +1), Dir 5 = SE (0, +1)
	static const int32 HexDirQ[6] = { +1, +1,  0, -1, -1,  0 };
	static const int32 HexDirR[6] = {  0, -1, -1,  0, +1, +1 };
	
	// BFS queue: (CellIndex, HexQ, HexR)
	TArray<TTuple<int32, int32, int32>> Queue;
	TSet<int32> Visited;
	
	// Start at (0,0)
	Queue.Add(MakeTuple(StartCell, 0, 0));
	Visited.Add(StartCell);
	HexToCellMap.Add(FHex(0, 0), StartCell);
	CellToHexMap.Add(StartCell, FHex(0, 0));
	
	int32 QueueIndex = 0;
	while (QueueIndex < Queue.Num())
	{
		int32 CellIdx = Queue[QueueIndex].Get<0>();
		int32 HexQ = Queue[QueueIndex].Get<1>();
		int32 HexR = Queue[QueueIndex].Get<2>();
		QueueIndex++;
		
		const FGoldbergCell& Cell = Cells[CellIdx];
		FVector CellDir = (Cell.Position - CachedOrigin).GetSafeNormal();
		
		// Build a local tangent frame for this cell to determine neighbor directions
		FVector Up = CellDir;
		FVector East = FVector::CrossProduct(FVector::UpVector, Up);
		if (East.IsNearlyZero()) East = FVector::RightVector;
		East.Normalize();
		FVector North = FVector::CrossProduct(Up, East).GetSafeNormal();
		
		// For each neighbor, compute its angular direction in the local frame
		// and map to the closest hex direction
		for (int32 NeighborIdx : Cell.Neighbors)
		{
			if (Visited.Contains(NeighborIdx)) continue;
			if (Cells[NeighborIdx].bPentagon) continue; // Skip pentagons
			
			FVector NeighborDir = (Cells[NeighborIdx].Position - CachedOrigin).GetSafeNormal();
			FVector ToNeighbor = (NeighborDir - CellDir).GetSafeNormal();
			
			// Project onto tangent plane
			float LocalX = FVector::DotProduct(ToNeighbor, East);
			float LocalY = FVector::DotProduct(ToNeighbor, North);
			
			// Convert to angle and find closest hex direction
			float Angle = FMath::Atan2(LocalY, LocalX); // -PI to PI
			// Hex directions are at 0, 60, 120, 180, -120, -60 degrees
			// That's 0, PI/3, 2PI/3, PI, -2PI/3, -PI/3 radians
			int32 BestDir = 0;
			float BestDiff = FLT_MAX;
			for (int32 d = 0; d < 6; ++d)
			{
				float DirAngle = d * (PI / 3.f); // 0, 60, 120, 180, 240, 300 degrees
				if (DirAngle > PI) DirAngle -= 2.f * PI; // Normalize to -PI to PI
				float Diff = FMath::Abs(FMath::UnwindRadians(Angle - DirAngle));
				if (Diff < BestDiff)
				{
					BestDiff = Diff;
					BestDir = d;
				}
			}
			
			// Compute neighbor hex coordinates
			int32 NeighborQ = HexQ + HexDirQ[BestDir];
			int32 NeighborR = HexR + HexDirR[BestDir];
			FHex NeighborHex(NeighborQ, NeighborR);
			
			// Handle collision - if this hex is already taken, find a free adjacent hex
			if (HexToCellMap.Contains(NeighborHex))
			{
				// Try other directions from the current cell
				bool bFound = false;
				for (int32 tryDir = 0; tryDir < 6 && !bFound; ++tryDir)
				{
					FHex TryHex(HexQ + HexDirQ[tryDir], HexR + HexDirR[tryDir]);
					if (!HexToCellMap.Contains(TryHex))
					{
						NeighborHex = TryHex;
						bFound = true;
					}
				}
				// If still no luck, expand outward
				if (!bFound)
				{
					int32 Ring = 2;
					while (!bFound && Ring < 100)
					{
						for (int32 d = 0; d < 6 && !bFound; ++d)
						{
							FHex TryHex(HexQ + HexDirQ[d] * Ring, HexR + HexDirR[d] * Ring);
							if (!HexToCellMap.Contains(TryHex))
							{
								NeighborHex = TryHex;
								bFound = true;
							}
						}
						Ring++;
					}
				}
			}
			
			Visited.Add(NeighborIdx);
			HexToCellMap.Add(NeighborHex, NeighborIdx);
			CellToHexMap.Add(NeighborIdx, NeighborHex);
			Queue.Add(MakeTuple(NeighborIdx, NeighborHex.Q, NeighborHex.R));
		}
	}
	
	UE_LOG(LogTemp, Log, TEXT("GoldbergGrid: Built hex mapping for %d cells (frequency %d)"), 
		HexToCellMap.Num(), CachedFrequency);
}

int32 UGoldbergGrid::GetCellIndexForHex(const FHex& Hex) const
{
	const int32* Found = HexToCellMap.Find(Hex);
	return Found ? *Found : INDEX_NONE;
}

FHex UGoldbergGrid::GetHexForCellIndex(int32 CellIndex) const
{
	const FHex* Found = CellToHexMap.Find(CellIndex);
	return Found ? *Found : FHex(0, 0);
}

bool UGoldbergGrid::IsValidHex(const FHex& Hex) const
{
	return HexToCellMap.Contains(Hex);
}

FRotator UGoldbergGrid::GetCellRotation(int32 CellIndex) const
{
	if (CellIndex < 0 || CellIndex >= Cells.Num())
	{
		return FRotator::ZeroRotator;
	}
	
	const FGoldbergCell& Cell = Cells[CellIndex];
	
	// Calculate rotation where surface normal is "up"
	FVector Up = (Cell.Position - CachedOrigin).GetSafeNormal();
	if (Up.IsNearlyZero())
	{
		Up = FVector::UpVector;
	}
	
	// Create orthonormal basis
	FVector East = FVector::CrossProduct(FVector::UpVector, Up);
	if (East.IsNearlyZero())
	{
		East = FVector::CrossProduct(FVector::RightVector, Up);
	}
	East.Normalize();
	FVector North = FVector::CrossProduct(Up, East).GetSafeNormal();
	
	// Build rotation matrix (Forward = East, Right = North, Up = Up)
	FMatrix RotMatrix(East, North, Up, FVector::ZeroVector);
	return RotMatrix.Rotator();
}

int32 UGoldbergGrid::GetValidCellCount() const
{
	return CachedValidCellCount;
}
