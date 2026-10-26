#include "BRRAudio.h"

#include "BRRArchive.h"
#include "BRRAssets.h"
#include "HAL/CriticalSection.h"
#include "Math/RandomStream.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"

#include "audio/mixer.h"
#include "audio/sound_bank.h"

#include <memory>
#include <string>
#include <unordered_map>

// ORR's mixer (audio/mixer.h) is declared over SDL; its SDL half, mixer.cpp, is not vendored. This
// file is that half for UE: the same voice table and the same rules, with a critical section where
// SDL held its device lock, and UE's audio thread (BRRAudio::Mix) where SDL called back. The mixing
// itself is mixer.cpp's, line for line.

namespace
{
	FCriticalSection GLock;            // what SDL_LockAudioDevice was
	int32 GRate = 48000;               // what Open was asked for: Mixer::open reads it
	void (*GMixFn)(void*, int16_t*, int) = nullptr; // Mixer::mix, reached through open()
	std::unique_ptr<bigred::Mixer> GMixer;
	float GEffectsVolume = 1.f, GMusicVolume = 0.7f;
	TArray<int16_t> GScratch;          // audio thread only
	FString GDumpPath;                 // DumpTo: where, how much, and what so far
	int32 GDumpFrames = 0;
	TArray<int16> GDump;

	void WriteWav(const FString& Path, const TArray<int16>& Pcm, int32 Rate)
	{
		TArray<uint8> Out;
		auto Put32 = [&](uint32 V) { for (int32 K = 0; K < 4; ++K) Out.Add(uint8(V >> (8 * K))); };
		auto Put16 = [&](uint16 V) { Out.Add(uint8(V)); Out.Add(uint8(V >> 8)); };
		const uint32 Bytes = uint32(Pcm.Num() * 2);
		Out.Append((const uint8*)"RIFF", 4); Put32(36 + Bytes); Out.Append((const uint8*)"WAVEfmt ", 8);
		Put32(16); Put16(1); Put16(2); Put32(Rate); Put32(Rate * 4); Put16(4); Put16(16);
		Out.Append((const uint8*)"data", 4); Put32(Bytes);
		Out.Append((const uint8*)Pcm.GetData(), Bytes);
		FFileHelper::SaveArrayToFile(Out, *Path);
	}

	// --- the music: whole CD tracks in memory, 16-bit stereo at 44.1 kHz ------------------
	struct FTrack
	{
		TArray<int16> Pcm; // interleaved stereo
		int32 Rate = 44100;
		bool IsValid() const { return Pcm.Num() >= 2; }
	};
	TSharedPtr<FTrack> GIntro, GLoop;  // swapped under GLock
	double GMusicPos = 0;              // in source frames, into GIntro then GLoop
	bool GInIntro = false;

	bool LoadWav(const FString& Path, FTrack& Out)
	{
		TArray<uint8> Raw;
		if (!FFileHelper::LoadFileToArray(Raw, *Path)) return false;
		auto Rd32 = [&](int32 P) { return uint32(Raw[P]) | uint32(Raw[P + 1]) << 8 | uint32(Raw[P + 2]) << 16 | uint32(Raw[P + 3]) << 24; };
		auto Rd16 = [&](int32 P) { return uint16(Raw[P] | Raw[P + 1] << 8); };
		if (Raw.Num() < 44 || FMemory::Memcmp(Raw.GetData(), "RIFF", 4) != 0 || FMemory::Memcmp(Raw.GetData() + 8, "WAVE", 4) != 0) return false;
		int32 Pos = 12, Channels = 2, Bits = 16;
		while (Pos + 8 <= Raw.Num())
		{
			const uint32 Id = Rd32(Pos), Len = Rd32(Pos + 4);
			const int32 Body = Pos + 8;
			if (int64(Body) + Len > Raw.Num()) break;
			if (Id == 0x20746d66u) // "fmt "
			{
				Channels = Rd16(Body + 2);
				Out.Rate = int32(Rd32(Body + 4));
				Bits = Rd16(Body + 14);
			}
			else if (Id == 0x61746164u) // "data"
			{
				if (Bits != 16 || (Channels != 1 && Channels != 2)) return false;
				const int32 Frames = int32(Len / (2 * Channels));
				Out.Pcm.SetNumUninitialized(Frames * 2);
				const int16* Src = reinterpret_cast<const int16*>(Raw.GetData() + Body);
				for (int32 F = 0; F < Frames; ++F)
				{
					Out.Pcm[F * 2] = Src[F * Channels];
					Out.Pcm[F * 2 + 1] = Src[F * Channels + Channels - 1];
				}
				return Out.IsValid();
			}
			Pos = Body + int32(Len) + int32(Len & 1);
		}
		return false;
	}

	// TrackNN.wav, or - the folder lacking it - another one at random (ORR's rule, music.h).
	TSharedPtr<FTrack> LoadTrack(int32 Number)
	{
		if (Number <= 0) return nullptr;
		const FString Dir = BRRAudio::GetMusicDir();
		if (Dir.IsEmpty()) return nullptr;
		FString Path = Dir / FString::Printf(TEXT("Track%02d.wav"), Number);
		if (!FPaths::FileExists(Path))
		{
			TArray<FString> Files;
			IFileManager::Get().FindFiles(Files, *(Dir / TEXT("Track*.wav")), true, false);
			if (Files.IsEmpty()) return nullptr;
			Path = Dir / Files[FMath::RandRange(0, Files.Num() - 1)];
		}
		TSharedPtr<FTrack> Track = MakeShared<FTrack>();
		return LoadWav(Path, *Track) ? Track : nullptr;
	}

	// Audio thread, under GLock: the music into Out, resampled linearly to the output rate.
	void RenderMusic(float* Out, int32 Frames)
	{
		for (int32 I = 0; I < Frames; ++I)
		{
			const TSharedPtr<FTrack>& T = GInIntro ? GIntro : GLoop;
			if (!T.IsValid()) return;
			const int32 N = T->Pcm.Num() / 2;
			int32 F = int32(GMusicPos);
			if (F + 1 >= N)
			{
				// The intro gives way to the loop; the loop starts over.
				GMusicPos = 0;
				GInIntro = false;
				if (!GLoop.IsValid()) { GIntro.Reset(); return; }
				continue;
			}
			const float A = float(GMusicPos - F);
			const int16* P = T->Pcm.GetData() + F * 2;
			Out[I * 2] += (P[0] + (P[2] - P[0]) * A) / 32768.f * GMusicVolume;
			Out[I * 2 + 1] += (P[1] + (P[3] - P[1]) * A) / 32768.f * GMusicVolume;
			GMusicPos += double(T->Rate) / GRate;
		}
	}
}

namespace bigred
{
	Mixer::~Mixer() { close(); }

	bool Mixer::open()
	{
		if (device_ != 0) return true;
		out_rate_ = GRate;
		// mix() is private; a lambda defined here may call it, and BRRAudio::Mix calls the lambda.
		GMixFn = [](void* M, int16_t* Out, int Frames) { static_cast<Mixer*>(M)->mix(Out, Frames); };
		device_ = 1;
		return true;
	}

	void Mixer::close()
	{
		FScopeLock Lock(&GLock);
		device_ = 0;
	}

	void Mixer::callback(void*, uint8_t*, int) {} // SDL's entry; UE's is BRRAudio::Mix

	void Mixer::mix(int16_t* out, int frames)
	{
		// As mixer.cpp, minus the music (BRRAudio mixes it, at its own volume). Runs with GLock held.
		bool speaking = false;
		for (const Voice& v : voices_) if (v.active && v.speech) speaking = true;
		const int32_t duck0 = duck_;
		if (speaking) duck_ = duck_ - kDuckAttack < kDuckGain ? kDuckGain : duck_ - kDuckAttack;
		else duck_ = duck_ + kDuckRelease > 256 ? 256 : duck_ + kDuckRelease;
		const int32_t duck1 = duck_;
		for (Voice& v : voices_)
		{
			if (!v.active || v.pcm == nullptr) continue;
			const int32_t vol = (v.volume * master_) >> 8;
			const int32_t lvol0 = (vol * (256 - v.pan)) >> 8;
			const int32_t rvol0 = (vol * v.pan) >> 8;
			const bool ducked = !v.speech && (duck0 < 256 || duck1 < 256);
			int32_t lvol = lvol0, rvol = rvol0;
			for (int i = 0; i < frames; ++i)
			{
				if (ducked)
				{
					const int32_t g = duck0 + (duck1 - duck0) * i / frames;
					lvol = (lvol0 * g) >> 8;
					rvol = (rvol0 * g) >> 8;
				}
				size_t idx = static_cast<size_t>(v.pos_fx >> 32);
				if (idx >= v.length)
				{
					if (!v.loop) { v.active = false; break; }
					idx = v.loop_start;
					v.pos_fx = static_cast<uint64_t>(v.loop_start) << 32;
				}
				if (v.loop && v.loop_end > v.loop_start && idx >= v.loop_end)
				{
					idx = v.loop_start + (idx - v.loop_end) % (v.loop_end - v.loop_start);
					v.pos_fx = static_cast<uint64_t>(idx) << 32;
				}
				const int32_t s = v.pcm[idx];
				int32_t l = out[i * 2] + ((s * lvol) >> 8);
				int32_t r = out[i * 2 + 1] + ((s * rvol) >> 8);
				out[i * 2] = static_cast<int16_t>(FMath::Clamp(l, -32768, 32767));
				out[i * 2 + 1] = static_cast<int16_t>(FMath::Clamp(r, -32768, 32767));
				v.pos_fx += v.step_fx;
			}
		}
	}

	VoiceId Mixer::play(const Sample& sample, const VoiceParams& p, bool loop, int32_t loop_start, int32_t loop_end, bool is_engine)
	{
		VoiceId id;
		if (device_ == 0 || !sample.valid()) return id;
		FScopeLock Lock(&GLock);
		if (is_engine)
		{
			int engines = 0;
			for (const Voice& v : voices_) if (v.active && v.engine) ++engines;
			if (engines >= kEngineVoices) return id;
		}
		int slot = -1;
		for (int i = 0; i < kVoices; ++i)
		{
			if (!voices_[i].active) { slot = i; break; }
		}
		if (slot < 0 && (loop || p.speech))
		{
			uint32_t oldest = 0xffffffffu;
			for (int i = 0; i < kVoices; ++i)
			{
				if (voices_[i].active && !voices_[i].loop && voices_[i].generation < oldest)
				{
					oldest = voices_[i].generation;
					slot = i;
				}
			}
		}
		if (slot < 0) return id;
		Voice& v = voices_[slot];
		v.pcm = sample.pcm.data();
		v.length = sample.pcm.size();
		v.pos_fx = 0;
		v.loop = loop;
		v.loop_start = (loop_start > 0 && static_cast<size_t>(loop_start) < v.length) ? static_cast<size_t>(loop_start) : 0;
		v.loop_end = (loop_end > 0 && static_cast<size_t>(loop_end) <= v.length) ? static_cast<size_t>(loop_end) : v.length;
		if (v.loop_end <= v.loop_start) { v.loop_start = 0; v.loop_end = v.length; }
		v.engine = is_engine;
		v.speech = p.speech;
		v.generation = next_generation_++;
		apply(v, p);
		v.active = true;
		id.slot = slot;
		id.generation = v.generation;
		return id;
	}

	void Mixer::apply(Voice& v, const VoiceParams& p)
	{
		int32_t rate = FMath::Clamp(p.rate, 1000, 96000);
		v.step_fx = (static_cast<uint64_t>(rate) << 32) / static_cast<uint32_t>(out_rate_);
		const int32_t cap = v.speech ? kSpeechCap : kVolumeCap;
		v.volume = FMath::Clamp(p.volume, 0, cap);
		v.pan = FMath::Clamp(p.pan, 0, 256);
	}

	void Mixer::update(VoiceId id, const VoiceParams& p)
	{
		if (device_ == 0 || !id.valid()) return;
		FScopeLock Lock(&GLock);
		Voice& v = voices_[id.slot];
		if (v.active && v.generation == id.generation) apply(v, p);
	}

	void Mixer::stop(VoiceId id)
	{
		if (device_ == 0 || !id.valid()) return;
		FScopeLock Lock(&GLock);
		Voice& v = voices_[id.slot];
		if (v.active && v.generation == id.generation) v.active = false;
	}

	bool Mixer::playing(VoiceId id) const
	{
		if (device_ == 0 || !id.valid()) return false;
		const Voice& v = voices_[id.slot];
		return v.active && v.generation == id.generation;
	}

	void Mixer::stop_all()
	{
		if (device_ == 0) return;
		FScopeLock Lock(&GLock);
		for (Voice& v : voices_) v.active = false;
	}

	void Mixer::set_music(MusicPlayer* music) { music_ = music; } // unused: BRRAudio plays the music

	int Mixer::active() const
	{
		int n = 0;
		for (const Voice& v : voices_) if (v.active) ++n;
		return n;
	}
}

namespace BRRAudio
{
	bigred::Mixer* GetMixer() { return GMixer && GMixer->ready() ? GMixer.get() : nullptr; }

	bool Open(int32 SampleRate)
	{
		if (GMixer && GMixer->ready()) return true;
		GRate = SampleRate > 0 ? SampleRate : 48000;
		GMixer = std::make_unique<bigred::Mixer>();
		return GMixer->open();
	}

	void Close()
	{
		StopMusic();
		if (GMixer) GMixer->close();
	}

	bool IsOpen() { return GMixer && GMixer->ready(); }

	void Mix(float* Out, int32 Frames)
	{
		FScopeLock Lock(&GLock);
		if (GMixer && GMixer->ready() && GMixFn)
		{
			GScratch.SetNumZeroed(Frames * 2, EAllowShrinking::No);
			FMemory::Memzero(GScratch.GetData(), Frames * 2 * sizeof(int16_t));
			GMixFn(GMixer.get(), GScratch.GetData(), Frames);
			const float Gain = GEffectsVolume / 32768.f;
			for (int32 I = 0; I < Frames * 2; ++I) Out[I] += GScratch[I] * Gain;
		}
		RenderMusic(Out, Frames);
		if (GDumpFrames > 0)
		{
			for (int32 I = 0; I < Frames * 2 && GDump.Num() < GDumpFrames * 2; ++I) GDump.Add(int16(FMath::Clamp(Out[I], -1.f, 1.f) * 32767.f));
			if (GDump.Num() >= GDumpFrames * 2)
			{
				WriteWav(GDumpPath, GDump, GRate);
				GDumpFrames = 0;
				GDump.Empty();
			}
		}
	}

	void DumpTo(const FString& Path, float Seconds)
	{
		FScopeLock Lock(&GLock);
		GDumpPath = Path;
		GDump.Reset();
		GDumpFrames = int32(Seconds * GRate);
	}

	FString DebugState()
	{
		FScopeLock Lock(&GLock);
		return FString::Printf(TEXT("%d voices, music %s at %.0f s"), GMixer && GMixer->ready() ? GMixer->active() : 0,
			GInIntro ? TEXT("intro") : GLoop.IsValid() ? TEXT("loop") : TEXT("off"),
			GMusicPos / FMath::Max((GInIntro ? GIntro : GLoop).IsValid() ? (GInIntro ? GIntro : GLoop)->Rate : 44100, 1));
	}

	void SetVolumes(float Effects, float Music)
	{
		FScopeLock Lock(&GLock);
		GEffectsVolume = FMath::Clamp(Effects, 0.f, 1.f);
		GMusicVolume = FMath::Clamp(Music, 0.f, 1.f);
	}

	void StopEffects()
	{
		if (GMixer) GMixer->stop_all();
	}

	void PlayOnce(const FString& DataDir, const FString& Dir, const FString& Name, float Volume)
	{
		bigred::Mixer* M = GetMixer();
		if (!M) return;
		// Kept for the session: a menu click must not decode its file every time.
		static bigred::SoundBank Bank;
		static FString BankDir;
		if (BankDir != DataDir)
		{
			Bank.configure(TCHAR_TO_UTF8(*DataDir), "", "", "");
			BankDir = DataDir;
		}
		const std::string D = TCHAR_TO_UTF8(*Dir);
		const bigred::Sample* S = (D == "front" || D.empty() || D == "vhc" || D == "lev" || D == "env")
			? Bank.get(D, TCHAR_TO_UTF8(*Name))
			: nullptr;
		if (!S && !D.empty() && D != "front")
		{
			// A named folder: resolve it as the `lev` prefix of a bank pointed there.
			static std::unordered_map<std::string, std::unique_ptr<bigred::SoundBank>> Named;
			std::unique_ptr<bigred::SoundBank>& B = Named[D];
			if (!B)
			{
				B = std::make_unique<bigred::SoundBank>();
				B->configure(TCHAR_TO_UTF8(*DataDir), "", D, "");
			}
			S = B->get("lev", TCHAR_TO_UTF8(*Name));
		}
		if (!S) return;
		bigred::VoiceParams P;
		P.rate = S->rate;
		P.volume = int32(FMath::Clamp(Volume, 0.f, 1.f) * 256);
		M->play(*S, P, false, 0, -1, false);
	}

	bigred::Sample GTrack;      // PlayTrack's, kept while its voice plays
	bigred::VoiceId GTrackVoice;

	void StopTrack()
	{
		if (GMixer && GTrackVoice.valid()) GMixer->stop(GTrackVoice);
		GTrackVoice = bigred::VoiceId();
	}

	void PlayTrack(const TArray<int16>& Pcm, int32 Rate)
	{
		bigred::Mixer* M = GetMixer();
		StopTrack();
		if (!M || Pcm.IsEmpty()) return;
		GTrack.pcm.assign(Pcm.GetData(), Pcm.GetData() + Pcm.Num());
		GTrack.rate = Rate;
		bigred::VoiceParams P;
		P.rate = Rate;
		P.volume = 256;
		P.speech = true; // full scale, as a spoken line
		GTrackVoice = M->play(GTrack, P, false, 0, -1, false);
	}

	void PlayMusic(int32 Intro, int32 Loop)
	{
		TSharedPtr<FTrack> I = Intro > 0 && Intro != Loop ? LoadTrack(Intro) : nullptr;
		TSharedPtr<FTrack> L = LoadTrack(Loop > 0 ? Loop : Intro);
		FScopeLock Lock(&GLock);
		GIntro = I;
		GLoop = L;
		GInIntro = GIntro.IsValid();
		GMusicPos = 0;
	}

	void StopMusic()
	{
		FScopeLock Lock(&GLock);
		GIntro.Reset();
		GLoop.Reset();
	}

	FString GetMusicDir()
	{
		FString Dir;
		if (GConfig) GConfig->GetString(TEXT("BRR"), TEXT("MusicDir"), Dir, GGameIni);
		if (Dir.IsEmpty())
		{
			const FString Data = BRR::GetDataDir();
			if (Data.IsEmpty()) return FString();
			Dir = Data / TEXT("../Music");
		}
		else if (FPaths::IsRelative(Dir))
		{
			Dir = FPaths::Combine(FPaths::ProjectDir(), Dir);
		}
		FPaths::CollapseRelativeDirectories(Dir);
		return FPaths::DirectoryExists(Dir) ? Dir : FString();
	}

	void CircuitMusic(const FString& DataDir, const FString& Level, int32& OutIntro, int32& OutLoop)
	{
		OutIntro = OutLoop = 0;
		static FBRRTournament Tourn;
		static FString TournDir;
		if (TournDir != DataDir)
		{
			FString Error;
			Tourn = FBRRTournament();
			BRR::LoadTournament(DataDir, Tourn, Error);
			TournDir = DataDir;
		}
		for (const FBRRTournamentTrack& T : Tourn.Tracks)
		{
			if (T.Level.Equals(Level, ESearchCase::IgnoreCase))
			{
				OutIntro = T.IntroCdTrack;
				OutLoop = T.MusicCdTrack;
				return;
			}
		}
	}
}
