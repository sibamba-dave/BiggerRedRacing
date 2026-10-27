#include "BRRTrees.h"

#include "BRRRefine.h"
#include "Math/RandomStream.h"
#include "BRRShapes.h"

BRR::ETreeKind BRR::TreeKindOf(const FString& Shape)
{
	const FString S = Shape.ToUpper();
	if (S == TEXT("SNOWPINE")) return ETreeKind::SnowPine;
	if (S.StartsWith(TEXT("PINE"))) return ETreeKind::Pine;
	if (S.StartsWith(TEXT("PALM"))) return ETreeKind::Palm;
	if (S.StartsWith(TEXT("CHTREE")) || S == TEXT("NEWTREE") || S == TEXT("INDTREE")) return ETreeKind::Broadleaf;
	if (S == TEXT("JAPTREE") || S == TEXT("JAPTRE2")) return ETreeKind::Bonsai;
	if (S.StartsWith(TEXT("JAPBUSH")) || S == TEXT("SHRUB") || S == TEXT("HEDGE2")) return ETreeKind::Bush;
	if (S == TEXT("W_PLANT") || S == TEXT("GRASS") || S == TEXT("PLANT") || S == TEXT("PLANT2")) return ETreeKind::Reeds;
	if (S.StartsWith(TEXT("CACTUS"))) return ETreeKind::Cactus;
	return ETreeKind::None;
}

TArray<BRR::FBRRTree> BRR::FindTrees(const FBRRWorld& World, TBitArray<>& OutPlant)
{
	TArray<FBRRTree> Trees;
	const FBRRMesh& Decor = World.Decor;
	const int32 Count = Decor.NumTriangles();
	OutPlant.Init(false, Count);
	if (Count == 0) return Trees;

	// The pieces: triangles joined through shared corners (union-find over welded points).
	struct FTri { const FBRRVertex* V; int32 Section; };
	TArray<FTri> Tris;
	Tris.Reserve(Count);
	for (int32 S = 0; S < Decor.Sections.Num(); ++S)
		for (int32 T = 0; T + 2 < Decor.Sections[S].Vertices.Num(); T += 3) Tris.Add({ &Decor.Sections[S].Vertices[T], S });
	TArray<int32> Parent;
	Parent.SetNumUninitialized(Tris.Num());
	for (int32 I = 0; I < Tris.Num(); ++I) Parent[I] = I;
	auto Find = [&](int32 X) { while (Parent[X] != X) { Parent[X] = Parent[Parent[X]]; X = Parent[X]; } return X; };
	TMap<FIntVector, int32> FirstAt;
	for (int32 I = 0; I < Tris.Num(); ++I)
		for (int32 K = 0; K < 3; ++K)
		{
			const FVector3f& P = Tris[I].V[K].Position;
			const FIntVector Key(FMath::RoundToInt32(P.X), FMath::RoundToInt32(P.Y), FMath::RoundToInt32(P.Z));
			if (const int32* Other = FirstAt.Find(Key)) Parent[Find(I)] = Find(*Other);
			else FirstAt.Add(Key, I);
		}
	TMap<int32, TArray<int32>> Pieces;
	for (int32 I = 0; I < Tris.Num(); ++I) Pieces.FindOrAdd(Find(I)).Add(I);

	// Each piece to the nearest placement (by its footprint).
	TMap<int32, TArray<int32>> ByPlacement; // placement -> triangles
	for (const TPair<int32, TArray<int32>>& Piece : Pieces)
	{
		FBox3f Box(ForceInit);
		for (const int32 I : Piece.Value) for (int32 K = 0; K < 3; ++K) Box += Tris[I].V[K].Position;
		int32 Best = INDEX_NONE;
		double BestD = 1500.0;
		for (int32 P = 0; P < World.Placements.Num(); ++P)
		{
			const FVector& L = World.Placements[P].Location;
			const double DX = FMath::Max3(double(Box.Min.X) - L.X, 0.0, L.X - double(Box.Max.X));
			const double DY = FMath::Max3(double(Box.Min.Y) - L.Y, 0.0, L.Y - double(Box.Max.Y));
			const double D = FMath::Sqrt(DX * DX + DY * DY) + 0.01 * FVector2D::Distance(FVector2D(L), FVector2D(FVector(Box.GetCenter())));
			if (D < BestD) BestD = D, Best = P;
		}
		if (Best != INDEX_NONE && TreeKindOf(World.Placements[Best].Shape) != ETreeKind::None) ByPlacement.FindOrAdd(Best).Append(Piece.Value);
	}

	auto Colour = [&](const FTri& T)
	{
		const FBRRMeshSection& S = Decor.Sections[T.Section];
		return World.Atlases.IsValidIndex(S.Atlas) ? FBRRWorld::AverageTexels(World.Atlases[S.Atlas], T.V[0].UV, T.V[1].UV, T.V[2].UV) : T.V[0].Color;
	};
	for (const TPair<int32, TArray<int32>>& Plant : ByPlacement)
	{
		const FBRRPlacement& P = World.Placements[Plant.Key];
		FBox3f Box(ForceInit);
		for (const int32 I : Plant.Value) for (int32 K = 0; K < 3; ++K) Box += Tris[I].V[K].Position;
		FBRRTree& Tree = Trees.AddDefaulted_GetRef();
		Tree.Kind = TreeKindOf(P.Shape);
		Tree.Base = FVector3f(float(P.Location.X), float(P.Location.Y), Box.Min.Z);
		Tree.Height = FMath::Max(Box.Max.Z - Box.Min.Z, 50.f);
		Tree.Radius = FMath::Max(FMath::Max(Box.Max.X - Box.Min.X, Box.Max.Y - Box.Min.Y) * 0.5f, 30.f);
		Tree.Yaw = float(P.Rotation.Rotator().Yaw);
		Tree.Seed = Plant.Key * 7919 + 17;
		// Its colours: the crown's from the upper part, the trunk's from the lower.
		FLinearColor Up = FLinearColor::Transparent, Low = FLinearColor::Transparent;
		for (const int32 I : Plant.Value)
		{
			const FTri& T = Tris[I];
			OutPlant[I] = true;
			const float Z = (T.V[0].Position.Z + T.V[1].Position.Z + T.V[2].Position.Z) / 3.f;
			const float Area = ((T.V[1].Position - T.V[0].Position) ^ (T.V[2].Position - T.V[0].Position)).Size() * 0.5f;
			const FLinearColor C = FLinearColor::FromSRGBColor(Colour(T)) * Area;
			if (Z > Box.Min.Z + Tree.Height * 0.4f) Up += FLinearColor(C.R, C.G, C.B, Area);
			else if (Z < Box.Min.Z + Tree.Height * 0.3f) Low += FLinearColor(C.R, C.G, C.B, Area);
		}
		if (Up.A > 0) Tree.Crown = FLinearColor(Up.R / Up.A, Up.G / Up.A, Up.B / Up.A).ToFColor(true);
		if (Low.A > 0)
		{
			const FColor L = FLinearColor(Low.R / Low.A, Low.G / Low.A, Low.B / Low.A).ToFColor(true);
			// A trunk that came out green is the crown's skirt: bark instead.
			if (!(L.G > L.R + 10 && L.G > L.B)) Tree.Trunk = L;
		}
		if (Up.A <= 0 && Low.A > 0) Tree.Crown = FLinearColor(Low.R / Low.A, Low.G / Low.A, Low.B / Low.A).ToFColor(true);
	}
	return Trees;
}

FBRRMesh BRR::WithoutPlants(const FBRRMesh& Decor, const TBitArray<>& Plant)
{
	FBRRMesh Out;
	int32 Index = 0;
	for (const FBRRMeshSection& S : Decor.Sections)
	{
		FBRRMeshSection& To = Out.Sections.AddDefaulted_GetRef();
		To.Atlas = S.Atlas;
		To.bTwoSided = S.bTwoSided;
		for (int32 T = 0; T + 2 < S.Vertices.Num(); T += 3, ++Index)
		{
			if (!Plant.IsValidIndex(Index) || !Plant[Index]) To.Vertices.Append(&S.Vertices[T], 3);
		}
		if (To.Vertices.IsEmpty()) Out.Sections.Pop();
	}
	return Out;
}

namespace
{
	using namespace BRRShapes;
	using FGrow = BRRShapes::FBuilder;
}

FBRRMesh BRR::GrowTree(const FBRRTree& Tree)
{
	FGrow G(Tree.Seed);
	FRandomStream& Rnd = G.Rand;
	const float H = Tree.Height, Rad = Tree.Radius;
	const FColor Bark = Tree.Trunk, Leaf = Tree.Crown;
	switch (Tree.Kind)
	{
	case ETreeKind::Pine:
	case ETreeKind::SnowPine:
	{
		G.Tube({ FVector3f(0, 0, -20), FVector3f(0, 0, H * 0.5f), FVector3f(0, 0, H * 0.92f) }, { Rad * 0.09f, Rad * 0.06f, Rad * 0.02f }, 10, Bark);
		const int32 Tiers = 7;
		for (int32 T = 0; T < Tiers; ++T)
		{
			const float F = float(T) / Tiers;
			const float Y = H * (0.18f + 0.74f * F);
			const float TierR = Rad * (1.f - 0.85f * F) * Rnd.FRandRange(0.9f, 1.05f);
			const float TierH = H * 0.24f;
			const FVector3f Apex(0, 0, Y + TierH);
			const int32 N = 18;
			TArray<FVector3f> Rim;
			for (int32 S = 0; S < N; ++S)
			{
				const float A = 2.f * PI * (S + Rnd.FRandRange(-0.2f, 0.2f)) / N + T;
				const float Rr = TierR * ((S & 1) ? 0.72f : 1.f) * Rnd.FRandRange(0.88f, 1.08f);
				Rim.Add(FVector3f(FMath::Cos(A) * Rr, FMath::Sin(A) * Rr, Y - ((S & 1) ? 0.f : TierH * 0.12f)));
			}
			const FColor Top = Tree.Kind == ETreeKind::SnowPine ? Mix(Leaf, FColor(235, 240, 245), 0.75f) : Shade(Leaf, 1.1f);
			const FColor Edge = Shade(Leaf, 0.75f);
			for (int32 S = 0; S < N; ++S)
			{
				const FVector3f& A = Rim[S];
				const FVector3f& B = Rim[(S + 1) % N];
				G.Solid(A, B, Apex, Edge, Edge, Top, FVector3f(0, 0, Y));
				G.Solid(A, B, FVector3f(0, 0, Y + TierH * 0.15f), Shade(Leaf, 0.5f), Shade(Leaf, 0.5f), Shade(Leaf, 0.4f), FVector3f(0, 0, Y + TierH));
			}
		}
		break;
	}
	case ETreeKind::Palm:
	{
		const FVector3f Lean = FVector3f(Rnd.FRandRange(-1.f, 1.f), Rnd.FRandRange(-1.f, 1.f), 0).GetSafeNormal() * H * 0.18f;
		TArray<FVector3f> Spine;
		TArray<float> Radii;
		for (int32 I = 0; I <= 10; ++I)
		{
			const float T = I / 10.f;
			Spine.Add(FVector3f(0, 0, H * 0.85f * T - 20.f) + Lean * T * T);
			Radii.Add(Rad * 0.07f * (1.f - 0.35f * T) * ((I & 1) ? 1.f : 0.88f));
		}
		G.Tube(Spine, Radii, 10, Bark);
		const FVector3f Top = Spine.Last();
		const int32 Fronds = 10;
		for (int32 F = 0; F < Fronds; ++F)
		{
			const float A = 2.f * PI * (F + Rnd.FRandRange(-0.25f, 0.25f)) / Fronds;
			const FVector3f Out(FMath::Cos(A), FMath::Sin(A), 0);
			const FVector3f Side(-Out.Y, Out.X, 0);
			const float Len = Rad * Rnd.FRandRange(0.95f, 1.2f), Rise = H * 0.12f, Droop = H * Rnd.FRandRange(0.18f, 0.3f);
			const int32 Seg = 8;
			auto At = [&](int32 I) { const float T = float(I) / Seg; return Top + Out * Len * T + FVector3f(0, 0, Rise * T * 2.f - (Rise * 2.f + Droop) * T * T); };
			for (int32 I = 0; I < Seg; ++I)
			{
				const float W0 = Rad * 0.16f * FMath::Sin(PI * I / Seg + 0.15f), W1 = Rad * 0.16f * FMath::Sin(PI * (I + 1) / Seg + 0.15f);
				const FVector3f P0 = At(I), P1 = At(I + 1);
				const FColor C0 = Shade(Leaf, 0.8f + 0.2f * I / Seg), C1 = Shade(Leaf, 0.8f + 0.2f * (I + 1) / Seg);
				// The leaflets: each side a saw of thin blades hanging off the rib.
				for (const float S : { -1.f, 1.f })
				{
					const FVector3f Hang(0, 0, -W1 * 0.6f);
					G.Tri(1, P0, P1, P1 + Side * (S * W1) + Hang, C0, C1, Shade(Leaf, 0.7f));
					G.Tri(1, P0, P0 + Side * (S * W0) + Hang * 0.8f, P1 + Side * (S * W1) + Hang, C0, Shade(Leaf, 0.7f), Shade(Leaf, 0.7f));
				}
			}
		}
		break;
	}
	case ETreeKind::Broadleaf:
	{
		// Not green: a dead tree (WATER1's, the original's bare black branches) - limbs, no leaves.
		const bool bBare = !(Leaf.G > Leaf.R * 1.05f && Leaf.G > Leaf.B) || Leaf.G < 45;
		if (bBare)
		{
			const FColor Wood = Mix(Leaf, Bark, 0.3f);
			const FVector3f Fork(0, 0, H * 0.45f);
			G.Tube({ FVector3f(0, 0, -20), FVector3f(Rad * 0.05f, 0, H * 0.25f), Fork }, { Rad * 0.14f, Rad * 0.1f, Rad * 0.08f }, 9, Wood);
			const int32 Limbs = 4 + Rnd.RandHelper(2);
			for (int32 L = 0; L < Limbs; ++L)
			{
				const float A = 2.f * PI * L / Limbs + Rnd.FRandRange(-0.4f, 0.4f);
				const FVector3f Out(FMath::Cos(A), FMath::Sin(A), 0);
				const FVector3f Tip = Fork + Out * Rad * Rnd.FRandRange(0.55f, 0.9f) + FVector3f(0, 0, H * Rnd.FRandRange(0.3f, 0.55f));
				const FVector3f Bend = (Fork + Tip) * 0.5f + Out * Rad * 0.1f;
				G.Tube({ Fork, Bend, Tip }, { Rad * 0.06f, Rad * 0.04f, Rad * 0.015f }, 6, Wood);
				for (int32 T = 0; T < 2; ++T)
				{
					const FVector3f From = FMath::Lerp(Bend, Tip, 0.3f + 0.3f * T);
					const FVector3f Twig = From + FVector3f(Rnd.FRandRange(-1.f, 1.f), Rnd.FRandRange(-1.f, 1.f), 0.8f).GetSafeNormal() * Rad * 0.35f;
					G.Tube({ From, Twig }, { Rad * 0.02f, Rad * 0.008f }, 5, Wood);
				}
			}
			break;
		}
		const float TrunkTop = H * 0.5f;
		G.Tube({ FVector3f(0, 0, -20), FVector3f(0, 0, TrunkTop * 0.6f), FVector3f(0, 0, TrunkTop) }, { Rad * 0.12f, Rad * 0.09f, Rad * 0.07f }, 10, Bark);
		const int32 Blobs = 9;
		const FVector3f Centre(0, 0, H * 0.66f);
		for (int32 B = 0; B < Blobs; ++B)
		{
			const FVector3f Dir = FVector3f(Rnd.FRandRange(-1.f, 1.f), Rnd.FRandRange(-1.f, 1.f), Rnd.FRandRange(-0.4f, 0.8f)).GetSafeNormal();
			const FVector3f At = Centre + FVector3f(Dir.X * Rad * 0.5f, Dir.Y * Rad * 0.5f, Dir.Z * H * 0.18f);
			if (B < 4) G.Tube({ FVector3f(0, 0, TrunkTop * 0.85f), (FVector3f(0, 0, TrunkTop) + At) * 0.5f, At }, { Rad * 0.06f, Rad * 0.045f, Rad * 0.03f }, 6, Bark);
			const float S = Rnd.FRandRange(0.42f, 0.6f);
			G.Blob(At, FVector3f(Rad * S, Rad * S, H * 0.2f * S / 0.5f), Leaf, 0.22f, 2, Tree.Seed + B);
		}
		break;
	}
	case ETreeKind::Bonsai:
	{
		// A Japanese pine: a twisting trunk, branches out to flat pads of needles.
		TArray<FVector3f> Spine;
		TArray<float> Radii;
		for (int32 I = 0; I <= 6; ++I)
		{
			const float T = I / 6.f;
			Spine.Add(FVector3f(FMath::Sin(T * 3.f + Tree.Seed) * Rad * 0.15f, FMath::Cos(T * 2.3f + Tree.Seed) * Rad * 0.12f, H * 0.8f * T - 20.f));
			Radii.Add(Rad * 0.1f * (1.f - 0.6f * T));
		}
		G.Tube(Spine, Radii, 8, Bark);
		const int32 Pads = 6;
		for (int32 P = 0; P < Pads; ++P)
		{
			const float T = 0.4f + 0.6f * P / (Pads - 1);
			const FVector3f From = Spine[FMath::Clamp(int32(T * 6), 0, 6)];
			const float A = P * 2.4f + Tree.Seed;
			const float Reach = P == Pads - 1 ? 0.f : Rad * Rnd.FRandRange(0.45f, 0.75f);
			const FVector3f To = From + FVector3f(FMath::Cos(A) * Reach, FMath::Sin(A) * Reach, H * 0.06f);
			if (Reach > 0) G.Tube({ From, (From + To) * 0.5f + FVector3f(0, 0, H * 0.04f), To }, { Rad * 0.04f, Rad * 0.03f, Rad * 0.02f }, 6, Bark);
			const float S = Rnd.FRandRange(0.3f, 0.42f) * (P == Pads - 1 ? 1.2f : 1.f);
			G.Blob(To + FVector3f(0, 0, H * 0.03f), FVector3f(Rad * S, Rad * S, H * 0.07f), Leaf, 0.25f, 2, Tree.Seed + P);
		}
		break;
	}
	case ETreeKind::Bush:
	{
		const int32 Blobs = 5;
		for (int32 B = 0; B < Blobs; ++B)
		{
			const float A = 2.f * PI * B / Blobs + Rnd.FRand();
			const float D = B == 0 ? 0.f : Rad * 0.45f;
			const float S = B == 0 ? 0.7f : 0.5f;
			G.Blob(FVector3f(FMath::Cos(A) * D, FMath::Sin(A) * D, H * 0.42f * S), FVector3f(Rad * S, Rad * S, H * 0.5f * S), Leaf, 0.25f, 2, Tree.Seed + B);
		}
		break;
	}
	case ETreeKind::Reeds:
	{
		const int32 Blades = 26;
		for (int32 B = 0; B < Blades; ++B)
		{
			const float A = Rnd.FRandRange(0.f, 2.f * PI);
			const FVector3f Root(FMath::Cos(A) * Rad * Rnd.FRandRange(0.f, 0.5f), FMath::Sin(A) * Rad * Rnd.FRandRange(0.f, 0.5f), -10.f);
			const FVector3f Out = FVector3f(Root.X, Root.Y, 0).GetSafeNormal() + FVector3f(Rnd.FRandRange(-0.3f, 0.3f), Rnd.FRandRange(-0.3f, 0.3f), 0);
			const FVector3f Side = FVector3f(-Out.Y, Out.X, 0).GetSafeNormal();
			const float Len = H * Rnd.FRandRange(0.6f, 1.05f), W = Rad * 0.06f;
			const int32 Seg = 4;
			for (int32 I = 0; I < Seg; ++I)
			{
				const float T0 = float(I) / Seg, T1 = float(I + 1) / Seg;
				auto P = [&](float T) { return Root + FVector3f(0, 0, Len * T) + Out * Rad * 0.9f * T * T; };
				const FColor C0 = Shade(Leaf, 0.6f + 0.5f * T0), C1 = Shade(Leaf, 0.6f + 0.5f * T1);
				G.Tri(1, P(T0) - Side * W * (1 - T0), P(T0) + Side * W * (1 - T0), P(T1) + Side * W * (1 - T1), C0, C0, C1);
				G.Tri(1, P(T0) - Side * W * (1 - T0), P(T1) + Side * W * (1 - T1), P(T1) - Side * W * (1 - T1), C0, C1, C1);
			}
		}
		break;
	}
	case ETreeKind::Cactus:
	{
		auto Column = [&](const TArray<FVector3f>& Path, float R0)
		{
			TArray<float> Radii;
			for (int32 I = 0; I < Path.Num(); ++I) Radii.Add(R0 * (I == Path.Num() - 1 ? 0.8f : 1.f));
			G.Tube(Path, Radii, 16, Leaf, 0.12f);
		};
		const float R0 = Rad * 0.28f;
		Column({ FVector3f(0, 0, -20), FVector3f(0, 0, H * 0.5f), FVector3f(0, 0, H * 0.95f) }, R0);
		for (const float S : { -1.f, 1.f })
		{
			if (Rnd.FRand() < 0.25f) continue;
			const float Y = H * Rnd.FRandRange(0.3f, 0.5f);
			Column({ FVector3f(0, 0, Y), FVector3f(S * Rad * 0.55f, 0, Y + H * 0.02f), FVector3f(S * Rad * 0.65f, 0, Y + H * 0.2f), FVector3f(S * Rad * 0.65f, 0, Y + H * 0.38f) }, R0 * 0.7f);
		}
		break;
	}
	default:
		break;
	}
	// Turned and set at its place; smooth-shaded.
	for (FBRRMeshSection& S : G.Mesh.Sections)
		for (FBRRVertex& V : S.Vertices) V.Position = Tree.Base + Turn(V.Position, Tree.Yaw);
	if (G.Mesh.Sections[1].Vertices.IsEmpty()) G.Mesh.Sections.Pop();
	if (G.Mesh.Sections[0].Vertices.IsEmpty()) G.Mesh.Sections.RemoveAt(0);
	BRR::SmoothNormals(G.Mesh, 70.f);
	return G.Mesh;
}
