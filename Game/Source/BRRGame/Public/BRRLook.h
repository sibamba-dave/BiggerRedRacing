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
// it sets. The remastered look's own geometry is shown and hidden here too: actors tagged
// BRRRemasterOnly (the refined terrain, the scattered detail) and BRRClassicOnly (the original's
// terrain, which stays solid while hidden), and the _HD version of any drawn-only mesh that has
// one (the round wheels).
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
class UStaticMesh;

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

	// The race's weather and hour (ABRRWeather): the sun this many degrees up (its bearing the
	// level's own), this bright against a clear day's, and the mist's density.
	void SetMood(float SunElevation, float SunBrightness, float MistDensity);

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

	// Each mesh's other look's version: the original's <mesh>_HD, and back (null: none).
	UPROPERTY()
	TMap<TObjectPtr<UStaticMesh>, TObjectPtr<UStaticMesh>> OtherLookMesh;
	UStaticMesh* MeshFor(UStaticMesh* Mesh, bool bInRemastered);
};
