#include "BRRLook.h"

#include "Components/DirectionalLightComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/VolumetricCloudComponent.h"
#include "Engine/DirectionalLight.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/SkyLight.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"
#include "Kismet/KismetMaterialLibrary.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialParameterCollection.h"
#include "Engine/Engine.h"

namespace
{
	void SetCVar(const TCHAR* Name, int32 Value)
	{
		if (IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(Name)) Var->Set(Value, ECVF_SetByCode);
	}

	bool HasCVar(const TCHAR* Name) { return IConsoleManager::Get().FindConsoleVariable(Name) != nullptr; }
	bool HasDlss() { return HasCVar(TEXT("r.NGX.DLSS.Enable")); }
}

FString ABRRLook::Upscaler() const { return HasDlss() ? TEXT("DLSS") : TEXT("TSR"); }

void ABRRLook::Build(const FVector& SunDirection, UMaterialParameterCollection* InCollection)
{
	Collection = InCollection;
	UWorld* World = GetWorld();
	FActorSpawnParameters Params;
	Params.Owner = this;

	// The level's sun, as the sky atmosphere's sun: a clear day, at the original's angle.
	Sun = World->SpawnActor<ADirectionalLight>(ADirectionalLight::StaticClass(), FTransform(SunDirection.Rotation()), Params);
	Sun->GetRootComponent()->SetMobility(EComponentMobility::Movable);
	UDirectionalLightComponent* SunLight = CastChecked<UDirectionalLightComponent>(Sun->GetLightComponent());
	SunLight->SetIntensity(10.f); // lux, the engine's daylight sun with the default atmosphere
	SunLight->SetAtmosphereSunLight(true);
	SunLight->SetDynamicShadowDistanceMovableLight(40000.f);

	Atmosphere = World->SpawnActor<ASkyAtmosphere>(ASkyAtmosphere::StaticClass(), FTransform::Identity, Params);

	SkyLight = World->SpawnActor<ASkyLight>(ASkyLight::StaticClass(), FTransform::Identity, Params);
	SkyLight->GetRootComponent()->SetMobility(EComponentMobility::Movable);
	SkyLight->GetLightComponent()->bRealTimeCapture = true;

	Clouds = World->SpawnActor<AVolumetricCloud>(AVolumetricCloud::StaticClass(), FTransform::Identity, Params);
	if (UMaterialInterface* CloudMaterial = LoadObject<UMaterialInterface>(nullptr,
		TEXT("/Engine/EngineSky/VolumetricClouds/m_SimpleVolumetricCloud_Inst.m_SimpleVolumetricCloud_Inst")))
	{
		Clouds->GetComponentByClass<UVolumetricCloudComponent>()->SetMaterial(CloudMaterial);
	}

	Fog = World->SpawnActor<AExponentialHeightFog>(AExponentialHeightFog::StaticClass(), FTransform::Identity, Params);
	UExponentialHeightFogComponent* FogComponent = Fog->GetComponent();
	FogComponent->SetFogDensity(0.004f);
	FogComponent->SetFogHeightFalloff(0.05f);
	FogComponent->SetVolumetricFog(true);

	// The remastered post process: filmic tonemapping and auto exposure (the engine's own), with
	// Lumen for global illumination and reflections.
	Post = World->SpawnActor<APostProcessVolume>(APostProcessVolume::StaticClass(), FTransform::Identity, Params);
	Post->bUnbound = true;
	Post->Priority = 10.f;
	FPostProcessSettings& P = Post->Settings;
	P.bOverride_AutoExposureMethod = true;
	P.AutoExposureMethod = AEM_Histogram;
	P.bOverride_AutoExposureApplyPhysicalCameraExposure = true;
	P.AutoExposureApplyPhysicalCameraExposure = false;
	P.bOverride_AutoExposureBias = true;
	P.AutoExposureBias = -1.f; // the sunlit palette art reads washed out at the engine's middle grey
	P.bOverride_ColorContrast = true;
	P.ColorContrast = FVector4(1.1, 1.1, 1.1, 1.0);
	P.bOverride_ColorSaturation = true;
	P.ColorSaturation = FVector4(1.15, 1.15, 1.15, 1.0);
	P.bOverride_DynamicGlobalIlluminationMethod = true;
	P.DynamicGlobalIlluminationMethod = EDynamicGlobalIlluminationMethod::Lumen;
	P.bOverride_ReflectionMethod = true;
	P.ReflectionMethod = EReflectionMethod::Lumen;
	P.bOverride_BloomIntensity = true;
	P.BloomIntensity = 0.5f;
	P.bOverride_MotionBlurAmount = true;
	P.MotionBlurAmount = 0.35f;
	P.bOverride_AmbientOcclusionIntensity = true;
	P.AmbientOcclusionIntensity = 0.5f;
	P.bOverride_VignetteIntensity = true;
	P.VignetteIntensity = 0.25f;
}

void ABRRLook::SetMood(float SunElevation, float SunBrightness, float MistDensity)
{
	if (Sun)
	{
		FRotator R = Sun->GetActorRotation();
		R.Pitch = -FMath::Clamp(SunElevation, 2.f, 85.f);
		Sun->SetActorRotation(R);
		Sun->GetLightComponent()->SetIntensity(10.f * SunBrightness);
	}
	if (Fog) Fog->GetComponent()->SetFogDensity(MistDensity);
	if (SkyLight) SkyLight->GetLightComponent()->RecaptureSky();
}

UStaticMesh* ABRRLook::MeshFor(UStaticMesh* Mesh, bool bInRemastered)
{
	const bool bIsHD = Mesh->GetName().EndsWith(TEXT("_HD"));
	if (bIsHD == bInRemastered) return nullptr;
	if (const TObjectPtr<UStaticMesh>* Known = OtherLookMesh.Find(Mesh)) return *Known;
	const FString Package = Mesh->GetOutermost()->GetName(), Name = Mesh->GetName();
	const FString Path = bIsHD ? FString::Printf(TEXT("%s.%s"), *Package.LeftChop(3), *Name.LeftChop(3))
		: FString::Printf(TEXT("%s_HD.%s_HD"), *Package, *Name);
	UStaticMesh* Other = LoadObject<UStaticMesh>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
	OtherLookMesh.Add(Mesh, Other);
	if (Other) OtherLookMesh.Add(Other, Mesh);
	return Other;
}

void ABRRLook::Apply(bool bInRemastered, bool bInPathTracing, bool bInFrameGeneration, bool bInNativeResolution)
{
	bInPathTracing = bInRemastered && bInPathTracing;
	bInFrameGeneration = bInRemastered && bInFrameGeneration;
	if (bApplied && bInRemastered == bRemastered && bInPathTracing == bPathTracing && bInFrameGeneration == bFrameGeneration
		&& bInNativeResolution == bNativeResolution) return;
	bApplied = true;
	bFrameGeneration = bInFrameGeneration;
	bNativeResolution = bInNativeResolution;
	bRemastered = bInRemastered;
	bPathTracing = bInPathTracing;
	UWorld* World = GetWorld();

	if (Collection) UKismetMaterialLibrary::SetScalarParameterValue(World, Collection, TEXT("Remaster"), bRemastered ? 1.f : 0.f);

	for (AActor* A : TArray<AActor*>{ Sun, Atmosphere, SkyLight, Clouds, Fog })
	{
		if (!A) continue;
		A->SetActorHiddenInGame(!bRemastered);
		for (UActorComponent* C : A->GetComponents())
		{
			if (USceneComponent* S = Cast<USceneComponent>(C)) S->SetVisibility(bRemastered);
		}
	}
	if (Post)
	{
		Post->BlendWeight = bRemastered ? 1.f : 0.f;
		// A still has no motion to blur: paused, the post process would smear the last frame's camera move.
		Post->Settings.MotionBlurAmount = bPathTracing ? 0.f : 0.35f;
	}
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (It->ActorHasTag(TEXT("BRRClassicSky"))) It->SetActorHiddenInGame(bRemastered);
		if (It->ActorHasTag(TEXT("BRRClassicLook")))
		{
			if (APostProcessVolume* Classic = Cast<APostProcessVolume>(*It)) Classic->BlendWeight = bRemastered ? 0.f : 1.f;
		}
		if (It->ActorHasTag(TEXT("BRRRemasterOnly"))) It->SetActorHiddenInGame(!bRemastered);
		if (It->ActorHasTag(TEXT("BRRClassicOnly"))) It->SetActorHiddenInGame(bRemastered);
		TInlineComponentArray<UStaticMeshComponent*> Meshes(*It);
		for (UStaticMeshComponent* M : Meshes)
		{
			if (M->IsCollisionEnabled() || !M->GetStaticMesh()) continue;
			if (UStaticMesh* Other = MeshFor(M->GetStaticMesh(), bRemastered)) M->SetStaticMesh(Other);
		}
	}
	if (bRemastered && SkyLight) SkyLight->GetLightComponent()->RecaptureSky();

	// The renderer: the classic look's exact palette wants no anti-aliasing, full resolution and
	// no GI; the remastered look Lumen on hardware ray tracing and an upscaler.
	SetCVar(TEXT("r.DynamicGlobalIlluminationMethod"), bRemastered ? 1 : 0);
	SetCVar(TEXT("r.ReflectionMethod"), bRemastered ? 1 : 0);
	SetCVar(TEXT("r.Lumen.HardwareRayTracing"), bRemastered ? 1 : 0);
	SetCVar(TEXT("r.Shadow.Virtual.Enable"), bRemastered ? 1 : 0);
	SetCVar(TEXT("r.AntiAliasingMethod"), bRemastered ? 4 : 0);
	SetCVar(TEXT("r.AmbientOcclusionLevels"), bRemastered ? -1 : 0); // classic: SSAO darkened the emissive palette around every object
	// Upscaling from two thirds of the resolution (DLSS / TSR "quality"), or none (brr.DLAA: DLSS at
	// 100% is DLAA - the plugin has no switch of its own for it). With NVIDIA's plugins: DLSS Super
	// Resolution with Ray Reconstruction denoising Lumen's rays, Frame Generation, and Reflex to win
	// back the latency it costs.
	SetCVar(TEXT("r.ScreenPercentage"), bRemastered && !bNativeResolution ? 67 : 100);
	// The path tracer renders every pixel itself and denoises its own result: no upscaler, no
	// generated frames, and no progress bar drawn into the image.
	const bool bRealTime = bRemastered && !bPathTracing;
	if (bPathTracing) SetCVar(TEXT("r.ScreenPercentage"), 100);
	if (HasDlss())
	{
		SetCVar(TEXT("r.NGX.DLSS.Enable"), bRealTime ? 1 : 0);
		SetCVar(TEXT("r.NGX.DLSS.DenoiserMode"), bRealTime ? 1 : 0);
	}
	if (HasCVar(TEXT("r.Streamline.DLSSG.Enable"))) SetCVar(TEXT("r.Streamline.DLSSG.Enable"), bFrameGeneration && bRealTime ? 1 : 0);
	if (HasCVar(TEXT("t.Streamline.Reflex.Enable"))) SetCVar(TEXT("t.Streamline.Reflex.Enable"), bRealTime ? 1 : 0);
	SetCVar(TEXT("r.PathTracing.Denoiser"), bPathTracing ? 1 : 0); // the engine's NNE denoiser, on the gathered samples
	SetCVar(TEXT("r.PathTracing.ProgressDisplay"), 0);
	if (GEngine) GEngine->Exec(World, bPathTracing ? TEXT("ShowFlag.PathTracing 1") : TEXT("ShowFlag.PathTracing 0"));
}
