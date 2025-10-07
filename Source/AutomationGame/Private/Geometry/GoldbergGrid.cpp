#include "Geometry/GoldbergGrid.h"

namespace
{
	struct FEdgeKey { int32 A,B; bool operator==(const FEdgeKey& O) const { return (A==O.A && B==O.B); } };
	struct FEdgeKeyHash { size_t operator()(const FEdgeKey& K) const { return (size_t(K.A)<<32) ^ size_t(K.B); } };
}

void UGoldbergGrid::Build(int32 Frequency, float Radius, const FVector& Origin)
{
	Cells.Reset(); CachedFrequency = Frequency; CachedRadius = Radius; CachedOrigin = Origin;
	Frequency = FMath::Max(1, Frequency);
	// Base icosahedron vertices
	const float Phi = (1.f + FMath::Sqrt(5.f))*0.5f;
	TArray<FVector> BaseVerts = {
		FVector(-1, Phi, 0), FVector(1, Phi,0), FVector(-1,-Phi,0), FVector(1,-Phi,0),
		FVector(0,-1, Phi), FVector(0,1, Phi), FVector(0,-1,-Phi), FVector(0,1,-Phi),
		FVector(Phi,0,-1), FVector(Phi,0,1), FVector(-Phi,0,-1), FVector(-Phi,0,1)
	};
	for (FVector& V: BaseVerts) V.Normalize();
	struct FTri{int32 A,B,C;};
	TArray<FTri> Tris = {
		{0,11,5},{0,5,1},{0,1,7},{0,7,10},{0,10,11},
		{1,5,9},{5,11,4},{11,10,2},{10,7,6},{7,1,8},
		{3,9,4},{3,4,2},{3,2,6},{3,6,8},{3,8,9},
		{4,9,5},{2,4,11},{6,2,10},{8,6,7},{9,8,1}
	};
	// Subdivide each triangle using frequency f, accumulate unique points.
	TArray<FVector> Points; Points.Reserve( (5*Frequency*Frequency) * 20 );
	TMap<uint64,int32> Cache;
	auto KeyFor = [&](int32 a,int32 b,int32 c)->uint64{ return (uint64(a)<<42) ^ (uint64(b)<<21) ^ uint64(c); };
	for (const FTri& T: Tris)
	{
		FVector A=BaseVerts[T.A]; FVector B=BaseVerts[T.B]; FVector C=BaseVerts[T.C];
		for(int i=0;i<=Frequency;++i){ float u=float(i)/Frequency; FVector Ai = FMath::Lerp(A,C,u);
			for(int j=0;j<=Frequency-i;++j){ float v=float(j)/Frequency; FVector P = FMath::Lerp(Ai, FMath::Lerp(B,C,u), v); P.Normalize();
				int32 Idx = Points.Add(P); (void)Idx; }
		}
	}
	// Very naive duplicate unify (O(n^2) acceptable for small frequencies now)
	TArray<FVector> Unique; Unique.Reserve(Points.Num());
	TArray<int32> Remap; Remap.SetNum(Points.Num());
	for(int i=0;i<Points.Num();++i){ bool Found=false; for(int j=0;j<Unique.Num();++j){ if(Points[i].Equals(Unique[j],1e-4f)){ Remap[i]=j; Found=true; break; }} if(!Found){ Remap[i]=Unique.Add(Points[i]); }}
	// Build adjacency by angular proximity (rough) – future: build dual properly.
	const float NeighborAngle = FMath::DegreesToRadians(75.f); // ~ hex
	TArray<TArray<int32>> Adj; Adj.SetNum(Unique.Num());
	for(int i=0;i<Unique.Num();++i){ for(int j=i+1;j<Unique.Num();++j){ float Ang = FMath::Acos( FMath::Clamp(FVector::DotProduct(Unique[i],Unique[j]), -1.f,1.f)); if(Ang < NeighborAngle){ Adj[i].Add(j); Adj[j].Add(i);} } }
	// Create cells from unique points
	Cells.Reserve(Unique.Num());
	for(int i=0;i<Unique.Num();++i){ FGoldbergCell Cell; Cell.Position = Origin + Unique[i]*Radius; Adj[i].Sort(); Cell.Neighbors = Adj[i]; Cell.bPentagon = (Adj[i].Num()==5); Cells.Add(Cell);} 
}
