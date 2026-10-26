#pragma once

#include "CoreMinimal.h"
#include "Components/SynthComponent.h"
#include "BRRSoundOutput.generated.h"

// Plays BRRAudio's mix - the original's samples through ORR's mixer rules, and the CD music - as
// one stereo stream, unspatialised (the original pans by itself).
UCLASS()
class BRRGAME_API UBRRSoundOutput : public USynthComponent
{
	GENERATED_BODY()

public:
	UBRRSoundOutput(const FObjectInitializer& ObjectInitializer);

protected:
	virtual bool Init(int32& SampleRate) override;
	virtual ISoundGeneratorPtr CreateSoundGenerator(const FSoundGeneratorInitParams& InParams) override;
};
