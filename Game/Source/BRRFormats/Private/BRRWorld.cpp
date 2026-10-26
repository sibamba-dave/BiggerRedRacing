#include "BRRWorld.h"

#include "BRRConvert.h"

#include <exception>
#include <string>

#include "formats/game_files.h"
#include "formats/level_script.h"
#include "formats/scenery.h"
#include "formats/texture.h"
#include "formats/vehicle_script.h"
#include "scene/level_scene.h"
#include "scene/vehicle_render.h"
#include "sim/race.h"
#include "sim/replay.h"

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

	// Groups ORR's flat triangle list into sections by (atlas, sidedness), mapping each vertex to UE
	// with ToUE and reversing winding for the handedness flip.
	template <typename FToUE>
	FBRRMesh ConvertTriangles(const std::vector<TerrainVertex>& Verts, size_t First, size_t Count, FToUE&& ToUE)
	{
		FBRRMesh Mesh;
		TMap<TPair<int32, bool>, int32> SectionOf;
		const size_t End = FMath::Min(First + Count, Verts.size());
		for (size_t T = First; T + 2 < End; T += 3)
		{
			const TerrainVertex& A = Verts[T];
			const int32 Atlas = A.has_texture != 0.0f ? int32(A.layer) : INDEX_NONE;
			const bool bTwoSided = A.cull == 0.0f;
			int32& Index = SectionOf.FindOrAdd({ Atlas, bTwoSided }, INDEX_NONE);
			if (Index == INDEX_NONE)
			{
				Index = Mesh.Sections.Num();
				FBRRMeshSection& New = Mesh.Sections.AddDefaulted_GetRef();
				New.Atlas = Atlas;
				New.bTwoSided = bTwoSided;
			}
			FBRRMeshSection& Section = Mesh.Sections[Index];
			for (const size_t K : { T, T + 2, T + 1 })
			{
				const TerrainVertex& V = Verts[K];
				FBRRVertex& Out = Section.Vertices.AddDefaulted_GetRef();
				Out.Position = FVector3f(ToUE(V.x, V.y, V.z));
				Out.UV = FVector2f(V.u, V.v);
				Out.Color = FLinearColor(V.r, V.g, V.b).ToFColor(false);
			}
		}
		return Mesh;
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
		if (!Scene.start_lights.empty())
		{
			for (const std::pair<int, int>& Range : Scene.lights_ranges)
				Out.StartLights.Add(ConvertTriangles(Scene.lights_buf, size_t(Range.first), size_t(Range.second), WorldRenderToUE));
		}
		for (const bigred::LevelAnim& Anim : Scene.anims) Out.AnimStepSeconds.Add(Anim.step_ms() / 1000.0);
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
			for (const bigred::VehiclePart& Part : Model.parts)
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
				FBRRWheel& Wheel = Vehicle.Wheels.AddDefaulted_GetRef();
				Wheel.Mesh = ConvertTriangles(Part.local, 0, Part.local.size(), PartToUE);
				Wheel.Axle = GameDirToUE(R[2], R[5], R[8]).GetSafeNormal();
				const FVector3f Size = Wheel.Mesh.GetBounds().GetSize();
				Wheel.Radius = FMath::Max(Size.X, Size.Z) * 0.5;
				Wheel.Socket = BRR::GameToUE(Part.pos[0], Part.pos[1], Part.pos[2]);
				Wheel.bRolls = Part.rolls;
				Wheel.bSteers = Part.steer_gain != 0;
			}
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
