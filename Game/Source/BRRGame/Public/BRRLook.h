// The race's two looks, switched at runtime (brr.Look, F5 in a race):
//
//   classic    - the original: unlit palette colours shown exactly (the import's ClassicLook post
//                process replaces the tonemapper), its banded sky, its view distance, 4:3, no
//                anti-aliasing.
//   remastered - the same world lit: the level's sun (its `light` line) in a sky atmosphere with
//                clouds and height fog, Lumen global illumination and reflections on hardware ray
//                tracing, virtual shadow maps, a filmic post process, upscaling (DLSS Super
//                Resolution with Ray Reconstruction, Frame Generation and Reflex when NVIDIA's
//                plugins are installed, else TSR), widescreen, everything in view.
//                brr.PathTracing 1 switches the path tracer on for stills.
//
// Materials carry both (MPC_BRR.Remaster); the rest this actor spawns and the renderer settings
// it sets.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "BRRLook.generated.h"

class ADirectionalLight;
class AExponentialHeightFog;
class APostProcessVolume;
class ASkyAtmosphere;
class ASkyLight;
class AVolumetricCloud;
class UMaterialParameterCollection;

UCLASS()
class BRRGAME_API ABRRLook : public AActor
{
	GENERATED_BODY()

public:
	// SunDirection: where the level's sun light travels (FBRRWorld::SunDirection).
	void Build(const FVector& SunDirection, UMaterialParameterCollection* Collection);

	// bFrameGeneration: DLSS Frame Generation, when NVIDIA's Streamline plugin is installed.
	// bNativeResolution: render at full resolution - DLAA with DLSS - rather than upscale.
	void Apply(bool bRemastered, bool bPathTracing, bool bFrameGeneration, bool bNativeResolution);
	bool IsRemastered() const { return bRemastered; }

	// Which upscaler the remastered look runs ("DLSS" or "TSR").
	FString Upscaler() const;

private:
	bool bRemastered = false;
	bool bPathTracing = false;
	bool bFrameGeneration = false;
	bool bNativeResolution = false;
	bool bApplied = false;

	UPROPERTY()
	TObjectPtr<UMaterialParameterCollection> Collection;

	UPROPERTY()
	TObjectPtr<ADirectionalLight> Sun;
	UPROPERTY()
	TObjectPtr<ASkyAtmosphere> Atmosphere;
	UPROPERTY()
	TObjectPtr<ASkyLight> SkyLight;
	UPROPERTY()
	TObjectPtr<AVolumetricCloud> Clouds;
	UPROPERTY()
	TObjectPtr<AExponentialHeightFog> Fog;
	UPROPERTY()
	TObjectPtr<APostProcessVolume> Post;
};
