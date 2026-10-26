#include "BRRSprites.h"

#include "BRRLevelMaterials.h"
#include "BRRRace.h"
#include "ProceduralMeshComponent.h"

ABRRSprites::ABRRSprites()
{
	Quads = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("Quads"));
	Quads->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Quads->SetCastShadow(false);
	SetRootComponent(Quads);
}

void ABRRSprites::Build(const FBRRWorld& World, const FString& Level)
{
	Sprites = World.Sprites;
	AnimStepSeconds = World.AnimStepSeconds;
	for (const FBRRSprite& S : Sprites)
	{
		for (const FBRRSprite::FFrame& F : S.Frames)
		{
			if (!F.bOn || SectionOfAtlas.Contains(F.Atlas)) continue;
			const int32 Section = SectionOfAtlas.Num();
			SectionOfAtlas.Add(F.Atlas, Section);
			Quads->SetMaterial(Section, BRR::LoadLevelMaterial(Level, F.Atlas, true));
		}
	}
}

void ABRRSprites::Update(const FBRRRace& Race, const FRotator& CameraRotation, double ElapsedSeconds)
{
	struct FSectionData
	{
		TArray<FVector> Positions;
		TArray<FVector> Normals;
		TArray<FVector2D> UVs;
		TArray<int32> Triangles;
	};
	TArray<FSectionData> Data;
	Data.SetNum(SectionOfAtlas.Num());

	const FRotationMatrix View(CameraRotation);
	const FVector Right = View.GetScaledAxis(EAxis::Y);
	const FVector Up = View.GetScaledAxis(EAxis::Z);
	const FVector Facing = -View.GetScaledAxis(EAxis::X);
	const double ElapsedMs = FMath::Max(ElapsedSeconds, 0.0) * 1000.0;

	for (const FBRRSprite& S : Sprites)
	{
		int32 K = 0;
		if (S.WreckInstance >= 0)
		{
			// Frame 0 standing, 1 + step of the flipbook once knocked over.
			const double Down = Race.WreckSeconds(S.WreckInstance);
			if (Down >= 0) K = 1 + FMath::Min(FMath::FloorToInt32(Down / BRR::WreckStepSeconds), BRR::WreckStepCount - 1);
			K = FMath::Min(K, S.Frames.Num() - 1);
		}
		else if (S.AnimSlot >= 0 && S.Frames.Num() > 1 && AnimStepSeconds.IsValidIndex(S.AnimSlot) && AnimStepSeconds[S.AnimSlot] > 0)
		{
			// LevelAnim::step_at, in whole milliseconds as the original's clock counts.
			const int64 StepMs = FMath::RoundToInt64(AnimStepSeconds[S.AnimSlot] * 1000.0);
			K = int32((int64(ElapsedMs) / StepMs) % S.Frames.Num());
		}
		const FBRRSprite::FFrame& F = S.Frames[K];
		if (!F.bOn) continue;
		const int32* Section = SectionOfAtlas.Find(F.Atlas);
		if (!Section) continue;

		// Corners (-1,-1) (+1,-1) (+1,+1) (-1,+1) across and up the view; V runs down the sheet.
		FSectionData& D = Data[*Section];
		const int32 Base = D.Positions.Num();
		static constexpr float Sx[4] = { -1.f, 1.f, 1.f, -1.f };
		static constexpr float Sy[4] = { -1.f, -1.f, 1.f, 1.f };
		for (int32 C = 0; C < 4; ++C)
		{
			D.Positions.Add(S.Centre + Right * (Sx[C] * S.HalfWidth) + Up * (Sy[C] * S.HalfHeight));
			D.Normals.Add(Facing);
			D.UVs.Add(FVector2D(Sx[C] < 0 ? F.UV0.X : F.UV1.X, Sy[C] < 0 ? F.UV1.Y : F.UV0.Y));
		}
		D.Triangles.Append({ Base, Base + 2, Base + 1, Base, Base + 3, Base + 2 });
	}

	for (int32 Section = 0; Section < Data.Num(); ++Section)
	{
		const FSectionData& D = Data[Section];
		if (D.Triangles.IsEmpty())
		{
			Quads->ClearMeshSection(Section);
			continue;
		}
		Quads->CreateMeshSection(Section, D.Positions, D.Triangles, D.Normals, D.UVs, {}, {}, false);
	}
}
