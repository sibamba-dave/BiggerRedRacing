// Race session and reference-handling measurement over the vendored ORR simulation. The tick
// order and the rubber band follow ORR sim/race_session.cpp (MIT, Copyright (c) 2026 PaPe); the
// player's car is fed from UE instead of being stepped by the original physics.
#include "BRRRace.h"

#include "BRRConvert.h"
#include "BRRRaceSound.h"

#include <cmath>
#include <exception>
#include <memory>
#include <vector>

#include "formats/game_files.h"
#include "formats/level_script.h"
#include "formats/texture.h"
#include "formats/vehicle_script.h"
#include "scene/chase_camera.h"
#include "scene/dashboard.h"
#include "scene/particle_render.h"
#include "sim/particles.h"
#include "BRRMeshConvert.h"
#include "scene/level_scene.h"
#include "scene/vehicle_render.h"
#include "sim/race.h"
#include "sim/race_rules.h"
#include "sim/replay.h"
#include "sim/vehicle.h"

using namespace BRR::Private;

namespace
{
	// The rev counter (0-250, the engine's pitch) the original's gear table gives for the car's
	// forward speed - VehicleSim::gear_curve's rev half, which is private to it. Wholly airborne
	// the original leaves the table alone and the revs climb 5 a tick.
	void PlayerGearbox(bigred::VehicleSim& P)
	{
		if (!P.tune) return;
		if (P.near_ground == 0)
		{
			P.rev = FMath::Min(P.rev + 5, 250);
			return;
		}
		const int32 Speed = P.vfwd >= 0 ? P.vfwd : -P.vfwd;
		const std::vector<bigred::GearRow>& Rows = P.vfwd >= 0 ? P.tune->gear_f : P.tune->gear_b;
		if (Rows.empty()) return;
		const size_t Last = std::min<size_t>(Rows.size(), 12) - 1;
		for (size_t R = 0; R < Last; ++R)
		{
			if (Speed < Rows[R + 1].speed)
			{
				const int32 Span = FMath::Max(Rows[R + 1].speed - Rows[R].speed, 1);
				P.gear = Rows[R].gear;
				P.rev = Rows[R].rev_lo + int32(int64(Rows[R].rev_hi - Rows[R].rev_lo) * (Speed - Rows[R].speed) / Span);
				return;
			}
		}
	}
}

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

	// The original's silhouette test (VehicleSim::hit_vehicles' narrow phase, private to the
	// vendored sim): two 32-sector silhouettes facing `Bearing` (A -> B) at `Dist`, the centre
	// pair first, then the neighbouring pairs i = 1..6 on either side, each against Dist / cos(i).
	bool SilhouetteContact(const int32_t* ReachA, int32 YawA, const int32_t* ReachB, int32 YawB, int32 Bearing, int32 Dist,
		int32& OutDepth, int32& OutSectorB)
	{
		static constexpr int32 Secant[8] = { 65536, 66820, 70936, 78819, 92682, 117962, 171254, 335926 };
		auto SectorOf = [](int32 B, int32 Y) { return int32(((B + 0x400 - Y) >> 11) & 0x1f); };
		const int32 SA = SectorOf(Bearing, YawA), SB = SectorOf(Bearing + 0x8000, YawB);
		if (ReachA[SA] + ReachB[SB] > Dist)
		{
			OutDepth = ReachA[SA] + ReachB[SB] - Dist;
			OutSectorB = SB;
			return true;
		}
		for (int32 K = 1; K < 7; ++K)
		{
			const int32 Far = int32((int64(Dist) * Secant[K]) >> 16);
			for (int32 Side = 0; Side < 2; ++Side)
			{
				const int32 IA = (Side == 0 ? SA + K : SA - K) & 0x1f;
				const int32 IB = (Side == 0 ? SB - K : SB + K) & 0x1f;
				const int32 Sum = ReachA[IA] + ReachB[IB];
				if (Sum <= Far) continue;
				OutDepth = int32((int64(Sum - Far) << 16) / Secant[K]);
				OutSectorB = IB;
				return true;
			}
		}
		return false;
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
	FVector CameraAim = FVector::ZeroVector, CameraAimPrev = FVector::ZeroVector; // UE, the view's target at the last two ticks
	double CameraOpen = 1.0, CameraOpenPrev = 1.0; // 1 with open sky over the camera, 0 under a roof

	// Where the view looks: the controller's aim point - the car lifted by the preset's aim
	// height, or the tunnel one.
	FVector AimOf(const bigred::VehicleSim& Car) const
	{
		const int32 AimHeight = Car.in_tunnel ? bigred::kTunnelAimHeight : bigred::kCameraPresets[Camera.preset].aim_height;
		return BRR::GameToUE(Car.world_x(), Car.world_y() - AimHeight, Car.world_z());
	}
	bigred::SceneryState Scenery;
	bigred::NitroReserve Nitros;
	double BoostAccel = 0; // cm/s/s along the player's car this tick
	TOptional<FVector2D> PlayerBounce; // UE ground velocity, cm/s, from a destructible's push-back
	FVector2D PlayerShove = FVector2D::ZeroVector; // UE cm, out of another car (PlayerContact)
	double PlayerSpin = 0;             // UE yaw rate, degrees/s, from another car
	bool bPlayerContact = false;
	int32 GhostRate = 0x200;
	std::string GameDir, LevelName, Script; // for the sound
	int32 PendingImpact = 0;                // 1/512 units per tick, the next tick's knock
	TOptional<FVector> PendingSpark;        // game units: where it struck (the impact sparks)
	bool bRecovered = false;
	std::unique_ptr<FBRRRaceSound> Sound;
	bigred::Dashboard Dash;   // the player's, when its vehicle has one
	bool bCabin = false;      // the cabin view is on
	bigred::ParticleField Particles;
	bigred::ParticleVisuals ParticleLooks;
	int32 RaceParticleBase = 0;   // where this scene's copy of the sheets starts
	int32 WorldParticleBase = INDEX_NONE;
	uint32 TimerTicks = 0;        // the original's 14 ms counter, which the emitter reads
	uint32 BandClock = 0;
	bool bSimPlayer = false;      // the player runs on the original's physics (SetPlayerSimulated)

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
			// Nobody is stepped by the original physics here - but a simulated player.
			Cars[I].replayed = !(bSimPlayer && int32(I) == Player);
			Cars[I].world = &Scenery;
		}
	}

	// The half of a tick after the player's: the camera, the rules, the ghosts, the ground
	// effects and the sound.
	void AdvanceField(bool bCarContact)
	{
		bigred::VehicleSim& P = Cars[Player];
		CameraPrev = Camera.now;
		Camera.step(P, Terrain);
		CameraAimPrev = CameraAim;
		CameraAim = AimOf(P);
		CameraOpenPrev = CameraOpen;
		CameraOpen += ((P.in_tunnel || Camera.under_roof() ? 0.0 : 1.0) - CameraOpen) * 0.15;

		LinkField();
		Race.tick(Field.data(), int(Field.size()));
		if (Race.started())
		{
			for (size_t B = 0; B < Cars.size(); ++B)
			{
				if (!Tracks[B].valid()) continue;
				Ghosts[B].rate = GhostRate;
				Ghosts[B].tick(Cars[B]);
				Cars[B].dress_replayed();
			}
			RubberBand();
		}
		// The ground effects, in the original's order: every car emits right after the physics,
		// then the field ages twice - it runs on the 14 ms timer, the physics on every other tick.
		for (bigred::VehicleSim& Car : Cars)
		{
			if (Car.tune) Particles.emit(Car, TimerTicks);
		}
		Particles.tick();
		Particles.tick();
		TimerTicks += 2;
		if (Sound)
		{
			Sound->OnTick(Race, Cars, Camera.now, bCarContact, bRecovered);
			bRecovered = false;
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
		I.GameDir = GameDir;
		I.LevelName = Name;
		I.Script = Script;

		const std::vector<std::string> Cars = bigred::parse_car_list(Script);
		const bigred::StartingGrid Grid = bigred::parse_grid(Script);
		const bigred::StartPlaces Places = bigred::parse_start_places(Script);
		const bigred::NitroReserve Nitros = bigred::parse_nitros(Script);
		I.Nitros = Nitros;
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

		{
			// The player's dashboard: its sheet and gauges come from the .VHC itself.
			bigred::VehicleTuning DashTune;
			bigred::ArchivePtr Vhc;
			if (bigred::load_vehicle(GameDir, ToUtf8(PlayerVehicle.ToUpper()), DashTune, &Vhc) && Vhc) I.Dash.load(*Vhc, DashTune);
		}
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
		I.CameraAim = I.CameraAimPrev = I.AimOf(I.Cars[I.Player]);
		{
			const std::string Env = bigred::env_name_for_level(Name);
			const bigred::ArchivePtr EnvArchive = bigred::game_archive_or_empty(GameDir, Env + ".ENV");
			std::vector<bigred::SpotEffect> Spots;
			if (const bigred::Blob* Txt = bigred::find_member(*EnvArchive, Env + ".TXT")) Spots = bigred::parse_spot_effects(std::string(Txt->begin(), Txt->end()));
			I.Particles.configure(Spots);
			I.RaceParticleBase = I.Scene.atlas_count;
			I.ParticleLooks = bigred::build_particle_visuals(Spots, *EnvArchive, I.Scene);
		}
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

// The player's half of VehicleSim::hit_vehicles against every other car, on the proxy: the
// opponents are replays, obstacles that are never pushed, so this is all the contact there is -
// and it is the original's, where Chaos left alone teleports a solid box into the player and
// shoves it out (a boat rammed off the grid spun ~140 degrees). Side on, the speed into the
// other car comes back at half restitution and the other's sideways speed gives a small spin;
// then the body is pushed out by the depth plus the original's 4-unit margin. Returns whether
// the velocity changed; the push and the spin are left in PlayerShove / PlayerSpin.
template <typename FRaceImpl> // FBRRRace::FImpl, which is private
static bool ContactCars(FRaceImpl& I, bigred::VehicleSim& P)
{
	namespace fx = bigred::fx;
	bool bVelocity = false;
	const int32 X0 = P.px, Z0 = P.pz;
	int32 YawRate = 0;
	for (size_t J = 0; J < I.Cars.size(); ++J)
	{
		const bigred::VehicleSim& O = I.Cars[J];
		if (int32(J) == I.Player || !O.active || !O.tune) continue;
		const int32 X = P.px >> 9, Y = P.py >> 9, Z = P.pz >> 9;
		const int32 DX = (O.px >> 9) - X, DZ = (O.pz >> 9) - Z;
		if (DX > 0x1000 || DX < -0x1000 || DZ > 0x1000 || DZ < -0x1000) continue;
		const bigred::VehicleBox& SB = P.tune->box;
		const bigred::VehicleBox& OB = O.tune->box;
		const int64 RR = int64(SB.radius) + OB.radius;
		if (int64(DX) * DX + int64(DZ) * DZ > RR * RR) continue;
		const int32 Dist = fx::hypot32(DX, DZ);
		const int32 Bearing = fx::atan2_bam16(DX, DZ) & 0xFFFF;
		int32 Depth = 0, OS = 0;
		if (!SilhouetteContact(SB.sector_reach.data(), P.yaw >> 8, OB.sector_reach.data(), O.yaw >> 8, Bearing, Dist, Depth, OS)) continue;
		const int32 OY = O.py >> 9; // +Y is down
		if (Y + SB.ytop > OY + OB.ybottom || OY + OB.ytop > Y + SB.ybottom) continue; // under or over it

		const int32 NB = FMath::Clamp(int32(OB.sector_normal[size_t(OS)]), 0x10, 0xf0);
		const int32 N0 = ((O.yaw >> 8) - (NB << 8)) & 0xFFFF; // the other car's outward normal
		const int32 N = (N0 - 0x8000) & 0xFFFF;
		const int32 ASelf = (N - P.travel) & 0xFFFF, AOther = (N - O.travel) & 0xFFFF;
		const int32 SSin = fx::mul15(P.speed, fx::sin16(ASelf)), SCos = fx::mul15(P.speed, fx::cos16(ASelf));
		const int32 OCos = fx::mul15(O.speed, fx::cos16(AOther)), OSin = fx::mul15(O.speed, fx::sin16(AOther));
		const int32 Closing = SCos - OCos;
		if (Closing >= 0)
		{
			const int32 Back = (Closing * 3) / 2 - SCos;
			P.travel = (N0 + fx::atan2_bam16(SSin, Back)) & 0xFFFF;
			P.speed = fx::hypot32(Back, SSin);
			YawRate = -OSin / 400;
			bVelocity = true;
		}
		const int32 Out = Depth + 4;
		P.px += fx::mul15(Out, fx::sin16(N0)) << 9;
		P.pz += fx::mul15(Out, fx::cos16(N0)) << 9;
		I.bPlayerContact = true;
	}
	if (I.bPlayerContact)
	{
		// Game (x, z) is UE (Y, X); yaw is BAM16 << 8 per tick either way round.
		I.PlayerShove = FVector2D(double(P.pz - Z0), double(P.px - X0)) / 512.0 * BRR::CmPerUnit;
		I.PlayerSpin = double(YawRate) / 256.0 * 360.0 / 65536.0 / FBRRRace::TickSeconds;
	}
	return bVelocity;
}

void FBRRRace::Tick(const FTransform& PlayerPose, const FVector& PlayerVelocity, bool bBoostKey)
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
		// The rest of what the car's sound reads (VehicleAudio::on_tick), which a replayed car is
		// never given: the touchdown, the time aloft, rolled over, the revs, this tick's knocks.
		P.vy = int32(FMath::RoundToInt64(V.Y));
		P.landing_impact = (P.near_ground_prev == 0 && P.near_ground != 0 && P.vy > 0) ? P.vy : 0;
		P.air_ticks = P.near_ground != 0 ? 0 : P.air_ticks + 1;
		P.rolled_over = PlayerPose.GetUnitAxis(EAxis::Z).Z < 0.0;
		PlayerGearbox(P);
		P.scenery_hit = I.PendingImpact > 0;
		P.impact_magnitude = I.PendingImpact;
		P.scenery_wrecked = -1;
		P.nitro_refused = false;
		P.just_destroyed = false;
		I.PendingImpact = 0;
		if (I.PendingSpark)
		{
			P.spark_x = int32(I.PendingSpark->X);
			P.spark_y = int32(I.PendingSpark->Y);
			P.spark_z = int32(I.PendingSpark->Z);
			P.spark_pending = true;
			I.PendingSpark.Reset();
		}

		// The boost, in VehicleSim::step's order: read_input counts a running burst down,
		// update_nitro fires one off the reserve (mode 1 on the key's edge, mode 2 while held),
		// and run_engine adds its thrust - all of `push` well under the cap, tapering to nothing
		// over the last `cap - nitro[3]`, pulling back past it.
		I.BoostAccel = 0;
		if (P.tune && (P.tune->nitro[0] == 1 || P.tune->nitro[0] == 2))
		{
			const int32_t* N = P.tune->nitro;
			if (P.boost_timer > 0) --P.boost_timer;
			const bool bFire = bBoostKey && (N[0] == 2 || !P.prev_nitro_key);
			P.nitro_refused = bFire && P.nitro_fuel <= 0 && N[0] == 1;
			if (bFire && P.nitro_fuel > 0 && P.boost_timer <= 0)
			{
				--P.nitro_fuel;
				P.boost_timer = N[1];
			}
			P.prev_nitro_key = bBoostKey;
			if (P.boost_timer > 0)
			{
				const int32 Cap = N[4], Span = N[4] - N[3], Push = N[2];
				const int32 Gap = Cap - P.vfwd;
				const int32 Boost = Gap <= 0 ? Gap : Gap > Span ? Push : int32(int64(Gap) * Push / FMath::Max(Span, 1));
				// vfwd is 1/512 world units per tick; Boost is added to it once a tick.
				I.BoostAccel = double(Boost) / 512.0 * BRR::CmPerUnit / (TickSeconds * TickSeconds);
			}
		}

		// The destructible half of VehicleSim::hit_scenery: whatever prop the body's box touches
		// goes down, for good, and on the tick it goes down it throws the car off as any blocker
		// does (ORR's kDestructiblesPushBack; VehicleSim::resolve_contact). Everything else the
		// car hits is static decor, and Chaos's.
		bool bBounced = false, bCarContact = false;
		if (P.tune && !I.Terrain.blockers.empty())
		{
			bigred::ObjectContact Contacts[bigred::VehicleSim::kMaxSceneryContacts];
			const int32 Hits = I.Terrain.blocker_probe(P.px >> 9, P.py >> 9, P.pz >> 9, P.yaw >> 8,
				I.Terrain.height_fx(P.px, P.pz) >> 9, P.tune->box, Contacts, bigred::VehicleSim::kMaxSceneryContacts);
			for (int32 C = 0; C < Hits; ++C)
			{
				const bigred::SimObject* Object = Contacts[C].object;
				if (!Object || !Object->destructible || !I.Scenery.wreck(Object->instance_index, P.tick)) continue;
				P.scenery_wrecked = Object->instance_index;
				P.scenery_wrecked_var = Object->splat_var;
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
		bCarContact = ContactCars(I, P);
		bBounced |= bCarContact;
		if (bBounced)
		{
			// Back to UE: the game's (x, z) ground velocity in 1/512 units per tick.
			const double Angle = double(P.travel) * 2.0 * PI / 65536.0;
			const double PerSecond = double(P.speed) / (512.0 * TickSeconds) * BRR::CmPerUnit;
			I.PlayerBounce = FVector2D(FMath::Cos(Angle), FMath::Sin(Angle)) * PerSecond;
		}

		I.AdvanceField(bCarContact);
	}
	catch (const std::exception& E)
	{
		UE_LOG(LogTemp, Error, TEXT("FBRRRace::Tick: %s"), UTF8_TO_TCHAR(E.what()));
	}
}

void FBRRRace::SetPlayerSimulated(bool bOn)
{
	Impl->bSimPlayer = bOn;
	Impl->LinkField();
}

bool FBRRRace::IsPlayerSimulated() const { return Impl->bSimPlayer; }

void FBRRRace::TickSimulated(const FInput& In)
{
	FImpl& I = *Impl;
	try
	{
		bigred::VehicleSim& P = I.Cars[I.Player];
		P.replayed = false;
		// The physics is frozen through the countdown (VehicleSim::place_on_grid).
		if (I.Race.started())
		{
			bigred::VehicleInput Orders;
			Orders.accel = In.bAccel;
			Orders.brake = In.bBrake;
			Orders.left = In.bLeft;
			Orders.right = In.bRight;
			Orders.nitro = In.bBoost;
			Orders.recover = In.bRecover;
			P.step(Orders);
			I.bRecovered |= In.bRecover;
		}
		I.BoostAccel = 0; // the step's own thrust
		I.AdvanceField(P.vehicle_hit >= 0);
	}
	catch (const std::exception& E)
	{
		UE_LOG(LogTemp, Error, TEXT("FBRRRace::TickSimulated: %s"), UTF8_TO_TCHAR(E.what()));
	}
}

FVector FBRRRace::PlayerSimVelocity() const
{
	// The game's polar ground velocity and its vertical one, 1/512 units per tick (+Y down).
	const bigred::VehicleSim& P = Impl->Cars[Impl->Player];
	const double Angle = double(P.travel) * 2.0 * PI / 65536.0;
	const double Scale = BRR::CmPerUnit / (512.0 * TickSeconds);
	return FVector(FMath::Cos(Angle) * P.speed * Scale, FMath::Sin(Angle) * P.speed * Scale, -P.vy * Scale);
}

void FBRRRace::RecoverPlayerAt(const FTransform& Pose)
{
	FImpl& I = *Impl;
	bigred::VehicleSim& P = I.Cars[I.Player];
	const FVector G = UEToGame(Pose.GetLocation());
	P.respawn.x = int32(FMath::RoundToInt64(G.X));
	P.respawn.z = int32(FMath::RoundToInt64(G.Z));
	P.respawn.yaw_bam16 = int32(FMath::RoundToInt64(Pose.Rotator().Yaw * 65536.0 / 360.0)) & 0xFFFF;
	P.recover();
	I.bRecovered = true;
}

void FBRRRace::EnableSound(bool bCommentary)
{
	FImpl& I = *Impl;
	if (I.Sound || !BRRAudio::GetMixer()) return;
	try
	{
		FBRRRaceSound::FConfig C;
		C.GameDir = I.GameDir;
		C.Level = I.LevelName;
		C.LevelTxt = I.Script;
		C.Player = I.Player;
		C.bCommentary = bCommentary;
		for (size_t R = 0; R < I.Cars.size(); ++R)
		{
			C.Vehicles.push_back(ToUtf8(I.VehicleNames.IsValidIndex(int32(R)) ? I.VehicleNames[int32(R)] : FString()));
			C.Tunings.push_back(I.Cars[R].tune);
			C.WreckColours.push_back(I.Colours.IsValidIndex(int32(R)) ? FMath::Max(I.Colours[int32(R)] - 1, 0) : 0);
		}
		I.Sound = std::make_unique<FBRRRaceSound>();
		I.Sound->Configure(C);
		I.Sound->OnRaceStart();
	}
	catch (const std::exception& E)
	{
		UE_LOG(LogTemp, Error, TEXT("FBRRRace::EnableSound: %s"), UTF8_TO_TCHAR(E.what()));
		I.Sound.reset();
	}
}

void FBRRRace::ReportPlayerImpact(double SpeedIntoCmPerSecond, const FVector& Where)
{
	// Sparks for a real knock only (over ~12 km/h into it), not every graze.
	if (!Where.IsZero() && SpeedIntoCmPerSecond > 350.0) Impl->PendingSpark = UEToGame(Where);
	// In the game's own measure, 1/512 units per tick: the strongest knock of the tick counts.
	const int32 Magnitude = int32(FMath::Min(SpeedIntoCmPerSecond / BRR::CmPerUnit * 512.0 * TickSeconds, double(0x7fffffff)));
	Impl->PendingImpact = FMath::Max(Impl->PendingImpact, Magnitude);
}

void FBRRRace::ReportPlayerRecovered() { Impl->bRecovered = true; }

FTransform FBRRRace::PlayerCameraTransform(double Alpha, double Distance, double Lift) const
{
	// camera_view works in ORR's render space (X mirrored about the map, +Y up); at scale 1 that
	// is game units, so map it back to the game frame and on to UE.
	const FImpl& I = *Impl;
	const bigred::CameraView V = bigred::camera_view(I.CameraPrev, I.Camera.now, float(Alpha), I.Scene.map_w, 1.0f);
	const double Mirror = double(I.Scene.map_w - 1) * bigred::kTerrainTexelWorldSize;
	const double Open = FMath::Lerp(I.CameraOpenPrev, I.CameraOpen, Alpha);
	auto Dir = [](const float D[3]) { return FVector(D[2], -D[0], D[1]); }; // render dir -> UE
	const FVector Ahead = Dir(V.forward).GetSafeNormal2D();
	// The view looks at the controller's aim point - the car lifted by the preset's aim height,
	// or the tunnel one - not at the car, which ORR's bearing pitches to. Measured on captures of
	// the original (WATER1 and DIRT1 grids, Tools/dosbox-x): the car sits ~16 degrees below the
	// view centre in both, which this reproduces; ORR's port kept it dead centre.
	// Blended between the last two ticks like the eye: aiming at the latest tick's while the eye
	// is a tick behind it would nod the view at the tick rate.
	const FVector Aim = FMath::Lerp(I.CameraAimPrev, I.CameraAim, Alpha);
	const FVector Original = BRR::GameToUE(Mirror - V.eye[0], -V.eye[1], V.eye[2]);
	const FVector Pull = (Original - Aim) * FVector(1, 1, 0) * (1.0 - FMath::Lerp(1.0, Distance, Open));
	const FVector Behind = Original - Pull;
	// The lift is over the aim point when that is higher: driving at a hill, the original's eye is
	// still down in the dip behind the car, and lifted from there it would stare into the slope.
	const double Base = Lift > 0.0 ? FMath::Max(Behind.Z, Aim.Z) : Behind.Z;
	const FVector Eye(Behind.X, Behind.Y, Behind.Z + Open * (Base - Behind.Z + Lift * FVector::Dist2D(Behind, Aim)));
	const FVector ToAim = Aim - Eye;
	const double Pitch = FMath::Atan2(ToAim.Z, ToAim.Size2D());
	const FVector Forward = Ahead * FMath::Cos(Pitch) + FVector::UpVector * FMath::Sin(Pitch);
	return FTransform(FRotationMatrix::MakeFromXZ(Forward, FVector::UpVector).ToQuat(), Eye);
}

double FBRRRace::PlayerCameraOpenness() const
{
	return Impl->CameraOpen;
}

void FBRRRace::CyclePlayerCamera(bool bCabinAvailable)
{
	FImpl& I = *Impl;
	if (I.bCabin)
	{
		I.bCabin = false;
		I.Camera.cycle_preset(); // from the third height back to the first
	}
	else if (I.Camera.preset == 2 && bCabinAvailable)
	{
		I.bCabin = true;         // the chase camera keeps running underneath
	}
	else
	{
		I.Camera.cycle_preset();
	}
}

void FBRRRace::SetParticleAtlasBase(int32 WorldBase) { Impl->WorldParticleBase = WorldBase; }

void FBRRRace::GetParticles(const FVector& CameraRight, const FVector& CameraUp, FBRRMesh& Out) const
{
	const FImpl& I = *Impl;
	Out = FBRRMesh();
	if (I.WorldParticleBase == INDEX_NONE || I.Particles.live_count() == 0) return;
	// UE directions into ORR's render space (X mirrored about the map, +Y up).
	const float Right[3] = { float(-CameraRight.Y), float(CameraRight.Z), float(CameraRight.X) };
	const float Up[3] = { float(-CameraUp.Y), float(CameraUp.Z), float(CameraUp.X) };
	const std::vector<bigred::TerrainVertex> Verts = bigred::place_particles(I.ParticleLooks, I.Particles, Right, Up, I.Scene.map_w, bigred::kRenderScale);
	const double Mirror = double(I.Scene.map_w - 1) * bigred::kTerrainTexelWorldSize;
	const double InvScale = 1.0 / double(bigred::kRenderScale);
	Out = ConvertTriangles(Verts, 0, Verts.size(), [&](float X, float Y, float Z)
	{
		return BRR::GameToUE(Mirror - X * InvScale, -Y * InvScale, Z * InvScale);
	});
	// This scene's sheets sit at different pages than the import's: same order, another start.
	for (FBRRMeshSection& S : Out.Sections)
	{
		if (S.Atlas >= I.RaceParticleBase) S.Atlas = I.WorldParticleBase + (S.Atlas - I.RaceParticleBase);
	}
}

bool FBRRRace::IsPlayerInCabin() const { return Impl->bCabin; }

bool FBRRRace::DrawDashboard(TArray<FColor>& OutPixels, int32& OutWidth, int32& OutHeight)
{
	FImpl& I = *Impl;
	if (!I.Dash.valid()) return false;
	const bigred::VehicleSim& Car = I.Cars[I.Player];
	bigred::DashValues V;
	V.by_source = {Car.vfwd, Car.rev, Car.nitro_fuel, Car.gear, (I.Terrain.height_fx(Car.px, Car.pz) - Car.py) >> 9};
	I.Dash.draw(V);
	const std::vector<uint8_t> Rgba = bigred::decode_tm_rgba(I.Dash.pixels().data(), I.Scene.palette);
	OutWidth = bigred::kTmWidth;
	OutHeight = bigred::kTmHeight;
	OutPixels.SetNumUninitialized(OutWidth * OutHeight);
	for (int32 P = 0; P < OutPixels.Num(); ++P) OutPixels[P] = FColor(Rgba[P * 4], Rgba[P * 4 + 1], Rgba[P * 4 + 2], Rgba[P * 4 + 3]);
	return true;
}

double FBRRRace::WreckSeconds(int32 Instance) const
{
	const FImpl& I = *Impl;
	if (!I.Scenery.is_wrecked(Instance)) return -1.0;
	return double(I.Cars[I.Player].tick - I.Scenery.wreck_tick[Instance]) * TickSeconds;
}

void FBRRRace::KnockOver(int32 Instance) { Impl->Scenery.wreck(Instance, Impl->Cars[Impl->Player].tick); }

uint32 FBRRRace::WreckRevision() const { return Impl->Scenery.revision; }

FBRRBoost FBRRRace::PlayerBoost() const
{
	const FImpl& I = *Impl;
	FBRRBoost Out;
	const bigred::VehicleSim& Car = I.Cars[I.Player];
	if (!Car.tune || I.Nitros.count <= 0) return Out;
	const int32 Mode = Car.tune->nitro[0];
	if (Mode == 1)
	{
		Out.Mode = 1;
		Out.Count = I.Nitros.count;
		Out.Left = Car.nitro_fuel;
	}
	else if (Mode == 2 && I.Nitros.full_scale_ticks > 0)
	{
		Out.Mode = 2;
		Out.Fraction = double(Car.nitro_fuel) / I.Nitros.full_scale_ticks;
	}
	return Out;
}

int32 FBRRRace::PlayerSurface() const { return Impl->Cars[Impl->Player].land & 7; }

bool FBRRRace::IsPlayerBoosting() const { return Impl->Cars[Impl->Player].boost_timer > 0; }

double FBRRRace::PlayerBoostAccel() const { return Impl->BoostAccel; }

bool FBRRRace::ConsumePlayerBounce(FVector2D& OutVelocityXY)
{
	if (!Impl->PlayerBounce.IsSet()) return false;
	OutVelocityXY = *Impl->PlayerBounce;
	Impl->PlayerBounce.Reset();
	return true;
}

bool FBRRRace::ConsumePlayerCarContact(FVector2D& OutShoveXY, double& OutYawDegreesPerSecond)
{
	if (!Impl->bPlayerContact) return false;
	OutShoveXY = Impl->PlayerShove;
	OutYawDegreesPerSecond = Impl->PlayerSpin;
	Impl->bPlayerContact = false;
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
