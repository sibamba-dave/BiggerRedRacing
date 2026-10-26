// Adapted from OpenRedRacing src/formats/bigred_archive.cpp (MIT, Copyright (c) 2026 PaPe).
#include "BRRArchive.h"

#include "HAL/PlatformMisc.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"

IMPLEMENT_MODULE(FDefaultModuleImpl, BRRFormats);

namespace
{
	bool DecompressRle(TConstArrayView<uint8> In, uint32 UnpackedSize, TArray<uint8>& Out, FString& OutError)
	{
		// The largest repeat emits 130 bytes per input byte; reject impossible declarations up front.
		if (uint64(UnpackedSize) > uint64(In.Num()) * 130u)
		{
			OutError = TEXT("RLE: advertised output cannot fit the compressed stream");
			return false;
		}
		Out.Reset(UnpackedSize);
		int32 I = 0;
		while (I < In.Num())
		{
			const uint8 Flag = In[I++];
			if (Flag > 127)
			{
				const int32 Count = Flag - 125;
				if (I >= In.Num() || uint32(Out.Num() + Count) > UnpackedSize)
				{
					OutError = TEXT("RLE: bad repeat run");
					return false;
				}
				const uint8 Value = In[I++];
				Out.AddUninitialized(Count);
				FMemory::Memset(Out.GetData() + Out.Num() - Count, Value, Count);
			}
			else
			{
				const int32 Count = Flag + 1;
				if (Count > In.Num() - I || uint32(Out.Num() + Count) > UnpackedSize)
				{
					OutError = TEXT("RLE: bad literal run");
					return false;
				}
				Out.Append(In.GetData() + I, Count);
				I += Count;
			}
		}
		if (uint32(Out.Num()) != UnpackedSize)
		{
			OutError = TEXT("RLE: output size mismatch");
			return false;
		}
		return true;
	}

	bool DecompressLzss(TConstArrayView<uint8> In, uint32 UnpackedSize, int32 Method, TArray<uint8>& Out, FString& OutError)
	{
		const int32 OffsetBits = 6 - Method;
		const int32 RunBits = 8 - OffsetBits;
		const int32 MaxRun = (1 << RunBits) + 2;
		const int32 MaxOffset = (1 << (OffsetBits + 8)) - 1;

		if (uint64(UnpackedSize) > uint64(In.Num()) * uint64(MaxRun))
		{
			OutError = TEXT("LZSS: advertised output cannot fit the compressed stream");
			return false;
		}

		Out.SetNumUninitialized(UnpackedSize);
		uint32 OutLen = 0;
		// +1 vs MaxOffset: the reference implementation lets the ring cursor reach MaxOffset before wrapping.
		TArray<uint8> Ring;
		Ring.SetNumZeroed(MaxOffset + 1);
		int32 RingPos = 0;

		int32 I = 0;
		const int32 N = In.Num();
		while (I < N)
		{
			const uint8 Flag = In[I++];
			for (int32 Bit = 0; Bit < 8; ++Bit)
			{
				if ((Flag >> Bit) & 1)
				{
					if (I >= N || OutLen >= UnpackedSize)
					{
						OutError = TEXT("LZSS: bad literal");
						return false;
					}
					const uint8 Byte = In[I++];
					Out[OutLen++] = Byte;
					Ring[RingPos] = Byte;
					RingPos = RingPos < MaxOffset ? RingPos + 1 : 0;
				}
				else
				{
					if (N - I < 2)
					{
						OutError = TEXT("LZSS: truncated back-reference");
						return false;
					}
					const uint8 B0 = In[I];
					const uint8 B1 = In[I + 1];
					I += 2;

					// Bit-for-bit port of ORR's decode loop: `Count` deliberately carries over from
					// the B0 loop and is only reset once K reaches OffsetBits.
					int32 Offset = -1;
					int32 Count = 0;
					for (int32 K = 0; K < 8; ++K)
					{
						if ((B0 >> K) & 1) Offset += 1 << Count;
						++Count;
					}
					int32 Run = 2;
					for (int32 K = 0; K < 8; ++K)
					{
						if (K == OffsetBits) Count = 0;
						if ((B1 >> K) & 1)
						{
							if (K >= OffsetBits) Run += 1 << Count;
							else Offset += 1 << Count;
						}
						++Count;
					}

					if (Offset < 0 || Run > MaxRun || OutLen + Run > UnpackedSize)
					{
						OutError = TEXT("LZSS: bad back-reference");
						return false;
					}
					for (int32 R = 0; R < Run; ++R)
					{
						if (Offset > MaxOffset || uint32(Offset) >= OutLen)
						{
							OutError = TEXT("LZSS: invalid offset during copy");
							return false;
						}
						const uint8 Byte = Ring[Offset];
						Ring[RingPos] = Byte;
						RingPos = RingPos < MaxOffset ? RingPos + 1 : 0;
						Out[OutLen++] = Byte;
						Offset = Offset < MaxOffset ? Offset + 1 : 0;
					}
				}
				if (I >= N) break;
			}
		}
		if (OutLen != UnpackedSize)
		{
			OutError = TEXT("LZSS: output size mismatch");
			return false;
		}
		return true;
	}
}

const FBRRArchiveEntry* FBRRArchive::Find(const FString& MemberName) const
{
	return Entries.FindByPredicate([&](const FBRRArchiveEntry& E) { return E.Name.Equals(MemberName, ESearchCase::IgnoreCase); });
}

bool FBRRArchive::Parse(TConstArrayView<uint8> Bytes, FBRRArchive& Out, FString& OutError)
{
	Out.Entries.Reset();
	const int32 Size = Bytes.Num();
	int32 Pos = 0;
	while (Pos < Size)
	{
		int32 End = Pos;
		while (End < Size && Bytes[End] != 0) ++End;
		if (End >= Size)
		{
			OutError = TEXT("archive: unterminated filename");
			return false;
		}
		if (End == Pos) break; // empty name = end of archive

		FBRRArchiveEntry& Entry = Out.Entries.AddDefaulted_GetRef();
		Entry.Name = FString::ConstructFromPtrSize(reinterpret_cast<const ANSICHAR*>(Bytes.GetData() + Pos), End - Pos);
		Pos = End + 1;

		if (Size - Pos < 9)
		{
			OutError = FString::Printf(TEXT("archive: truncated header for %s"), *Entry.Name);
			return false;
		}
		uint32 UnpackedSize = 0;
		FMemory::Memcpy(&Entry.PackedSize, Bytes.GetData() + Pos, 4);
		FMemory::Memcpy(&UnpackedSize, Bytes.GetData() + Pos + 4, 4);
		Entry.Method = Bytes[Pos + 8];
		Pos += 9;

		if (Entry.Method > 6 || Entry.PackedSize > uint32(Size - Pos))
		{
			OutError = FString::Printf(TEXT("archive: bad method %d or truncated chunk for %s"), Entry.Method, *Entry.Name);
			return false;
		}
		const TConstArrayView<uint8> Chunk(Bytes.GetData() + Pos, Entry.PackedSize);
		Pos += Entry.PackedSize;

		bool bOk = true;
		FString Error;
		if (Entry.Method == 0)
		{
			bOk = Entry.PackedSize == UnpackedSize;
			Error = TEXT("stored size mismatch");
			Entry.Data = TArray<uint8>(Chunk);
		}
		else if (Entry.Method == 1)
		{
			bOk = DecompressRle(Chunk, UnpackedSize, Entry.Data, Error);
		}
		else
		{
			bOk = DecompressLzss(Chunk, UnpackedSize, Entry.Method, Entry.Data, Error);
		}
		if (!bOk)
		{
			OutError = FString::Printf(TEXT("%s: %s"), *Entry.Name, *Error);
			return false;
		}
	}
	return true;
}

bool FBRRArchive::Load(const FString& Path, FBRRArchive& Out, FString& OutError)
{
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *Path))
	{
		OutError = FString::Printf(TEXT("could not read %s"), *Path);
		return false;
	}
	return Parse(Bytes, Out, OutError);
}

FString BRR::GetDataDir()
{
	FString Dir = FPlatformMisc::GetEnvironmentVariable(TEXT("BRR_DATA_DIR"));
	if (Dir.IsEmpty() && GConfig)
	{
		GConfig->GetString(TEXT("BRR"), TEXT("DataDir"), Dir, GGameIni);
	}
	if (Dir.IsEmpty()) return FString();
	if (FPaths::IsRelative(Dir))
	{
		Dir = FPaths::Combine(FPaths::ProjectDir(), Dir);
	}
	FPaths::CollapseRelativeDirectories(Dir);
	return FPaths::DirectoryExists(Dir) ? Dir : FString();
}
