// Standalone original assets: palettes, texture pages, full screens, sprite sheets, fonts and the
// tournament catalogue. Archives are named as on disk ("ALWAYS.WAP", "DIRT1.LEV"); members as
// inside them ("DEFAULT.COL"). All loaders return false with OutError on failure.
#pragma once

#include "CoreMinimal.h"
#include "BRRWorld.h"

struct FBRRTournamentTrack
{
	FString Level;          // "DIRT1"
	FString GlobeText;      // name on the globe screen
	FString ViewText;       // name on the circuit presentation screen
	int32 IntroCdTrack = 0; // original CD-DA track numbers (music is user-supplied, see README)
	int32 MusicCdTrack = 0;
	int32 NameIndex = -1;   // globe-name sprite index
	int32 GlobeX = 0;
	int32 GlobeY = 0;
	int32 Planet = 0;
};

struct FBRRTournamentRound
{
	int32 Number = 0;
	TArray<FString> Levels;
	int32 Need = 1;         // races that must be won to advance
};

struct FBRRTournament
{
	TArray<FBRRTournamentTrack> Tracks;
	TArray<FString> SingleRaceOrder;
	TArray<FBRRTournamentRound> Rounds;
};

// A level's mini-map (TOPMAP.SPR glyph 0) and the original's projection onto it.
struct BRRFORMATS_API FBRRTopMap
{
	FBRRImage Image;
	int32 A = 0, B = 0, Scale = 0;

	// Pixel of Image a UE world location falls on (the dot centre, as the original places it).
	FVector2D ToPixel(const FVector& Location) const;
};

namespace BRR
{
	// TOPMAP.SPR of <Level>.LEV through the level's palette (FBRRWorld::Palette).
	BRRFORMATS_API bool LoadTopMap(const FString& DataDir, const FString& Level, const TArray<FColor>& Palette,
		FBRRTopMap& Out, FString& OutError);

	// Glyph of SML_ALPH.SPR (the interface alphabet) that draws C, or -1.
	BRRFORMATS_API int32 InterfaceGlyph(TCHAR C);

	// Raw bytes of one archive member.
	BRRFORMATS_API bool LoadMember(const FString& DataDir, const FString& Archive, const FString& Member,
		TArray<uint8>& OutBytes, FString& OutError);

	// 768-byte 6-bit VGA palette (.COL) expanded to 8-bit colours.
	BRRFORMATS_API bool LoadPalette(const FString& DataDir, const FString& Archive, const FString& Member,
		TArray<FColor>& OutPalette, FString& OutError);

	// 256x192 texture page (.TM) through Palette; index 0 becomes transparent.
	BRRFORMATS_API bool LoadTexturePage(const FString& DataDir, const FString& Archive, const FString& Member,
		const TArray<FColor>& Palette, FBRRImage& OutImage, FString& OutError);

	// 320x200 screen with its own palette (.RAW).
	BRRFORMATS_API bool LoadScreen(const FString& DataDir, const FString& Archive, const FString& Member,
		FBRRImage& OutImage, FString& OutError);

	// Every glyph of a sprite sheet (.SPR) or bitmap font (.FNT) through Palette; index 0 is
	// transparent. Empty glyphs keep their slot so indices match the game's sprite numbers.
	BRRFORMATS_API bool LoadSprites(const FString& DataDir, const FString& Archive, const FString& Member,
		const TArray<FColor>& Palette, TArray<FBRRImage>& OutSprites, FString& OutError);

	// TOURN.TXT from TOURN.WAP.
	BRRFORMATS_API bool LoadTournament(const FString& DataDir, FBRRTournament& Out, FString& OutError);
}
