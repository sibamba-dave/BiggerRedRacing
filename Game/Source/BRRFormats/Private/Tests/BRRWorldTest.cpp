#include "BRRArchive.h"
#include "BRRWorld.h"

#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS

// The vertical-slice level: everything the importer needs must come out non-empty and in range.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBRRWorldDirt1Test, "BRR.Formats.World.Dirt1",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBRRWorldDirt1Test::RunTest(const FString& Parameters)
{
	const FString DataDir = BRR::GetDataDir();
	if (DataDir.IsEmpty())
	{
		AddWarning(TEXT("BRR data dir not found (set BRR_DATA_DIR); skipping."));
		return true;
	}
	FBRRWorld World;
	FString Error;
	if (!TestTrue(TEXT("DIRT1 + JEEP loads"), FBRRWorld::Load(DataDir, TEXT("DIRT1"), { TEXT("JEEP") }, 1, World, Error)))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("map width"), World.MapWidth, 256);
	TestTrue(TEXT("terrain triangles"), World.Terrain.NumTriangles() > 100000);
	TestTrue(TEXT("decor triangles"), World.Decor.NumTriangles() > 0);
	TestTrue(TEXT("placements"), World.Placements.Num() > 0);
	TestTrue(TEXT("gates"), World.Gates.Num() >= 14);
	TestEqual(TEXT("grid slots"), World.Grid.Num(), 6);
	TestTrue(TEXT("atlases"), World.Atlases.Num() > 0);
	TestEqual(TEXT("one vehicle"), World.Vehicles.Num(), 1);
	TestEqual(TEXT("view screen"), World.View.Width, 320);
	TestTrue(TEXT("ghosts"), World.Ghosts.Num() >= 6);
	if (TestTrue(TEXT("destructibles"), World.Wrecks.Num() > 0))
	{
		int32 Poses = 0;
		for (const FBRRWreck& W : World.Wrecks)
		{
			TestEqual(TEXT("one entry per flipbook step"), W.StepMesh.Num(), BRR::WreckStepCount);
			TestTrue(TEXT("destructible is a scenery instance"), World.Placements.IsValidIndex(W.Instance));
			for (const int32 M : W.StepMesh) TestTrue(TEXT("step mesh index in range"), M >= -1 && M < W.Steps.Num());
			Poses += W.Steps.Num();
		}
		AddInfo(FString::Printf(TEXT("destructibles: %d pieces, %d distinct knock-over poses"), World.Wrecks.Num(), Poses));
	}
	if (TestTrue(TEXT("billboards"), World.Sprites.Num() > 0))
	{
		int32 OfWrecks = 0, Animated = 0;
		for (const FBRRSprite& S : World.Sprites)
		{
			TestTrue(TEXT("billboard has a size"), S.HalfWidth > 0 && S.HalfHeight > 0);
			for (const FBRRSprite::FFrame& F : S.Frames)
			{
				if (F.bOn) TestTrue(TEXT("billboard frame on a real atlas"), World.Atlases.IsValidIndex(F.Atlas));
			}
			if (S.WreckInstance >= 0)
			{
				++OfWrecks;
				TestTrue(TEXT("a destructible's billboard has its flipbook"), S.Frames.Num() == 1 + BRR::WreckStepCount);
				TestTrue(TEXT("its destructible is exported"), World.Wrecks.ContainsByPredicate([&S](const FBRRWreck& W) { return W.Instance == S.WreckInstance; }));
			}
			Animated += S.AnimSlot >= 0 && World.AnimStepSeconds.IsValidIndex(S.AnimSlot);
		}
		AddInfo(FString::Printf(TEXT("billboards: %d, %d of destructibles, %d animated; %d anim slots"), World.Sprites.Num(), OfWrecks,
			Animated, World.AnimStepSeconds.Num()));
	}
	if (TestEqual(TEXT("one pose list per anim slot"), World.AnimSteps.Num(), World.AnimStepSeconds.Num()))
	{
		for (int32 S = 0; S < World.AnimSteps.Num(); ++S)
		{
			FString Tris;
			for (const FBRRMesh& Step : World.AnimSteps[S]) Tris += FString::Printf(TEXT(" %d"), Step.NumTriangles());
			TestTrue(TEXT("anim slot has steps"), World.AnimSteps[S].Num() > 0);
			AddInfo(FString::Printf(TEXT("anim slot %d: %.3f s a step, triangles per step:%s"), S, World.AnimStepSeconds[S], *Tris));
		}
	}
	if (TestEqual(TEXT("start light states"), World.StartLights.Num(), 4))
	{
		for (int32 K = 0; K < 4; ++K)
		{
			TestTrue(FString::Printf(TEXT("start light state %d has geometry"), K), World.StartLights[K].NumTriangles() > 0);
		}
		AddInfo(FString::Printf(TEXT("start lights: %d/%d/%d/%d triangles"), World.StartLights[0].NumTriangles(),
			World.StartLights[1].NumTriangles(), World.StartLights[2].NumTriangles(), World.StartLights[3].NumTriangles()));
	}
	for (const FBRRGhostTrack& G : World.Ghosts)
	{
		AddInfo(FString::Printf(TEXT("ghost %d.REC type %d car %d colour %d, %d ticks (%.1f s)"),
			G.Slot, G.Type, G.VehicleIndex, G.Colour, G.Records.Num(), G.Records.Num() * 0.028));
	}

	const FBox3f Terrain = World.Terrain.GetBounds();
	AddInfo(FString::Printf(TEXT("DIRT1 terrain %d tris, bounds %s"), World.Terrain.NumTriangles(), *Terrain.ToString()));
	AddInfo(FString::Printf(TEXT("DIRT1 decor %d tris in %d sections, %d placements, %d gates, %d atlases, laps %d"),
		World.Decor.NumTriangles(), World.Decor.Sections.Num(), World.Placements.Num(), World.Gates.Num(), World.Atlases.Num(), World.Laps));
	for (const FBRRStart& S : World.Grid) AddInfo(FString::Printf(TEXT("grid %s yaw %.1f"), *S.Location.ToString(), S.Yaw));
	if (World.Vehicles.Num() == 1)
	{
		const FBRRVehicleMesh& Jeep = World.Vehicles[0];
		const FVector3f Size = Jeep.Body.GetBounds().GetSize();
		AddInfo(FString::Printf(TEXT("JEEP body %d tris, size %s cm, %d wheels"), Jeep.Body.NumTriangles(), *Size.ToString(), Jeep.Wheels.Num()));
		for (const FBRRWheel& W : Jeep.Wheels) AddInfo(FString::Printf(TEXT("  wheel at %s rolls=%d steers=%d"), *W.Socket.ToString(), W.bRolls, W.bSteers));
		TestTrue(TEXT("jeep has a body"), Jeep.Body.NumTriangles() > 0);
	}
	// Grid cars sit inside the map.
	for (const FBRRStart& S : World.Grid)
		TestTrue(TEXT("grid slot inside terrain"), Terrain.IsInsideOrOnXY(FVector3f(S.Location)));
	return true;
}

// Every shipped level and vehicle loads through the full ORR scene pipeline.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBRRWorldAllContentTest, "BRR.Formats.World.AllContent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBRRWorldAllContentTest::RunTest(const FString& Parameters)
{
	const FString DataDir = BRR::GetDataDir();
	if (DataDir.IsEmpty())
	{
		AddWarning(TEXT("BRR data dir not found (set BRR_DATA_DIR); skipping."));
		return true;
	}
	TArray<FString> Levels, Vehicles;
	IFileManager::Get().FindFiles(Levels, *FPaths::Combine(DataDir, TEXT("*.LEV")), true, false);
	IFileManager::Get().FindFiles(Vehicles, *FPaths::Combine(DataDir, TEXT("*.VHC")), true, false);
	for (FString& V : Vehicles) V = FPaths::GetBaseFilename(V);

	for (const FString& File : Levels)
	{
		const FString Level = FPaths::GetBaseFilename(File);
		FBRRWorld World;
		FString Error;
		const bool bOk = FBRRWorld::Load(DataDir, Level, {}, 1, World, Error);
		if (TestTrue(FString::Printf(TEXT("%s loads (%s)"), *Level, *Error), bOk))
		{
			TestTrue(FString::Printf(TEXT("%s has terrain"), *Level), World.Terrain.NumTriangles() > 0);
			AddInfo(FString::Printf(TEXT("%s: %d terrain / %d decor tris, %d gates, %d cars"), *Level,
				World.Terrain.NumTriangles(), World.Decor.NumTriangles(), World.Gates.Num(), World.CarList.Num()));
			// Which scenery it places, and how often (SHAPES lines: what the remastered look may replace).
			TMap<FString, int32> Shapes;
			for (const FBRRPlacement& P : World.Placements) ++Shapes.FindOrAdd(P.Shape);
			Shapes.ValueSort([](int32 A, int32 B) { return A > B; });
			FString List;
			for (const TPair<FString, int32>& S : Shapes) List += FString::Printf(TEXT(" %s:%d"), *S.Key, S.Value);
			AddInfo(FString::Printf(TEXT("SHAPES %s:%s"), *Level, *List));
		}
	}

	FBRRWorld World;
	FString Error;
	const bool bOk = FBRRWorld::Load(DataDir, TEXT("DIRT1"), Vehicles, 1, World, Error);
	if (TestTrue(FString::Printf(TEXT("all vehicles load (%s)"), *Error), bOk))
	{
		for (const FBRRVehicleMesh& V : World.Vehicles)
		{
			TestTrue(FString::Printf(TEXT("%s has a body"), *V.Name), V.Body.NumTriangles() > 0);
			AddInfo(FString::Printf(TEXT("%s: %d tris, size %s, %d parts"), *V.Name, V.Body.NumTriangles(),
				*V.Body.GetBounds().GetSize().ToString(), V.Wheels.Num()));
		}
	}
	return true;
}

// The remastered look shades water, ice and lava as such (FBRRWorld::LiquidOf): each level's
// liquid comes out where its art and height say it is - a sea-level surface in the liquid's colour.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBRRWorldLiquidsTest, "BRR.Formats.World.Liquids",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBRRWorldLiquidsTest::RunTest(const FString& Parameters)
{
	const FString DataDir = BRR::GetDataDir();
	if (DataDir.IsEmpty())
	{
		AddWarning(TEXT("BRR data dir not found (set BRR_DATA_DIR); skipping."));
		return true;
	}
	struct FCase { const TCHAR* Level; EBRRLiquid Liquid; int32 MinTriangles; };
	for (const FCase& Case : { FCase{ TEXT("WATER1"), EBRRLiquid::Water, 3000 }, FCase{ TEXT("DIRT1"), EBRRLiquid::Water, 60000 },
		FCase{ TEXT("SNOW2"), EBRRLiquid::Ice, 100000 }, FCase{ TEXT("MOON2"), EBRRLiquid::Lava, 100000 } })
	{
		FBRRWorld World;
		FString Error;
		if (!TestTrue(FString::Printf(TEXT("%s loads: %s"), Case.Level, *Error), FBRRWorld::Load(DataDir, Case.Level, {}, 1, World, Error))) continue;
		int32 Count = 0, Unclassified = 0, Other = 0;
		for (const FBRRMeshSection& S : World.Terrain.Sections)
		{
			for (const FBRRVertex& V : S.Vertices)
			{
				if (V.Land > 7) { ++Unclassified; continue; }
				const EBRRLiquid L = World.LiquidOf(V.Land);
				Count += L == Case.Liquid;
				Other += L != Case.Liquid && L != EBRRLiquid::None;
			}
		}
		TestEqual(FString::Printf(TEXT("%s: every terrain triangle has a surface"), Case.Level), Unclassified, 0);
		TestEqual(FString::Printf(TEXT("%s: one kind of liquid"), Case.Level), Other, 0);
		TestTrue(FString::Printf(TEXT("%s: %d liquid triangles"), Case.Level, Count / 3), Count / 3 >= Case.MinTriangles);
	}
	return true;
}

// Decor laid on the ground (the starting grid) is lifted clear of the terrain so the two do not
// z-fight (FBRRWorld::GroundDecalTriangles): every level with a grid has some.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBRRWorldGroundDecalsTest, "BRR.Formats.World.GroundDecals",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBRRWorldGroundDecalsTest::RunTest(const FString& Parameters)
{
	const FString DataDir = BRR::GetDataDir();
	if (DataDir.IsEmpty())
	{
		AddWarning(TEXT("BRR data dir not found (set BRR_DATA_DIR); skipping."));
		return true;
	}
	for (const TCHAR* Level : { TEXT("DIRT1"), TEXT("WATER1"), TEXT("CITY1"), TEXT("SNOW1"), TEXT("MOON1") })
	{
		FBRRWorld World;
		FString Error;
		if (!TestTrue(FString::Printf(TEXT("%s loads: %s"), Level, *Error), FBRRWorld::Load(DataDir, Level, {}, 1, World, Error))) continue;
		AddInfo(FString::Printf(TEXT("%s: %d of %d decor triangles lifted off the ground"), Level, World.GroundDecalTriangles, World.Decor.NumTriangles()));
		TestTrue(FString::Printf(TEXT("%s: the grid is lifted"), Level), World.GroundDecalTriangles >= 4);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBRRWorldGroundKindsTest, "BRR.Formats.World.GroundKinds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBRRWorldGroundKindsTest::RunTest(const FString& Parameters)
{
	const FString DataDir = BRR::GetDataDir();
	if (DataDir.IsEmpty())
	{
		AddWarning(TEXT("BRR data dir not found (set BRR_DATA_DIR); skipping."));
		return true;
	}
	static const TCHAR* Names[] = { TEXT("none"), TEXT("dirt"), TEXT("grass"), TEXT("sand"), TEXT("snow"), TEXT("rock"), TEXT("asphalt"), TEXT("regolith") };
	// Each theme's ground mostly what it looks like.
	const TPair<const TCHAR*, EBRRGround> Expect[] = { { TEXT("DIRT1"), EBRRGround::Grass }, { TEXT("CITY4"), EBRRGround::Asphalt },
		{ TEXT("SNOW1"), EBRRGround::Snow }, { TEXT("MOON1"), EBRRGround::Regolith } };
	TArray<FString> Levels;
	IFileManager::Get().FindFiles(Levels, *(DataDir / TEXT("*.LEV")), true, false);
	for (FString& L : Levels) L = FPaths::GetBaseFilename(L).ToUpper();
	Levels.Sort();
	for (const FString& Level : Levels)
	{
		FBRRWorld World;
		FString Error;
		if (!TestTrue(FString::Printf(TEXT("%s loads: %s"), *Level, *Error), FBRRWorld::Load(DataDir, Level, {}, 1, World, Error))) continue;
		int32 Count[int32(EBRRGround::Count)] = {};
		for (const FBRRMeshSection& S : World.Terrain.Sections)
		{
			for (int32 T = 0; T + 2 < S.Vertices.Num(); T += 3)
			{
				const FColor Average = World.Atlases.IsValidIndex(S.Atlas)
					? FBRRWorld::AverageTexels(World.Atlases[S.Atlas], S.Vertices[T].UV, S.Vertices[T + 1].UV, S.Vertices[T + 2].UV) : S.Vertices[T].Color;
				++Count[int32(World.GroundOf(S.Vertices[T].Land, Average))];
			}
		}
		FString Line;
		for (int32 K = 0; K < int32(EBRRGround::Count); ++K) Line += FString::Printf(TEXT(" %s %d"), Names[K], Count[K]);
		AddInfo(FString::Printf(TEXT("GROUND %s:%s"), *Level, *Line));
		for (const TPair<const TCHAR*, EBRRGround>& E : Expect)
		{
			if (Level != E.Key) continue;
			int32 Most = 1;
			for (int32 K = 2; K < int32(EBRRGround::Count); ++K) if (Count[K] > Count[Most]) Most = K;
			TestEqual(FString::Printf(TEXT("%s: the ground is mostly %s"), *Level, Names[int32(E.Value)]), Most, int32(E.Value));
		}
	}
	return true;
}

#endif
