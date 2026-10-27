#include "BRRArt.h"

#include "BRRPixelArt.h"
#include "BRRWorld.h"
#include "Engine/Texture2D.h"
#include "HAL/IConsoleManager.h"

namespace
{
	TAutoConsoleVariable<int32> CVarSmoothArt(TEXT("brr.SmoothArt"), 1,
		TEXT("1: the remastered look draws the original's fonts and sprites smoothed at many times their size; 0: their own pixels."));

	struct FArtInfo
	{
		FIntPoint Size;
		bool bSmooth = false;
	};
	TMap<TWeakObjectPtr<const UTexture2D>, FArtInfo>& Infos()
	{
		static TMap<TWeakObjectPtr<const UTexture2D>, FArtInfo> Map;
		return Map;
	}
}

bool BRRArt::SmoothWanted()
{
	static const IConsoleVariable* Look = IConsoleManager::Get().FindConsoleVariable(TEXT("brr.Look"));
	return CVarSmoothArt.GetValueOnGameThread() != 0 && (!Look || Look->GetInt() != 0);
}

UTexture2D* BRRArt::MakeTexture(const FBRRImage& Image, bool bSmooth)
{
	if (Image.Width <= 0 || Image.Height <= 0) return nullptr;
	// Small art many times over, a big one (a map) a few: never past 2048 a side.
	const int32 Longest = FMath::Max(Image.Width, Image.Height);
	const int32 Scale = Longest <= 64 ? 8 : Longest <= 256 ? 4 : 2;
	const bool bDo = bSmooth && Longest * Scale <= 2048 && BRR::IsFlatArt(Image);
	const FBRRImage Smooth = bDo ? BRR::SmoothPixelArt(Image, Scale) : FBRRImage();
	const FBRRImage& Use = bDo ? Smooth : Image;
	UTexture2D* T = UTexture2D::CreateTransient(Use.Width, Use.Height, PF_B8G8R8A8);
	if (!T) return nullptr;
	T->Filter = bDo ? TF_Bilinear : TF_Nearest;
	T->SRGB = true;
	T->NeverStream = true;
	void* Data = T->GetPlatformData()->Mips[0].BulkData.Lock(LOCK_READ_WRITE);
	FMemory::Memcpy(Data, Use.Pixels.GetData(), Use.Pixels.Num() * sizeof(FColor));
	T->GetPlatformData()->Mips[0].BulkData.Unlock();
	T->UpdateResource();
	// Forget textures gone since.
	for (auto It = Infos().CreateIterator(); It; ++It) if (!It->Key.IsValid()) It.RemoveCurrent();
	Infos().Add(T, { FIntPoint(Image.Width, Image.Height), bDo });
	return T;
}

FVector2D BRRArt::Size(const UTexture2D* Texture)
{
	if (!Texture) return FVector2D::ZeroVector;
	if (const FArtInfo* Info = Infos().Find(Texture)) return FVector2D(Info->Size);
	return FVector2D(Texture->GetSizeX(), Texture->GetSizeY());
}

bool BRRArt::IsSmooth(const UTexture2D* Texture)
{
	const FArtInfo* Info = Texture ? Infos().Find(Texture) : nullptr;
	return Info && Info->bSmooth;
}
