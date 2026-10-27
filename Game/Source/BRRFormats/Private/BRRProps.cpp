#include "BRRProps.h"

#include "BRRShapes.h"

using namespace BRRShapes;

BRR::EPropKind BRR::PropKindOf(const FString& Shape)
{
	const FString S = Shape.ToUpper();
	if (S == TEXT("TYRES")) return EPropKind::Tyres;
	if (S == TEXT("CONE2")) return EPropKind::Cone;
	if (S == TEXT("HAYBALE")) return EPropKind::Haybale;
	if (S == TEXT("FENCE")) return EPropKind::Fence;
	if (S == TEXT("BARRIER") || S == TEXT("LONGBAR") || S == TEXT("WALL2") || S == TEXT("SLATS") || S == TEXT("WALLFRIG")) return EPropKind::Barrier;
	if (S == TEXT("LAMPOST")) return EPropKind::Lamp;
	if (S == TEXT("TELEPOLE") || S == TEXT("POLE")) return EPropKind::Pole;
	if (S.StartsWith(TEXT("ROCK"))) return EPropKind::Rock;
	if (S == TEXT("ROADSIGN") || S == TEXT("DIR1") || S == TEXT("DIR2") || S == TEXT("CHEVRONS") || S.StartsWith(TEXT("MSIGN")) || S.StartsWith(TEXT("BILBORD"))
		|| S == TEXT("SIGN1") || S == TEXT("SIGNPOST") || S == TEXT("BSIGN") || S == TEXT("ARROSIGN") || S == TEXT("ARROW") || S.StartsWith(TEXT("VSIGN"))) return EPropKind::Sign;
	if (S.Contains(TEXT("TUNN")) || S.Contains(TEXT("BRIJ")) || S.Contains(TEXT("BRIDGE")) || S.StartsWith(TEXT("BR_")) || S == TEXT("FOOT_BR")
		|| S.StartsWith(TEXT("RAMP")) || S.StartsWith(TEXT("PLAT")) || S.StartsWith(TEXT("MARSPLA")) || S == TEXT("SUPP") || S == TEXT("HUG_FLR") || S == TEXT("SLOPE")
		|| S == TEXT("FLAT") || S.StartsWith(TEXT("JOIN")) || S == TEXT("JETTY") || S == TEXT("STEPS") || S == TEXT("HELIPAD") || S == TEXT("OVERHANG")) return EPropKind::Structure;
	static const TCHAR* Buildings[] = { TEXT("BLOCK"), TEXT("OFFICE"), TEXT("SHOP"), TEXT("BIGBLK"), TEXT("BIGBUIL"), TEXT("PUB"), TEXT("MUDHUT"), TEXT("PAGODA"),
		TEXT("BAY_ROOF"), TEXT("BAY_POLE"), TEXT("FONEBOX"), TEXT("CHINA1"), TEXT("IND1"), TEXT("IND2"), TEXT("TOWER"), TEXT("VTOWER"), TEXT("SPIRE"), TEXT("VSPIRE"),
		TEXT("PINNACLE"), TEXT("CITADEL"), TEXT("DOME"), TEXT("LUNAPOD"), TEXT("ROCKET"), TEXT("MONOLITH"), TEXT("NODE"), TEXT("VNODE"), TEXT("CRANE"),
		TEXT("STATUE"), TEXT("COLUMN"), TEXT("START"), TEXT("GANTRY"), TEXT("BAYWATCH"), TEXT("HAWT"), TEXT("WALL"), TEXT("VWALL"), TEXT("MWALL"), TEXT("LEG"), TEXT("BIN_LID"),
		TEXT("BUILD"), TEXT("TEST"), TEXT("ENT2"), TEXT("BUS") };
	for (const TCHAR* B : Buildings) if (S.StartsWith(B)) return EPropKind::Building;
	return EPropKind::None;
}

namespace
{
	struct FTri { const FBRRVertex* V; int32 Section; int32 Index; };

	// The decor's pieces (runs of triangles joined at their corners), each given to the placement
	// nearest its footprint: placement -> its pieces -> their triangles.
	TMap<int32, TArray<TArray<int32>>> GroupDecor(const FBRRWorld& World, const TArray<FTri>& Tris, TFunctionRef<bool(int32)> Wanted)
	{
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
		TMap<int32, TArray<TArray<int32>>> Out;
		for (TPair<int32, TArray<int32>>& Piece : Pieces)
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
			if (Best != INDEX_NONE && Wanted(Best)) Out.FindOrAdd(Best).Add(MoveTemp(Piece.Value));
		}
		return Out;
	}

	FColor MeanColour(const FBRRWorld& World, const TArray<FTri>& Tris, const TArray<int32>& Of)
	{
		FLinearColor Sum = FLinearColor::Transparent;
		for (const int32 I : Of)
		{
			const FTri& T = Tris[I];
			const FBRRMeshSection& S = World.Decor.Sections[T.Section];
			const FColor C = World.Atlases.IsValidIndex(S.Atlas) ? FBRRWorld::AverageTexels(World.Atlases[S.Atlas], T.V[0].UV, T.V[1].UV, T.V[2].UV) : T.V[0].Color;
			const float Area = ((T.V[1].Position - T.V[0].Position) ^ (T.V[2].Position - T.V[0].Position)).Size() * 0.5f + 1e-3f;
			const FLinearColor L = FLinearColor::FromSRGBColor(C);
			Sum += FLinearColor(L.R * Area, L.G * Area, L.B * Area, Area);
		}
		return Sum.A > 0 ? FLinearColor(Sum.R / Sum.A, Sum.G / Sum.A, Sum.B / Sum.A).ToFColor(true) : FColor(128, 128, 128);
	}

	const FQuat4f Identity = FQuat4f::Identity;
	const FColor Metal(150, 155, 160), DarkMetal(42, 44, 48), Rubber(26, 26, 28);
}

FBRRMesh BRR::BuildProps(const FBRRWorld& World, TBitArray<>& OutReplaced, int32& OutCount)
{
	FBRRMesh Out;
	OutCount = 0;
	const FBRRMesh& Decor = World.Decor;
	OutReplaced.Init(false, Decor.NumTriangles());
	TArray<FTri> Tris;
	for (int32 S = 0; S < Decor.Sections.Num(); ++S)
		for (int32 T = 0; T + 2 < Decor.Sections[S].Vertices.Num(); T += 3) Tris.Add({ &Decor.Sections[S].Vertices[T], S, Tris.Num() });
	if (Tris.IsEmpty()) return Out;
	const TMap<int32, TArray<TArray<int32>>> Groups = GroupDecor(World, Tris, [&](int32 P) { return PropKindOf(World.Placements[P].Shape) != EPropKind::None; });

	// The sections: built shapes (flat colours, one- and two-sided), then the signs' own art by page.
	Out.Sections.SetNum(2);
	Out.Sections[1].bTwoSided = true;
	TMap<TPair<int32, bool>, int32> ArtSection;
	auto Keep = [&](const FTri& T)
	{
		const FBRRMeshSection& From = Decor.Sections[T.Section];
		int32* S = ArtSection.Find({ From.Atlas, From.bTwoSided });
		if (!S)
		{
			S = &ArtSection.Add({ From.Atlas, From.bTwoSided }, Out.Sections.Num());
			FBRRMeshSection& New = Out.Sections.AddDefaulted_GetRef();
			New.Atlas = From.Atlas;
			New.bTwoSided = From.bTwoSided;
		}
		for (int32 K = 0; K < 3; ++K) { FBRRVertex V = T.V[K]; V.Normal = FVector3f::ZeroVector; Out.Sections[*S].Vertices.Add(V); }
	};

	for (const TPair<int32, TArray<TArray<int32>>>& Group : Groups)
	{
		const FBRRPlacement& Place = World.Placements[Group.Key];
		const EPropKind Kind = PropKindOf(Place.Shape);
		const FQuat4f Rot = FQuat4f(Place.Rotation);
		const FVector3f Origin = FVector3f(Place.Location);
		auto Local = [&](const FVector3f& P) { return Rot.UnrotateVector(P - Origin); };
		TArray<int32> All;
		FBox3f Box(ForceInit);
		for (const TArray<int32>& Piece : Group.Value)
		{
			All.Append(Piece);
			for (const int32 I : Piece) for (int32 K = 0; K < 3; ++K) Box += Local(Tris[I].V[K].Position);
		}
		const FVector3f E = Box.GetSize(), C = Box.GetCenter();
		// A name is a guess at what it is: one far from that kind's size (CITY's BARRIER is a wall
		// of a building, not a kerb) stays the original, rounded.
		const float Low = FMath::Min(E.X, E.Y);
		const bool bFits =
			Kind == EPropKind::Tyres ? E.Z < 260.f && Low < 200.f :
			Kind == EPropKind::Cone ? E.Z < 160.f :
			Kind == EPropKind::Haybale ? E.Z < 300.f :
			Kind == EPropKind::Fence ? E.Z < 300.f && Low < 90.f :
			Kind == EPropKind::Barrier ? E.Z < 170.f && Low < 150.f :
			Kind == EPropKind::Lamp || Kind == EPropKind::Pole ? E.Z > 200.f && Low < 600.f : true;
		if (!bFits) continue;
		for (const int32 I : All) OutReplaced[I] = true;
		const FColor Colour = MeanColour(World, Tris, All);
		FBuilder B(Group.Key * 92821 + 7);
		FRandomStream& Rnd = B.Rand;
		const bool bAlongX = E.X >= E.Y;
		const float Long = bAlongX ? E.X : E.Y, Short = bAlongX ? E.Y : E.X;
		const FQuat4f Along = bAlongX ? Identity : FQuat4f(FVector3f::UpVector, PI * 0.5f); // local X along the length

		switch (Kind)
		{
		case EPropKind::Tyres:
		{
			// Stacks of tyres filling its footprint, painted ones among the black when it is coloured.
			const float R = FMath::Max(Short * 0.5f, 20.f);
			const float TyreH = R * 0.62f;
			const int32 Across = FMath::Max(1, FMath::RoundToInt32(Long / (2.f * R)));
			const int32 Up = FMath::Max(1, FMath::RoundToInt32(E.Z / TyreH));
			const bool bPainted = FMath::Max3(Colour.R, Colour.G, Colour.B) - FMath::Min3(Colour.R, Colour.G, Colour.B) > 40;
			for (int32 A = 0; A < Across; ++A)
				for (int32 U = 0; U < Up; ++U)
				{
					const float X = -Long * 0.5f + (A + 0.5f) * Long / Across;
					const FVector3f At = FVector3f(C.X, C.Y, Box.Min.Z + (U + 0.5f) * E.Z / Up) + Along.RotateVector(FVector3f(X, 0, 0));
					B.Torus(At, R * 0.66f, R * 0.32f, E.Z / Up * 0.48f, bPainted && ((A + U) & 1) ? Colour : Rubber);
				}
			break;
		}
		case EPropKind::Cone:
		{
			const float R = FMath::Max(FMath::Min(E.X, E.Y) * 0.4f, 8.f), H = E.Z;
			const FColor W(235, 235, 230);
			B.Box(FVector3f(C.X, C.Y, Box.Min.Z + 1.5f), FVector3f(R * 1.15f, R * 1.15f, 1.5f), Identity, Rubber);
			B.Lathe(FVector3f(C.X, C.Y, Box.Min.Z + 3.f),
				{ {R, 0}, {R * 0.66f, H * 0.35f}, {R * 0.62f, H * 0.35f}, {R * 0.52f, H * 0.47f}, {R * 0.5f, H * 0.47f}, {R * 0.38f, H * 0.62f}, {R * 0.36f, H * 0.62f}, {R * 0.26f, H * 0.74f}, {R * 0.1f, H * 0.97f}, {0, H * 0.97f} },
				{ Colour, Colour, W, W, Colour, Colour, W, W, Colour, Colour }, 20);
			break;
		}
		case EPropKind::Haybale:
			B.Blob(C, E * 0.5f, Colour, 0.05f, 3, Group.Key, 0.8f);
			break;
		case EPropKind::Fence:
		{
			// Posts every metre and a half, three rails.
			const float H = E.Z;
			const int32 Posts = FMath::Max(2, FMath::RoundToInt32(Long / 150.f) + 1);
			const FVector3f Mid(C.X, C.Y, Box.Min.Z);
			for (int32 P = 0; P < Posts; ++P)
			{
				const float X = -Long * 0.5f + Long * P / (Posts - 1);
				B.Box(Mid + Along.RotateVector(FVector3f(X, 0, H * 0.5f)), FVector3f(5.f, 5.f, H * 0.5f), Along, Shade(Colour, Rnd.FRandRange(0.85f, 1.05f)));
			}
			for (const float Y : { 0.3f, 0.62f, 0.9f })
				B.Box(Mid + Along.RotateVector(FVector3f(0, 4.f, H * Y)), FVector3f(Long * 0.5f, 2.5f, FMath::Max(H * 0.05f, 4.f)), Along, Shade(Colour, 1.05f));
			break;
		}
		case EPropKind::Barrier:
		{
			// Concrete barrier sections, the profile a crash barrier's, a joint every three metres.
			const float H = E.Z, W = FMath::Max(Short, 20.f);
			const TArray<FVector2f> Profile = { {-W * 0.5f, 0}, {W * 0.5f, 0}, {W * 0.5f, H * 0.1f}, {W * 0.28f, H * 0.3f}, {W * 0.18f, H}, {-W * 0.18f, H}, {-W * 0.28f, H * 0.3f}, {-W * 0.5f, H * 0.1f} };
			const int32 Sections = FMath::Max(1, FMath::RoundToInt32(Long / 300.f));
			const FVector3f Mid(C.X, C.Y, Box.Min.Z);
			for (int32 S = 0; S < Sections; ++S)
			{
				const float X0 = -Long * 0.5f + Long * S / Sections + 1.5f, X1 = -Long * 0.5f + Long * (S + 1) / Sections - 1.5f;
				B.Sweep(Mid, Along, X0, X1, Profile, Shade(Colour, (S & 1) ? 0.92f : 1.04f));
			}
			break;
		}
		case EPropKind::Lamp:
		{
			const float H = E.Z;
			const FVector3f Foot(C.X, C.Y, Box.Min.Z);
			B.Lathe(Foot, { {14, 0}, {14, 40}, {9, 60}, {7, H * 0.92f}, {0, H * 0.92f} }, { DarkMetal, DarkMetal, DarkMetal, DarkMetal, DarkMetal }, 12);
			const FVector3f Top = Foot + FVector3f(0, 0, H * 0.9f), Reach = Along.RotateVector(FVector3f(FMath::Max(Long * 0.45f, H * 0.18f), 0, 0));
			B.Tube({ Top, Top + Reach * 0.5f + FVector3f(0, 0, H * 0.06f), Top + Reach }, { 5.f, 4.f, 4.f }, 8, DarkMetal);
			B.Box(Top + Reach + FVector3f(0, 0, -6), FVector3f(26, 12, 6), Along, DarkMetal);
			B.Box(Top + Reach + FVector3f(0, 0, -13), FVector3f(22, 9, 1.5f), Along, FColor(255, 240, 200));
			break;
		}
		case EPropKind::Pole:
		{
			const float H = E.Z;
			const FVector3f Foot(C.X, C.Y, Box.Min.Z);
			const FColor Wood = Shade(Colour, 0.9f);
			B.Lathe(Foot, { {13, -20}, {11, H * 0.97f}, {0, H}, }, { Wood, Wood, Wood }, 10);
			B.Box(Foot + FVector3f(0, 0, H * 0.88f), FVector3f(6, 75, 6), Along, Wood);
			for (const float Y : { -60.f, -20.f, 20.f, 60.f })
				B.Lathe(Foot + Along.RotateVector(FVector3f(0, Y, H * 0.88f + 6)), { {3, 0}, {4, 4}, {2.5f, 9}, {0, 9} }, { Metal, Metal, Metal, Metal }, 6);
			break;
		}
		case EPropKind::Rock:
		{
			FBuilder Rock(Group.Key * 31 + 3);
			Rock.Blob(C, E * 0.52f, Colour, 0.32f, 3, Group.Key, 0.3f);
			for (FBRRMeshSection& S : Rock.Mesh.Sections)
				for (FBRRVertex& V : S.Vertices) V.Position.Z = FMath::Max(V.Position.Z, Box.Min.Z - 10.f); // sat in the ground
			for (int32 S = 0; S < Rock.Mesh.Sections.Num(); ++S) B.Mesh.Sections[S].Vertices.Append(Rock.Mesh.Sections[S].Vertices);
			break;
		}
		case EPropKind::Structure:
			// Driven on or through: exactly the race's, crisp.
			for (const int32 I : All) Keep(Tris[I]);
			break;
		case EPropKind::Building:
		{
			// Each box-like part kept crisp and given depth; anything rounder goes back to the
			// rounded scenery (a dome, a rocket, a spire suit it).
			const bool bCity = Place.Shape.StartsWith(TEXT("BLOCK")) || Place.Shape.StartsWith(TEXT("OFFICE")) || Place.Shape.StartsWith(TEXT("SHOP"))
				|| Place.Shape.StartsWith(TEXT("BIG")) || Place.Shape.StartsWith(TEXT("PUB")) || Place.Shape.StartsWith(TEXT("IND"));
			for (const TArray<int32>& Piece : Group.Value)
			{
				FBox3f P(ForceInit);
				float Area = 0;
				for (const int32 I : Piece)
				{
					for (int32 K = 0; K < 3; ++K) P += Local(Tris[I].V[K].Position);
					Area += ((Tris[I].V[1].Position - Tris[I].V[0].Position) ^ (Tris[I].V[2].Position - Tris[I].V[0].Position)).Size() * 0.5f;
				}
				const FVector3f PE = P.GetSize(), PC = P.GetCenter();
				const float BoxArea = 2.f * (PE.X * PE.Y + PE.Y * PE.Z + PE.X * PE.Z);
				const bool bBoxy = Piece.Num() <= 48 && BoxArea > 0 && Area > BoxArea * 0.55f && Area < BoxArea * 1.15f;
				if (!bBoxy)
				{
					for (const int32 I : Piece) OutReplaced[Tris[I].Index] = false; // rounded with the rest
					continue;
				}
				for (const int32 I : Piece) Keep(Tris[I]);
				if (PE.Z < 120.f || FMath::Min(PE.X, PE.Y) < 80.f) continue; // a part too small to dress
				const FColor Wall = MeanColour(World, Tris, Piece);
				const FColor Trim = Shade(Wall, 0.72f), Base = Shade(Wall, 0.6f);
				const float Out = 6.f, Lip = FMath::Clamp(PE.Z * 0.03f, 12.f, 35.f);
				// Cornice and plinth all round, pilasters at the corners.
				for (const int32 Ax : { 0, 1 })
				{
					const int32 Other = 1 - Ax;
					for (const float Side : { -0.5f, 0.5f })
					{
						FVector3f Half, At = PC;
						At[Other] += Side * (PE[Other] + Out);
						Half[Ax] = PE[Ax] * 0.5f + Out * 2.f;
						Half[Other] = Out;
						Half.Z = Lip * 0.5f;
						B.Box(FVector3f(At.X, At.Y, P.Max.Z - Lip * 0.5f), Half, Identity, Trim);
						Half.Z = Lip * 0.7f;
						B.Box(FVector3f(At.X, At.Y, P.Min.Z + Lip * 0.7f), FVector3f(Half.X, Half.Y, Half.Z) + FVector3f(Ax == 0 ? 0 : 2, Ax == 1 ? 0 : 2, 0), Identity, Base);
					}
				}
				for (const float SX : { -0.5f, 0.5f })
					for (const float SY : { -0.5f, 0.5f })
						B.Box(FVector3f(PC.X + SX * PE.X, PC.Y + SY * PE.Y, PC.Z), FVector3f(Out * 1.6f, Out * 1.6f, PE.Z * 0.5f), Identity, Shade(Wall, 0.85f));
				// A city roof's plant: air conditioning units, vents, a water tank, an aerial.
				if (bCity && PE.X * PE.Y > 40000.f)
				{
					const float Top = P.Max.Z;
					const int32 Units = FMath::Clamp(int32(PE.X * PE.Y / 400000.f), 1, 6);
					const FColor Plant(150, 152, 150);
					for (int32 U = 0; U < Units; ++U)
					{
						const FVector3f At(P.Min.X + PE.X * Rnd.FRandRange(0.2f, 0.8f), P.Min.Y + PE.Y * Rnd.FRandRange(0.2f, 0.8f), Top);
						B.Box(At + FVector3f(0, 0, 35), FVector3f(60, 45, 35), FQuat4f(FVector3f::UpVector, Rnd.FRandRange(0.f, PI)), Plant);
						B.Lathe(At + FVector3f(0, 0, 70), { {22, 0}, {22, 4}, {0, 4} }, { DarkMetal, DarkMetal, DarkMetal }, 12);
					}
					if (Rnd.FRand() < 0.6f)
					{
						const FVector3f T(P.Min.X + PE.X * 0.75f, P.Min.Y + PE.Y * 0.3f, Top);
						for (const float SX : { -50.f, 50.f })
							for (const float SY : { -50.f, 50.f }) B.Box(T + FVector3f(SX, SY, 90), FVector3f(5, 5, 90), Identity, DarkMetal);
						B.Lathe(T + FVector3f(0, 0, 180), { {85, 0}, {85, 180}, {60, 215}, {0, 225} }, { Shade(Wall, 0.7f), Shade(Wall, 0.8f), Shade(Wall, 0.75f), Shade(Wall, 0.75f) }, 16);
					}
					const FVector3f Aerial(P.Min.X + PE.X * 0.25f, P.Min.Y + PE.Y * 0.7f, Top);
					B.Lathe(Aerial, { {4, 0}, {2, 450}, {0, 455} }, { Metal, Metal, Metal }, 6);
				}
			}
			break;
		}
		case EPropKind::Sign:
		{
			// Each piece: a slim upright one is a post (round, metal); anything else is the sign's
			// art, kept crisp, a flat one framed.
			for (const TArray<int32>& Piece : Group.Value)
			{
				FBox3f P(ForceInit);
				for (const int32 I : Piece) for (int32 K = 0; K < 3; ++K) P += Local(Tris[I].V[K].Position);
				const FVector3f PE = P.GetSize(), PC = P.GetCenter();
				if (PE.Z > 3.f * FMath::Max(PE.X, PE.Y))
				{
					const float R = FMath::Max(PE.X, PE.Y) * 0.6f + 1.5f;
					B.Lathe(FVector3f(PC.X, PC.Y, P.Min.Z - 10.f), { {R, 0}, {R, PE.Z + 10.f}, {0, PE.Z + 10.f + R} }, { Metal, Metal, Metal }, 10);
					continue;
				}
				for (const int32 I : Piece) Keep(Tris[I]);
				const float Thin = FMath::Min3(PE.X, PE.Y, PE.Z), Big = FMath::Max3(PE.X, PE.Y, PE.Z);
				if (Thin > Big * 0.15f) continue;
				// The frame: four bars round the board's edges, in its plane.
				const int32 Axis = PE.X == Thin ? 0 : PE.Y == Thin ? 1 : 2;
				const int32 A = (Axis + 1) % 3, Bx = (Axis + 2) % 3;
				const float Wd = FMath::Clamp(0.04f * FMath::Min(PE[A], PE[Bx]), 2.f, 6.f);
				for (const int32 Edge : { A, Bx })
				{
					const int32 Other = Edge == A ? Bx : A;
					for (const float Side : { -0.5f, 0.5f })
					{
						FVector3f Centre = PC, Half;
						Centre[Other] += Side * PE[Other];
						Half[Edge] = PE[Edge] * 0.5f + Wd;
						Half[Other] = Wd;
						Half[Axis] = FMath::Max(PE[Axis] * 0.5f, 1.f) + Wd * 0.6f;
						B.Box(Centre, Half, Identity, DarkMetal);
					}
				}
			}
			break;
		}
		default:
			break;
		}
		// Built in the placement's frame: set where it stands.
		for (int32 S = 0; S < 2; ++S)
			for (FBRRVertex& V : B.Mesh.Sections[S].Vertices)
			{
				V.Position = Origin + Rot.RotateVector(V.Position);
				Out.Sections[S].Vertices.Add(V);
			}
		++OutCount;
	}
	// The built shapes smooth-shaded at their own folds; the kept art as it was drawn.
	FBRRMesh Built;
	Built.Sections = { Out.Sections[0], Out.Sections[1] };
	BRR::SmoothNormals(Built, 40.f);
	Out.Sections[0] = Built.Sections[0];
	Out.Sections[1] = Built.Sections[1];
	Out.Sections.RemoveAll([](const FBRRMeshSection& S) { return S.Vertices.IsEmpty(); });
	return Out;
}
