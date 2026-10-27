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
	if (S == TEXT("FLAG") || S == TEXT("FLAG2")) return EPropKind::Flag;
	// The landmarks made again one by one (BuildLandmark).
	if (S == TEXT("MUDHUT") || S == TEXT("CHINA1") || S == TEXT("PAGODA1") || S == TEXT("PAGODA2") || S == TEXT("BAYWATCH") || S.StartsWith(TEXT("ROCKET"))) return EPropKind::Landmark;
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

	// A builder's shapes turned about Z by Angle and set at Origin (in its frame), into another.
	void Merge(FBuilder& Into, FBuilder& From, float Angle, const FVector3f& Origin)
	{
		const FQuat4f Q(FVector3f::UpVector, Angle);
		for (int32 S = 0; S < 2; ++S)
			for (FBRRVertex V : From.Mesh.Sections[S].Vertices) { V.Position = Origin + Q.RotateVector(V.Position); Into.Mesh.Sections[S].Vertices.Add(V); }
	}

	// The landmarks, each in its own way, in their box (the placement's frame), the long way along
	// X (Along), in the original's upper (roof, nose) and lower (walls, body) colours.
	void BuildLandmark(const FString& Shape, FBuilder& B, const FBox3f& Box, const FQuat4f& Along, FColor Upper, FColor Lower)
	{
		const FVector3f E = Box.GetSize(), C = Box.GetCenter();
		const FVector3f Foot(C.X, C.Y, Box.Min.Z);
		const float Long = FMath::Max(E.X, E.Y), Short = FMath::Min(E.X, E.Y), H = E.Z;
		const FColor Wood(92, 62, 38), Dark(30, 26, 24), Metal(150, 155, 160);
		if (Shape == TEXT("MUDHUT"))
		{
			// A round hut: mud walls, a dark door, a deep thatched cone with a ragged fringe.
			const float R = Short * 0.42f;
			B.Lathe(Foot, { {R * 1.04f, -10}, {R, H * 0.48f}, {R * 0.97f, H * 0.5f} }, { Shade(Lower, 0.8f), Lower, Shade(Lower, 1.05f) }, 28);
			B.Box(Foot + Along.RotateVector(FVector3f(R * 0.98f, 0, H * 0.2f)), FVector3f(4, R * 0.22f, H * 0.2f), Along, Dark);
			const int32 Sides = 36;
			FBuilder Roof(7);
			for (int32 S = 0; S < Sides; ++S)
			{
				const int32 N = (S + 1) % Sides;
				const float A0 = 2.f * PI * S / Sides, A1 = 2.f * PI * (S + 1) / Sides;
				const float Fr0 = R * (1.28f + 0.06f * Hash(S, 0, 0, 7)), Fr1 = R * (1.28f + 0.06f * Hash(N, 0, 0, 7));
				const float Lo0 = H * (0.4f - 0.04f * Hash(S, 1, 0, 7)), Lo1 = H * (0.4f - 0.04f * Hash(N, 1, 0, 7));
				const FVector3f P0(FMath::Cos(A0) * Fr0, FMath::Sin(A0) * Fr0, Lo0), P1(FMath::Cos(A1) * Fr1, FMath::Sin(A1) * Fr1, Lo1), Apex(0, 0, H * 1.02f);
				const FColor Thatch = Shade(Upper, 0.85f + 0.3f * Hash(S, 2, 0, 7));
				Roof.Solid(P0, P1, Apex, Shade(Thatch, 0.75f), Shade(Thatch, 0.75f), Shade(Upper, 1.1f), FVector3f(0, 0, H * 0.6f));
				Roof.Solid(P0, P1, FVector3f(0, 0, H * 0.45f), Shade(Upper, 0.45f), Shade(Upper, 0.45f), Shade(Upper, 0.35f), FVector3f(0, 0, H));
			}
			Merge(B, Roof, 0.f, Foot);
		}
		else if (Shape == TEXT("CHINA1"))
		{
			// A pavilion: red posts, dark walls set in, two hipped roofs with upturned eaves.
			const float R = Short * 0.36f, Wall = H * 0.45f;
			for (const float SX : { -1.f, 1.f })
				for (const float SY : { -1.f, 1.f })
					B.Lathe(Foot + FVector3f(SX * R, SY * R, 0), { {9, 0}, {8, Wall}, {0, Wall} }, { Lower, Lower, Lower }, 10);
			B.Box(Foot + FVector3f(0, 0, Wall * 0.5f), FVector3f(R * 0.9f, R * 0.9f, Wall * 0.5f), Identity, Shade(Lower, 0.45f));
			for (int32 Tier = 0; Tier < 2; ++Tier)
			{
				FBuilder Roof(Tier + 11);
				const float Z0 = Wall + Tier * H * 0.22f, Span = R * (Tier ? 1.05f : 1.55f), Rise = H * (Tier ? 0.32f : 0.22f);
				Roof.Lathe(FVector3f::ZeroVector, { {Span * 1.04f, Z0 + 10}, {Span, Z0}, {Span * 0.55f, Z0 + Rise * 0.55f}, {Span * 0.12f, Z0 + Rise}, {0, Z0 + Rise} },
					{ Shade(Upper, 0.9f), Shade(Upper, 0.8f), Upper, Shade(Upper, 1.1f), Shade(Upper, 1.1f) }, 4);
				// The eaves' corners turned up.
				for (FBRRVertex& V : Roof.Mesh.Sections[0].Vertices)
				{
					const float Out = FVector2f(V.Position.X, V.Position.Y).Size() / Span;
					if (Out > 0.9f) V.Position.Z += (Out - 0.9f) * Span * 0.6f;
				}
				Merge(B, Roof, PI * 0.25f, Foot);
			}
			B.Lathe(Foot + FVector3f(0, 0, Wall + H * 0.54f), { {4, 0}, {1, H * 0.12f}, {0, H * 0.12f} }, { Dark, Dark, Dark }, 6);
		}
		else if (Shape == TEXT("PAGODA1"))
		{
			// A torii's legs: round red pillars on black feet, the tie beam through them.
			const float Half = Long * 0.5f - Short * 0.4f, R = FMath::Max(Short * 0.32f, 18.f);
			for (const float S : { -1.f, 1.f })
			{
				const FVector3f At = Foot + Along.RotateVector(FVector3f(S * Half, 0, 0));
				B.Lathe(At, { {R * 1.25f, 0}, {R * 1.2f, H * 0.1f}, {R, H * 0.1f}, {R * 0.88f, H}, {0, H} }, { Dark, Dark, Lower, Lower, Lower }, 20);
			}
			B.Box(Foot + FVector3f(0, 0, H * 0.82f), FVector3f(Long * 0.5f + R, R * 0.55f, R * 0.6f), Along, Shade(Lower, 0.95f));
		}
		else if (Shape == TEXT("PAGODA2"))
		{
			// A torii's top: the black-capped lintel, its ends sweeping up.
			const int32 Seg = 16;
			const float Span = Long * 0.5f + 20.f, Depth = FMath::Max(Short * 0.6f, 30.f), Th = FMath::Max(H * 0.3f, 25.f);
			auto Lift = [&](float T) { return FMath::Pow(FMath::Abs(T), 3.f) * Th * 1.6f; };
			for (int32 I = 0; I < Seg; ++I)
			{
				const float X0 = -Span + 2.f * Span * I / Seg, X1 = -Span + 2.f * Span * (I + 1) / Seg;
				const float Zm = Box.Max.Z - Th + (Lift(X0 / Span) + Lift(X1 / Span)) * 0.5f;
				const FVector3f Mid = Foot + Along.RotateVector(FVector3f((X0 + X1) * 0.5f, 0, 0));
				const float Slope = FMath::Atan2(Lift(X1 / Span) - Lift(X0 / Span), X1 - X0);
				const FQuat4f Q = Along * FQuat4f(FVector3f::RightVector, -Slope);
				const float Len = (X1 - X0) * 0.5f + 1.f;
				B.Box(FVector3f(Mid.X, Mid.Y, Zm + Th * 0.25f), FVector3f(Len, Depth * 0.55f, Th * 0.25f), Q, Dark);
				B.Box(FVector3f(Mid.X, Mid.Y, Zm - Th * 0.25f), FVector3f(Len, Depth * 0.5f, Th * 0.25f), Q, Lower);
			}
		}
		else if (Shape == TEXT("BAYWATCH"))
		{
			// A lifeguard tower: stilts, a deck with a rail, a hut, a sloping roof.
			const float Deck = H * 0.35f, HX = E.X * 0.42f, HY = E.Y * 0.42f;
			for (const float SX : { -1.f, 1.f })
				for (const float SY : { -1.f, 1.f })
				{
					B.Lathe(Foot + FVector3f(SX * HX, SY * HY, -20), { {8, 0}, {7, Deck + 20}, {0, Deck + 20} }, { Wood, Wood, Wood }, 8);
					B.Lathe(Foot + FVector3f(SX * HX, SY * HY, Deck), { {3, 0}, {3, H * 0.2f}, {0, H * 0.2f} }, { Metal, Metal, Metal }, 6);
				}
			B.Box(Foot + FVector3f(0, 0, Deck), FVector3f(HX + 10, HY + 10, 6), Identity, Wood);
			for (const int32 Ax : { 0, 1 })
				for (const float S : { -1.f, 1.f })
				{
					FVector3f At = Foot + FVector3f(0, 0, Deck + H * 0.2f);
					const FVector3f Half(Ax ? HX : 2.f, Ax ? 2.f : HY, 2.f);
					At[Ax ? 1 : 0] += S * (Ax ? HY : HX);
					B.Box(At, Half, Identity, FColor(225, 228, 230));
				}
			B.Box(Foot + FVector3f(0, 0, Deck + H * 0.22f), FVector3f(HX * 0.7f, HY * 0.7f, H * 0.22f), Identity, Lower);
			B.Box(Foot + FVector3f(0, 0, Deck + H * 0.47f), FVector3f(HX * 1.05f, HY * 1.05f, 5), FQuat4f(FVector3f::ForwardVector, 0.08f), Upper);
		}
		else // ROCKET0-2: a cartoon rocket: banded body, nose cone, four fins (MOON3's are animated decor, ABRRAnimDecor: none here yet)
		{
			const float R = Short * 0.3f;
			TArray<FVector2f> Body;
			TArray<FColor> Cols;
			const int32 Bands = 6;
			for (int32 K = 0; K <= Bands; ++K)
			{
				const float T = float(K) / Bands;
				const float Rad = R * (0.75f + 0.45f * FMath::Sin(PI * (0.15f + 0.7f * T)));
				Body.Add({ Rad, H * 0.62f * T });
				Cols.Add((K & 1) ? Shade(Lower, 0.6f) : Lower);
				if (K > 0 && K < Bands) { Body.Add({ Rad, H * 0.62f * T + 0.5f }); Cols.Add((K & 1) ? Lower : Shade(Lower, 0.6f)); }
			}
			Body.Append({ {R * 0.6f, H * 0.8f}, {R * 0.25f, H * 0.93f}, {0, H} });
			Cols.Append({ Upper, Upper, Shade(Upper, 1.1f) });
			B.Lathe(Foot, Body, Cols, 24);
			for (int32 F = 0; F < 4; ++F)
			{
				const FQuat4f Q(FVector3f::UpVector, PI * 0.5f * F + PI * 0.25f);
				B.Box(Foot + Q.RotateVector(FVector3f(R * 1.05f, 0, H * 0.14f)), FVector3f(R * 0.45f, 3.f, H * 0.14f), Q, Upper);
			}
		}
	}
	const FColor Metal(150, 155, 160), DarkMetal(42, 44, 48), Rubber(26, 26, 28);
}

TArray<BRR::FBRRFlag> BRR::FindFlags(const FBRRWorld& World)
{
	TArray<FBRRFlag> Flags;
	TArray<FTri> Tris;
	for (int32 S = 0; S < World.Decor.Sections.Num(); ++S)
		for (int32 T = 0; T + 2 < World.Decor.Sections[S].Vertices.Num(); T += 3) Tris.Add({ &World.Decor.Sections[S].Vertices[T], S, Tris.Num() });
	if (Tris.IsEmpty()) return Flags;
	const TMap<int32, TArray<TArray<int32>>> Groups = GroupDecor(World, Tris, [&](int32 P) { return PropKindOf(World.Placements[P].Shape) == EPropKind::Flag; });
	for (const TPair<int32, TArray<TArray<int32>>>& Group : Groups)
	{
		const FBRRPlacement& Place = World.Placements[Group.Key];
		const FQuat4f Rot = FQuat4f(Place.Rotation);
		const FVector3f Origin = FVector3f(Place.Location);
		FBox3f Box(ForceInit);
		TArray<int32> All;
		for (const TArray<int32>& Piece : Group.Value)
		{
			All.Append(Piece);
			for (const int32 I : Piece) for (int32 K = 0; K < 3; ++K) Box += Rot.UnrotateVector(Tris[I].V[K].Position - Origin);
		}
		const FVector3f E = Box.GetSize();
		FBRRFlag& F = Flags.AddDefaulted_GetRef();
		const bool bAlongX = E.X >= E.Y;
		const float Width = FMath::Max(bAlongX ? E.X : E.Y, 60.f);
		// The pole at the box's one end, the cloth off the other way.
		const FVector3f Pole = bAlongX ? FVector3f(Box.Min.X, Box.GetCenter().Y, Box.Max.Z) : FVector3f(Box.GetCenter().X, Box.Min.Y, Box.Max.Z);
		F.Top = Origin + Rot.RotateVector(Pole);
		F.Out = Rot.RotateVector(bAlongX ? FVector3f::ForwardVector : FVector3f::RightVector);
		F.Width = Width;
		F.Height = FMath::Clamp(E.Z * 0.35f, 40.f, Width * 0.8f);
		F.Colour = MeanColour(World, Tris, All);
	}
	return Flags;
}

void BRR::MakeSpectator(int32 Variant, FBRRMesh& OutShirt, FBRRMesh& OutRest)
{
	FBuilder Shirt(Variant * 13 + 1), Rest(Variant * 13 + 2);
	const FColor Skin(224, 172, 136), Denim(58, 78, 122), Jeans(70, 90, 140), Boots(70, 48, 30), Straw(214, 182, 110), Beard(96, 64, 40), Hair(110, 74, 42);
	const FColor CapRed(190, 30, 30), CapWhite(235, 235, 230), Can(200, 200, 205), ChairGreen(60, 130, 80), CoolerBlue(40, 90, 180);
	const bool bSitting = Variant == 3;
	const float Hip = bSitting ? 45.f : 88.f, Shoulder = Hip + 58.f;
	// The shirt: a torso and the arms, in light and dark checks the crowd's tint makes plaid.
	const int32 Rows = 6, Cols = 4;
	for (int32 R = 0; R < Rows; ++R)
		for (int32 C = 0; C < Cols; ++C)
		{
			const float Z0 = Hip + (Shoulder - Hip) * R / Rows, Z1 = Hip + (Shoulder - Hip) * (R + 1) / Rows;
			const FColor Check = ((R + C) & 1) ? FColor(250, 250, 250) : FColor(150, 150, 150);
			const float Y0 = -20.f + 40.f * C / Cols, Y1 = -20.f + 40.f * (C + 1) / Cols;
			Shirt.Box(FVector3f(0, (Y0 + Y1) * 0.5f, (Z0 + Z1) * 0.5f), FVector3f(12.f, (Y1 - Y0) * 0.5f, (Z1 - Z0) * 0.5f), FQuat4f::Identity, Check);
		}
	const bool bArmUp = Variant == 2;
	for (const float Side : { -1.f, 1.f })
	{
		const FVector3f Sh(0, Side * 24.f, Shoulder - 6.f);
		const FVector3f Hand = bArmUp && Side > 0 ? Sh + FVector3f(5, 6, 52) : bSitting ? Sh + FVector3f(28, Side * 4, -28) : Sh + FVector3f(4, Side * 6, -52);
		Shirt.Tube({ Sh, (Sh + Hand) * 0.5f, Hand }, { 6.5f, 6.f, 5.5f }, 8, FColor(200, 200, 200));
		Rest.Blob(Hand, FVector3f(5.5f, 5.5f, 6.f), Skin, 0.05f, 1, Variant);
		if (bArmUp && Side > 0) Rest.Lathe(Hand + FVector3f(0, 0, 4), { {3.3f, 0}, {3.3f, 12}, {0, 12} }, { Can, Can, Can }, 10);
	}
	// Legs and boots: overalls' denim, or jeans; bent to sit.
	for (const float Side : { -1.f, 1.f })
	{
		const FColor Leg = (Variant == 1 || Variant == 3) ? Jeans : Denim;
		if (bSitting)
		{
			Rest.Tube({ FVector3f(0, Side * 10, Hip), FVector3f(40, Side * 10, Hip) }, { 8.f, 7.5f }, 8, Leg);
			Rest.Tube({ FVector3f(40, Side * 10, Hip), FVector3f(42, Side * 10, 8) }, { 7.5f, 7.f }, 8, Leg);
			Rest.Box(FVector3f(46, Side * 10, 4), FVector3f(12, 6, 4), FQuat4f::Identity, Boots);
		}
		else
		{
			Rest.Tube({ FVector3f(0, Side * 10, Hip), FVector3f(0, Side * 10, 8) }, { 8.f, 7.f }, 8, Leg);
			Rest.Box(FVector3f(5, Side * 10, 4), FVector3f(13, 6, 4), FQuat4f::Identity, Boots);
		}
	}
	// Overalls' bib and straps over the shirt.
	if (Variant == 0 || Variant == 2)
	{
		Rest.Box(FVector3f(12.6f, 0, Hip + 18), FVector3f(1.2f, 15, 20), FQuat4f::Identity, Denim);
		for (const float Side : { -1.f, 1.f }) Rest.Box(FVector3f(12.6f, Side * 11, Shoulder - 12), FVector3f(1.2f, 2.5f, 13), FQuat4f::Identity, Denim);
		Rest.Box(FVector3f(0, 0, Hip + 2), FVector3f(12.8f, 20.5f, 6), FQuat4f::Identity, Denim);
	}
	// Head, and what is on it.
	const FVector3f Head(2, 0, Shoulder + 15);
	Rest.Tube({ FVector3f(0, 0, Shoulder - 2), FVector3f(1, 0, Shoulder + 6) }, { 6.f, 6.f }, 8, Skin);
	Rest.Blob(Head, FVector3f(11, 10, 12.5f), Skin, 0.04f, 2, Variant);
	if (Variant == 0) Rest.Blob(Head + FVector3f(7, 0, -7), FVector3f(6, 9, 8), Beard, 0.25f, 2, Variant + 5);
	if (Variant == 0)
	{
		// The straw cowboy hat: a wide brim turned up at the sides, a dented crown.
		Rest.Lathe(Head + FVector3f(0, 0, 8), { {24, 0}, {22, 2}, {10, 3}, {9.5f, 12}, {6, 14}, {0, 13} }, { Straw, Straw, Straw, Straw, Straw, Shade(Straw, 0.9f) }, 20);
		for (FBRRVertex& V : Rest.Mesh.Sections[0].Vertices)
		{
			const FVector3f D = V.Position - (Head + FVector3f(0, 0, 8));
			if (D.Z > -1 && D.Z < 3 && FVector2f(D.X, D.Y).Size() > 14.f) V.Position.Z += FMath::Square(D.Y / 24.f) * 7.f;
		}
	}
	else if (Variant == 2)
	{
		Rest.Blob(Head + FVector3f(-5, 0, 3), FVector3f(9, 10.5f, 10), Hair, 0.2f, 2, Variant + 7);       // the top
		Rest.Blob(Head + FVector3f(-11, 0, -10), FVector3f(5, 9, 11), Hair, 0.3f, 2, Variant + 9);       // the mullet
	}
	else
	{
		// The trucker cap: a white front, a red crown, the bill.
		Rest.Blob(Head + FVector3f(-1, 0, 6), FVector3f(11.5f, 11, 7), CapRed, 0.f, 2, Variant, 0.2f);
		Rest.Box(Head + FVector3f(6, 0, 7), FVector3f(4, 9, 5), FQuat4f::Identity, CapWhite);
		Rest.Box(Head + FVector3f(15, 0, 3), FVector3f(7, 8.5f, 0.8f), FQuat4f(FVector3f::RightVector, -0.12f), CapRed);
	}
	if (bSitting)
	{
		// The lawn chair and the cooler beside it.
		Rest.Box(FVector3f(8, 0, Hip - 4), FVector3f(26, 24, 2), FQuat4f::Identity, ChairGreen);
		Rest.Box(FVector3f(-14, 0, Hip + 28), FVector3f(2, 24, 30), FQuat4f(FVector3f::RightVector, 0.15f), ChairGreen);
		for (const float SX : { -12.f, 28.f })
			for (const float SY : { -22.f, 22.f })
				Rest.Tube({ FVector3f(SX, SY, 0), FVector3f(SX, SY, Hip - 4) }, { 1.5f, 1.5f }, 5, FColor(190, 190, 195));
		Rest.Box(FVector3f(10, 48, 20), FVector3f(20, 14, 20), FQuat4f::Identity, CoolerBlue);
		Rest.Box(FVector3f(10, 48, 41), FVector3f(21, 15, 2), FQuat4f::Identity, CapWhite);
	}
	OutShirt = MoveTemp(Shirt.Mesh);
	OutRest = MoveTemp(Rest.Mesh);
	for (FBRRMesh* M : { &OutShirt, &OutRest })
	{
		M->Sections.RemoveAll([](const FBRRMeshSection& S) { return S.Vertices.IsEmpty(); });
		BRR::SmoothNormals(*M, 50.f);
	}
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
		case EPropKind::Landmark:
		{
			// Its colours: the upper part's (roof, nose) and the lower's (walls, body).
			TArray<int32> Up, Low;
			for (const int32 I : All)
			{
				const float Z = Local((Tris[I].V[0].Position + Tris[I].V[1].Position + Tris[I].V[2].Position) / 3.f).Z;
				(Z > Box.Min.Z + E.Z * 0.5f ? Up : Low).Add(I);
			}
			BuildLandmark(Place.Shape.ToUpper(), B, Box, Along, Up.IsEmpty() ? Colour : MeanColour(World, Tris, Up), Low.IsEmpty() ? Colour : MeanColour(World, Tris, Low));
			break;
		}
		case EPropKind::Flag:
		{
			// The pole; its cloth is the race's (FindFlags, ABRRCrowd), which waves it.
			const bool bX = E.X >= E.Y;
			const FVector3f Foot = bX ? FVector3f(Box.Min.X, C.Y, Box.Min.Z) : FVector3f(C.X, Box.Min.Y, Box.Min.Z);
			B.Lathe(Foot - FVector3f(0, 0, 20), { {4, 0}, {3, E.Z + 20}, {5, E.Z + 22}, {0, E.Z + 28} }, { Metal, Metal, FColor(210, 180, 60), FColor(210, 180, 60) }, 8);
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
