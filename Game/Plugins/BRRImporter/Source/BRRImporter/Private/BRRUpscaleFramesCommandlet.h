#pragma once

#include "Commandlets/Commandlet.h"

#include "BRRUpscaleFramesCommandlet.generated.h"

// The remastered intro's title cards (Tools/brr_film.py): the original's flat motion graphics
// redrawn smooth at Scale times their size (BRR::SmoothPixelArt).
//   UnrealEditor-Cmd.exe BiggerRedRacing.uproject -run=BRRUpscaleFrames -In=<dir> -Out=<dir> -Frames=106-120,136-174 [-Scale=6]
// reads <In>/frame_NNNN.png for each frame in the ranges (end excluded) and writes <Out>/frame_NNNN.png.
UCLASS()
class UBRRUpscaleFramesCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UBRRUpscaleFramesCommandlet();
	virtual int32 Main(const FString& Params) override;
};
