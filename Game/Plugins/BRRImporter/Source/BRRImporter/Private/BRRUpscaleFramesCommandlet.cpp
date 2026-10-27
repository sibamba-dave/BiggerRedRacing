#include "BRRUpscaleFramesCommandlet.h"

#include "BRRPixelArt.h"
#include "BRRWorld.h"
#include "HAL/FileManager.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"

DEFINE_LOG_CATEGORY_STATIC(LogBRRUpscaleFrames, Log, All);

UBRRUpscaleFramesCommandlet::UBRRUpscaleFramesCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 UBRRUpscaleFramesCommandlet::Main(const FString& Params)
{
	FString In, Out, Ranges;
	int32 Scale = 6;
	FParse::Value(*Params, TEXT("Scale="), Scale);
	if (!FParse::Value(*Params, TEXT("In="), In) || !FParse::Value(*Params, TEXT("Out="), Out) || !FParse::Value(*Params, TEXT("Frames="), Ranges, false))
	{
		UE_LOG(LogBRRUpscaleFrames, Error, TEXT("Usage: -run=BRRUpscaleFrames -In=<dir> -Out=<dir> -Frames=a-b,c-d [-Scale=6]"));
		return 1;
	}
	IFileManager::Get().MakeDirectory(*Out, true);
	TArray<FString> Parts;
	Ranges.ParseIntoArray(Parts, TEXT(","));
	int32 Done = 0;
	for (const FString& Part : Parts)
	{
		FString A, B;
		if (!Part.Split(TEXT("-"), &A, &B)) A = B = Part;
		const int32 First = FCString::Atoi(*A), End = FMath::Max(FCString::Atoi(*B), First + 1);
		for (int32 F = First; F < End; ++F)
		{
			const FString Name = FString::Printf(TEXT("frame_%04d.png"), F);
			FImage Loaded;
			if (!FImageUtils::LoadImage(*(In / Name), Loaded)) continue;
			FImage Bgra;
			Loaded.CopyTo(Bgra, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
			FBRRImage Frame;
			Frame.Width = Bgra.SizeX;
			Frame.Height = Bgra.SizeY;
			Frame.Pixels.SetNumUninitialized(Frame.Width * Frame.Height);
			FMemory::Memcpy(Frame.Pixels.GetData(), Bgra.RawData.GetData(), Frame.Pixels.Num() * sizeof(FColor));
			for (FColor& C : Frame.Pixels) C.A = 255; // a whole picture: nothing see-through
			const FBRRImage Big = BRR::SmoothPixelArt(Frame, Scale, 3);
			FImageView View(const_cast<FColor*>(Big.Pixels.GetData()), Big.Width, Big.Height);
			if (FImageUtils::SaveImageByExtension(*(Out / Name), View)) ++Done;
		}
	}
	UE_LOG(LogBRRUpscaleFrames, Display, TEXT("%d frames upscaled x%d into %s"), Done, Scale, *Out);
	return Done > 0 ? 0 : 1;
}
