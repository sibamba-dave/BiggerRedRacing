// Race session and reference-handling measurement over the vendored ORR simulation. The tick
// order and the rubber band follow ORR sim/race_session.cpp (MIT, Copyright (c) 2026 PaPe); the
// player's car is fed from UE instead of being stepped by the original physics.
#include "BRRRace.h"

#include "BRRConvert.h"

#include <cmath>
#include <exception>
#include <memory>
#include <vector>

#include "formats/game_files.h"
#include "formats/level_script.h"
#include "formats/vehicle_script.h"
#include "scene/chase_camera.h"
#include "scene/level_scene.h"
#include "scene/vehicle_render.h"
#include "sim/race.h"
#include "sim/race_rules.h"
#include "sim/replay.h"
#include "sim/vehicle.h"

using namespace BRR::Private;

namespace
{
	constexpr int32 GridRacers = 6;

	// Body-origin UE transform of a simulated (or proxied) car.
	FTransform CarTransform(const bigred::VehicleSim& Car)
	{
		float M[9];
		bigred::vehicle_rotation(float(Car.pitch_bam16() * BamToRad), float(Car.yaw_bam16() * BamToRad),
			float(Car.roll_bam16() * BamToRad), M);
		const FVector Location = BRR::GameToUE(Car.px / 512.0, Car.py / 512.0, Car.pz / 512.0);
		return FTransform(GameRotationToUE(M), Location);
	}

	// The level's simulation terrain, built as ORR's physics bench builds it.
	void BuildSimTerrain(const bigred::LevelScene& Scene, const std::string& Script, bigred::SimTerrain& Ter)
	{
		Ter.mph = Scene.mph_data;
		Ter.mpc = Scene.mpc_data;
		Ter.cht = Scene.cht_size >= 512 ? Scene.cht_data : nullptr;
		Ter.w = Scene.map_w;
		Ter.h = Scene.map_h;
		const std::vector<uint8_t> Land = bigred::parse_land_table(Script);
		for (size_t I = 0; I < 16 && I < Land.size(); ++I) Ter.land[I] = Land[I];
		auto FtLookup = [&Scene](const std::string& Shape) -> const bigred::Blob*
		{
			auto It = Scene.lev.find(Shape + ".FT");
			return It == Scene.lev.end() ? nullptr : &It->second;
		};
		Ter.objects = bigred::build_sim_objects(Scene.instances, Scene.shape_names_by_slot, FtLookup);
		Ter.blockers = bigred::build_sim_blockers(Scene.instances, Scene.shape_names_by_slot, FtLookup);
		Ter.build_blocker_grid();
	}
}

const TCHAR* FBRRHandlingMetrics::SurfaceName(int32 Surface)
{
	static const TCHAR* Names[] = { TEXT("TRACK"), TEXT("GRIPPY"), TEXT("SLIPPY"), TEXT("CLIFFS"), TEXT("GRASS"), TEXT("SAND"), TEXT("RAMPS"), TEXT("AIR") };
	return Names[Surface & 7];
}

bool BRR::MeasureHandling(const FString& DataDir, const FString& Vehicle, FBRRHandlingMetrics& Out, FString& OutError)
{
	try
	{
		bigred::VehicleTuning Tuning;
		if (!bigred::load_vehicle(ToUtf8(DataDir), ToUtf8(Vehicle.ToUpper()), Tuning))
		{
			OutError = FString::Printf(TEXT("could not load %s.VHC"), *Vehicle);
			return false;
		}
		Out = FBRRHandlingMetrics();
		Out.Vehicle = Vehicle.ToUpper();

		// A flat map, every cell the same ground: land[0] picks the surface type under test.
		const std::vector<uint8_t> Flat(256 * 256, 0);
		const bigred::RaceRules Rules;
		const bigred::RespawnPoint Centre{ 128 * 512, 128 * 512, 0 };
		const double ToCmPerSecond = 1000.0 / bigred::kPhysicsTickMs / 512.0 * BRR::CmPerUnit;
		const int32 TicksPerSecond = 1000 / bigred::kPhysicsTickMs;

		for (int32 Surface = 0; Surface < 8; ++Surface)
		{
			bigred::SimTerrain Ter;
			Ter.mph = Flat.data();
			Ter.mpc = Flat.data();
			Ter.land[0] = uint8_t(Surface);
			FBRRSurfaceMetrics& M = Out.Surfaces[Surface];

			auto Fresh = [&](bigred::VehicleSim& Car)
			{
				Car.reset(Tuning, Ter, Rules, Centre);
				Car.place_on_grid();
			};
			auto Speed = [&](const bigred::VehicleSim& Car) { return Car.speed * ToCmPerSecond; };

			// Acceleration, then braking from the top speed.
			bigred::VehicleSim Car;
			Fresh(Car);
			bigred::VehicleInput Accel;
			Accel.accel = true;
			TArray<double> Samples;
			for (int32 T = 0; T < 25 * TicksPerSecond; ++T)
			{
				Car.step(Accel);
				Samples.Add(Speed(Car));
			}
			for (const double S : Samples) M.TopSpeed = FMath::Max(M.TopSpeed, S);
			for (int32 T = 0; T < Samples.Num(); ++T)
			{
				if (M.TimeTo60 == 0 && Samples[T] >= 0.6 * M.TopSpeed) M.TimeTo60 = (T + 1) * FBRRRace::TickSeconds;
				if (M.TimeTo90 == 0 && Samples[T] >= 0.9 * M.TopSpeed) M.TimeTo90 = (T + 1) * FBRRRace::TickSeconds;
			}
			bigred::VehicleInput Brake;
			Brake.brake = true;
			for (int32 T = 0; T < 20 * TicksPerSecond; ++T)
			{
				Car.step(Brake);
				if (Speed(Car) <= 0.02 * M.TopSpeed)
				{
					M.BrakeTime = (T + 1) * FBRRRace::TickSeconds;
					break;
				}
			}

			// Reverse from rest.
			Fresh(Car);
			for (int32 T = 0; T < 15 * TicksPerSecond; ++T)
			{
				Car.step(Brake);
				M.ReverseTopSpeed = FMath::Max(M.ReverseTopSpeed, FMath::Abs(Speed(Car)));
			}

			// Turning: up to speed, then full lock; yaw rate over the last two seconds of four.
			Fresh(Car);
			for (int32 T = 0; T < 4 * TicksPerSecond; ++T) Car.step(Accel);
			bigred::VehicleInput Turn = Accel;
			Turn.left = true;
			int64 YawSum = 0;
			int32 PrevYaw = Car.yaw_bam16();
			double SpeedSum = 0;
			int32 Counted = 0;
			for (int32 T = 0; T < 4 * TicksPerSecond; ++T)
			{
				Car.step(Turn);
				const int32 Yaw = Car.yaw_bam16();
				const int16 Delta = int16((Yaw - PrevYaw) & 0xFFFF);
				PrevYaw = Yaw;
				if (T >= 2 * TicksPerSecond)
				{
					YawSum += Delta;
					SpeedSum += FMath::Abs(Speed(Car));
					++Counted;
				}
			}
			if (Counted > 0)
			{
				M.TurnYawRate = FMath::Abs(double(YawSum)) * 360.0 / 65536.0 / (Counted * FBRRRace::TickSeconds);
				M.TurnSpeed = SpeedSum / Counted;
			}

			// The yaw-rate curves, sampled every quarter second along held full-lock turns.
			auto Trajectory = [&](double RampSeconds, bool bPower, TArray<FVector2D>& OutCurve)
			{
				Fresh(Car);
				for (int32 T = 0; T < int32(RampSeconds * TicksPerSecond); ++T) Car.step(Accel);
				bigred::VehicleInput Lock;
				Lock.left = true;
				Lock.accel = bPower;
				const int32 Window = TicksPerSecond / 4;
				int32 Prev = Car.yaw_bam16();
				for (int32 W = 0; W < 24; ++W)
				{
					int64 Sum = 0;
					double SpeedAt = 0;
					for (int32 T = 0; T < Window; ++T)
					{
						Car.step(Lock);
						const int32 Yaw = Car.yaw_bam16();
						Sum += int16((Yaw - Prev) & 0xFFFF);
						Prev = Yaw;
						SpeedAt += FMath::Abs(Speed(Car));
					}
					if (W >= 2) // past the build-up
						OutCurve.Add(FVector2D(SpeedAt / Window, FMath::Abs(double(Sum)) * 360.0 / 65536.0 / (Window * FBRRRace::TickSeconds)));
				}
				OutCurve.Sort([](const FVector2D& A, const FVector2D& B) { return A.X < B.X; });
			};
			Trajectory(0.0, true, M.TurnCurve);
			Trajectory(8.0, false, M.CoastTurnCurve);

			// The build-up: full lock under power from speed, time to 63% of the rate 3 s in.
			{
				Fresh(Car);
				for (int32 T = 0; T < 4 * TicksPerSecond; ++T) Car.step(Accel);
				TArray<double> Rates;
				int32 Prev = Car.yaw_bam16();
				for (int32 T = 0; T < 3 * TicksPerSecond; ++T)
				{
					Car.step(Turn);
					const int32 Yaw = Car.yaw_bam16();
					Rates.Add(FMath::Abs(double(int16((Yaw - Prev) & 0xFFFF))));
					Prev = Yaw;
				}
				double Final = 0;
				for (int32 T = Rates.Num() - TicksPerSecond / 2; T < Rates.Num(); ++T) Final += Rates[T];
				Final /= TicksPerSecond / 2;
				for (int32 T = 0; T < Rates.Num(); ++T)
				{
					if (Rates[T] >= 0.63 * Final)
					{
						M.YawTimeConstant = (T + 1) * FBRRRace::TickSeconds;
						break;
					}
				}
			}
		}
		return true;
	}
	catch (const std::exception& E)
	{
		OutError = FString::Printf(TEXT("%s: %s"), *Vehicle, UTF8_TO_TCHAR(E.what()));
		return false;
	}
}

struct FBRRRace::FImpl
{
	bigred::LevelScene Scene;
	bigred::SimTerrain Terrain;
	bigred::RaceRules Rules;
	bigred::RaceState Race;
	bigred::ReplayRates Rates;
	std::vector<std::unique_ptr<bigred::VehicleTuning>> Tunings; // stable addresses: cars point at them
	std::vector<bigred::ReplayTrack> Tracks;                      // per racer, empty for the player
	std::vector<bigred::VehicleSim> Cars;
	std::vector<bigred::VehicleSim> StartCars; // the field as placed on the grid
	std::vector<bigred::VehicleSim*> Field;
	std::vector<bigred::GhostDriver> Ghosts;
	TArray<FString> VehicleNames;
	TArray<int32> Colours;
	int32 Player = 0;
	bigred::ChaseCamera Camera;
	bigred::CameraState CameraPrev;
	bigred::SceneryState Scenery;
	TOptional<FVector2D> PlayerBounce; // UE ground velocity, cm/s, from a destructible's push-back
	int32 GhostRate = 0x200;
	uint32 BandClock = 0;

	void LinkField()
	{
		Field.resize(Cars.size());
		for (size_t I = 0; I < Cars.size(); ++I) Field[I] = &Cars[I];
		for (size_t I = 0; I < Cars.size(); ++I)
		{
			Cars[I].field = Field.data();
			Cars[I].field_count = int(Cars.size());
			Cars[I].field_self = int(I);
			Cars[I].rules = &Rules;
			Cars[I].replayed = true; // nobody is stepped by the original physics here
			Cars[I].world = &Scenery;
		}
	}

	// ORR RaceSession::rubber_band_step: every 448 ms the ghosts' playback rate moves 0x10 towards
	// the rate the player's place calls for.
	void RubberBand()
	{
		BandClock += 2;
		if (!Rules.rubber_band || (BandClock & 0x1f) != 0) return;
		const int32 Place0 = Race.progress(Player).place - 1;
		int32 Mode = Place0 == 0 ? 1 : 0;
		if (Place0 != 0 && Place0 < 3) Mode = 3;
		if (Place0 > 3) Mode = 2;
		for (int32 I = 0; I < int32(Cars.size()); ++I)
			if (Race.progress(I).lap >= Race.laps() - 1) Mode = 3;
		switch (Mode)
		{
		case 1: if (GhostRate < Rates.max) GhostRate += 0x10; break;
		case 2: if (Rates.min < GhostRate) GhostRate -= 0x10; break;
		case 3:
			if (GhostRate < Rates.normal) GhostRate += 0x10;
			if (Rates.normal < GhostRate) GhostRate -= 0x10;
			break;
		default: break;
		}
	}
};

FBRRRace::FBRRRace() : Impl(MakeUnique<FImpl>()) {}
FBRRRace::~FBRRRace() = default;

TUniquePtr<FBRRRace> FBRRRace::Create(const FString& DataDir, const FString& Level, const FString& PlayerVehicle, FString& OutError)
{
	TUniquePtr<FBRRRace> Race(new FBRRRace());
	FImpl& I = *Race->Impl;
	try
	{
		const std::string GameDir = ToUtf8(DataDir);
		const std::string Name = ToUtf8(Level.ToUpper());
		if (!I.Scene.load(GameDir, Name))
		{
			OutError = FString::Printf(TEXT("could not load %s.LEV"), *Level);
			return nullptr;
		}
		std::string Script;
		if (const bigred::Blob* Txt = bigred::find_member(I.Scene.lev, Name + ".TXT")) Script.assign(Txt->begin(), Txt->end());
		BuildSimTerrain(I.Scene, Script, I.Terrain);

		const std::vector<std::string> Cars = bigred::parse_car_list(Script);
		const bigred::StartingGrid Grid = bigred::parse_grid(Script);
		const bigred::StartPlaces Places = bigred::parse_start_places(Script);
		const bigred::NitroReserve Nitros = bigred::parse_nitros(Script);
		I.Rates = bigred::parse_replay_rates(Script);
		// The original draws the player's block as rand() % 6 unless the level fixes it (`spos`).
		I.Player = Places.present ? FMath::Clamp(int32(Places.single), 0, GridRacers - 1) : FMath::RandRange(0, GridRacers - 1);

		std::vector<bigred::Gate> Gates;
		if (const bigred::Blob* Cse = bigred::find_member(I.Scene.lev, "MAP.CSE")) Gates = bigred::parse_gates(Cse->data(), Cse->size());
		bigred::mark_unsafe_gates(Gates, bigred::parse_unsafe_gates(Script));
		std::vector<bigred::CheckLine> Lines = bigred::build_check_lines(I.Scene.instances, I.Scene.shape_names_by_slot,
			[&I](const std::string& Shape) -> const bigred::Blob*
			{
				auto It = I.Scene.lev.find(Shape + ".FT");
				return It == I.Scene.lev.end() ? nullptr : &It->second;
			});
		bigred::mark_checkpoint_gates(Gates, Lines);
		I.Race.configure(std::move(Gates), bigred::parse_laps(Script), bigred::parse_countdown_ticks(Script), GridRacers, std::move(Lines));

		I.Tracks.resize(GridRacers);
		I.Cars.resize(GridRacers);
		I.Ghosts.resize(GridRacers);
		for (int32 B = 0; B < GridRacers; ++B)
		{
			std::string Vehicle = ToUtf8(PlayerVehicle.ToUpper());
			int32 Colour = B % 6;
			if (const bigred::Blob* Rec = bigred::find_member(I.Scene.lev, std::to_string(B) + ".REC"))
			{
				bigred::ReplayTrack Track = bigred::parse_replay(*Rec, B);
				if (Track.valid())
				{
					Colour = Track.colour % 6;
					if (B != I.Player && Track.vehicle_index >= 0 && size_t(Track.vehicle_index) < Cars.size())
					{
						Vehicle = Cars[Track.vehicle_index];
						I.Tracks[B] = std::move(Track);
					}
				}
			}
			auto Tuning = std::make_unique<bigred::VehicleTuning>();
			if (!bigred::load_vehicle(GameDir, Vehicle, *Tuning))
			{
				OutError = FString::Printf(TEXT("could not load %s.VHC"), *FromUtf8(Vehicle));
				return nullptr;
			}
			bigred::VehicleSim& Car = I.Cars[B];
			Car.reset(*Tuning, I.Terrain, I.Rules, I.Race.start_of(Grid, B), Nitros);
			Car.place_on_grid();
			if (I.Tracks[B].valid()) I.Ghosts[B].start(I.Tracks[B], Car);
			I.Tunings.push_back(std::move(Tuning));
			I.VehicleNames.Add(FromUtf8(Vehicle));
			I.Colours.Add(Colour + 1);
		}
		// The player races in red (team colour 1), and the ghost whose tape says red takes the
		// player's block colour instead, so each colour is on the grid once: what captures of the
		// original show (WATER1: a red player against yellow, blue, orange, green and magenta).
		for (int32 R = 0; R < I.Colours.Num(); ++R)
		{
			if (R != I.Player && I.Colours[R] == 1) I.Colours[R] = I.Colours[I.Player];
		}
		I.Colours[I.Player] = 1;
		I.Scenery.resize(I.Scene.instances.size());
		I.LinkField();
		I.StartCars = I.Cars;
		I.Camera.reset(I.Cars[I.Player], I.Terrain);
		I.CameraPrev = I.Camera.now;
		return Race;
	}
	catch (const std::exception& E)
	{
		OutError = FString::Printf(TEXT("%s: %s"), *Level, UTF8_TO_TCHAR(E.what()));
		return nullptr;
	}
}

int32 FBRRRace::NumRacers() const { return int32(Impl->Cars.size()); }
int32 FBRRRace::PlayerIndex() const { return Impl->Player; }
bool FBRRRace::IsGhost(int32 Racer) const { return Impl->Tracks[Racer].valid(); }
FString FBRRRace::VehicleOf(int32 Racer) const { return Impl->VehicleNames[Racer]; }
int32 FBRRRace::ColourOf(int32 Racer) const { return Impl->Colours[Racer]; }
FTransform FBRRRace::RacerTransform(int32 Racer) const { return CarTransform(Impl->Cars[Racer]); }

TArray<FVector> FBRRRace::GhostPath(int32 Racer) const
{
	TArray<FVector> Path;
	const FImpl& I = *Impl;
	if (!I.Tracks[Racer].valid()) return Path;
	try
	{
		// A private replay of the tape from the racer's grid pose, at the original's normal rate.
		bigred::VehicleSim Car = I.StartCars[Racer];
		bigred::GhostDriver Driver;
		Driver.start(I.Tracks[Racer], Car);
		const int32 End = (int32(I.Tracks[Racer].records.size()) << 12) - 1;
		while (Driver.time < End)
		{
			Driver.tick(Car);
			Path.Add(BRR::GameToUE(Car.px / 512.0, Car.py / 512.0, Car.pz / 512.0));
		}
	}
	catch (const std::exception& E)
	{
		UE_LOG(LogTemp, Error, TEXT("FBRRRace::GhostPath: %s"), UTF8_TO_TCHAR(E.what()));
	}
	return Path;
}

void FBRRRace::Tick(const FTransform& PlayerPose, const FVector& PlayerVelocity)
{
	FImpl& I = *Impl;
	try
	{
		// The player, mirrored into its proxy: position, heading and velocity, then the ground
		// sampling a replayed car gets, so every field the rules read is current.
		bigred::VehicleSim& P = I.Cars[I.Player];
		const FVector G = UEToGame(PlayerPose.GetLocation());
		P.px = int32(FMath::RoundToInt64(G.X * 512.0));
		P.py = int32(FMath::RoundToInt64(G.Y * 512.0));
		P.pz = int32(FMath::RoundToInt64(G.Z * 512.0));
		P.yaw = int32(FMath::RoundToInt64(PlayerPose.Rotator().Yaw * 65536.0 / 360.0)) << 8;
		P.pitch = P.roll = 0;
		const FVector V = UEToGame(PlayerVelocity) * (512.0 * TickSeconds); // 1/512 units per tick
		P.speed = int32(FMath::Clamp(FMath::Sqrt(V.X * V.X + V.Z * V.Z), 0.0, double(0xFFFA)));
		P.travel = int32(FMath::RoundToInt64(FMath::Atan2(V.X, V.Z) * 65536.0 / (2.0 * PI))) & 0xFFFF;
		P.dress_replayed();

		// The destructible half of VehicleSim::hit_scenery: whatever prop the body's box touches
		// goes down, for good, and on the tick it goes down it throws the car off as any blocker
		// does (ORR's kDestructiblesPushBack; VehicleSim::resolve_contact). Everything else the
		// car hits is static decor, and Chaos's.
		bool bBounced = false;
		if (P.tune && !I.Terrain.blockers.empty())
		{
			bigred::ObjectContact Contacts[bigred::VehicleSim::kMaxSceneryContacts];
			const int32 Hits = I.Terrain.blocker_probe(P.px >> 9, P.py >> 9, P.pz >> 9, P.yaw >> 8,
				I.Terrain.height_fx(P.px, P.pz) >> 9, P.tune->box, Contacts, bigred::VehicleSim::kMaxSceneryContacts);
			for (int32 C = 0; C < Hits; ++C)
			{
				const bigred::SimObject* Object = Contacts[C].object;
				if (!Object || !Object->destructible || !I.Scenery.wreck(Object->instance_index, P.tick)) continue;
				// The reflection off the contact plane at half restitution: the part of the speed
				// going into the piece comes back halved, the part along its face is kept.
				const int32 Normal = Contacts[C].normal_bam16;
				int32 Into = (P.travel - (Normal + 0x8000)) & 0xFFFF;
				if (Into > 0x8000) Into -= 0x10000;
				if (Into >= 0x4000 || Into <= -0x4000) continue; // running away from it
				const int32 Inward = -bigred::fx::mul15(P.speed, bigred::fx::cos16(Into)) / 2;
				const int32 Along = bigred::fx::mul15(P.speed, bigred::fx::sin16(Into));
				P.speed = bigred::fx::hypot32(Along, Inward);
				P.travel = (Normal + 0x8000 + bigred::fx::atan2_bam16(Along, Inward)) & 0xFFFF;
				bBounced = true;
			}
		}
		if (bBounced)
		{
			// Back to UE: the game's (x, z) ground velocity in 1/512 units per tick.
			const double Angle = double(P.travel) * 2.0 * PI / 65536.0;
			const double PerSecond = double(P.speed) / (512.0 * TickSeconds) * BRR::CmPerUnit;
			I.PlayerBounce = FVector2D(FMath::Cos(Angle), FMath::Sin(Angle)) * PerSecond;
		}

		I.CameraPrev = I.Camera.now;
		I.Camera.step(P, I.Terrain);

		I.LinkField();
		I.Race.tick(I.Field.data(), int(I.Field.size()));
		if (I.Race.started())
		{
			for (size_t B = 0; B < I.Cars.size(); ++B)
			{
				if (!I.Tracks[B].valid()) continue;
				I.Ghosts[B].rate = I.GhostRate;
				I.Ghosts[B].tick(I.Cars[B]);
				I.Cars[B].dress_replayed();
			}
			I.RubberBand();
		}
	}
	catch (const std::exception& E)
	{
		UE_LOG(LogTemp, Error, TEXT("FBRRRace::Tick: %s"), UTF8_TO_TCHAR(E.what()));
	}
}

FTransform FBRRRace::PlayerCameraTransform(double Alpha) const
{
	// camera_view works in ORR's render space (X mirrored about the map, +Y up); at scale 1 that
	// is game units, so map it back to the game frame and on to UE.
	const FImpl& I = *Impl;
	const bigred::CameraView V = bigred::camera_view(I.CameraPrev, I.Camera.now, float(Alpha), I.Scene.map_w, 1.0f);
	const double Mirror = double(I.Scene.map_w - 1) * bigred::kTerrainTexelWorldSize;
	const FVector Eye = BRR::GameToUE(Mirror - V.eye[0], -V.eye[1], V.eye[2]);
	auto Dir = [](const float D[3]) { return FVector(D[2], -D[0], D[1]); }; // render dir -> UE
	const FVector Ahead = Dir(V.forward).GetSafeNormal2D();
	// The view looks at the controller's aim point - the car lifted by the preset's aim height,
	// or the tunnel one - not at the car, which ORR's bearing pitches to. Measured on captures of
	// the original (WATER1 and DIRT1 grids, Tools/dosbox-x): the car sits ~16 degrees below the
	// view centre in both, which this reproduces; ORR's port kept it dead centre.
	const bigred::VehicleSim& Car = I.Cars[I.Player];
	const int32 AimHeight = Car.in_tunnel ? bigred::kTunnelAimHeight : bigred::kCameraPresets[I.Camera.preset].aim_height;
	const FVector Aim = BRR::GameToUE(Car.world_x(), Car.world_y() - AimHeight, Car.world_z());
	const FVector ToAim = Aim - Eye;
	const double Pitch = FMath::Atan2(ToAim.Z, ToAim.Size2D());
	const FVector Forward = Ahead * FMath::Cos(Pitch) + FVector::UpVector * FMath::Sin(Pitch);
	return FTransform(FRotationMatrix::MakeFromXZ(Forward, FVector::UpVector).ToQuat(), Eye);
}

void FBRRRace::CyclePlayerCamera() { Impl->Camera.cycle_preset(); }

double FBRRRace::WreckSeconds(int32 Instance) const
{
	const FImpl& I = *Impl;
	if (!I.Scenery.is_wrecked(Instance)) return -1.0;
	return double(I.Cars[I.Player].tick - I.Scenery.wreck_tick[Instance]) * TickSeconds;
}

void FBRRRace::KnockOver(int32 Instance) { Impl->Scenery.wreck(Instance, Impl->Cars[Impl->Player].tick); }

uint32 FBRRRace::WreckRevision() const { return Impl->Scenery.revision; }

bool FBRRRace::ConsumePlayerBounce(FVector2D& OutVelocityXY)
{
	if (!Impl->PlayerBounce.IsSet()) return false;
	OutVelocityXY = *Impl->PlayerBounce;
	Impl->PlayerBounce.Reset();
	return true;
}

bool FBRRRace::HasStarted() const { return Impl->Race.started(); }
int32 FBRRRace::CountdownNumber() const { return Impl->Race.countdown_number(); }
bool FBRRRace::CountdownBeeped() const { return Impl->Race.countdown_beeped(); }
bool FBRRRace::IsRaceOver() const { return Impl->Race.race_over(); }
int32 FBRRRace::Laps() const { return Impl->Race.laps(); }
double FBRRRace::RaceTime() const { return Impl->Race.race_ticks() * bigred::kGameTickMs / 1000.0; }

FBRRRacerStatus FBRRRace::Status(int32 Racer) const
{
	const bigred::RacerProgress& P = Impl->Race.progress(Racer);
	FBRRRacerStatus S;
	S.Lap = P.lap;
	S.Place = P.place;
	S.bFinished = P.finished;
	S.bWrongWay = P.wrong_way;
	S.bMissedCheckpoint = P.missed_check;
	S.bCheckpointThisTick = P.checkpoint_crossed;
	S.bLapThisTick = P.lap_crossed;
	for (const uint32_t Ticks : P.lap_ticks) S.LapTimes.Add(Ticks * bigred::kGameTickMs / 1000.0);
	return S;
}
