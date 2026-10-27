#include "BRRRefine.h"

#include "Math/RandomStream.h"

namespace
{
	// MakeMesh's face normal: UE's winding is clockwise-front.
	FVector3f FaceCross(const FVector3f& A, const FVector3f& B, const FVector3f& C) { return (C - A) ^ (B - A); }

	// Cuts every triangle into Divisions x Divisions; Place gives each new point's position from
	// the corners and its barycentric weights (corner 0, 1, 2).
	template <typename FPlace>
	FBRRMesh SubdivideWith(const FBRRMesh& Mesh, int32 Divisions, FPlace Place)
	{
		FBRRMesh Out;
		const int32 N = FMath::Max(Divisions, 1);
		for (const FBRRMeshSection& Section : Mesh.Sections)
		{
			FBRRMeshSection& To = Out.Sections.AddDefaulted_GetRef();
			To.Atlas = Section.Atlas;
			To.bTwoSided = Section.bTwoSided;
			To.Vertices.Reserve(Section.Vertices.Num() * N * N);
			for (int32 T = 0; T + 2 < Section.Vertices.Num(); T += 3)
			{
				const FBRRVertex* V = &Section.Vertices[T];
				// Point (I, J): I steps toward corner 1, J toward corner 2.
				auto Point = [&](int32 I, int32 J)
				{
					const float B1 = float(I) / N, B2 = float(J) / N, B0 = 1.f - B1 - B2;
					FBRRVertex P = V[0];
					P.UV = V[0].UV * B0 + V[1].UV * B1 + V[2].UV * B2;
					P.Position = Place(V, FVector3f(B0, B1, B2));
					P.Normal = FVector3f::ZeroVector;
					return P;
				};
				for (int32 J = 0; J < N; ++J)
				{
					for (int32 I = 0; I + J < N; ++I)
					{
						To.Vertices.Add(Point(I, J));
						To.Vertices.Add(Point(I + 1, J));
						To.Vertices.Add(Point(I, J + 1));
						if (I + J + 1 < N)
						{
							To.Vertices.Add(Point(I + 1, J));
							To.Vertices.Add(Point(I + 1, J + 1));
							To.Vertices.Add(Point(I, J + 1));
						}
					}
				}
			}
		}
		return Out;
	}

	FVector3f Linear(const FBRRVertex* V, const FVector3f& B) { return V[0].Position * B.X + V[1].Position * B.Y + V[2].Position * B.Z; }

	void AddTriangle(FBRRMeshSection& S, const FVector3f& A, const FVector3f& B, const FVector3f& C, FColor Colour, const FVector3f& Outward)
	{
		const bool bFlip = (FaceCross(A, B, C) | Outward) < 0.f;
		for (const FVector3f& P : { A, bFlip ? C : B, bFlip ? B : C })
		{
			FBRRVertex& V = S.Vertices.AddDefaulted_GetRef();
			V.Position = P;
			V.Color = Colour;
			V.UV = FVector2f::ZeroVector;
		}
	}
}

void BRR::SmoothNormals(FBRRMesh& Mesh, float MaxAngleDegrees)
{
	// One entry per corner, sorted by its position (to half a centimetre) so the corners meeting
	// at a point are neighbours.
	struct FCorner { FIntVector Key; int32 Section; int32 Index; };
	TArray<FCorner> Corners;
	Corners.Reserve(Mesh.NumTriangles() * 3);
	for (int32 S = 0; S < Mesh.Sections.Num(); ++S)
	{
		const TArray<FBRRVertex>& Vs = Mesh.Sections[S].Vertices;
		for (int32 I = 0; I < Vs.Num(); ++I)
		{
			const FVector3f& P = Vs[I].Position;
			Corners.Add({ FIntVector(FMath::RoundToInt32(P.X * 2.f), FMath::RoundToInt32(P.Y * 2.f), FMath::RoundToInt32(P.Z * 2.f)), S, I });
		}
	}
	Corners.Sort([](const FCorner& A, const FCorner& B)
	{
		return A.Key.X != B.Key.X ? A.Key.X < B.Key.X : A.Key.Y != B.Key.Y ? A.Key.Y < B.Key.Y : A.Key.Z < B.Key.Z;
	});
	auto Cross = [&](const FCorner& C)
	{
		const FBRRVertex* T = &Mesh.Sections[C.Section].Vertices[C.Index - C.Index % 3];
		return FaceCross(T[0].Position, T[1].Position, T[2].Position);
	};
	const float MinCos = FMath::Cos(FMath::DegreesToRadians(MaxAngleDegrees));
	TArray<FVector3f> Faces;
	for (int32 Start = 0; Start < Corners.Num();)
	{
		int32 End = Start + 1;
		while (End < Corners.Num() && Corners[End].Key == Corners[Start].Key) ++End;
		Faces.Reset();
		for (int32 K = Start; K < End; ++K) Faces.Add(Cross(Corners[K]));
		for (int32 K = Start; K < End; ++K)
		{
			const FVector3f Own = Faces[K - Start].GetSafeNormal();
			if (Own.IsNearlyZero()) continue;
			FVector3f Sum = FVector3f::ZeroVector;
			for (const FVector3f& F : Faces)
			{
				if ((F.GetSafeNormal() | Own) >= MinCos) Sum += F;
			}
			Mesh.Sections[Corners[K].Section].Vertices[Corners[K].Index].Normal = Sum.GetSafeNormal();
		}
		Start = End;
	}
}

FBRRMesh BRR::Subdivide(const FBRRMesh& Mesh, int32 Divisions)
{
	return SubdivideWith(Mesh, Divisions, &Linear);
}

FBRRMesh BRR::RoundWheel(const FBRRMesh& Wheel, const FVector& InAxle, int32 Divisions)
{
	const FVector3f Axle = FVector3f(InAxle.GetSafeNormal());
	const FVector3f Centre = Wheel.GetBounds().GetCenter();
	auto Radial = [&](const FVector3f& P) { const FVector3f D = P - Centre; return D - Axle * (D | Axle); };
	FBRRMesh Out = SubdivideWith(Wheel, Divisions, [&](const FBRRVertex* V, const FVector3f& B)
	{
		const FVector3f P = Linear(V, B);
		const FVector3f D = Radial(P);
		const float Length = D.Size();
		if (Length < 0.01f) return P;
		const float Radius = Radial(V[0].Position).Size() * B.X + Radial(V[1].Position).Size() * B.Y + Radial(V[2].Position).Size() * B.Z;
		return P + D * (Radius / Length - 1.f);
	});
	SmoothNormals(Out, 50.f);
	return Out;
}

void BRR::SplitWater(const FBRRWorld& World, const FBRRMesh& Refined, FBRRMesh& OutLand, FBRRMesh& OutWater, FBRRMesh& OutBed)
{
	auto IsWater = [&](uint8 Land) { return World.LiquidOf(Land) == EBRRLiquid::Water; };
	auto Split = [&](const FBRRMesh& From, FBRRMesh& Kept, bool bKeepWet)
	{
		for (const FBRRMeshSection& S : From.Sections)
		{
			FBRRMeshSection& To = Kept.Sections.AddDefaulted_GetRef();
			To.Atlas = S.Atlas;
			To.bTwoSided = S.bTwoSided;
			for (int32 T = 0; T + 2 < S.Vertices.Num(); T += 3)
			{
				if (IsWater(S.Vertices[T].Land) == bKeepWet) To.Vertices.Append(&S.Vertices[T], 3);
			}
			if (To.Vertices.IsEmpty()) Kept.Sections.Pop();
		}
	};
	OutLand = FBRRMesh();
	OutWater = FBRRMesh();
	OutBed = FBRRMesh();
	// The land is all of the refined terrain: under the water it is the bed, already let down
	// (FBRRWorld::ShoreSinkAt), as sand.
	constexpr uint8 Sand = 5;
	OutLand = Refined;
	for (FBRRMeshSection& S : OutLand.Sections)
	{
		for (FBRRVertex& V : S.Vertices) if (IsWater(V.Land)) V.Land = Sand;
	}
	// The water: the original's, and over every triangle beside it at its level, so it runs into
	// the let-down banks as far as their surface stays under it - the shore is where they cross.
	Split(World.Terrain, OutWater, true);
	const double Cell = BRR::TerrainCellUnits * BRR::CmPerUnit;
	auto CornerOf = [Cell](const FVector3f& P) { return FIntPoint(FMath::RoundToInt32(P.X / Cell), FMath::RoundToInt32(P.Y / Cell)); };
	TMap<FIntPoint, float> Level;
	for (const FBRRMeshSection& S : OutWater.Sections)
	{
		for (const FBRRVertex& V : S.Vertices)
		{
			float& Z = Level.FindOrAdd(CornerOf(V.Position), V.Position.Z);
			Z = FMath::Min(Z, V.Position.Z);
		}
	}
	FBRRMeshSection& Shore = OutWater.Sections.AddDefaulted_GetRef();
	if (OutWater.Sections.Num() > 1) Shore.Atlas = OutWater.Sections[0].Atlas;
	for (const FBRRMeshSection& S : World.Terrain.Sections)
	{
		for (int32 T = 0; T + 2 < S.Vertices.Num(); T += 3)
		{
			if (IsWater(S.Vertices[T].Land)) continue;
			float Z = TNumericLimits<float>::Max();
			bool bSunk = false;
			for (int32 K = 0; K < 3; ++K)
			{
				const FVector3f& P = S.Vertices[T + K].Position;
				if (const float* L = Level.Find(CornerOf(P))) Z = FMath::Min(Z, *L);
				bSunk |= World.ShoreSinkAt(P.X, P.Y) > 1.0;
			}
			if (Z == TNumericLimits<float>::Max() || !bSunk) continue;
			for (int32 K = 0; K < 3; ++K)
			{
				FBRRVertex V = S.Vertices[T + K];
				V.Position.Z = Z;
				Shore.Vertices.Add(V);
			}
		}
	}
	if (Shore.Vertices.IsEmpty()) OutWater.Sections.Pop();
}

FBRRMesh BRR::MakeGrassTuft(int32 Seed)
{
	FRandomStream Rand(Seed * 7919 + 13);
	FBRRMesh Mesh;
	FBRRMeshSection& S = Mesh.Sections.AddDefaulted_GetRef();
	S.bTwoSided = true;
	const int32 Blades = 22, Segments = 4;
	for (int32 B = 0; B < Blades; ++B)
	{
		const float Angle = Rand.FRandRange(0.f, 2.f * PI);
		const FVector3f Root(Rand.FRandRange(-18.f, 18.f), Rand.FRandRange(-18.f, 18.f), -2.f);
		const FVector3f Across(FMath::Cos(Angle), FMath::Sin(Angle), 0.f);
		const FVector3f Lean = FVector3f(-Across.Y, Across.X, 0.f) * Rand.FRandRange(-0.5f, 0.5f) + (Root.GetSafeNormal() * 0.3f);
		const float Height = Rand.FRandRange(22.f, 44.f), Width = Rand.FRandRange(1.6f, 2.6f);
		auto At = [&](int32 K, float Side)
		{
			const float T = float(K) / Segments;
			// Bending over more toward the tip, narrowing to a point.
			return Root + FVector3f(0, 0, Height * T) + Lean * (Height * T * T) + Across * (Side * Width * (1.f - T));
		};
		for (int32 K = 0; K < Segments; ++K)
		{
			// Darker at the root, where the blades shade each other.
			const uint8 Low = uint8(FMath::Lerp(150.f, 255.f, float(K) / Segments)), High = uint8(FMath::Lerp(150.f, 255.f, float(K + 1) / Segments));
			const FVector3f Facing = FVector3f(-Across.Y, Across.X, 0.f);
			FBRRVertex Quad[4];
			const FVector3f P[4] = { At(K, -1), At(K, 1), At(K + 1, 1), At(K + 1, -1) };
			const uint8 Shade[4] = { Low, Low, High, High };
			for (int32 I = 0; I < 4; ++I)
			{
				Quad[I].Position = P[I];
				Quad[I].Color = FColor(Shade[I], Shade[I], Shade[I]);
				Quad[I].UV = FVector2f::ZeroVector;
				Quad[I].Normal = (Facing + FVector3f(0, 0, 0.6f)).GetSafeNormal(); // lit like the ground, not edge-on
			}
			for (const int32 I : { 0, 2, 1, 0, 3, 2 }) S.Vertices.Add(Quad[I]);
		}
	}
	return Mesh;
}

FBRRMesh BRR::MakeStone(int32 Seed, int32 Detail)
{
	FRandomStream Rand(Seed * 104729 + 7);
	// An icosahedron cut Detail times, each point pushed in and out by a few random waves.
	const float G = (1.f + FMath::Sqrt(5.f)) * 0.5f;
	const FVector3f Ico[12] = { { -1, G, 0 }, { 1, G, 0 }, { -1, -G, 0 }, { 1, -G, 0 }, { 0, -1, G }, { 0, 1, G }, { 0, -1, -G }, { 0, 1, -G },
		{ G, 0, -1 }, { G, 0, 1 }, { -G, 0, -1 }, { -G, 0, 1 } };
	const int32 Faces[20][3] = { { 0, 11, 5 }, { 0, 5, 1 }, { 0, 1, 7 }, { 0, 7, 10 }, { 0, 10, 11 }, { 1, 5, 9 }, { 5, 11, 4 }, { 11, 10, 2 },
		{ 10, 7, 6 }, { 7, 1, 8 }, { 3, 9, 4 }, { 3, 4, 2 }, { 3, 2, 6 }, { 3, 6, 8 }, { 3, 8, 9 }, { 4, 9, 5 }, { 2, 4, 11 }, { 6, 2, 10 },
		{ 8, 6, 7 }, { 9, 8, 1 } };
	FVector3f Waves[5];
	float Phase[5];
	for (int32 W = 0; W < 5; ++W)
	{
		Waves[W] = FVector3f(Rand.GetUnitVector()) * Rand.FRandRange(1.5f, 4.f);
		Phase[W] = Rand.FRandRange(0.f, 2.f * PI);
	}
	const FVector3f Squash(Rand.FRandRange(0.8f, 1.2f), Rand.FRandRange(0.8f, 1.2f), Rand.FRandRange(0.45f, 0.7f));
	auto Shape = [&](const FVector3f& Direction)
	{
		const FVector3f D = Direction.GetSafeNormal();
		float R = 1.f;
		for (int32 W = 0; W < 5; ++W) R += 0.09f * FMath::Sin((D | Waves[W]) + Phase[W]);
		const FVector3f P = D * R * Squash * 50.f;
		return FVector3f(P.X, P.Y, P.Z + 50.f * Squash.Z * 0.55f); // sunk a little into the ground
	};
	TArray<TArray<FVector3f>> Triangles;
	for (const auto& F : Faces) Triangles.Add({ Ico[F[0]].GetSafeNormal(), Ico[F[1]].GetSafeNormal(), Ico[F[2]].GetSafeNormal() });
	for (int32 Pass = 0; Pass < Detail; ++Pass)
	{
		TArray<TArray<FVector3f>> Next;
		for (const TArray<FVector3f>& T : Triangles)
		{
			const FVector3f AB = (T[0] + T[1]).GetSafeNormal(), BC = (T[1] + T[2]).GetSafeNormal(), CA = (T[2] + T[0]).GetSafeNormal();
			Next.Add({ T[0], AB, CA });
			Next.Add({ AB, T[1], BC });
			Next.Add({ CA, BC, T[2] });
			Next.Add({ AB, BC, CA });
		}
		Triangles = MoveTemp(Next);
	}
	FBRRMesh Mesh;
	FBRRMeshSection& S = Mesh.Sections.AddDefaulted_GetRef();
	S.bTwoSided = false;
	for (const TArray<FVector3f>& T : Triangles)
	{
		const FVector3f Mid = (T[0] + T[1] + T[2]).GetSafeNormal();
		// Lighter on top, as the weather leaves stone.
		const uint8 Shade = uint8(FMath::Clamp(200.f + 40.f * Mid.Z + Rand.FRandRange(-12.f, 12.f), 0.f, 255.f));
		AddTriangle(S, Shape(T[0]), Shape(T[1]), Shape(T[2]), FColor(Shade, Shade, Shade), Mid);
	}
	SmoothNormals(Mesh, 70.f);
	return Mesh;
}

TArray<BRR::FBRRScatter> BRR::ScatterGround(const FBRRWorld& World, double Reach)
{
	TArray<FBRRScatter> Out;
	const int32 Rows = World.MapHeight, Cols = World.MapWidth;
	if (Rows < 2 || Cols < 2) return Out;
	const double Cell = BRR::TerrainCellUnits * BRR::CmPerUnit;
	constexpr uint8 Track = 0, Grippy = 1, Ramps = 6;
	auto IsTrack = [](uint8 Land) { return Land == Track || Land == Grippy || Land == Ramps; };
	auto CellOf = [&](const FVector3f& P) { return FIntPoint(FMath::Clamp(FMath::FloorToInt32(P.X / Cell), 0, Rows - 1), FMath::Clamp(FMath::FloorToInt32(P.Y / Cell), 0, Cols - 1)); };

	// The cells within Reach of the track.
	TBitArray<> Near(false, Rows * Cols);
	const int32 Spread = FMath::CeilToInt32(Reach / Cell);
	for (const FBRRMeshSection& S : World.Terrain.Sections)
	{
		for (int32 T = 0; T + 2 < S.Vertices.Num(); T += 3)
		{
			if (!IsTrack(S.Vertices[T].Land)) continue;
			const FIntPoint C = CellOf((S.Vertices[T].Position + S.Vertices[T + 1].Position + S.Vertices[T + 2].Position) / 3.f);
			for (int32 R = FMath::Max(C.X - Spread, 0); R <= FMath::Min(C.X + Spread, Rows - 1); ++R)
				for (int32 Col = FMath::Max(C.Y - Spread, 0); Col <= FMath::Min(C.Y + Spread, Cols - 1); ++Col) Near[R * Cols + Col] = true;
		}
	}

	// Per square metre, by ground kind: grass tufts, pebbles, rocks, snow lumps.
	auto Density = [](EBRRGround G, int32 Kind)
	{
		switch (G)
		{
		case EBRRGround::Grass: return Kind == 0 ? 0.3f : Kind == 1 ? 0.02f : 0.f;
		case EBRRGround::Dirt: return Kind == 1 ? 0.12f : Kind == 0 ? 0.04f : Kind == 2 ? 0.002f : 0.f;
		case EBRRGround::Sand: return Kind == 1 ? 0.05f : 0.f;
		case EBRRGround::Rock: return Kind == 2 ? 0.012f : Kind == 1 ? 0.15f : 0.f;
		case EBRRGround::Snow: return Kind == 3 ? 0.03f : 0.f;
		default: return 0.f;
		}
	};
	const uint32 LevelSeed = GetTypeHash(World.LevelName);
	int32 Triangle = 0;
	for (const FBRRMeshSection& S : World.Terrain.Sections)
	{
		const FBRRImage* Page = World.Atlases.IsValidIndex(S.Atlas) ? &World.Atlases[S.Atlas] : nullptr;
		for (int32 T = 0; T + 2 < S.Vertices.Num(); T += 3, ++Triangle)
		{
			const FBRRVertex* V = &S.Vertices[T];
			if (IsTrack(V[0].Land)) continue;
			const FVector3f A = V[0].Position, B = V[1].Position, C = V[2].Position;
			if (!Near[CellOf((A + B + C) / 3.f).X * Cols + CellOf((A + B + C) / 3.f).Y]) continue;
			const FColor Average = Page ? FBRRWorld::AverageTexels(*Page, V[0].UV, V[1].UV, V[2].UV) : V[0].Color;
			const EBRRGround Ground = World.GroundOf(V[0].Land, Average);
			if (Ground == EBRRGround::None) continue;
			const FVector3f Cross = FaceCross(A, B, C);
			const FVector3f Up = Cross.GetSafeNormal();
			if (Up.Z < 0.2f) continue; // walls and overhangs
			const float AreaM2 = Cross.Size() * 0.5f / 10000.f;
			FRandomStream Rand(int32(HashCombine(LevelSeed, uint32(Triangle))));
			for (int32 Kind = 0; Kind < 4; ++Kind)
			{
				const float Expected = Density(Ground, Kind) * AreaM2;
				int32 Count = FMath::FloorToInt32(Expected);
				if (Rand.FRand() < Expected - Count) ++Count;
				for (int32 K = 0; K < Count; ++K)
				{
					float U = Rand.FRand(), W = Rand.FRand();
					if (U + W > 1.f) { U = 1.f - U; W = 1.f - W; }
					FVector3f P = A + (B - A) * U + (C - A) * W;
					const double Sink = World.ShoreSinkAt(P.X, P.Y);
					if (Sink > 10.0) continue; // the banks let down into the water
					P.Z += float(World.RefinedOffsetAt(P.X, P.Y) - Sink);
					FColor Under = V[0].Color;
					if (Page)
					{
						const FVector2f UV = V[0].UV + (V[1].UV - V[0].UV) * U + (V[2].UV - V[0].UV) * W;
						const int32 X = FMath::Clamp(int32(UV.X * Page->Width), 0, Page->Width - 1), Y = FMath::Clamp(int32(UV.Y * Page->Height), 0, Page->Height - 1);
						Under = Page->Pixels[Y * Page->Width + X];
						if (Under.A == 0) Under = Average;
					}
					FBRRScatter& Item = Out.AddDefaulted_GetRef();
					Item.Kind = uint8(Kind);
					Item.Location = P;
					Item.Yaw = Rand.FRandRange(0.f, 360.f);
					Item.Scale = Kind == 0 ? Rand.FRandRange(0.7f, 1.3f) : Kind == 1 ? Rand.FRandRange(0.1f, 0.3f) : Kind == 2 ? Rand.FRandRange(0.6f, 1.8f) : Rand.FRandRange(0.4f, 1.0f);
					Item.Up = Kind == 0 ? (Up + FVector3f::UpVector * 2.f).GetSafeNormal() : Up;
					// Grass brighter than the ground it grows from (lit through, and over its shadow); stones the ground's.
					const FLinearColor Linear = FLinearColor::FromSRGBColor(Under);
					Item.Colour = Kind == 0 ? Linear * Rand.FRandRange(1.3f, 1.8f) : Kind == 3 ? FLinearColor(0.85f, 0.88f, 0.95f) : Linear * Rand.FRandRange(0.8f, 1.1f);
				}
			}
		}
	}
	return Out;
}
