// The original's interface art as textures: its fonts and sprites at their own pixels in the
// classic look, smoothed many times over in the remastered one (BRR::SmoothPixelArt) - which
// only flat art gets: a picture stays its pixels. Draw by Size, the art's own size in its
// pixels, whichever texture it is.
#pragma once

#include "CoreMinimal.h"
#include "BRRWorld.h"

class UTexture2D;
struct FBRRImage;

namespace BRRArt
{
	// Whether the interface art is drawn smooth now: the remastered look, unless brr.SmoothArt 0.
	BRRGAME_API bool SmoothWanted();
	// Whether the remastered look is on (brr.Look).
	BRRGAME_API bool Remastered();
	// The race's own decor (the destructible and animated pieces, built when it starts) as the look
	// wants it: rounded like the imported scenery (BRR::SubdivideSmooth) in the remastered look.
	BRRGAME_API FBRRMesh LookMesh(const FBRRMesh& Mesh);

	// Image as a texture; smoothed when bSmooth and it is flat art. Keep it referenced (UPROPERTY)
	// as any transient texture.
	BRRGAME_API UTexture2D* MakeTexture(const FBRRImage& Image, bool bSmooth);

	// The art's own size, in its pixels.
	BRRGAME_API FVector2D Size(const UTexture2D* Texture);

	// Whether a texture is a smoothed one: drawn blended at its soft edges rather than masked.
	BRRGAME_API bool IsSmooth(const UTexture2D* Texture);
}
