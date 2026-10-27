#include "BRRWeather.h"

#include "BRRLook.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "HAL/IConsoleManager.h"
#include "Materials/MaterialInterface.h"
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
	if (Look) Look->SetMood(SunElevation, Sun, Mist);

	Drops.Reset();
	const int32 Count = Kind == EFall::Rain ? int32(3500 * Strength) + 400 : Kind == EFall::Snow ? int32(2500 * Strength) + 300 : 0;
	for (int32 I = 0; I < Count; ++I) Drops.Add(FVector(Random.FRand() * BoxSize, Random.FRand() * BoxSize, Random.FRand() * BoxHeight));
	UE_LOG(LogBRRWeather, Display, TEXT("%s: %s"), *Level, *Describe());
}

FString ABRRWeather::Describe() const
{
	const TCHAR* What = Kind == EFall::Rain ? TEXT("rain") : Kind == EFall::Snow ? TEXT("snow") : TEXT("dry");
	return FString::Printf(TEXT("%s %.0f%%, sun %.0f degrees up, mist %.3f"), What, Strength * 100.f, SunElevation, Mist);
}

void ABRRWeather::Update(const FVector& CameraLocation, const FVector& CameraRight, const FVector& CameraUp, float DeltaSeconds, bool bOn)
{
	if (!bOn || Kind == EFall::None || !Material || Drops.IsEmpty())
	{
		if (Fall->GetNumSections() > 0) Fall->ClearAllMeshSections();
		return;
	}
	const float Dt = FMath::Min(DeltaSeconds, 0.1f);
	const bool bRain = Kind == EFall::Rain;
	const FVector Velocity = (bRain ? FVector(0, 0, -900.f) : FVector(0, 0, -110.f)) + Wind * (bRain ? 0.6f : 1.f);
	const FVector Corner = CameraLocation - FVector(BoxSize * 0.5f, BoxSize * 0.5f, BoxHeight * 0.6f);
	TArray<FVector> Positions;
	TArray<int32> Triangles;
	TArray<FVector2D> UVs;
	TArray<FColor> Colours;
	Positions.Reserve(Drops.Num() * 4);
	const FColor C = bRain ? FColor(190, 200, 210, uint8(60 + 60 * Strength)) : FColor(250, 252, 255, 210);
	const FVector Along = bRain ? -Velocity.GetSafeNormal() * 45.f : CameraUp * 2.2f;
	const FVector Across = bRain ? CameraRight * 0.8f : CameraRight * 2.2f;
	for (int32 I = 0; I < Drops.Num(); ++I)
	{
		FVector& D = Drops[I];
		D += Velocity * Dt;
		if (!bRain) D += FVector(FMath::Sin(D.Z * 0.01f + I) * 30.f, FMath::Cos(D.Z * 0.013f + I) * 30.f, 0) * Dt; // flakes wander
		// Wrapped in the box, which travels with the camera.
		FVector W = Corner + D;
		W.X = Corner.X + FMath::Fmod(FMath::Fmod(W.X - Corner.X, BoxSize) + BoxSize, BoxSize);
		W.Y = Corner.Y + FMath::Fmod(FMath::Fmod(W.Y - Corner.Y, BoxSize) + BoxSize, BoxSize);
		W.Z = Corner.Z + FMath::Fmod(FMath::Fmod(W.Z - Corner.Z, BoxHeight) + BoxHeight, BoxHeight);
		D = W - Corner;
		const int32 Base = Positions.Num();
		Positions.Append({ W - Across - Along, W + Across - Along, W - Across + Along, W + Across + Along });
		const double Seed = double(I % 50);
		UVs.Append({ FVector2D(Seed, 0), FVector2D(Seed + 1, 0), FVector2D(Seed, 1), FVector2D(Seed + 1, 1) });
		Colours.Append({ C, C, C, C });
		Triangles.Append({ Base, Base + 1, Base + 2, Base + 1, Base + 3, Base + 2 });
	}
	TArray<FVector> Normals;
	Normals.Init(FVector::UpVector, Positions.Num());
	Fall->CreateMeshSection(0, Positions, Triangles, Normals, UVs, Colours, TArray<FProcMeshTangent>(), false);
	Fall->SetMaterial(0, Material);
}
