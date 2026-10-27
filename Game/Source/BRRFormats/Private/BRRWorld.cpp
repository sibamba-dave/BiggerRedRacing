#include "BRRWorld.h"
#include "BRRMeshConvert.h"
#include "BRRRefine.h"

#include "BRRConvert.h"

#include <exception>
#include <string>

#include "formats/game_files.h"
#include "formats/level_script.h"
#include "formats/scenery.h"
#include "formats/texture.h"
#include "formats/vehicle_script.h"
#include "scene/level_scene.h"
#include "scene/dashboard.h"
#include "scene/particle_render.h"
#include "scene/vehicle_render.h"
#include "sim/race.h"
#include "sim/replay.h"
#include "sim/vehicle.h"

namespace
{
	using bigred::TerrainVertex;
	using namespace BRR::Private;

	FBRRImage ImageFromRgba(const std::vector<uint8_t>& Rgba, int32 Width, int32 Height)
	{
		FBRRImage Image;
		Image.Width = Width;
		Image.Height = Height;
		Image.Pixels.SetNumUninitialized(Width * Height);
		for (int32 I = 0; I < Width * Height; ++I)
		{
			Image.Pixels[I] = FColor(Rgba[I * 4], Rgba[I * 4 + 1], Rgba[I * 4 + 2], Rgba[I * 4 + 3]);
		}
		return Image;
	}

	// ORR "local render space" (+Y up, true world units) -> UE local cm.
	FVector LocalRenderToUE(float X, float Y, float Z) { return BRR::GameToUE(X, -Y, Z); }

	FBRRMesh ConvertLocal(const std::vector<TerrainVertex>& Verts)
	{
		return ConvertTriangles(Verts, 0, Verts.size(), &LocalRenderToUE);
	}
}

int32 FBRRMesh::NumTriangles() const
{
	int32 Count = 0;
	for (const FBRRMeshSection& S : Sections) Count += S.Vertices.Num() / 3;
	return Count;
}

FBox3f FBRRMesh::GetBounds() const
{
	FBox3f Box(ForceInit);
	for (const FBRRMeshSection& S : Sections)
		for (const FBRRVertex& V : S.Vertices) Box += V.Position;
	return Box;
}

EBRRLiquid FBRRWorld::LiquidOf(uint8 Land) const
{
	constexpr uint8 Track = 0, Slippy = 2;
	if (LevelName.StartsWith(TEXT("WATER"), ESearchCase::IgnoreCase)) return Land == Track ? EBRRLiquid::Water : EBRRLiquid::None;
	if (Land != Slippy) return EBRRLiquid::None;
	if (LevelName.StartsWith(TEXT("SNOW"), ESearchCase::IgnoreCase)) return EBRRLiquid::Ice;
	if (LevelName.StartsWith(TEXT("MOON"), ESearchCase::IgnoreCase)) return EBRRLiquid::Lava;
	return EBRRLiquid::Water;
}

EBRRGround FBRRWorld::GroundOf(uint8 Land, FColor Average) const
{
	constexpr uint8 Track = 0, Grippy = 1, Cliffs = 3, Grass = 4, Sand = 5, Ramps = 6, Air = 7;
	if (Land > 7 || Land == Air || LiquidOf(Land) != EBRRLiquid::None) return EBRRGround::None;
	auto Is = [this](const TCHAR* Theme) { return LevelName.StartsWith(Theme, ESearchCase::IgnoreCase); };
	// The palette art is stylised (SNOW's snow is lilac, the moon red and tan): the theme first.
	if (Is(TEXT("MOON"))) return EBRRGround::Regolith;
	if (Is(TEXT("SNOW"))) return Land == Cliffs ? EBRRGround::Rock : EBRRGround::Snow;
	if (Land == Sand) return EBRRGround::Sand;
	// Then the colour as painted (hue in degrees, saturation, value; sRGB, not linearised): green
	// is grass whatever the type says (whole hillsides are CLIFFS), grey is made ground or rock,
	// the rest earth.
	const FLinearColor Hsv = FLinearColor(Average.R / 255.f, Average.G / 255.f, Average.B / 255.f).LinearRGBToHSV();
	const float Hue = Hsv.R, Sat = Hsv.G, Value = Hsv.B;
	if (Hue >= 65.f && Hue <= 170.f && Sat > 0.2f) return EBRRGround::Grass;
	const bool bRoad = Land == Track || Land == Grippy || Land == Ramps;
	if (bRoad && Is(TEXT("CITY")) && Sat < 0.35f) return EBRRGround::Asphalt; // the streets, kerbs and markings among them
	if (Sat < 0.2f) return bRoad ? EBRRGround::Asphalt : Land == Cliffs ? EBRRGround::Rock : EBRRGround::Dirt;
	if (bRoad && Hue >= 20.f && Hue < 55.f && Value > 0.65f) return EBRRGround::Sand;
	return EBRRGround::Dirt;
}

FColor FBRRWorld::AverageTexels(const FBRRImage& Page, FVector2f A, FVector2f B, FVector2f C)
{
	if (Page.Width <= 0 || Page.Height <= 0) return FColor::Black;
	const FVector2f Min(FMath::Min3(A.X, B.X, C.X), FMath::Min3(A.Y, B.Y, C.Y));
	const FVector2f Max(FMath::Max3(A.X, B.X, C.X), FMath::Max3(A.Y, B.Y, C.Y));
	const int32 X0 = FMath::Clamp(FMath::FloorToInt32(Min.X * Page.Width), 0, Page.Width - 1);
	const int32 X1 = FMath::Clamp(FMath::CeilToInt32(Max.X * Page.Width) - 1, X0, Page.Width - 1);
	const int32 Y0 = FMath::Clamp(FMath::FloorToInt32(Min.Y * Page.Height), 0, Page.Height - 1);
	const int32 Y1 = FMath::Clamp(FMath::CeilToInt32(Max.Y * Page.Height) - 1, Y0, Page.Height - 1);
	const int32 Step = FMath::Max(1, FMath::Max(X1 - X0, Y1 - Y0) / 32);
	uint64 Sum[3] = {}, N = 0;
	for (int32 Y = Y0; Y <= Y1; Y += Step)
	{
		for (int32 X = X0; X <= X1; X += Step)
		{
			const FColor& P = Page.Pixels[Y * Page.Width + X];
			if (P.A == 0) continue;
			Sum[0] += P.R; Sum[1] += P.G; Sum[2] += P.B; ++N;
		}
	}
	return N ? FColor(uint8(Sum[0] / N), uint8(Sum[1] / N), uint8(Sum[2] / N)) : FColor::Black;
}

double FBRRWorld::GroundHeightAt(double UEX, double UEY) const
{
	if (MapWidth <= 0 || MapHeight <= 0 || HeightMap.Num() != MapWidth * MapHeight) return 0.0;
	const double Cell = BRR::TerrainCellUnits * BRR::CmPerUnit;
	const double GX = FMath::Clamp(UEY / Cell, 0.0, double(MapWidth - 1));
	const double GZ = FMath::Clamp(UEX / Cell, 0.0, double(MapHeight - 1));
	const int32 X0 = FMath::FloorToInt32(GX), Z0 = FMath::FloorToInt32(GZ);
	const int32 X1 = FMath::Min(X0 + 1, MapWidth - 1), Z1 = FMath::Min(Z0 + 1, MapHeight - 1);
	auto H = [&](int32 X, int32 Z) { return double(HeightMap[Z * MapWidth + X]); };
	const double FX = GX - X0, FZ = GZ - Z0;
	const double Height = FMath::Lerp(FMath::Lerp(H(X0, Z0), H(X1, Z0), FX), FMath::Lerp(H(X0, Z1), H(X1, Z1), FX), FZ);
	return Height * bigred::kHeightWorldScale * BRR::CmPerUnit;
}

double FBRRWorld::RefinedOffsetAt(double UEX, double UEY) const
{
	const int32 Rows = MapHeight, Cols = MapWidth;
	if (Rows < 2 || Cols < 2 || CornerZ.Num() != Rows * Cols) return 0.0;
	const double Cell = BRR::TerrainCellUnits * BRR::CmPerUnit;
	const double GR = FMath::Clamp(UEX / Cell, 0.0, double(Rows - 1)), GC = FMath::Clamp(UEY / Cell, 0.0, double(Cols - 1));
	const int32 R0 = FMath::Min(FMath::FloorToInt32(GR), Rows - 2), C0 = FMath::Min(FMath::FloorToInt32(GC), Cols - 2);
	const double FR = GR - R0, FC = GC - C0;
	auto At = [&](const TArray<float>& A, int32 R, int32 C) { return double(A[FMath::Clamp(R, 0, Rows - 1) * Cols + FMath::Clamp(C, 0, Cols - 1)]); };
	const double Weight = FMath::Lerp(FMath::Lerp(At(CornerWeight, R0, C0), At(CornerWeight, R0, C0 + 1), FC),
		FMath::Lerp(At(CornerWeight, R0 + 1, C0), At(CornerWeight, R0 + 1, C0 + 1), FC), FR);
	if (Weight <= 0.0) return 0.0;
	auto CatmullRom = [](double P0, double P1, double P2, double P3, double T)
	{
		return 0.5 * (2 * P1 + (P2 - P0) * T + (2 * P0 - 5 * P1 + 4 * P2 - P3) * T * T + (3 * P1 - P0 - 3 * P2 + P3) * T * T * T);
	};
	double Rowsums[4];
	for (int32 K = 0; K < 4; ++K)
	{
		const int32 R = R0 - 1 + K;
		Rowsums[K] = CatmullRom(At(CornerZ, R, C0 - 1), At(CornerZ, R, C0), At(CornerZ, R, C0 + 1), At(CornerZ, R, C0 + 2), FC);
	}
	const double Smooth = CatmullRom(Rowsums[0], Rowsums[1], Rowsums[2], Rowsums[3], FR);
	const double Flat = FMath::Lerp(FMath::Lerp(At(CornerZ, R0, C0), At(CornerZ, R0, C0 + 1), FC),
		FMath::Lerp(At(CornerZ, R0 + 1, C0), At(CornerZ, R0 + 1, C0 + 1), FC), FR);
	return Weight * FMath::Clamp(Smooth - Flat, -RefineClamp, RefineClamp);
}

double FBRRWorld::ShoreSinkAt(double UEX, double UEY) const
{
	const int32 Rows = MapHeight, Cols = MapWidth;
	if (Rows < 2 || Cols < 2 || CornerSink.Num() != Rows * Cols) return 0.0;
	const double Cell = BRR::TerrainCellUnits * BRR::CmPerUnit;
	const double GR = FMath::Clamp(UEX / Cell, 0.0, double(Rows - 1)), GC = FMath::Clamp(UEY / Cell, 0.0, double(Cols - 1));
	const int32 R0 = FMath::Min(FMath::FloorToInt32(GR), Rows - 2), C0 = FMath::Min(FMath::FloorToInt32(GC), Cols - 2);
	const double FR = GR - R0, FC = GC - C0;
	auto At = [&](int32 R, int32 C) { return double(CornerSink[FMath::Clamp(R, 0, Rows - 1) * Cols + FMath::Clamp(C, 0, Cols - 1)]); };
	const double Most = FMath::Max(FMath::Max(At(R0, C0), At(R0, C0 + 1)), FMath::Max(At(R0 + 1, C0), At(R0 + 1, C0 + 1)));
	if (Most <= 0.0) return 0.0;
	auto CatmullRom = [](double P0, double P1, double P2, double P3, double T)
	{
		return 0.5 * (2 * P1 + (P2 - P0) * T + (2 * P0 - 5 * P1 + 4 * P2 - P3) * T * T + (3 * P1 - P0 - 3 * P2 + P3) * T * T * T);
	};
	double Rowsums[4];
	for (int32 K = 0; K < 4; ++K)
	{
		const int32 R = R0 - 1 + K;
		Rowsums[K] = CatmullRom(At(R, C0 - 1), At(R, C0), At(R, C0 + 1), At(R, C0 + 2), FC);
	}
	return FMath::Clamp(CatmullRom(Rowsums[0], Rowsums[1], Rowsums[2], Rowsums[3], FR), 0.0, Most);
}

FBRRMesh FBRRWorld::RefinedTerrain(int32 Divisions) const
{
	FBRRMesh Out = BRR::Subdivide(Terrain, Divisions);
	for (FBRRMeshSection& Section : Out.Sections)
	{
		for (FBRRVertex& V : Section.Vertices) V.Position.Z += float(RefinedOffsetAt(V.Position.X, V.Position.Y) - ShoreSinkAt(V.Position.X, V.Position.Y));
	}
	BRR::SmoothNormals(Out, 50.f);
	return Out;
}

bool FBRRWorld::Load(const FString& DataDir, const FString& Level, const TArray<FString>& VehicleNames,
	int32 Colour, FBRRWorld& Out, FString& OutError)
{
	Out = FBRRWorld();
	Out.LevelName = Level;
	try
	{
		const std::string GameDir = ToUtf8(DataDir);
		const std::string Name = ToUtf8(Level.ToUpper());

		bigred::LevelScene Scene;
		Scene.load_shade_table(GameDir);
		if (!Scene.load(GameDir, Name))
		{
			OutError = FString::Printf(TEXT("could not load %s.LEV from %s"), *Level, *DataDir);
			return false;
		}

		for (const bigred::RGB& C : Scene.palette) Out.Palette.Add(FColor(C.r, C.g, C.b, 255));
		if (Out.Palette.Num() == 256)
		{
			for (int32 Band = 0; Band < 16; ++Band) Out.SkyStrip.Add(Out.Palette[239 - Band]);
			Out.SkyStrip.Add(Out.Palette[223]);
			Out.SkyStrip.Add(Out.Palette[222]);
		}

		Out.MapWidth = Scene.map_w;
		Out.MapHeight = Scene.map_h;
		Out.HeightMap = TArray<uint8>(Scene.mph_data, Scene.map_w * Scene.map_h);

		std::string Script;
		if (const bigred::Blob* Txt = bigred::find_member(Scene.lev, Name + ".TXT"))
		{
			Script.assign(Txt->begin(), Txt->end());
			Out.ScriptText = FromUtf8(Script);
		}
		{
			// ORR sun_direction, in render space (+Y up), mapped to UE as every direction is.
			const bigred::SunLight Sun = bigred::parse_light(Script);
			const double A = Sun.elevation * 2.0 * PI / 65536.0, B = Sun.azimuth * 2.0 * PI / 65536.0;
			const FVector Render(-FMath::Cos(A) * FMath::Sin(B), -FMath::Sin(A), -FMath::Cos(A) * FMath::Cos(B));
			Out.SunDirection = FVector(Render.Z, -Render.X, Render.Y).GetSafeNormal();
		}
		if (const bigred::Blob* Raw = bigred::find_member(Scene.lev, "VIEW.RAW"))
		{
			if (Raw->size() == bigred::kRawSize)
				Out.View = ImageFromRgba(bigred::decode_raw_rgba(Raw->data(), Raw->size()), bigred::kRawWidth, bigred::kRawHeight);
		}

		// World geometry, exactly as ORR draws it: terrain then every static decor instance.
		size_t TerrainVerts = 0;
		const std::vector<TerrainVertex> World = Scene.build_world_mesh(Scene.height_scale, &TerrainVerts);
		const double MirrorAxis = double(Scene.map_w - 1) * bigred::kTerrainTexelWorldSize;
		const double InvScale = 1.0 / double(bigred::kRenderScale);
		auto WorldRenderToUE = [&](float X, float Y, float Z)
		{
			return BRR::GameToUE(MirrorAxis - X * InvScale, -Y * InvScale, Z * InvScale);
		};
		Out.Terrain = ConvertTriangles(World, 0, TerrainVerts, WorldRenderToUE);
		Out.Decor = ConvertTriangles(World, TerrainVerts, World.size() - TerrainVerts, WorldRenderToUE);
		{
			// Each terrain triangle's surface type, as the physics reads it at its centre.
			bigred::SimTerrain Ter;
			Ter.mph = Scene.mph_data;
			Ter.mpc = Scene.mpc_data;
			Ter.cht = Scene.cht_size >= 512 ? Scene.cht_data : nullptr;
			Ter.w = Scene.map_w;
			Ter.h = Scene.map_h;
			const std::vector<uint8_t> Land = bigred::parse_land_table(Script);
			for (size_t I = 0; I < 16 && I < Land.size(); ++I) Ter.land[I] = Land[I];
			const double ToFixed = 512.0 / BRR::CmPerUnit;
			for (FBRRMeshSection& Section : Out.Terrain.Sections)
			{
				for (int32 T = 0; T + 2 < Section.Vertices.Num(); T += 3)
				{
					const FVector3f C = (Section.Vertices[T].Position + Section.Vertices[T + 1].Position + Section.Vertices[T + 2].Position) / 3.f;
					const uint8 Type = uint8(Ter.land_type(int32(C.Y * ToFixed), int32(C.X * ToFixed)));
					for (int32 K = 0; K < 3; ++K) Section.Vertices[T + K].Land = Type;
				}
			}

			// Decor laid on the ground - the starting grid's slab, start lines, pads - is conformed
			// to the heightmap, so it is exactly coplanar with the terrain under it. The original
			// draws back to front with no depth buffer and the later face simply wins; with one, the
			// two z-fight, and the remastered look's jittered upscaler turns that into flicker. So
			// lift each such triangle clear: above the terrain wherever it is sampled (the cell's
			// own triangulation can bulge over a slab's long edge) by GroundDecalClearance. ORR
			// separates the decals stacked on the slab itself (kDecalLayerLift, 2 units apart).
			auto GroundZ = [&](const FVector3f& P)
			{
				return -double(Ter.height_fx(int32(P.Y * ToFixed), int32(P.X * ToFixed))) / 512.0 * BRR::CmPerUnit;
			};
			constexpr double OnGround = 1.0, StackedLayers = 3 * 2 * BRR::CmPerUnit, GroundDecalClearance = 2.0; // cm
			for (FBRRMeshSection& Section : Out.Decor.Sections)
			{
				for (int32 T = 0; T + 2 < Section.Vertices.Num(); T += 3)
				{
					FBRRVertex* V = &Section.Vertices[T];
					const FVector3f A = V[0].Position, B = V[1].Position, C = V[2].Position;
					// On the ground, or one of ORR's stacked layers just over it: all three corners.
					auto Laid = [&](const FVector3f& P) { const double H = P.Z - GroundZ(P); return H > -OnGround && H < StackedLayers; };
					if (!Laid(A) || !Laid(B) || !Laid(C)) continue;
					double Above = 0;
					for (int32 S = 0; S <= 4; ++S)
					{
						for (int32 U = 0; U + S <= 4; ++U)
						{
							const FVector3f P = (A * float(4 - S - U) + B * float(S) + C * float(U)) / 4.f;
							Above = FMath::Max(Above, GroundZ(P) - P.Z);
						}
					}
					const float Lift = float(Above + GroundDecalClearance);
					for (int32 K = 0; K < 3; ++K) V[K].Position.Z += Lift;
					++Out.GroundDecalTriangles;
				}
			}

			// The ground let down for the water (ShoreSinkAt): a corner is wet when every cell around
			// it holds open water, its depth its distance, in corners, from the nearest dry one; a dry
			// corner beside the water is the shore. (The track's corners are let off below.)
			{
				const double Cell = BRR::TerrainCellUnits * BRR::CmPerUnit;
				const int32 Rows = Out.MapHeight, Cols = Out.MapWidth;
				TBitArray<> WetCell(false, Rows * Cols);
				for (const FBRRMeshSection& Section : Out.Terrain.Sections)
				{
					for (int32 T = 0; T + 2 < Section.Vertices.Num(); T += 3)
					{
						if (Out.LiquidOf(Section.Vertices[T].Land) != EBRRLiquid::Water) continue;
						const FVector3f Mid = (Section.Vertices[T].Position + Section.Vertices[T + 1].Position + Section.Vertices[T + 2].Position) / 3.f;
						const int32 R = FMath::FloorToInt32(Mid.X / Cell), Col = FMath::FloorToInt32(Mid.Y / Cell);
						if (R >= 0 && R < Rows && Col >= 0 && Col < Cols) WetCell[R * Cols + Col] = true;
					}
				}
				TArray<int32> Steps;
				Steps.Init(-1, Rows * Cols);
				TArray<int32> Queue;
				for (int32 R = 0; R < Rows; ++R)
				{
					for (int32 Col = 0; Col < Cols; ++Col)
					{
						bool bWet = true;
						for (int32 DR = -1; DR <= 0 && bWet; ++DR)
							for (int32 DC = -1; DC <= 0 && bWet; ++DC)
							{
								const int32 CR = R + DR, CC = Col + DC;
								if (CR >= 0 && CC >= 0 && CR < Rows && CC < Cols && !WetCell[CR * Cols + CC]) bWet = false;
							}
						if (!bWet)
						{
							Steps[R * Cols + Col] = 0;
							Queue.Add(R * Cols + Col);
						}
					}
				}
				for (int32 Head = 0; Head < Queue.Num(); ++Head)
				{
					const int32 R = Queue[Head] / Cols, Col = Queue[Head] % Cols;
					for (const FIntPoint D : { FIntPoint(1, 0), FIntPoint(-1, 0), FIntPoint(0, 1), FIntPoint(0, -1) })
					{
						const int32 NR = R + D.X, NC = Col + D.Y;
						if (NR < 0 || NC < 0 || NR >= Rows || NC >= Cols || Steps[NR * Cols + NC] >= 0) continue;
						Steps[NR * Cols + NC] = Steps[Queue[Head]] + 1;
						Queue.Add(NR * Cols + NC);
					}
				}
				Out.CornerSink.SetNumUninitialized(Rows * Cols);
				for (int32 I = 0; I < Rows * Cols; ++I)
				{
					const int32 R = I / Cols, Col = I % Cols;
					bool bShore = false;
					for (int32 DR = -1; DR <= 0; ++DR)
						for (int32 DC = -1; DC <= 0; ++DC)
						{
							const int32 CR = R + DR, CC = Col + DC;
							if (CR >= 0 && CC >= 0 && CR < Rows && CC < Cols && WetCell[CR * Cols + CC]) bShore = true;
						}
					Out.CornerSink[I] = Steps[I] > 0 ? float(FMath::Min(Steps[I] * WaterDepthStep, WaterDepthMax)) : bShore ? float(ShoreSink) : 0.f;
				}
			}

			// The refinement's corners (RefinedOffsetAt): corner (Row, Col) at UE (Row, Col) cells.
			{
				const double Cell = BRR::TerrainCellUnits * BRR::CmPerUnit;
				const int32 Rows = Out.MapHeight, Cols = Out.MapWidth;
				Out.CornerZ.SetNumUninitialized(Rows * Cols);
				Out.CornerWeight.Init(1.f, Rows * Cols);
				for (int32 R = 0; R < Rows; ++R)
					for (int32 Col = 0; Col < Cols; ++Col)
						Out.CornerZ[R * Cols + Col] = float(GroundZ(FVector3f(float(R * Cell), float(Col * Cell), 0.f)));
				constexpr uint8 Track = 0, Grippy = 1, Ramps = 6;
				for (const FBRRMeshSection& Section : Out.Terrain.Sections)
				{
					for (int32 T = 0; T + 2 < Section.Vertices.Num(); T += 3)
					{
						const uint8 Type = Section.Vertices[T].Land;
						// Open water is drawn apart (BRR::SplitWater) over ground that is only seen
						// through it: its corners take the refinement and are let down instead.
						if (Out.LiquidOf(Type) == EBRRLiquid::Water) continue;
						const bool bFixed = Type == Ramps || Out.LiquidOf(Type) != EBRRLiquid::None;
						const bool bTrack = Type == Track || Type == Grippy;
						if (!bFixed && !bTrack) continue;
						const FVector3f Mid = (Section.Vertices[T].Position + Section.Vertices[T + 1].Position + Section.Vertices[T + 2].Position) / 3.f;
						const int32 R0 = FMath::FloorToInt32(Mid.X / Cell), C0 = FMath::FloorToInt32(Mid.Y / Cell);
						for (int32 R = R0; R <= R0 + 1; ++R)
							for (int32 Col = C0; Col <= C0 + 1; ++Col)
								if (R >= 0 && R < Rows && Col >= 0 && Col < Cols)
								{
									float& W = Out.CornerWeight[R * Cols + Col];
									W = 0.f;
									// The dry track, the ramps and the other liquids keep their height.
									if (Out.CornerSink.Num() == Rows * Cols) Out.CornerSink[R * Cols + Col] = 0.f;
								}
					}
				}
			}
		}
		if (!Scene.start_lights.empty())
		{
			for (const std::pair<int, int>& Range : Scene.lights_ranges)
				Out.StartLights.Add(ConvertTriangles(Scene.lights_buf, size_t(Range.first), size_t(Range.second), WorldRenderToUE));
		}
		for (const bigred::LevelAnim& Anim : Scene.anims) Out.AnimStepSeconds.Add(Anim.step_ms() / 1000.0);
		for (const std::vector<std::vector<TerrainVertex>>& Slot : Scene.anim_step_bufs)
		{
			TArray<FBRRMesh>& Steps = Out.AnimSteps.AddDefaulted_GetRef();
			for (const std::vector<TerrainVertex>& Step : Slot) Steps.Add(ConvertTriangles(Step, 0, Step.size(), WorldRenderToUE));
		}
		const double RenderToCm = InvScale * BRR::CmPerUnit;
		for (const bigred::SpritePlacement& S : Scene.sprites)
		{
			if (S.frames.empty()) continue;
			FBRRSprite& Sprite = Out.Sprites.AddDefaulted_GetRef();
			Sprite.Centre = WorldRenderToUE(S.x, S.y, S.z);
			Sprite.HalfWidth = float(S.half_w * RenderToCm);
			Sprite.HalfHeight = float(S.half_h * RenderToCm);
			Sprite.AnimSlot = S.anim_slot;
			if (S.wreck >= 0 && size_t(S.wreck) < Scene.wrecks.size()) Sprite.WreckInstance = Scene.wrecks[S.wreck].instance;
			for (const bigred::SpritePlacement::Frame& F : S.frames)
			{
				FBRRSprite::FFrame& Frame = Sprite.Frames.AddDefaulted_GetRef();
				Frame.bOn = F.on;
				Frame.Atlas = int32(F.layer >= 0 ? F.layer : S.layer);
				Frame.UV0 = FVector2f(F.u0, F.v0);
				Frame.UV1 = FVector2f(F.u1, F.v1);
			}
		}
		static_assert(bigred::LevelScene::kWreckStepCount == BRR::WreckStepCount, "flipbook length");
		for (const bigred::LevelScene::WreckPiece& Piece : Scene.wrecks)
		{
			FBRRWreck& W = Out.Wrecks.AddDefaulted_GetRef();
			W.Instance = Piece.instance;
			W.Intact = ConvertTriangles(Scene.wreck_buf, size_t(Piece.intact.first), size_t(Piece.intact.second), WorldRenderToUE);
			TMap<TPair<int32, int32>, int32> MeshOf;
			for (const std::pair<int, int>& Range : Piece.steps)
			{
				int32 Index = -1;
				if (Range != Piece.intact)
				{
					const TPair<int32, int32> Key(Range.first, Range.second);
					if (const int32* Found = MeshOf.Find(Key)) Index = *Found;
					else
					{
						Index = W.Steps.Add(ConvertTriangles(Scene.wreck_buf, size_t(Range.first), size_t(Range.second), WorldRenderToUE));
						MeshOf.Add(Key, Index);
					}
				}
				W.StepMesh.Add(Index);
			}
		}

		for (const bigred::SceneryInstance& Inst : Scene.instances)
		{
			FBRRPlacement& P = Out.Placements.AddDefaulted_GetRef();
			if (Inst.shape_index >= 0 && size_t(Inst.shape_index) < Scene.shape_names_by_slot.size())
				P.Shape = FromUtf8(Scene.shape_names_by_slot[Inst.shape_index]);
			P.Location = BRR::GameToUE(Inst.x, Inst.y, Inst.z);
			float M[9];
			Inst.rotation_matrix(M);
			P.Rotation = GameRotationToUE(M);
			P.Variant = Inst.variant_index;
			P.Category = Inst.collision_category_byte;
			P.Param = Inst.aux_byte;
		}

		if (const bigred::Blob* Cse = bigred::find_member(Scene.lev, "MAP.CSE"))
		{
			std::vector<bigred::Gate> Gates = bigred::parse_gates(Cse->data(), Cse->size());
			bigred::mark_unsafe_gates(Gates, bigred::parse_unsafe_gates(Script));
			for (const bigred::Gate& G : Gates)
			{
				FBRRGate& Gate = Out.Gates.AddDefaulted_GetRef();
				Gate.Anchor = FVector2D(G.z, G.x) * BRR::CmPerUnit;
				Gate.AngleRaw = G.angle;
				Gate.Length = G.length * BRR::CmPerUnit;
				Gate.HalfDepth = G.half_depth * BRR::CmPerUnit;
				Gate.HalfDepthDelta = G.half_depth_delta * BRR::CmPerUnit;
				Gate.bSafe = G.safe;
			}
		}

		const bigred::StartingGrid Grid = bigred::parse_grid(Script);
		if (Grid.present)
		{
			for (int32 Slot = 0; Slot < 6; ++Slot)
			{
				const bigred::RespawnPoint R = bigred::grid_slot(Grid, 6, Slot);
				FBRRStart& Start = Out.Grid.AddDefaulted_GetRef();
				Start.Location = BRR::GameToUE(R.x, 0, R.z);
				Start.Location.Z = Out.GroundHeightAt(Start.Location.X, Start.Location.Y);
				Start.Yaw = BRR::Bam16ToDegrees(R.yaw_bam16);
			}
		}
		for (const std::string& Car : bigred::parse_car_list(Script))
		{
			(bigred::is_presentation_vehicle(Car) ? Out.PresentationVehicles : Out.CarList).Add(FromUtf8(Car));
		}
		Out.Laps = bigred::parse_laps(Script);

		for (int32 Slot = 0; Slot < 10; ++Slot)
		{
			const bigred::Blob* Rec = bigred::find_member(Scene.lev, std::to_string(Slot) + ".REC");
			if (!Rec) continue;
			const bigred::ReplayTrack Track = bigred::parse_replay(*Rec, Slot);
			if (!Track.valid()) continue;
			FBRRGhostTrack& Ghost = Out.Ghosts.AddDefaulted_GetRef();
			Ghost.Slot = Slot;
			Ghost.Type = Track.type;
			Ghost.Character = Track.character;
			Ghost.VehicleIndex = Track.vehicle_index;
			Ghost.Logo = Track.logo;
			Ghost.Colour = Track.colour;
			Ghost.Records.Reserve(int32(Track.records.size()));
			for (const std::array<int16_t, 7>& R : Track.records)
			{
				TStaticArray<int16, 7>& Record = Ghost.Records.AddDefaulted_GetRef();
				for (int32 K = 0; K < 7; ++K) Record[K] = R[K];
			}
		}
		const bigred::ReplayRates Rates = bigred::parse_replay_rates(Script);
		Out.GhostRateMin = Rates.min;
		Out.GhostRateNormal = Rates.normal;
		Out.GhostRateMax = Rates.max;

		// Vehicles append their sheets to the scene's atlases, so they are built before the pages are read.
		const bigred::ArchivePtr Ingame = bigred::game_archive_or_empty(GameDir, "INGAME.WAP");
		for (const FString& VehicleName : VehicleNames)
		{
			bigred::VehicleTuning Tuning;
			bigred::ArchivePtr Vhc;
			if (!bigred::load_vehicle(GameDir, ToUtf8(VehicleName.ToUpper()), Tuning, &Vhc) || !Vhc)
			{
				OutError = FString::Printf(TEXT("could not load %s.VHC"), *VehicleName);
				return false;
			}
			const bigred::VehicleModel Model = bigred::build_vehicle_model(*Vhc, *Ingame, Tuning, Colour, Scene);
			FBRRVehicleMesh& Vehicle = Out.Vehicles.AddDefaulted_GetRef();
			Vehicle.Name = VehicleName.ToUpper();
			Vehicle.Colour = Colour;
			Vehicle.Body = ConvertLocal(Model.body);
			auto AddParts = [&](const std::vector<bigred::VehiclePart>& Parts, TArray<FBRRWheel>& OutParts)
			{
			for (const bigred::VehiclePart& Part : Parts)
			{
				// The part's own orientation (a quarter-turn yaw puts every wheel's axle, the mesh's
				// local Z, across the car) is baked in, as place_vehicle applies it before the body's.
				float R[9];
				bigred::vehicle_rotation(float(Part.rot_bam16[0] * BamToRad), float(Part.rot_bam16[1] * BamToRad),
					float(Part.rot_bam16[2] * BamToRad), R);
				auto PartToUE = [&R](float X, float Y, float Z)
				{
					const double GY = -Y; // local render space -> game local
					return BRR::GameToUE(R[0] * X + R[1] * GY + R[2] * Z, R[3] * X + R[4] * GY + R[5] * Z,
						R[6] * X + R[7] * GY + R[8] * Z);
				};
				FBRRWheel& Wheel = OutParts.AddDefaulted_GetRef();
				Wheel.Mesh = ConvertTriangles(Part.local, 0, Part.local.size(), PartToUE);
				Wheel.Axle = GameDirToUE(R[2], R[5], R[8]).GetSafeNormal();
				const FVector3f Size = Wheel.Mesh.GetBounds().GetSize();
				Wheel.Radius = FMath::Max(Size.X, Size.Z) * 0.5;
				Wheel.Socket = BRR::GameToUE(Part.pos[0], Part.pos[1], Part.pos[2]);
				Wheel.bRolls = Part.rolls;
				Wheel.bSteers = Part.steer_gain != 0;
			}
			};
			AddParts(Model.parts, Vehicle.Wheels);
			if (Tuning.sections.size() >= 2)
			{
				const bigred::VehicleModel Cabin = bigred::build_vehicle_model(*Vhc, *Ingame, Tuning, Colour, Scene, nullptr, nullptr, nullptr, 1);
				if (Cabin.valid())
				{
					Vehicle.Cabin = ConvertLocal(Cabin.body);
					AddParts(Cabin.parts, Vehicle.CabinParts);
					const int32_t* In = Tuning.sections[1].internal;
					Vehicle.CabinEye = BRR::GameToUE(In[0], In[1], In[2]);
					Vehicle.CabinPitchDown = In[3] * 360.0 / 65536.0;
					bigred::Dashboard Dash;
					if (Dash.load(*Vhc, Tuning))
					{
						const auto It = Cabin.sheet_layers.find(Dash.sheet_name());
						if (It != Cabin.sheet_layers.end()) Vehicle.DashAtlas = It->second;
					}
				}
			}
		}

		{
			// The ground effects, last: their sheets go after everything else's.
			const std::string Env = bigred::env_name_for_level(Name);
			const bigred::ArchivePtr EnvArchive = bigred::game_archive_or_empty(GameDir, Env + ".ENV");
			std::vector<bigred::SpotEffect> Spots;
			if (const bigred::Blob* Txt = bigred::find_member(*EnvArchive, Env + ".TXT")) Spots = bigred::parse_spot_effects(std::string(Txt->begin(), Txt->end()));
			const int32 Before = Scene.atlas_count;
			bigred::build_particle_visuals(Spots, *EnvArchive, Scene);
			Out.ParticleAtlasBase = Before;
			Out.ParticleAtlasCount = Scene.atlas_count - Before;
		}

		const size_t PageBytes = size_t(BRR::AtlasWidth) * BRR::AtlasHeight * 4;
		for (int32 Page = 0; Page < Scene.atlas_count; ++Page)
		{
			const std::vector<uint8_t> Rgba(Scene.atlas_rgb.begin() + Page * PageBytes, Scene.atlas_rgb.begin() + (Page + 1) * PageBytes);
			Out.Atlases.Add(ImageFromRgba(Rgba, BRR::AtlasWidth, BRR::AtlasHeight));
		}
		return true;
	}
	catch (const std::exception& E)
	{
		OutError = FString::Printf(TEXT("%s: %s"), *Level, UTF8_TO_TCHAR(E.what()));
		return false;
	}
}
