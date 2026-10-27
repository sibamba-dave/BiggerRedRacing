#include "BRRArchive.h"
#include "BRRRefine.h"
#include "BRRWorld.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBRRRefineTest, "BRR.Formats.Refine",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBRRRefineTest::RunTest(const FString& Parameters)
{
	// Shapes first: they need no data.
	{
		const FBRRMesh Tuft = BRR::MakeGrassTuft(1);
		TestTrue(TEXT("a grass tuft has blades"), Tuft.NumTriangles() > 80);
		const FBRRMesh Stone = BRR::MakeStone(1, 2);
		TestEqual(TEXT("a stone is a twice-cut icosahedron"), Stone.NumTriangles(), 320);
		const FBox3f Box = Stone.GetBounds();
		TestTrue(TEXT("a stone is about a metre"), Box.GetSize().X > 60.f && Box.GetSize().X < 160.f);

		// Subdivision: a cube's 90 degree folds are creases (its corners stay); an octahedron's
		// 109 degree ones are not at a 120 degree crease angle (it rounds towards a sphere).
		auto Solid = [](const TArray<FVector3f>& P, const TArray<int32>& Idx)
		{
			FBRRMesh M;
			FBRRMeshSection& S = M.Sections.AddDefaulted_GetRef();
			for (const int32 I : Idx) { FBRRVertex V; V.Position = P[I]; S.Vertices.Add(V); }
			return M;
		};
		const TArray<FVector3f> CubeP = { {0,0,0},{100,0,0},{100,100,0},{0,100,0},{0,0,100},{100,0,100},{100,100,100},{0,100,100} };
		const TArray<int32> CubeI = { 0,2,1, 0,3,2, 4,5,6, 4,6,7, 0,1,5, 0,5,4, 1,2,6, 1,6,5, 2,3,7, 2,7,6, 3,0,4, 3,4,7 };
		const FBRRMesh Cube = BRR::SubdivideSmooth(Solid(CubeP, CubeI), 2, 50.f);
		TestEqual(TEXT("sixteen triangles for one"), Cube.NumTriangles(), 12 * 16);
		bool bCorner = false;
		for (const FBRRVertex& V : Cube.Sections[0].Vertices) bCorner |= V.Position.Equals(FVector3f(100, 100, 100), 0.01f);
		TestTrue(TEXT("a cube keeps its corners"), bCorner);
		const TArray<FVector3f> OctP = { {100,0,0},{-100,0,0},{0,100,0},{0,-100,0},{0,0,100},{0,0,-100} };
		const TArray<int32> OctI = { 0,2,4, 2,1,4, 1,3,4, 3,0,4, 2,0,5, 1,2,5, 3,1,5, 0,3,5 };
		const FBRRMesh Ball = BRR::SubdivideSmooth(Solid(OctP, OctI), 3, 120.f);
		float Lo = 1e9f, Hi = 0;
		for (const FBRRVertex& V : Ball.Sections[0].Vertices) Lo = FMath::Min(Lo, V.Position.Size()), Hi = FMath::Max(Hi, V.Position.Size());
		TestTrue(FString::Printf(TEXT("an octahedron rounds (radius %.0f to %.0f)"), Lo, Hi), Hi / Lo < 1.25f);
	}
	const FString DataDir = BRR::GetDataDir();
	if (DataDir.IsEmpty())
	{
		AddWarning(TEXT("BRR data dir not found (set BRR_DATA_DIR); skipping."));
		return true;
	}
	for (const TCHAR* Level : { TEXT("DIRT1"), TEXT("WATER1"), TEXT("CITY4"), TEXT("SNOW1"), TEXT("MOON1"), TEXT("RUF1") })
	{
		FBRRWorld World;
		FString Error;
		if (!TestTrue(FString::Printf(TEXT("%s loads: %s"), Level, *Error), FBRRWorld::Load(DataDir, Level, { TEXT("JEEP") }, 1, World, Error))) continue;

		// The refined surface meets the original at the corners, stays within the clamp, and
		// within a third of it on the track and nothing on ice and lava (open water is drawn apart).
		double MaxOffset = 0, MaxTrack = 0, MaxLiquid = 0, MaxCorner = 0;
		for (const FBRRMeshSection& S : World.Terrain.Sections)
		{
			for (int32 T = 0; T + 2 < S.Vertices.Num(); T += 3)
			{
				const FVector3f Mid = (S.Vertices[T].Position + S.Vertices[T + 1].Position + S.Vertices[T + 2].Position) / 3.f;
				const double Offset = FMath::Abs(World.RefinedOffsetAt(Mid.X, Mid.Y));
				const uint8 Land = S.Vertices[T].Land;
				MaxOffset = FMath::Max(MaxOffset, Offset);
				if ((Land == 0 || Land == 1) && World.LiquidOf(Land) == EBRRLiquid::None) MaxTrack = FMath::Max(MaxTrack, Offset);
				if (World.LiquidOf(Land) != EBRRLiquid::None && World.LiquidOf(Land) != EBRRLiquid::Water) MaxLiquid = FMath::Max(MaxLiquid, Offset);
				MaxCorner = FMath::Max(MaxCorner, FMath::Abs(World.RefinedOffsetAt(S.Vertices[T].Position.X, S.Vertices[T].Position.Y)));
			}
		}
		const TArray<BRR::FBRRScatter> Scatter = BRR::ScatterGround(World, 2500.0);
		int32 Kinds[4] = {};
		for (const BRR::FBRRScatter& Item : Scatter) ++Kinds[Item.Kind];
		AddInfo(FString::Printf(TEXT("REFINE %s: offset max %.1f cm, on the track %.1f, at the original's corners %.1f; scatter: %d grass, %d pebbles, %d rocks, %d lumps"),
			Level, MaxOffset, MaxTrack, MaxCorner, Kinds[0], Kinds[1], Kinds[2], Kinds[3]));
		TestTrue(FString::Printf(TEXT("%s: within the clamp"), Level), MaxOffset <= FBRRWorld::RefineClamp + 0.01);
		TestTrue(FString::Printf(TEXT("%s: gentle on the track"), Level), MaxTrack <= FBRRWorld::RefineClamp / 3 + 0.01);
		TestTrue(FString::Printf(TEXT("%s: flat on liquids"), Level), MaxLiquid <= FBRRWorld::RefineClamp / 2);
		TestTrue(FString::Printf(TEXT("%s: scatter stays affordable"), Level), Scatter.Num() < 400000);

		// Open edges inside the map would be T-junctions: cracks once subdivided.
		{
			TMap<TPair<FIntVector, FIntVector>, int32> Edges;
			auto Key = [](const FVector3f& P) { return FIntVector(FMath::RoundToInt32(P.X), FMath::RoundToInt32(P.Y), FMath::RoundToInt32(P.Z)); };
			auto Less = [](const FIntVector& A, const FIntVector& B) { return A.X != B.X ? A.X < B.X : A.Y != B.Y ? A.Y < B.Y : A.Z < B.Z; };
			for (const FBRRMeshSection& S : World.Terrain.Sections)
				for (int32 T = 0; T + 2 < S.Vertices.Num(); T += 3)
					for (int32 K = 0; K < 3; ++K)
					{
						FIntVector A = Key(S.Vertices[T + K].Position), B = Key(S.Vertices[T + (K + 1) % 3].Position);
						if (Less(B, A)) Swap(A, B);
						++Edges.FindOrAdd({ A, B });
					}
			const FBox3f Bounds = World.Terrain.GetBounds();
			int32 Open = 0;
			for (const auto& E : Edges)
			{
				if (E.Value != 1) continue;
				const FIntVector& A = E.Key.Key; const FIntVector& B = E.Key.Value;
				auto Border = [&](const FIntVector& P) { return P.X <= Bounds.Min.X + 2 || P.X >= Bounds.Max.X - 2 || P.Y <= Bounds.Min.Y + 2 || P.Y >= Bounds.Max.Y - 2; };
				if (!(Border(A) && Border(B))) ++Open;
			}
			AddInfo(FString::Printf(TEXT("EDGES %s: %d edges, %d open inside the map"), Level, Edges.Num(), Open));
		}
		const FBRRMesh Refined = World.RefinedTerrain(2);
		TestEqual(FString::Printf(TEXT("%s: four triangles for each"), Level), Refined.NumTriangles(), World.Terrain.NumTriangles() * 4);

		// A rounded wheel's tread lies on one circle.
		if (!World.Vehicles.IsEmpty() && !World.Vehicles[0].Wheels.IsEmpty())
		{
			const FBRRWheel& Wheel = World.Vehicles[0].Wheels[0];
			const FBRRMesh Round = BRR::RoundWheel(Wheel.Mesh, Wheel.Axle, 4);
			const FVector3f Centre = Wheel.Mesh.GetBounds().GetCenter(), Axle = FVector3f(Wheel.Axle.GetSafeNormal());
			float Outer = 0, Spread = 0;
			for (const FBRRMeshSection& S : Wheel.Mesh.Sections)
				for (const FBRRVertex& V : S.Vertices) { const FVector3f D = V.Position - Centre; Outer = FMath::Max(Outer, (D - Axle * (D | Axle)).Size()); }
			for (const FBRRMeshSection& S : Round.Sections)
				for (const FBRRVertex& V : S.Vertices)
				{
					const FVector3f D = V.Position - Centre;
					const float R = (D - Axle * (D | Axle)).Size();
					if (R > Outer * 0.97f) Spread = FMath::Max(Spread, Outer - R);
				}
			TestTrue(FString::Printf(TEXT("%s: the rounded tread is a circle (off by %.2f cm)"), Level, Spread), Spread < Outer * 0.02f);
		}
	}
	return true;
}

#endif
