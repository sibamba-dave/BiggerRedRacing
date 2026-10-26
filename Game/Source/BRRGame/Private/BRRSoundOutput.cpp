#include "BRRSoundOutput.h"

#include "BRRAudio.h"
#include "Sound/SoundGenerator.h"

namespace
{
	class FBRRSoundGenerator : public ISoundGenerator
	{
	public:
		virtual int32 OnGenerateAudio(float* OutAudio, int32 NumSamples) override
		{
			FMemory::Memzero(OutAudio, NumSamples * sizeof(float));
			BRRAudio::Mix(OutAudio, NumSamples / 2);
			return NumSamples;
		}
	};
}

UBRRSoundOutput::UBRRSoundOutput(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	NumChannels = 2;
	bAllowSpatialization = false;
	bIsUISound = true; // the music plays on through a pause (photo mode)
}

bool UBRRSoundOutput::Init(int32& SampleRate)
{
	NumChannels = 2;
	return BRRAudio::Open(SampleRate);
}

ISoundGeneratorPtr UBRRSoundOutput::CreateSoundGenerator(const FSoundGeneratorInitParams& InParams)
{
	return ISoundGeneratorPtr(new FBRRSoundGenerator());
}
