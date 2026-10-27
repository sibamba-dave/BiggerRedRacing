#include "BRRWeather.h"

#include "BRRLook.h"
#include "Kismet/KismetMaterialLibrary.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "HAL/IConsoleManager.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialParameterCollection.h"
#include "ProceduralMeshComponent.h"

DEFINE_LOG_CATEGORY_STATIC(LogBRRWeather, Log, All);

namespace
{
	TAutoConsoleVariable<int32> CVarWeather(TEXT("brr.Weather"), 1,
		TEXT("The remastered look's weather: 0 always a clear day, 1 drawn for each race, 2 always foul."));
	constexpr float BoxSize = 4000.f, BoxHeight = 2200.f; // cm about the camera
}

ABRRWeather::ABRRWeather()
{
	Fall = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("Fall"));
	Fall->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Fall->SetCastShadow(false);
	SetRootComponent(Fall);
	PrimaryActorTick.bCanEverTick = false;
	Tags.Add(TEXT("BRRRemasterOnly"));
}

void ABRRWeather::Build(const FString& Level, ABRRLook* Look)
{
	Material = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/BRR/Materials/M_BRR_Haze.M_BRR_Haze"), nullptr, LOAD_NoWarn | LOAD_Quiet);
	int32 Seed = 0;
	if (FParse::Value(FCommandLine::Get(), TEXT("BRRWeatherSeed="), Seed)) Random.Initialize(Seed); // a pinned draw, for benches and comparisons
	else Random.Initialize(int32(FDateTime::Now().GetTicks() & 0x7FFFFFFF));
	int32 Mode = CVarWeather.GetValueOnGameThread();
	FParse::Value(FCommandLine::Get(), TEXT("BRRWeather="), Mode); // the race is built before -ExecCmds run
	const FString L = Level.ToUpper();
	auto Is = [&L](const TCHAR* P) { return L.StartsWith(P); };

	// The hour: dawn or dusk a quarter of the time, a high sun another, the rest between.
	const float Hour = Random.FRand();
	SunElevation = Hour < 0.25f ? Random.FRandRange(5.f, 14.f) : Hour < 0.75f ? Random.FRandRange(20.f, 45.f) : Random.FRandRange(50.f, 70.f);

	// What falls, as the country has it.
	float RainChance = 0.22f, SnowChance = 0.f, MistChance = 0.22f;
	if (Is(TEXT("WATER"))) RainChance = 0.35f, MistChance = 0.35f;
	if (Is(TEXT("RIG"))) RainChance = 0.3f;
	if (Is(TEXT("RUF")) || Is(TEXT("DIRT3"))) RainChance = 0.06f, MistChance = 0.1f; // the desert
	if (Is(TEXT("SNOW"))) RainChance = 0.f, SnowChance = 0.55f, MistChance = 0.3f;
	if (Is(TEXT("MOON"))) RainChance = 0.f, MistChance = 0.f;
	if (Mode == 0) RainChance = SnowChance = MistChance = 0.f, SunElevation = 40.f;
	if (Mode == 2 && !Is(TEXT("MOON"))) (SnowChance > 0 ? SnowChance : RainChance) = 1.f, MistChance = 1.f;

	const float Draw = Random.FRand();
	Kind = Draw < SnowChance ? EFall::Snow : Draw < SnowChance + RainChance ? EFall::Rain : EFall::None;
	Strength = Kind == EFall::None ? 0.f : Mode == 2 ? Random.FRandRange(0.7f, 1.f) : FMath::Square(Random.FRandRange(0.35f, 1.f));
	Mist = Random.FRand() < MistChance ? Random.FRandRange(0.008f, 0.035f) : 0.002f;
	Mist += Strength * 0.02f;
	const float Sun = 1.f - 0.75f * Strength;
	const float WindAngle = Random.FRandRange(0.f, 2.f * PI), WindSpeed = Random.FRandRange(100.f, 600.f) * (0.4f + Strength);
	Wind = FVector(FMath::Cos(WindAngle), FMath::Sin(WindAngle), 0) * WindSpeed;
	if (Look)
	{
		// The sky wears the weather: overcast with the fall's strength and the mist, and the
		// ground soaked when it rains (snow lands dry: the ground is already white).
		ABRRLook::FBRRMood Mood;
		Mood.SunElevation = SunElevation;
		Mood.SunBrightness = Sun;
		Mood.MistDensity = Mist;
		Mood.CloudCoverage = FMath::Clamp(0.15f + 0.8f * Strength + (Mist - 0.002f) * 8.f, 0.1f, 0.95f);
		Mood.Wetness = Kind == EFall::Rain ? Strength : 0.f;
		Look->SetMood(Mood);
	}

	// The box of drops, built once: all four corners of a quad sit on the drop's point, and the
	// material (M_BRR_Rain / M_BRR_Snow) falls, wraps and spreads them in the shader. The only
	// per-frame work left is carrying the actor along with the camera.
	const bool bRain = Kind == EFall::Rain;
	const int32 Count = bRain ? int32(3500 * Strength) + 400 : Kind == EFall::Snow ? int32(2500 * Strength) + 300 : 0;
	if (Count > 0)
	{
		Material = LoadObject<UMaterialInterface>(nullptr, bRain ? TEXT("/Game/BRR/Materials/M_BRR_Rain.M_BRR_Rain") : TEXT("/Game/BRR/Materials/M_BRR_Snow.M_BRR_Snow"), nullptr, LOAD_NoWarn | LOAD_Quiet);
		UMaterialParameterCollection* Mpc = LoadObject<UMaterialParameterCollection>(nullptr, TEXT("/Game/BRR/Materials/MPC_BRR.MPC_BRR"), nullptr, LOAD_NoWarn | LOAD_Quiet);
		if (!Material || !Mpc)
		{
			UE_LOG(LogBRRWeather, Warning, TEXT("No %s or MPC weather speeds: re-import a level; the sky stays dry"), bRain ? TEXT("M_BRR_Rain") : TEXT("M_BRR_Snow"));
		}
		else
		{
			const FVector Velocity = Wind * (bRain ? 0.6f : 1.f);
			UKismetMaterialLibrary::SetScalarParameterValue(GetWorld(), Mpc, TEXT("WeatherVelX"), Velocity.X);
			UKismetMaterialLibrary::SetScalarParameterValue(GetWorld(), Mpc, TEXT("WeatherVelY"), Velocity.Y);
			UKismetMaterialLibrary::SetScalarParameterValue(GetWorld(), Mpc, TEXT("WeatherFall"), bRain ? 900.f : 110.f);
			TArray<FVector> Positions;
			TArray<int32> Triangles;
			TArray<FVector> Normals;
			TArray<FVector2D> Corners, Seeds, Empty;
			TArray<FColor> Colours;
			Positions.Reserve(Count * 4);
			const FColor C = bRain ? FColor(190, 200, 210, uint8(60 + 60 * Strength)) : FColor(250, 252, 255, 210);
			for (int32 I = 0; I < Count; ++I)
			{
				const FVector D(Random.FRand() * BoxSize, Random.FRand() * BoxSize, Random.FRand() * BoxHeight);
				const int32 Base = Positions.Num();
				Positions.Append({ D, D, D, D });
				Corners.Append({ FVector2D(0, 0), FVector2D(1, 0), FVector2D(0, 1), FVector2D(1, 1) });
				const FVector2D Id(I % 50, I * 0.37f);
				Seeds.Append({ Id, Id, Id, Id });
				Colours.Append({ C, C, C, C });
				Triangles.Append({ Base, Base + 1, Base + 2, Base + 1, Base + 3, Base + 2 });
			}
			Normals.Init(FVector::UpVector, Positions.Num());
			Fall->CreateMeshSection(0, Positions, Triangles, Normals, Corners, Seeds, Empty, Empty, Colours, TArray<FProcMeshTangent>(), false);
			Fall->SetMaterial(0, Material);
			Fall->SetVisibleInRayTracing(false);
			Fall->BoundsScale = 1.5f; // the shader moves drops anywhere in the box
		}
	}
	UE_LOG(LogBRRWeather, Display, TEXT("%s: %s"), *Level, *Describe());
}

FString ABRRWeather::Describe() const
{
	const TCHAR* What = Kind == EFall::Rain ? TEXT("rain") : Kind == EFall::Snow ? TEXT("snow") : TEXT("dry");
	return FString::Printf(TEXT("%s %.0f%%, sun %.0f degrees up, mist %.3f"), What, Strength * 100.f, SunElevation, Mist);
}

void ABRRWeather::Update(const FVector& CameraLocation, bool bOn)
{
	static const IConsoleVariable* Effects = IConsoleManager::Get().FindConsoleVariable(TEXT("brr.Effects"));
	const bool bShow = bOn && Fall->GetNumSections() > 0 && (!Effects || Effects->GetInt() != 0);
	if (bShow != bShown)
	{
		bShown = bShow;
		SetActorHiddenInGame(!bShow);
	}
	// The box rides with the camera; the drops fall and wrap inside it, in the shader.
	if (bShow) SetActorLocation(CameraLocation - FVector(BoxSize * 0.5f, BoxSize * 0.5f, BoxHeight * 0.6f));
}
