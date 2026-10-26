#pragma once

#include "CoreMinimal.h"

// The game's sound output: the original's samples, mixed as ORR mixes them (bigred::Mixer - eight
// voices, three engines, the original's half-scale volume cap, the commentator ducking the rest),
// with the CD music underneath. BRRGame's sound component pulls the mix from UE's audio thread;
// the race (FBRRRace::EnableSound) and the menus queue sounds into it from the game thread.
namespace BRRAudio
{
	// Starts the mixer at UE's output rate. Idempotent; false if it cannot run.
	BRRFORMATS_API bool Open(int32 SampleRate);
	BRRFORMATS_API void Close();
	BRRFORMATS_API bool IsOpen();

	// Audio thread: adds the mix into Out, Frames stereo frames interleaved, -1..1.
	BRRFORMATS_API void Mix(float* Out, int32 Frames);

	// 0..1 each: the sound effects (and speech), and the music - two settings, as in the original.
	BRRFORMATS_API void SetVolumes(float Effects, float Music);

	// Stops every sound effect (not the music): a race ending, a level change.
	BRRFORMATS_API void StopEffects();

	// For the perf log: voices playing, and whether music is.
	BRRFORMATS_API FString DebugState();
	// Writes the next Seconds of the output to a 16-bit WAV (-BRRAudioDump=<path>: a check by ear,
	// or by numbers, of what a run sounded like).
	BRRFORMATS_API void DumpTo(const FString& Path, float Seconds);

	// Plays one sample from the original's folders once - a menu click, a jingle. Dir is the
	// `hnd fx` prefix: "front" (SAMPLES), "" (GEN), or a folder name.
	BRRFORMATS_API void PlayOnce(const FString& DataDir, const FString& Dir, const FString& Name, float Volume = 1.f);

	// The CD music (README, Tools/brr_cd.py: the rip's Music/TrackNN.wav). The original's model:
	// each context asks for a CD track - `Intro` once, then `Loop` for as long as it lasts (TOURN.TXT
	// gives both per circuit; the front end plays track 2). A track the folder lacks plays another
	// at random, as ORR does; no folder at all is silence, not an error.
	BRRFORMATS_API void PlayMusic(int32 Intro, int32 Loop);
	BRRFORMATS_API void StopMusic();
	// Where the rip's music is: [BRR] MusicDir in Game.ini, else <DataDir>/../Music.
	BRRFORMATS_API FString GetMusicDir();
	// The circuit's two CD tracks from TOURN.TXT (0 when it declares none).
	BRRFORMATS_API void CircuitMusic(const FString& DataDir, const FString& Level, int32& OutIntro, int32& OutLoop);
}
