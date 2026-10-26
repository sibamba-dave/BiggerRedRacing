#pragma once

#include "CoreMinimal.h"
#include "BRRWorld.h"

// A menu screen of the original: `<NAME>.SCN`, the same archive as a .WAP, holding the screen's
// script `<NAME>.TXT` and usually its picture (`raw`) and sprite sheet (`spr`). The script places
// everything on the 320x200 screen:
//
//   stext <group>, <KEY>, <font>, ...    a string of TOURN.<LANG>; its own `@X` places it
//   mbox <id>, x, y, w, h                a click box; boxes may share an id (OK and "back")
//   anisprite <group>, <first>, <n>, x, y  a sprite, cycling first.. by the `anim` line
//   key <id>, &<scancode>                a shortcut for box <id>, per `iflang` language
//   anim <period>, <count>, <mask>, v..  the cycles: fonts 2..6 wibble, sprites step
//   mouse <glyph>                        the pointer
//
// Groups are what the game shows or hides at run time (a results screen's headings, the
// network-only lines); which is the game's business, not the script's.
struct FBRRScnText { int32 Group = 0; FString Key; int32 Font = 0; };
struct FBRRScnBox { int32 Id = 0; int32 X = 0, Y = 0, W = 0, H = 0; };
struct FBRRScnSprite { int32 Group = 0; int32 First = 0; int32 X = 0, Y = 0; };
struct FBRRScnKey { int32 Id = 0; int32 ScanCode = 0; };

struct BRRFORMATS_API FBRRScn
{
	FString Name;
	FBRRImage Background;           // empty when the screen borrows another's (TRAKSCN, CARSCN)
	TArray<FColor> Palette;         // the picture's - the sprites are drawn through it
	TArray<FBRRImage> Sprites;      // the `spr` sheet, glyph by glyph
	TArray<FBRRScnText> Texts;
	TArray<FBRRScnBox> Boxes;
	TArray<FBRRScnSprite> AniSprites;
	TArray<FBRRScnKey> Keys;        // for the language asked for, then ANY
	int32 Mouse = -1;
	int32 SpriteFrames = 5;         // `anim .., 255, 0,1,2,3,4`: frames per anisprite
	int32 SpritePeriod = 5;         // in 14 ms timer ticks
	int32 FontPeriod = 5;

	const FBRRScnBox* FindBox(int32 Id) const;
};

namespace BRR
{
	// Loads `<Name>.SCN`. A screen with no `raw` of its own takes PictureArchive/PictureMember
	// instead (TRAKSCN: the circuit's VIEW.RAW; CARSCN: the theme's CARSEL.RAW). Language picks
	// the `iflang` block of keys ("ENG", ...).
	BRRFORMATS_API bool LoadScn(const FString& DataDir, const FString& Name, const FString& Language, FBRRScn& Out, FString& OutError,
		const FString& PictureArchive = FString(), const FString& PictureMember = FString());
}
