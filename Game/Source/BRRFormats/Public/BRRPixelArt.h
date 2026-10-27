#pragma once

#include "CoreMinimal.h"
#include "BRRWorld.h"

// The remastered look's interface art: the original's fonts and HUD sprites drawn at many times
// their size with smooth edges - the same shapes and the same colours, without the pixel steps.
namespace BRR
{
	// Whether an image is flat art - a few palette colours in areas, as the fonts and the HUD's
	// sprites are - rather than a painted or rendered picture (the menus' backdrops), which
	// SmoothPixelArt would turn into a poster.
	BRRFORMATS_API bool IsFlatArt(const FBRRImage& Image, int32 MaxColours = 24);

	// Image at Scale times its size. Each of its colours (and its see-through background) is a
	// region; each region's mask is interpolated smoothly (Catmull-Rom) between the pixel
	// centres, and every output point takes the region that is strongest there - so a staircase
	// edge becomes a line or a curve, boundaries between colours stay crisp, and no colour is
	// made up. A near tie goes to the rarer colour around it, which keeps one-pixel strokes and
	// diagonals joined. Edges are anti-aliased by sampling Samples x Samples points per output
	// pixel: see-through coverage in the alpha, the colour the covered points' mean.
	BRRFORMATS_API FBRRImage SmoothPixelArt(const FBRRImage& Image, int32 Scale, int32 Samples = 4);
}
