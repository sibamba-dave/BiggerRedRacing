#include "BRRAssets.h"

#include "BRRConvert.h"

#include <exception>
#include <string>

#include "formats/game_files.h"
#include "formats/level_script.h"
#include "formats/localised_text.h"
#include "formats/sprite.h"
#include "formats/texture.h"
#include "formats/threedw.h"
#include "formats/tourn.h"

namespace
{
	std::string ToUtf8(const FString& S) { return std::string(TCHAR_TO_UTF8(*S)); }
	FString FromUtf8(const std::string& S) { return FString(UTF8_TO_TCHAR(S.c_str())); }

	// Runs Body, turning any ORR exception into OutError.
	template <typename FBody>
	bool Guard(const FString& What, FString& OutError, FBody&& Body)
	{
		try
		{
			return Body();
		}
		catch (const std::exception& E)
		{
			OutError = FString::Printf(TEXT("%s: %s"), *What, UTF8_TO_TCHAR(E.what()));
			return false;
		}
	}

	// game_archive caches each archive for the process, so the returned member outlives the call.
	const bigred::Blob* FindMember(const FString& DataDir, const FString& Archive, const FString& Member, FString& OutError)
	{
		const bigred::ArchivePtr Arc = bigred::game_archive(ToUtf8(DataDir), ToUtf8(Archive.ToUpper()));
		const bigred::Blob* Blob = Arc ? bigred::find_member(*Arc, ToUtf8(Member.ToUpper())) : nullptr;
		if (!Blob) OutError = FString::Printf(TEXT("%s not found in %s"), *Member, *Archive);
		return Blob;
	}

	std::vector<bigred::RGB> ToRgb(const TArray<FColor>& Palette)
	{
		std::vector<bigred::RGB> Out(Palette.Num());
		for (int32 I = 0; I < Palette.Num(); ++I) Out[I] = { Palette[I].R, Palette[I].G, Palette[I].B };
		return Out;
	}

	FBRRImage FromRgba(const std::vector<uint8_t>& Rgba, int32 Width, int32 Height)
	{
		FBRRImage Image;
		Image.Width = Width;
		Image.Height = Height;
		Image.Pixels.SetNumUninitialized(Width * Height);
		for (int32 I = 0; I < Width * Height; ++I)
			Image.Pixels[I] = FColor(Rgba[I * 4], Rgba[I * 4 + 1], Rgba[I * 4 + 2], Rgba[I * 4 + 3]);
		return Image;
	}
}

bool BRR::LoadMember(const FString& DataDir, const FString& Archive, const FString& Member, TArray<uint8>& OutBytes, FString& OutError)
{
	return Guard(Archive, OutError, [&]
	{
		const bigred::Blob* Blob = FindMember(DataDir, Archive, Member, OutError);
		if (!Blob) return false;
		OutBytes = TArray<uint8>(Blob->data(), int32(Blob->size()));
		return true;
	});
}

bool BRR::LoadPalette(const FString& DataDir, const FString& Archive, const FString& Member, TArray<FColor>& OutPalette, FString& OutError)
{
	return Guard(Archive, OutError, [&]
	{
		const bigred::Blob* Blob = FindMember(DataDir, Archive, Member, OutError);
		if (!Blob) return false;
		const std::vector<bigred::RGB> Rgb = bigred::load_palette_rgb(Blob->data(), Blob->size());
		OutPalette.Reset(int32(Rgb.size()));
		for (const bigred::RGB& C : Rgb) OutPalette.Add(FColor(C.r, C.g, C.b, 255));
		return OutPalette.Num() == 256;
	});
}

bool BRR::LoadTexturePage(const FString& DataDir, const FString& Archive, const FString& Member, const TArray<FColor>& Palette, FBRRImage& OutImage, FString& OutError)
{
	return Guard(Archive, OutError, [&]
	{
		const bigred::Blob* Blob = FindMember(DataDir, Archive, Member, OutError);
		if (!Blob) return false;
		if (Blob->size() != bigred::kTmSize)
		{
			OutError = FString::Printf(TEXT("%s is %d bytes, not a texture page"), *Member, int32(Blob->size()));
			return false;
		}
		OutImage = FromRgba(bigred::decode_tm_rgba(Blob->data(), ToRgb(Palette)), bigred::kTmWidth, bigred::kTmHeight);
		return true;
	});
}

bool BRR::LoadScreen(const FString& DataDir, const FString& Archive, const FString& Member, FBRRImage& OutImage, FString& OutError)
{
	return Guard(Archive, OutError, [&]
	{
		const bigred::Blob* Blob = FindMember(DataDir, Archive, Member, OutError);
		if (!Blob) return false;
		if (Blob->size() != bigred::kRawSize)
		{
			OutError = FString::Printf(TEXT("%s is %d bytes, not a 320x200 screen"), *Member, int32(Blob->size()));
			return false;
		}
		OutImage = FromRgba(bigred::decode_raw_rgba(Blob->data(), Blob->size()), bigred::kRawWidth, bigred::kRawHeight);
		return true;
	});
}

bool BRR::LoadSprites(const FString& DataDir, const FString& Archive, const FString& Member, const TArray<FColor>& Palette, TArray<FBRRImage>& OutSprites, FString& OutError)
{
	return Guard(Archive, OutError, [&]
	{
		const bigred::Blob* Blob = FindMember(DataDir, Archive, Member, OutError);
		if (!Blob) return false;
		const bool bFont = Member.EndsWith(TEXT(".FNT"), ESearchCase::IgnoreCase);
		const std::vector<bigred::SpriteGlyph> Glyphs = bFont ? bigred::decode_fnt(Blob->data(), Blob->size())
			: bigred::decode_spr(Blob->data(), Blob->size());
		const std::vector<bigred::RGB> Rgb = ToRgb(Palette);
		OutSprites.Reset(int32(Glyphs.size()));
		for (const bigred::SpriteGlyph& G : Glyphs)
		{
			OutSprites.Add(G.valid() ? FromRgba(bigred::sprite_glyph_rgba(G, Rgb), G.width, G.height) : FBRRImage());
		}
		return !OutSprites.IsEmpty();
	});
}

bool BRR::LoadTournament(const FString& DataDir, FBRRTournament& Out, FString& OutError)
{
	return Guard(TEXT("TOURN.WAP"), OutError, [&]
	{
		const bigred::Blob* Blob = FindMember(DataDir, TEXT("TOURN.WAP"), TEXT("TOURN.TXT"), OutError);
		if (!Blob) return false;
		const bigred::TournCatalog Catalog = bigred::parse_tourn_catalog(std::string(Blob->begin(), Blob->end()));
		Out = FBRRTournament();
		for (const bigred::TournTrack& T : Catalog.tracks)
		{
			FBRRTournamentTrack& Track = Out.Tracks.AddDefaulted_GetRef();
			Track.Level = FromUtf8(T.level);
			Track.GlobeText = FromUtf8(T.globe_text);
			Track.ViewText = FromUtf8(T.view_text);
			Track.IntroCdTrack = T.intro_track;
			Track.MusicCdTrack = T.music_track;
			Track.NameIndex = T.name_index;
			Track.GlobeX = T.globe_x;
			Track.GlobeY = T.globe_y;
			Track.Planet = T.planet;
		}
		for (const std::string& L : Catalog.single_order) Out.SingleRaceOrder.Add(FromUtf8(L));
		for (const bigred::TournRound& R : Catalog.rounds)
		{
			FBRRTournamentRound& Round = Out.Rounds.AddDefaulted_GetRef();
			Round.Number = R.number;
			Round.Need = R.need;
			for (const std::string& L : R.levels) Round.Levels.Add(FromUtf8(L));
		}
		return !Out.Tracks.IsEmpty();
	});
}

FVector2D FBRRTopMap::ToPixel(const FVector& Location) const
{
	const FVector Game = BRR::Private::UEToGame(Location);
	bigred::TopmapProjection P;
	P.a = A;
	P.b = B;
	P.scale = Scale;
	int Px = 0, Py = 0;
	bigred::topmap_pixel(P, int32(Game.X), int32(Game.Z), Px, Py);
	return FVector2D(Px, Py);
}

bool BRR::LoadTopMap(const FString& DataDir, const FString& Level, const TArray<FColor>& Palette, FBRRTopMap& Out, FString& OutError)
{
	return Guard(Level, OutError, [&]
	{
		const bigred::Blob* Blob = FindMember(DataDir, Level + TEXT(".LEV"), TEXT("TOPMAP.SPR"), OutError);
		if (!Blob) return false;
		const std::vector<bigred::SpriteGlyph> Glyphs = bigred::decode_spr(Blob->data(), Blob->size());
		if (Glyphs.empty() || !Glyphs[0].valid())
		{
			OutError = TEXT("TOPMAP.SPR has no map glyph");
			return false;
		}
		const bigred::TopmapProjection P = bigred::topmap_projection(Glyphs[0]);
		Out.A = P.a;
		Out.B = P.b;
		Out.Scale = P.scale;
		Out.Image = FromRgba(bigred::sprite_glyph_rgba(Glyphs[0], ToRgb(Palette)), Glyphs[0].width, Glyphs[0].height);
		return true;
	});
}

int32 BRR::InterfaceGlyph(TCHAR C)
{
	return bigred::interface_glyph(char32_t(C), false);
}

int32 BRR::InterfaceGlyph(TCHAR C, bool bWibbly)
{
	return bigred::interface_glyph(char32_t(C), bWibbly);
}

bool BRR::LoadScreen(const FString& DataDir, const FString& Archive, const FString& Member, FBRRImage& OutImage, TArray<FColor>& OutPalette, FString& OutError)
{
	if (!LoadScreen(DataDir, Archive, Member, OutImage, OutError)) return false;
	return Guard(Archive, OutError, [&]
	{
		const bigred::Blob* Blob = FindMember(DataDir, Archive, Member, OutError);
		if (!Blob) return false;
		const std::vector<bigred::RGB> Rgb = bigred::load_palette_rgb(Blob->data(), bigred::kRawPaletteBytes);
		OutPalette.Reset(int32(Rgb.size()));
		for (const bigred::RGB& C : Rgb) OutPalette.Add(FColor(C.r, C.g, C.b, 255));
		return OutPalette.Num() == 256;
	});
}

TArray<FString> BRR::LevelCars(const FString& DataDir, const FString& Level)
{
	TArray<FString> Out;
	FString Error;
	Guard(Level, Error, [&]
	{
		const bigred::Blob* Txt = FindMember(DataDir, Level + TEXT(".LEV"), Level.ToUpper() + TEXT(".TXT"), Error);
		if (!Txt) return false;
		for (const std::string& Car : bigred::parse_car_list(std::string(Txt->begin(), Txt->end())))
		{
			if (!bigred::is_presentation_vehicle(Car)) Out.Add(FromUtf8(Car));
		}
		return true;
	});
	return Out;
}

FString BRR::LevelEnvironment(const FString& Level)
{
	return FromUtf8(bigred::env_name_for_level(ToUtf8(Level.ToUpper())));
}

struct FBRRText::FImpl
{
	bigred::TextTable Table;
};

FBRRText::FBRRText() : Impl(MakeUnique<FImpl>()) {}
FBRRText::~FBRRText() = default;

bool FBRRText::Load(const FString& DataDir, const FString& Language)
{
	try
	{
		const std::string Dir = ToUtf8(DataDir);
		return Impl->Table.load(Dir, Language.IsEmpty() ? bigred::config_language(Dir) : ToUtf8(Language.ToUpper()));
	}
	catch (const std::exception&)
	{
		return false;
	}
}

bool FBRRText::IsReady() const { return Impl->Table.ready(); }

FString FBRRText::Text(const FString& Key, const FString& Fallback) const
{
	return FromUtf8(Impl->Table.text(ToUtf8(Key), ToUtf8(Fallback)));
}

TArray<FString> FBRRText::Lines(const FString& Key) const
{
	TArray<FString> Out;
	for (const std::string& L : Impl->Table.text_lines(ToUtf8(Key))) Out.Add(FromUtf8(L));
	return Out;
}

TArray<TArray<FString>> FBRRText::MacroLines(const FString& Key) const
{
	TArray<TArray<FString>> Out;
	for (const std::vector<std::string>& L : Impl->Table.macro_lines(ToUtf8(Key)))
	{
		TArray<FString>& Row = Out.AddDefaulted_GetRef();
		for (const std::string& P : L) Row.Add(FromUtf8(P));
	}
	return Out;
}

TArray<FBRRTextRun> FBRRText::Layout(const FString& Key) const
{
	TArray<FBRRTextRun> Out;
	for (const bigred::TextTable::Run& R : Impl->Table.layout(ToUtf8(Key)))
	{
		FBRRTextRun& Run = Out.AddDefaulted_GetRef();
		Run.X = R.x;
		Run.Y = R.y;
		Run.Ramp = R.ramp;
		Run.Font = R.font;
		Run.Text = FromUtf8(R.text);
	}
	return Out;
}

