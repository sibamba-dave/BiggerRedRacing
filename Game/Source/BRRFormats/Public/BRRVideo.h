#pragma once

#include "CoreMinimal.h"
#include "BRRWorld.h"

// The original's two videos, decoded as they play (a port of Tools/brr_video.py; formats in
// Docs/Formats.md):
//
//   VIDEO/INTROSND.RPL  the intro: an Acorn ARMovie of Eidos Escape 122 frames, each chunk with
//                       1470 samples of Eidos 4-bit ADPCM at 22050 Hz - 15 frames a second by
//                       its sound, whatever the header's 25 says;
//   RACING/VICTORY.MVI  the championship's end: RLE delta frames at 17 a second, with the
//                       effects it starts on given frames.
//
// Frames are delta-coded, so they decode in order: Advance() moves on one.
class BRRFORMATS_API FBRRVideo
{
public:
	FBRRVideo();
	~FBRRVideo();
	bool Open(const FString& Path, FString& OutError);
	bool IsOpen() const;

	int32 Width() const;
	int32 Height() const;
	int32 FrameCount() const;
	double FramesPerSecond() const;

	// Decodes the next frame into the picture; false at the end.
	bool Advance();
	int32 FrameIndex() const;              // the frame the picture shows, -1 before the first
	const FBRRImage& Picture() const;      // RGBA

	// The whole sound track, mono 16-bit, decoded on Open (the MVI's effects mixed in place).
	const TArray<int16>& Sound() const;
	int32 SoundRate() const;

private:
	struct FImpl;
	TUniquePtr<FImpl> Impl;
};
