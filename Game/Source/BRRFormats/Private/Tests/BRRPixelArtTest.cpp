#include "BRRAssets.h"
#include "BRRArchive.h"
#include "BRRPixelArt.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBRRPixelArtTest, "BRR.Formats.PixelArt",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// The smoothed interface art keeps its strokes and its colours; the fonts' smoothed glyphs are
// written to Saved/BRRSheets/Smooth_<sheet>.ppm (on magenta) to look at.
bool FBRRPixelArtTest::RunTest(const FString& Parameters)
{
	{
		// A one-pixel white stroke down a red square, on nothing.
		FBRRImage I;
		I.Width = I.Height = 7;
		I.Pixels.Init(FColor(0, 0, 0, 0), 49);
		for (int32 Y = 1; Y < 6; ++Y)
			for (int32 X = 1; X < 6; ++X) I.Pixels[Y * 7 + X] = X == 3 ? FColor::White : FColor::Red;
		const FBRRImage S = BRR::SmoothPixelArt(I, 8);
		TestEqual(TEXT("eight times the size"), S.Width, 56);
		bool bStroke = true;
		for (int32 Y = 12; Y < 44; ++Y) bStroke &= S.Pixels[Y * 56 + 28] == FColor::White;
		TestTrue(TEXT("the stroke survives"), bStroke);
		// Only where two colours meet does a pixel mix them (its anti-aliasing).
		int32 Solid = 0, Own = 0;
		for (const FColor& C : S.Pixels) if (C.A == 255) ++Solid, Own += C == FColor::White || C == FColor::Red;
		TestTrue(TEXT("no colour is made up inside"), Own > Solid * 9 / 10);
		TestTrue(TEXT("a square is flat art"), BRR::IsFlatArt(I));
	}
	const FString DataDir = BRR::GetDataDir();
	if (DataDir.IsEmpty()) return true;
	FString Error;
	TArray<FColor> Palette;
	if (!BRR::LoadPalette(DataDir, TEXT("ALWAYS.WAP"), TEXT("DEFAULT.COL"), Palette, Error)) return true;
	for (const TCHAR* Sheet : { TEXT("SML_ALPH.SPR"), TEXT("WIBS1.SPR"), TEXT("WIBS3.SPR") })
	{
		TArray<FBRRImage> Glyphs;
		if (!BRR::LoadSprites(DataDir, TEXT("ALWAYS.WAP"), Sheet, Palette, Glyphs, Error)) continue;
		// The first 40 glyphs in a row, original above, smoothed (x8) below.
		constexpr int32 Scale = 8;
		int32 W = 0, H = 0;
		const int32 Count = FMath::Min(Glyphs.Num(), 40);
		for (int32 G = 0; G < Count; ++G) W += Glyphs[G].Width * Scale + 4, H = FMath::Max(H, Glyphs[G].Height * Scale);
		if (W == 0) continue;
		TArray<FColor> Sheet2;
		Sheet2.Init(FColor(255, 0, 255), W * H * 2);
		int32 X0 = 0, Flat = 0;
		for (int32 G = 0; G < Count; ++G)
		{
			const FBRRImage& I = Glyphs[G];
			if (I.Width <= 0) continue;
			Flat += BRR::IsFlatArt(I);
			const FBRRImage S = BRR::SmoothPixelArt(I, Scale);
			for (int32 Y = 0; Y < S.Height; ++Y)
				for (int32 X = 0; X < S.Width; ++X)
				{
					const FColor Big = I.Pixels[(Y / Scale) * I.Width + X / Scale];
					if (Big.A) Sheet2[Y * W + X0 + X] = Big;
					const FColor C = S.Pixels[Y * S.Width + X];
					const float A = C.A / 255.f;
					FColor& D = Sheet2[(H + Y) * W + X0 + X];
					D = FColor(uint8(FMath::Lerp(255.f, float(C.R), A)), uint8(FMath::Lerp(0.f, float(C.G), A)), uint8(FMath::Lerp(255.f, float(C.B), A)));
				}
			X0 += I.Width * Scale + 4;
		}
		TArray<uint8> Ppm;
		const FTCHARToUTF8 Head(*FString::Printf(TEXT("P6\n%d %d\n255\n"), W, H * 2));
		Ppm.Append(reinterpret_cast<const uint8*>(Head.Get()), Head.Length());
		for (const FColor& C : Sheet2) Ppm.Append({ C.R, C.G, C.B });
		FFileHelper::SaveArrayToFile(Ppm, *(FPaths::ProjectSavedDir() / TEXT("BRRSheets") / FString::Printf(TEXT("Smooth_%s.ppm"), *FPaths::GetBaseFilename(Sheet))));
		AddInfo(FString::Printf(TEXT("SMOOTH %s: %d glyphs, %d flat"), Sheet, Count, Flat));
	}
	return true;
}

#endif
