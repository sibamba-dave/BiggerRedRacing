#include "BRRVideo.h"

#include "Misc/FileHelper.h"

// A port of Tools/brr_video.py, which documents both formats; the decoding is line for line.

namespace
{
	const int32 ImaSteps[89] = {
		7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66,
		73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408,
		449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
		2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630,
		9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767 };
	const int32 ImaIndex[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };

	uint32 Rd32(const uint8* P) { return uint32(P[0]) | uint32(P[1]) << 8 | uint32(P[2]) << 16 | uint32(P[3]) << 24; }
	uint16 Rd16(const uint8* P) { return uint16(P[0] | P[1] << 8); }

	FColor Vga(const uint8* P) { auto E = [](uint8 V) { return uint8((V << 2 | V >> 4) & 255); }; return FColor(E(P[0]), E(P[1]), E(P[2]), 255); }

	// LSB-first bits, reading zeros past the end.
	struct FBits
	{
		const uint8* Data;
		int64 Size;
		int64 Pos; // in bits
		uint32 Get(int32 N)
		{
			uint32 V = 0;
			for (int32 K = 0; K < 4; ++K)
			{
				const int64 B = (Pos >> 3) + K;
				V |= uint32(B < Size ? Data[B] : 0) << (8 * K);
			}
			V >>= (Pos & 7);
			Pos += N;
			return N >= 32 ? V : V & ((1u << N) - 1);
		}
		uint32 ECode()
		{
			if (!Get(1)) return 0;
			uint32 V = Get(3);
			if (V != 7) return V + 1;
			V = Get(7);
			if (V != 127) return V + 8;
			return Get(12) + 135;
		}
	};

	void Cell(FBits& B, uint8 (&Out)[4])
	{
		const uint32 Mask = B.Get(4);
		if (Mask == 0 || Mask == 15)
		{
			const uint8 C = uint8(B.Get(7) * 2 + (Mask == 15));
			Out[0] = Out[1] = Out[2] = Out[3] = C;
			return;
		}
		const uint8 C[2] = { uint8(B.Get(8)), uint8(B.Get(8)) };
		for (int32 I = 0; I < 4; ++I) Out[I] = C[(Mask >> I) & 1];
	}
}

struct FBRRVideo::FImpl
{
	enum class EKind { None, Rpl, Mvi } Kind = EKind::None;
	TArray<uint8> Data;
	int32 W = 0, H = 0, Count = 0, Frame = -1;
	double Fps = 15;
	TArray<uint8> Indices;
	FColor Palette[256];
	FBRRImage Picture;
	TArray<int16> Sound;
	int32 Rate = 22050;
	// RPL: the chunk catalogue; MVI: where the next frame's records start.
	struct FChunk { int64 Offset; int32 Video, Audio; };
	TArray<FChunk> Chunks;
	int64 Next = 0;

	bool OpenRpl(FString& OutError)
	{
		TArray<FString> Lines;
		const int32 HeaderBytes = FMath::Min(Data.Num(), 1024);
		FString Header;
		for (int32 I = 0; I < HeaderBytes; ++I) Header.AppendChar(TCHAR(Data[I]));
		Header.ParseIntoArray(Lines, TEXT("\n"), false);
		auto Field = [&](int32 I) { FString F = Lines.IsValidIndex(I) ? Lines[I].TrimStartAndEnd() : FString(); int32 Sp; if (F.FindChar(' ', Sp)) F.LeftInline(Sp); return F; };
		if (Field(4) != TEXT("122")) { OutError = TEXT("ARMovie: not Escape 122"); return false; }
		W = FCString::Atoi(*Field(5));
		H = FCString::Atoi(*Field(6));
		Rate = FCString::Atoi(*Field(10));
		const int64 Catalogue = FCString::Atoi64(*Field(17));
		if (W <= 0 || H <= 0 || Catalogue <= 0 || Catalogue >= Data.Num()) { OutError = TEXT("ARMovie: bad header"); return false; }
		// "offset,video;audio" entries, whitespace separated.
		FString Cat;
		for (int64 I = Catalogue; I < Data.Num(); ++I) Cat.AppendChar(TCHAR(Data[I]));
		TArray<FString> Entries;
		Cat.ParseIntoArrayWS(Entries);
		for (FString E : Entries)
		{
			if (!E.Contains(TEXT(","))) continue;
			E.ReplaceInline(TEXT(";"), TEXT(","));
			TArray<FString> P;
			E.ParseIntoArray(P, TEXT(","));
			if (P.Num() < 3) continue;
			const FChunk C{ FCString::Atoi64(*P[0]), FCString::Atoi(*P[1]), FCString::Atoi(*P[2]) };
			if (C.Offset + C.Video + C.Audio <= Data.Num()) Chunks.Add(C);
		}
		Count = Chunks.Num();
		// The sound, all of it: Eidos 4-bit ADPCM, high nibble first.
		int32 Pred = 0, Step = 0;
		for (const FChunk& C : Chunks)
		{
			for (int32 I = 0; I < C.Audio; ++I)
			{
				const uint8 B = Data[C.Offset + C.Video + I];
				for (const uint8 Code : { uint8(B >> 4), uint8(B & 15) })
				{
					const int32 Diff = (Code & 7) * ImaSteps[Step] >> 2;
					Pred = FMath::Clamp(Code & 8 ? Pred - Diff : Pred + Diff, -32768, 32767);
					Step = FMath::Clamp(Step + ImaIndex[Code], 0, 88);
					Sound.Add(int16(Pred));
				}
			}
		}
		// The frame rate the sound keeps: samples per chunk.
		Fps = Sound.Num() > 0 && Count > 0 ? double(Rate) * Count / Sound.Num() : 15.0;
		return Count > 0;
	}

	void FrameRpl(const FChunk& C)
	{
		const uint8* Chunk = Data.GetData() + C.Offset;
		if (C.Video < 10 || Rd32(Chunk) != 0x116) return;
		const int32 Size = FMath::Min(int32(Rd32(Chunk + 4)), C.Video);
		const int32 PalBytes = Rd16(Chunk + 8);
		for (int32 I = 0; I + 2 < FMath::Min(PalBytes, 768) && 10 + I + 2 < Size; I += 3) Palette[I / 3] = Vga(Chunk + 10 + I);
		FBits Bits{ Chunk, Size, int64(10 + PalBytes) * 8 };
		int64 Skip = -1;
		for (int32 By = 0; By < H; By += 8)
		{
			for (int32 Bx = 0; Bx < W; Bx += 8)
			{
				if (Skip < 0) Skip = Bits.ECode();
				if (Skip > 0)
				{
					--Skip; // at 0 the next block is coded with no count of its own
					continue;
				}
				Skip = -1;
				uint8 Cells[16][4];
				bool Have[16] = {};
				while (!Bits.Get(1))
				{
					uint8 C4[4];
					Cell(Bits, C4);
					const uint32 Mask = Bits.Get(16);
					for (int32 I = 0; I < 16; ++I)
					{
						if ((Mask >> I) & 1) { FMemory::Memcpy(Cells[I], C4, 4); Have[I] = true; }
					}
				}
				if (!Bits.Get(1))
				{
					const uint32 Mask = Bits.Get(16);
					for (int32 I = 0; I < 16; ++I)
					{
						if ((Mask >> I) & 1) { Cell(Bits, Cells[I]); Have[I] = true; }
					}
				}
				for (int32 I = 0; I < 16; ++I)
				{
					if (!Have[I]) continue;
					const int32 O = (By + (I >> 2) * 2) * W + Bx + (I & 3) * 2;
					if (O + W + 1 >= Indices.Num()) continue;
					Indices[O] = Cells[I][0];
					Indices[O + 1] = Cells[I][1];
					Indices[O + W] = Cells[I][2];
					Indices[O + W + 1] = Cells[I][3];
				}
			}
		}
	}

	bool OpenMvi(FString& OutError)
	{
		const uint8* D = Data.GetData();
		W = Rd16(D + 4);
		H = Rd16(D + 6);
		Count = Rd16(D + 8);
		Fps = FMath::Max(int32(Rd16(D + 10)), 1);
		Next = 0x10;
		// The effects, mixed onto one track by the frame each starts on (8-bit unsigned).
		TArray<int32> Mix;
		int64 O = 0x10;
		for (int32 N = 0; N < Count && O < Data.Num(); ++N)
		{
			while (O < Data.Num() && D[O])
			{
				if (D[O] == 1) O += 769;
				else if (D[O] == 2 && O + 4 <= Data.Num())
				{
					const int32 Size = Rd16(D + O + 1);
					const uint8* Wav = D + O + 4;
					if (Size > 44)
					{
						Rate = int32(Rd32(Wav + 24));
						for (int32 At = 12; At + 8 <= Size; )
						{
							const uint32 Len = Rd32(Wav + At + 4);
							if (FMemory::Memcmp(Wav + At, "data", 4) == 0)
							{
								const int32 Start = int32(int64(N) * Rate / Fps);
								const int32 Samples = FMath::Min(int32(Len), Size - At - 8);
								if (Mix.Num() < Start + Samples) Mix.SetNumZeroed(Start + Samples);
								for (int32 I = 0; I < Samples; ++I) Mix[Start + I] += int32(Wav[At + 8 + I]) - 128;
								break;
							}
							At += 8 + int32(Len);
						}
					}
					O += 4 + Size;
				}
				else { OutError = TEXT("MVI: unknown record"); return false; }
			}
			O += 3 + Rd16(D + O + 1);
		}
		Sound.SetNumUninitialized(Mix.Num());
		for (int32 I = 0; I < Mix.Num(); ++I) Sound[I] = int16(FMath::Clamp(Mix[I], -128, 127) * 256);
		return Count > 0;
	}

	bool FrameMvi()
	{
		const uint8* D = Data.GetData();
		int64 O = Next;
		while (O < Data.Num() && D[O])
		{
			if (D[O] == 1)
			{
				for (int32 I = 0; I < 256; ++I) Palette[I] = Vga(D + O + 1 + I * 3);
				O += 769;
			}
			else O += 4 + Rd16(D + O + 1);
		}
		if (O + 3 > Data.Num()) return false;
		const int64 End = FMath::Min(O + 3 + Rd16(D + O + 1), int64(Data.Num()));
		int64 P = 0;
		for (int64 Q = O + 3; Q < End; )
		{
			const uint8 C = D[Q];
			int32 N;
			if (C >= 0x80)
			{
				N = C - 0x80;
				for (int32 I = 0; I < N && P + I < Indices.Num() && Q + 1 + I < End; ++I)
				{
					if (D[Q + 1 + I]) Indices[P + I] = D[Q + 1 + I];
				}
				Q += 1 + N;
			}
			else
			{
				uint8 V;
				if (C == 0) { N = Rd16(D + Q + 1); V = D[Q + 3]; Q += 4; }
				else { N = C; V = D[Q + 1]; Q += 2; }
				if (V) for (int32 I = 0; I < N && P + I < Indices.Num(); ++I) Indices[P + I] = V;
			}
			P += N;
		}
		Next = End;
		return true;
	}
};

FBRRVideo::FBRRVideo() : Impl(MakeUnique<FImpl>()) {}
FBRRVideo::~FBRRVideo() = default;

bool FBRRVideo::Open(const FString& Path, FString& OutError)
{
	*Impl = FImpl();
	if (!FFileHelper::LoadFileToArray(Impl->Data, *Path)) { OutError = Path + TEXT(": not found"); return false; }
	bool bOk = false;
	if (Impl->Data.Num() > 16 && FMemory::Memcmp(Impl->Data.GetData(), "ARMovie", 7) == 0)
	{
		Impl->Kind = FImpl::EKind::Rpl;
		bOk = Impl->OpenRpl(OutError);
	}
	else if (Impl->Data.Num() > 16 && FMemory::Memcmp(Impl->Data.GetData(), "MVI\0", 4) == 0)
	{
		Impl->Kind = FImpl::EKind::Mvi;
		bOk = Impl->OpenMvi(OutError);
	}
	else OutError = Path + TEXT(": neither ARMovie nor MVI");
	if (!bOk) { Impl->Kind = FImpl::EKind::None; return false; }
	Impl->Indices.SetNumZeroed(Impl->W * Impl->H);
	for (FColor& C : Impl->Palette) C = FColor::Black;
	Impl->Picture.Width = Impl->W;
	Impl->Picture.Height = Impl->H;
	Impl->Picture.Pixels.Init(FColor::Black, Impl->W * Impl->H);
	return true;
}

bool FBRRVideo::IsOpen() const { return Impl->Kind != FImpl::EKind::None; }
int32 FBRRVideo::Width() const { return Impl->W; }
int32 FBRRVideo::Height() const { return Impl->H; }
int32 FBRRVideo::FrameCount() const { return Impl->Count; }
double FBRRVideo::FramesPerSecond() const { return Impl->Fps; }
int32 FBRRVideo::FrameIndex() const { return Impl->Frame; }
const FBRRImage& FBRRVideo::Picture() const { return Impl->Picture; }
const TArray<int16>& FBRRVideo::Sound() const { return Impl->Sound; }
int32 FBRRVideo::SoundRate() const { return Impl->Rate; }

bool FBRRVideo::Advance()
{
	FImpl& I = *Impl;
	if (!IsOpen() || I.Frame + 1 >= I.Count) return false;
	++I.Frame;
	if (I.Kind == FImpl::EKind::Rpl) I.FrameRpl(I.Chunks[I.Frame]);
	else if (!I.FrameMvi()) return false;
	for (int32 P = 0; P < I.Indices.Num(); ++P) I.Picture.Pixels[P] = I.Palette[I.Indices[P]];
	return true;
}
