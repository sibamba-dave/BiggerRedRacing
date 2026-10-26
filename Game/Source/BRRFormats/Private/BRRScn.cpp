#include "BRRScn.h"

#include "BRRAssets.h"

const FBRRScnBox* FBRRScn::FindBox(int32 Id) const
{
	return Boxes.FindByPredicate([Id](const FBRRScnBox& B) { return B.Id == Id && B.W > 2; });
}

namespace
{
	// "1, 10,1,250,155" -> {1, 10, 1, 250, 155}; "&1C" is hex. Words are skipped.
	TArray<int32> Numbers(const FString& Args)
	{
		TArray<int32> Out;
		TArray<FString> Parts;
		Args.ParseIntoArray(Parts, TEXT(","), true);
		for (FString P : Parts)
		{
			P.TrimStartAndEndInline();
			if (P.StartsWith(TEXT("&"))) Out.Add(FParse::HexNumber(*P.Mid(1)));
			else if (!P.IsEmpty() && (FChar::IsDigit(P[0]) || P[0] == '-')) Out.Add(FCString::Atoi(*P));
		}
		return Out;
	}
}

bool BRR::LoadScn(const FString& DataDir, const FString& Name, const FString& Language, FBRRScn& Out, FString& OutError,
	const FString& PictureArchive, const FString& PictureMember)
{
	Out = FBRRScn();
	Out.Name = Name.ToUpper();
	const FString Archive = Out.Name + TEXT(".SCN");
	TArray<uint8> Script;
	if (!BRR::LoadMember(DataDir, Archive, Out.Name + TEXT(".TXT"), Script, OutError)) return false;
	const FString Text = FString(Script.Num(), UTF8_TO_TCHAR(reinterpret_cast<const ANSICHAR*>(Script.GetData())));

	FString Raw, Spr;
	FString Lang = TEXT("ANY");
	const FString Want = Language.IsEmpty() ? TEXT("ENG") : Language.ToUpper();
	TArray<FString> Lines;
	Text.ParseIntoArrayLines(Lines);
	int32 AnimLines = 0;
	for (FString Line : Lines)
	{
		int32 Semi;
		if (Line.FindChar(';', Semi)) Line.LeftInline(Semi);
		Line.TrimStartAndEndInline();
		if (Line.IsEmpty()) continue;
		FString Word, Args;
		if (!Line.Split(TEXT(" "), &Word, &Args)) Word = Line;
		Word = Word.ToLower();
		Args.TrimStartAndEndInline();
		if (Word == TEXT("iflang")) { Lang = Args.ToUpper(); continue; }
		if (Word == TEXT("raw")) Raw = Args;
		else if (Word == TEXT("spr")) Spr = Args;
		else if (Word == TEXT("mouse")) Out.Mouse = FCString::Atoi(*Args);
		else if (Word == TEXT("stext"))
		{
			// "1,CONFIG_SINGLE,  5,32767,-1,-1" - and the authors sometimes drop a comma.
			TArray<FString> Parts;
			Args.Replace(TEXT("\t"), TEXT(" ")).ParseIntoArray(Parts, TEXT(","), true);
			if (Parts.Num() < 2) continue;
			FBRRScnText& T = Out.Texts.AddDefaulted_GetRef();
			T.Group = FCString::Atoi(*Parts[0].TrimStartAndEnd());
			TArray<FString> KeyFont;
			Parts[1].TrimStartAndEnd().ParseIntoArrayWS(KeyFont);
			T.Key = KeyFont.IsEmpty() ? FString() : KeyFont[0];
			T.Font = KeyFont.Num() > 1 ? FCString::Atoi(*KeyFont[1]) : Parts.Num() > 2 ? FCString::Atoi(*Parts[2].TrimStartAndEnd()) : 0;
		}
		else if (Word == TEXT("mbox"))
		{
			const TArray<int32> N = Numbers(Args);
			if (N.Num() >= 5) Out.Boxes.Add({ N[0], N[1], N[2], N[3], N[4] });
		}
		else if (Word == TEXT("anisprite"))
		{
			const TArray<int32> N = Numbers(Args);
			if (N.Num() >= 5) Out.AniSprites.Add({ N[0], N[1], N[3], N[4] });
		}
		else if (Word == TEXT("key"))
		{
			if (Lang != TEXT("ANY") && Lang != Want) continue;
			const TArray<int32> N = Numbers(Args);
			if (N.Num() >= 2) Out.Keys.Add({ N[0], N[1] });
		}
		else if (Word == TEXT("anim"))
		{
			const TArray<int32> N = Numbers(Args);
			if (N.Num() >= 3 && N[2] == 255)
			{
				Out.SpritePeriod = FMath::Max(N[0], 1);
				Out.SpriteFrames = FMath::Max(N[1], 1);
			}
			else if (N.Num() >= 1 && AnimLines++ == 0)
			{
				Out.FontPeriod = FMath::Max(N[0], 1);
			}
		}
	}

	const FString PicArchive = Raw.IsEmpty() ? PictureArchive : Archive;
	const FString PicMember = Raw.IsEmpty() ? PictureMember : Raw;
	if (!PicArchive.IsEmpty() && !PicMember.IsEmpty())
	{
		if (!BRR::LoadScreen(DataDir, PicArchive, PicMember, Out.Background, Out.Palette, OutError)) return false;
	}
	if (!Spr.IsEmpty() && Out.Palette.Num() == 256)
	{
		FString Ignored;
		BRR::LoadSprites(DataDir, Archive, Spr, Out.Palette, Out.Sprites, Ignored);
	}
	return true;
}
