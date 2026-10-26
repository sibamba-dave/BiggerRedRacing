#pragma once

#include "CoreMinimal.h"

#include "audio/mixer.h"
#include "audio/sound_bank.h"
#include "audio/vehicle_audio.h"
#include "formats/level_script.h"
#include "scene/chase_camera.h"
#include "sim/race.h"
#include "sim/vehicle.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace BRRAudio { bigred::Mixer* GetMixer(); }

// Everything a race sounds like - every car's engine, skid, roll and nitro voices, contacts, the
// race's own events and the countdown tones, and the circuit's commentator. A port of ORR's
// game/race_audio.cpp (which sits on its RaceSession, not vendored) onto FBRRRace's own field.
// Presentation only: it reads the race once a tick and writes nothing back, and its dice are its own.
class FBRRRaceSound
{
public:
	struct FConfig
	{
		std::string GameDir, Level, LevelTxt;
		std::vector<std::string> Vehicles;                     // per racer
		std::vector<const bigred::VehicleTuning*> Tunings;     // per racer
		std::vector<int> WreckColours;                         // per racer: RED GREEN YELLOW BLUE ORANGE PURPLE
		int Player = 0;
		bool bCommentary = true;
	};

	~FBRRRaceSound() { Stop(); } // the voices point into this object's banks
	void Configure(const FConfig& C);
	bool Ready() const { return Mixer != nullptr; }

	// The starter's line, once.
	void OnRaceStart();
	// Once per race tick, after it has stepped. bPlayerHitCar: another car ran into the player
	// this tick (FBRRRace's contact); bRespawned: the player was put back on the track.
	void OnTick(const bigred::RaceState& Race, const std::vector<bigred::VehicleSim>& Cars, const bigred::CameraState& Listener,
		bool bPlayerHitCar, bool bRespawned);
	void Stop();

	static constexpr uint32 WreckSayTicks = 88; // the DEAD line, 175 timer ticks after the wreck

private:
	void Play(const char* Dir, const std::string& Name, int32_t Volume, bool bSpeech = false);
	void Say(const char* Name, int Count, int& Last);
	void Commentary(const bigred::RaceState& Race, const std::vector<bigred::VehicleSim>& Cars, bool bRespawned);
	static void ListenerTo(const bigred::CameraState& Cam, const bigred::VehicleSim& C, int32_t& Distance, int32_t& Pan);

	bigred::Mixer* Mixer = nullptr;
	FConfig Cfg;
	bigred::SoundBank Sounds;
	bigred::VehicleAudio CarAudio;
	std::unordered_map<std::string, std::unique_ptr<bigred::SoundBank>> OtherBanks;
	std::vector<bigred::VehicleAudio> OtherAudio;

	struct FCommentator
	{
		int32_t GeneralIn = 0;
		bool bJumpSaid = false, bRolled = false, bDropping = false;
	};
	FCommentator Me;
	bigred::LevelCommentary Lines{};
	int LastGeneral = -1, LastJump = -1, LastRoll = -1;
	bool bFinishSaid = false, bWasRespawned = false;
	uint32_t Dice = 0x2545f491u;
	bigred::VoiceId SpeechVoice;
};
