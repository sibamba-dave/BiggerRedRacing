#include "BRRRaceSound.h"

#include <cmath>

// Ported from ORR's game/race_audio.cpp (MIT, Copyright (c) 2026 PaPe); the rules and their
// reasons are documented there. What differs is only where the race lives: ORR's RaceSession and
// RaceField, here FBRRRace's cars and RaceState, with the player's car driven by Chaos.

void FBRRRaceSound::Configure(const FConfig& C)
{
	Cfg = C;
	Mixer = BRRAudio::GetMixer();
	if (!Mixer) return;
	const int N = int(Cfg.Vehicles.size());
	const std::string Env = bigred::env_name_for_level(Cfg.Level);
	Sounds.configure(Cfg.GameDir, Cfg.Vehicles[Cfg.Player], Cfg.Level, Env);
	Lines = bigred::parse_commentary(Cfg.LevelTxt);
	// Read now rather than on the first line said: a WAV decoded mid-race is a hitch.
	if (Cfg.bCommentary)
	{
		auto Preload = [&](const char* Name, int Count)
		{
			for (int K = 1; K <= Count; ++K) Sounds.get("lev", std::string(Name) + std::to_string(K));
		};
		Preload("GENRAL", Lines.general);
		Preload("JUMP", Lines.jump);
		Preload("ROLL", Lines.roll);
		Sounds.get("lev", "READYGO");
	}
	Sounds.get("front", "TONE1");
	Sounds.get("front", "TONE2");
	CarAudio.configure(Cfg.Tunings[Cfg.Player], &Sounds, Mixer);
	OtherAudio.assign(size_t(N), bigred::VehicleAudio());
	OtherBanks.clear();
	for (int I = 0; I < N; ++I)
	{
		if (I == Cfg.Player || !Cfg.Tunings[I]) continue;
		std::unique_ptr<bigred::SoundBank>& Bank = OtherBanks[Cfg.Vehicles[I]];
		if (!Bank)
		{
			Bank = std::make_unique<bigred::SoundBank>();
			Bank->configure(Cfg.GameDir, Cfg.Vehicles[I], Cfg.Level, Env);
		}
		OtherAudio[size_t(I)].configure(Cfg.Tunings[I], Bank.get(), Mixer);
	}
	Cfg.WreckColours.resize(size_t(N), 0);
}

void FBRRRaceSound::OnRaceStart()
{
	if (Cfg.bCommentary) Play("lev", "READYGO", 256, true);
}

void FBRRRaceSound::Stop()
{
	CarAudio.stop();
	for (bigred::VehicleAudio& A : OtherAudio) A.stop();
	if (Mixer) Mixer->stop_all();
}

void FBRRRaceSound::ListenerTo(const bigred::CameraState& Cam, const bigred::VehicleSim& C, int32_t& Distance, int32_t& Pan)
{
	const double Dx = double(C.px >> 9) - double(Cam.x);
	const double Dy = double(C.py >> 9) - double(Cam.y);
	const double Dz = double(C.pz >> 9) - double(Cam.z);
	const double D = std::sqrt(Dx * Dx + Dy * Dy + Dz * Dz);
	Distance = D > 0x7fffffff ? 0x7fffffff : int32_t(D);
	Pan = 128;
	const double Flat = std::sqrt(Dx * Dx + Dz * Dz);
	if (Distance <= 99 || Flat < 1.0) return;
	const double Yaw = (Cam.yaw & 0xFFFF) * (6.283185307179586 / 65536.0);
	const double Side = (Dx * std::cos(Yaw) - Dz * std::sin(Yaw)) / Flat;
	Pan = FMath::Clamp(128 + int32_t(Side * 128.0), 0, 256);
}

void FBRRRaceSound::Play(const char* Dir, const std::string& Name, int32_t Volume, bool bSpeech)
{
	if (!Mixer) return;
	const bigred::Sample* S = Sounds.get(Dir, Name);
	if (!S) return;
	bigred::VoiceParams P;
	P.rate = S->rate;
	P.volume = Volume;
	P.speech = bSpeech;
	const bigred::VoiceId Id = Mixer->play(*S, P, false, 0, -1, false);
	if (bSpeech) SpeechVoice = Id;
}

void FBRRRaceSound::Say(const char* Name, int Count, int& Last)
{
	if (!Cfg.bCommentary || Count <= 0) return;
	if (Mixer && Mixer->playing(SpeechVoice)) return; // one line at a time; a late one is dropped
	Dice = Dice * 1103515245u + 12345u;
	int V = int((Dice >> 16) & 0x7fff) % Count;
	if (V == Last) V = (Last + 1) % Count;
	Last = V;
	Play("lev", std::string(Name) + std::to_string(V + 1), 256, true);
}

void FBRRRaceSound::OnTick(const bigred::RaceState& Race, const std::vector<bigred::VehicleSim>& Cars, const bigred::CameraState& Listener,
	bool bPlayerHitCar, bool bRespawned)
{
	if (!Mixer) return;
	const int Player = Cfg.Player;
	int32_t Dist = 0, Pan = 128;
	ListenerTo(Listener, Cars[size_t(Player)], Dist, Pan);
	CarAudio.on_tick(Cars[size_t(Player)], Dist, 128);
	for (int I = 0; I < int(Cars.size()) && I < int(OtherAudio.size()); ++I)
	{
		if (I == Player || !Cfg.Tunings[I]) continue;
		int32_t D = 0, P = 128;
		ListenerTo(Listener, Cars[size_t(I)], D, P);
		OtherAudio[size_t(I)].on_tick(Cars[size_t(I)], D, P);
	}
	// Car against car: the sound belongs to the car that was hit. Only the player is hit here
	// (FBRRRace's contact is the player's half).
	if (bPlayerHitCar) CarAudio.on_car_contact(false, Dist, 128);
	const bigred::RacerProgress& P = Race.progress(Player);
	if (P.wrong_way_now && !P.finished) CarAudio.on_race_event(3, Dist, 128);
	else if (P.missed_window_now) CarAudio.on_race_event(4, Dist, 128);
	else if (P.missed_check_now) CarAudio.on_race_event(2, Dist, 128);
	else if (P.lap_crossed) CarAudio.on_race_event(1, Dist, 128);
	else if (P.checkpoint_crossed) CarAudio.on_race_event(0, Dist, 128);
	Commentary(Race, Cars, bRespawned);
	// TONE1 on three, two, one and TONE2 on the green light.
	if (Race.countdown_beeped()) Play("front", Race.countdown_number() == 0 ? "TONE2" : "TONE1", 256);
}

void FBRRRaceSound::Commentary(const bigred::RaceState& Race, const std::vector<bigred::VehicleSim>& Cars, bool bRespawned)
{
	// EXTRA3, once each time the car is put back on the track, commentary or not.
	if (bRespawned && !bWasRespawned) Play("front", "EXTRA3", 256);
	bWasRespawned = bRespawned;
	if (!Cfg.bCommentary) return;
	for (int I = 0; I < int(Cars.size()); ++I)
	{
		const bigred::VehicleSim& C = Cars[size_t(I)];
		if (C.damage >= 0 || C.wrecked_ticks != int32_t(WreckSayTicks)) continue;
		static constexpr const char* ColourWord[6] = {"RED", "GREEN", "YELLOW", "BLUE", "ORANGE", "PURPLE"};
		int NoRepeat = -1;
		if (Lines.dead > 1) Say("DEAD", Lines.dead, NoRepeat);
		else Say(ColourWord[Cfg.WreckColours[size_t(I)] % 6], Lines.dead == 1 ? 1 : 2, NoRepeat);
	}
	const bigred::RacerProgress& P = Race.progress(Cfg.Player);
	if (!bFinishSaid && P.finished)
	{
		bFinishSaid = true;
		int NoRepeat = -1;
		Say("FINISH", Lines.finish, NoRepeat);
	}
	const bigred::VehicleSim& V = Cars[size_t(Cfg.Player)];
	if (P.checkpoint_crossed || P.lap_crossed || P.missed_check_now)
	{
		Dice = Dice * 1103515245u + 12345u;
		if ((((Dice >> 16) & 0x7fff) & 3) == 0) Me.GeneralIn = 105;
	}
	if (Me.GeneralIn > 0 && --Me.GeneralIn == 0) Say("GENRAL", Lines.general, LastGeneral);
	if (bRespawned) Me.bDropping = true;
	if (V.near_ground != 0)
	{
		Me.bJumpSaid = false;
		Me.bDropping = false;
	}
	else if (!Me.bJumpSaid && V.air_ticks >= 20 && V.height_above > 220 && !Me.bDropping)
	{
		Me.bJumpSaid = true;
		Dice = Dice * 1103515245u + 12345u;
		if ((((Dice >> 16) & 0x7fff) & 1) != 0) Say("JUMP", Lines.jump, LastJump);
	}
	if (V.rolled_over)
	{
		if (!Me.bRolled) Say("ROLL", Lines.roll, LastRoll);
		Me.bRolled = true;
	}
	else
	{
		Me.bRolled = false;
	}
}
