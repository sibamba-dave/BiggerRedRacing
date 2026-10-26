// Big Red Racing archive container, shared by .LEV .ENV .VHC .SCN .WAP .TEA.
//
// Adapted from OpenRedRacing src/formats/bigred_archive.{h,cpp}
// (https://github.com/nynjardin/OpenRedRacing, MIT, Copyright (c) 2026 PaPe).
//
// Layout: no file header; sequential entries of
//   char name[] '\0', u32 packed_size, u32 unpacked_size, u8 method, u8 data[packed_size]
// terminated by an empty name. Method 0 = stored, 1 = RLE, 2-6 = LZSS (offset bits = 6 - method).
#pragma once

#include "CoreMinimal.h"

struct BRRFORMATS_API FBRRArchiveEntry
{
	FString Name;
	uint32 PackedSize = 0;
	uint8 Method = 0;
	TArray<uint8> Data;
};

struct BRRFORMATS_API FBRRArchive
{
	// Entries in file order (names are unique in shipped data, but order is kept for diagnostics).
	TArray<FBRRArchiveEntry> Entries;

	// Case-insensitive lookup; returns nullptr when the member is absent.
	const FBRRArchiveEntry* Find(const FString& MemberName) const;

	// Decodes an in-memory archive. On failure returns false and fills OutError; Out is left partial.
	static bool Parse(TConstArrayView<uint8> Bytes, FBRRArchive& Out, FString& OutError);

	// Reads and decodes an archive from disk.
	static bool Load(const FString& Path, FBRRArchive& Out, FString& OutError);
};

namespace BRR
{
	// Folder containing ALWAYS.WAP: BRR_DATA_DIR env var, else [BRR] DataDir in DefaultGame.ini
	// (relative to the project dir). Returns an empty string when neither resolves to a real folder.
	BRRFORMATS_API FString GetDataDir();
}
