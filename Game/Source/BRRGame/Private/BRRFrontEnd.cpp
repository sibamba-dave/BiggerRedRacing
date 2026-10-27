#include "BRRFrontEnd.h"
#include "BRRArt.h"
#include "MediaPlayer.h"
#include "MediaSoundComponent.h"
#include "MediaTexture.h"

#include "BRRAudio.h"
#include "BRRGameInstance.h"
#include "BRRLook.h"
#include "BRRMenuScreens.h"
#include "BRRSoundOutput.h"
#include "AudioDevice.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Canvas.h"
#include "Engine/GameViewportClient.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/Texture2D.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/KismetMaterialLibrary.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "UnrealClient.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialParameterCollection.h"

namespace
{
	constexpr double TimerTick = 0.014;      // the original's 14 ms timer: the menus' animations count it
	constexpr double BackdropDistance = 1000; // cm in front of the menu camera
	constexpr float MenuFov = 60.f;          // horizontal, over the 4:3 picture

	// DOS set-1 scan codes the scripts' `key` lines use -> UE keys.
	FKey KeyForScanCode(int32 Code)
	{
		static const TMap<int32, FKey> Map = {
			{ 0x01, EKeys::Escape }, { 0x1C, EKeys::Enter }, { 0x39, EKeys::SpaceBar },
			{ 0x02, EKeys::One }, { 0x03, EKeys::Two }, { 0x04, EKeys::Three }, { 0x05, EKeys::Four }, { 0x06, EKeys::Five },
			{ 0x07, EKeys::Six }, { 0x08, EKeys::Seven }, { 0x09, EKeys::Eight }, { 0x0A, EKeys::Nine }, { 0x0B, EKeys::Zero },
			{ 0x10, EKeys::Q }, { 0x11, EKeys::W }, { 0x12, EKeys::E }, { 0x13, EKeys::R }, { 0x14, EKeys::T }, { 0x15, EKeys::Y },
			{ 0x16, EKeys::U }, { 0x17, EKeys::I }, { 0x18, EKeys::O }, { 0x19, EKeys::P }, { 0x1E, EKeys::A }, { 0x1F, EKeys::S },
			{ 0x20, EKeys::D }, { 0x21, EKeys::F }, { 0x22, EKeys::G }, { 0x23, EKeys::H }, { 0x24, EKeys::J }, { 0x25, EKeys::K },
			{ 0x26, EKeys::L }, { 0x2C, EKeys::Z }, { 0x2D, EKeys::X }, { 0x2E, EKeys::C }, { 0x2F, EKeys::V }, { 0x30, EKeys::B },
			{ 0x31, EKeys::N }, { 0x32, EKeys::M },
		};
		const FKey* K = Map.Find(Code);
		return K ? *K : EKeys::Invalid;
	}
}

// --- painter ---------------------------------------------------------------------------------

void FBRRMenuPainter::Image(UTexture2D* Texture, double X, double Y, double W, double H, FLinearColor Tint)
{
	if (!Texture || !Canvas) return;
	if (W < 0) W = BRRArt::Size(Texture).X;
	if (H < 0) H = BRRArt::Size(Texture).Y;
	FCanvasTileItem Tile(ToScreen(X, Y), Texture->GetResource(), FVector2D(W * Scale.X, H * Scale.Y), Tint);
	Tile.BlendMode = BRRArt::IsSmooth(Texture) ? SE_BLEND_Translucent : SE_BLEND_Masked;
	Canvas->DrawItem(Tile);
}

double FBRRMenuPainter::TextWidth(ABRRFrontEndGameMode* Menu, const FString& Words, int32 Font)
{
	const bool bWibbly = Font >= 2;
	const FBRRMenuFont& F = Menu->Font(bWibbly ? 2 : 0);
	double W = 0;
	for (TCHAR C : Words)
	{
		const int32 G = BRR::InterfaceGlyph(C, bWibbly);
		W += F.Sizes.IsValidIndex(G) && F.Sizes[G].X > 0 ? F.Sizes[G].X : F.Height / 3;
	}
	return W;
}

double FBRRMenuPainter::Text(const FString& Words, double X, double Y, int32 Font, int32 Ramp, const TArray<FColor>& Palette, int32 WibblePhase)
{
	const bool bWibbly = Font >= 2;
	int32 Sheet = 0;
	if (bWibbly)
	{
		// The `anim 5,5,7, 2,3,4,5,6` cycle: the five drawings in turn, every 5 timer ticks.
		const int32 Frame = int32(Menu->Seconds() / (5 * TimerTick));
		Sheet = 2 + (Frame + WibblePhase) % 5;
	}
	const FBRRMenuFont& F = Menu->Font(Sheet);
	// Ramp 5 is yellow in every menu palette: the focused choice. A screen on a picture of its
	// own (TRAKSCN's VIEW.RAW) has no such ramp, so the focus is that yellow everywhere.
	const int32 Index = Ramp >= 0 ? Ramp * 16 + 2 : 16;
	const FLinearColor Tint = Ramp == FocusRamp ? FLinearColor(FColor(255, 231, 33))
		: Palette.IsValidIndex(Index) ? FLinearColor(Palette[Index]) : FLinearColor::White;
	for (TCHAR C : Words)
	{
		const int32 G = BRR::InterfaceGlyph(C, bWibbly);
		if (F.Glyphs.IsValidIndex(G) && F.Glyphs[G]) Image(F.Glyphs[G], X, Y, F.Sizes[G].X, F.Sizes[G].Y, Tint);
		X += F.Sizes.IsValidIndex(G) && F.Sizes[G].X > 0 ? F.Sizes[G].X : F.Height / 3;
	}
	return X;
}

void FBRRMenuPainter::Entry(const FString& Key, int32 Font, const TArray<FColor>& Palette, FVector2D At, int32 Ramp, int32 WibblePhase)
{
	UBRRGameInstance* Game = Menu->Game();
	if (!Game) return;
	double X = At.X, Y = At.Y;
	for (const FBRRTextRun& R : Game->Text.Layout(Key))
	{
		if (R.X >= 0) X = At.X + R.X;
		if (R.Y >= 0) Y = At.Y + R.Y;
		const int32 RunFont = R.Font >= 0 ? R.Font : Font;
		X = Text(R.Text, X, Y, RunFont, Ramp != -2 ? Ramp : R.Ramp, Palette, WibblePhase);
	}
}

void FBRRMenuPainter::Box(double X, double Y, double W, double H, FLinearColor Colour)
{
	FCanvasTileItem Tile(ToScreen(X, Y), FVector2D(W * Scale.X, H * Scale.Y), Colour);
	Tile.BlendMode = SE_BLEND_Translucent;
	Canvas->DrawItem(Tile);
}

// --- HUD ----------------------------------------------------------------------------------

void ABRRMenuHUD::DrawHUD()
{
	Super::DrawHUD();
	if (ABRRFrontEndGameMode* Mode = GetWorld()->GetAuthGameMode<ABRRFrontEndGameMode>()) Mode->Draw(Canvas);
}

// --- front end ------------------------------------------------------------------------------

ABRRFrontEndGameMode::ABRRFrontEndGameMode()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bTickEvenWhenPaused = true;
	HUDClass = ABRRMenuHUD::StaticClass();
	DefaultPawnClass = nullptr;
	SoundOutput = CreateDefaultSubobject<UBRRSoundOutput>(TEXT("Sound"));
	SoundOutput->bAutoActivate = false;
}

UBRRGameInstance* ABRRFrontEndGameMode::Game() const { return GetGameInstance<UBRRGameInstance>(); }

FString ABRRFrontEndGameMode::DataDir() const { return Game() ? Game()->DataDir : FString(); }

void ABRRFrontEndGameMode::StartPlay()
{
	Super::StartPlay();
	UBRRGameInstance* G = Game();
	if (!G || G->DataDir.IsEmpty())
	{
		UE_LOG(LogTemp, Error, TEXT("Front end: no game data (set BRR_DATA_DIR) or no BRRGameInstance"));
		return;
	}
	LoadFonts();

	// The stage: a camera, the picture on a backdrop filling its 4:3 view, the classic look so
	// every pixel of it is the palette's.
	Camera = GetWorld()->SpawnActor<ACameraActor>(FVector::ZeroVector, FRotator::ZeroRotator);
	UCameraComponent* Lens = Camera->GetCameraComponent();
	Lens->SetFieldOfView(MenuFov);
	Lens->bConstrainAspectRatio = true;
	Lens->SetAspectRatio(4.f / 3.f);
	if (UStaticMesh* Plane = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane")))
	{
		const double Width = 2.0 * BackdropDistance * FMath::Tan(FMath::DegreesToRadians(MenuFov * 0.5));
		// The plane faces up; pitched to face the camera, and turned a quarter about its own
		// normal first so the picture stands upright.
		const FQuat Facing = FQuat(FRotator(90, 0, 0)) * FQuat(FRotator(0, 90, 0));
		Backdrop = GetWorld()->SpawnActor<AStaticMeshActor>(FVector(BackdropDistance, 0, 0), Facing.Rotator());
		Backdrop->GetStaticMeshComponent()->SetMobility(EComponentMobility::Movable);
		Backdrop->GetStaticMeshComponent()->SetStaticMesh(Plane);
		Backdrop->SetActorScale3D(FVector(Width / 100.0, Width * 0.75 / 100.0, 1.0));
		if (UMaterialInterface* Master = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/BRR/Materials/M_BRR_Textured.M_BRR_Textured")))
		{
			BackdropMaterial = UMaterialInstanceDynamic::Create(Master, this);
			Backdrop->GetStaticMeshComponent()->SetMaterial(0, BackdropMaterial);
		}
	}
	UMaterialParameterCollection* Collection = LoadObject<UMaterialParameterCollection>(nullptr, TEXT("/Game/BRR/Materials/MPC_BRR.MPC_BRR"));
	Look = GetWorld()->SpawnActor<ABRRLook>();
	Look->Build(FVector(0.3, 0.2, -1).GetSafeNormal(), Collection);
	Look->Apply(false, false, false, true);
	if (Collection) UKismetMaterialLibrary::SetScalarParameterValue(this, Collection, TEXT("DrawDistance"), 1e9f);
	if (APlayerController* PC = GetWorld()->GetFirstPlayerController())
	{
		PC->SetViewTarget(Camera);
		// The menus take the mouse: a visible cursor that is never captured or locked (the
		// project's capture-on-launch default swallowed it, so hover and click never arrived).
		PC->bShowMouseCursor = true;
		PC->bEnableClickEvents = true;
		PC->bEnableMouseOverEvents = true;
		PC->SetInputMode(FInputModeGameAndUI().SetHideCursorDuringCapture(false).SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock));
		if (UGameViewportClient* Viewport = GetWorld()->GetGameViewport())
		{
			Viewport->SetMouseCaptureMode(EMouseCaptureMode::NoCapture);
			Viewport->SetMouseLockMode(EMouseLockMode::DoNotLock);
		}
	}

	// The front end's music: CD track 2, round and round.
	if (FAudioDevice* Device = GetWorld()->GetAudioDeviceRaw())
	{
		if (BRRAudio::Open(int32(Device->GetSampleRate())))
		{
			SoundOutput->Start();
			BRRAudio::PlayMusic(2, 2);
			FString Dump; // -BRRAudioDump=<wav>: the menus' sound, by ear or by numbers (as the race has it)
			if (FParse::Value(FCommandLine::Get(), TEXT("BRRAudioDump="), Dump)) BRRAudio::DumpTo(Dump, 12.f);
		}
	}
	G->ApplySettings();

	BRRMenu::Open(*this);

	FString WalkList;
	if (FParse::Value(FCommandLine::Get(), TEXT("BRRMenuWalk="), WalkList, false))
	{
		TArray<FString> Ids;
		WalkList.ParseIntoArray(Ids, TEXT(","));
		for (const FString& Id : Ids) Walk.Add(FCString::Atoi(*Id));
		WalkStep = 0;
		WalkAt = 2.0;
	}
}

void ABRRFrontEndGameMode::UpdateWalk()
{
	if (WalkStep < 0 || Clock < WalkAt) return;
	FScreenshotRequest::RequestScreenshot(FString::Printf(TEXT("Menu_%d.png"), WalkStep), false, false);
	if (WalkStep >= Walk.Num())
	{
		WalkStep = -1;
		FTimerHandle Quit;
		GetWorldTimerManager().SetTimer(Quit, FTimerDelegate::CreateLambda([this] { UKismetSystemLibrary::QuitGame(this, nullptr, EQuitPreference::Quit, false); }), 1.0f, false, -1.f);
		return;
	}
	const int32 Id = Walk[WalkStep++];
	WalkAt = Clock + 1.5;
	if (TSharedPtr<FBRRMenuScreen> S = Top())
	{
		if (Id == 99) {} // a wait
		else if (Id == -1) S->OnBack();
		else if (Id == -3) BRRMenu::ShowSampleResults(*this);
		else if (Id == -4 || Id == -5) BRRMenu::ShowVideo(*this, Id == -5);
		else if (Id == -2) S->OnLeftRight(1);
		else S->OnBox(Id);
	}
}

void ABRRFrontEndGameMode::EndPlay(const EEndPlayReason::Type Reason)
{
	for (const TSharedRef<FBRRMenuScreen>& S : Stack) S->Leave();
	Stack.Reset();
	BRRAudio::StopEffects();
	Super::EndPlay(Reason);
}

void ABRRFrontEndGameMode::LoadFonts()
{
	const FString Dir = DataDir();
	FString Error;
	BRR::LoadPalette(Dir, TEXT("ALWAYS.WAP"), TEXT("DEFAULT.COL"), DefaultPalette, Error);
	bArtSmooth = BRRArt::SmoothWanted();
	Fonts.Reset();
	Fonts.SetNum(7);
	auto Load = [&](int32 Slot, const TCHAR* Member)
	{
		TArray<FBRRImage> Glyphs;
		if (!BRR::LoadSprites(Dir, TEXT("ALWAYS.WAP"), Member, DefaultPalette, Glyphs, Error)) return;
		FBRRMenuFont& F = Fonts[Slot];
		F.Height = 0;
		for (const FBRRImage& G : Glyphs)
		{
			F.Glyphs.Add(G.Width > 0 && G.Height > 0 ? MakeTexture(G, true) : nullptr);
			F.Sizes.Add(FIntPoint(G.Width, G.Height));
			F.Height = FMath::Max(F.Height, G.Height);
		}
	};
	Load(0, TEXT("SML_ALPH.SPR"));
	Fonts[1] = Fonts[0];
	const TCHAR* Wibs[5] = { TEXT("WIBS1.SPR"), TEXT("WIBS2.SPR"), TEXT("WIBS3.SPR"), TEXT("WIBS4.SPR"), TEXT("WIBS5.SPR") };
	for (int32 K = 0; K < 5; ++K) Load(2 + K, Wibs[K]);
}

const FBRRMenuFont& ABRRFrontEndGameMode::Font(int32 Index) const
{
	return Fonts[FMath::Clamp(Index, 0, Fonts.Num() - 1)];
}

UTexture2D* ABRRFrontEndGameMode::MakeTexture(const FBRRImage& Image, bool bArt)
{
	if (bArt)
	{
		// Interface art (a font, a sprite): smoothed in the remastered look (BRRArt).
		UTexture2D* Art = BRRArt::MakeTexture(Image, BRRArt::SmoothWanted());
		if (Art) Textures.Add(Art);
		return Art;
	}
	UTexture2D* T = UTexture2D::CreateTransient(Image.Width, Image.Height, PF_B8G8R8A8);
	if (!T) return nullptr;
	T->Filter = TF_Nearest;
	T->SRGB = true;
	T->NeverStream = true;
	void* Data = T->GetPlatformData()->Mips[0].BulkData.Lock(LOCK_READ_WRITE);
	FMemory::Memcpy(Data, Image.Pixels.GetData(), Image.Pixels.Num() * sizeof(FColor));
	T->GetPlatformData()->Mips[0].BulkData.Unlock();
	T->UpdateResource();
	Textures.Add(T);
	return T;
}

void ABRRFrontEndGameMode::ShowPicture(const FBRRImage& Picture)
{
	if (!BackdropMaterial || Picture.Width <= 0) return;
	UTexture2D* T = MakeTexture(Picture);
	BackdropMaterial->SetTextureParameterValue(TEXT("Atlas"), T);
	BackdropMaterial->SetTextureParameterValue(TEXT("AtlasHD"), T);
}

bool ABRRFrontEndGameMode::PlayMovie(const FString& Path)
{
	if (!FPaths::FileExists(Path)) return false;
	if (!MoviePlayer)
	{
		MoviePlayer = NewObject<UMediaPlayer>(this);
		MoviePlayer->PlayOnOpen = true;
		MoviePlayer->SetLooping(false);
		MovieOut = NewObject<UMediaTexture>(this);
		MovieOut->AutoClear = true;
		MovieOut->SetMediaPlayer(MoviePlayer);
		MovieOut->UpdateResource();
		MovieSound = NewObject<UMediaSoundComponent>(this, TEXT("MovieSound"));
		MovieSound->bIsUISound = true;
		MovieSound->SetMediaPlayer(MoviePlayer);
		MovieSound->RegisterComponentWithWorld(GetWorld());
	}
	// The movie's sound runs only while a movie does: left running on a closed player it starved
	// and buzzed under the menus from the end of the intro on (a play test).
	MovieSound->Start();
	bMovieStarted = false;
	// The whole window, not the menus' 4:3.
	if (Camera) Camera->GetCameraComponent()->bConstrainAspectRatio = false;
	return MoviePlayer->OpenFile(Path);
}

void ABRRFrontEndGameMode::StopMovie()
{
	if (MoviePlayer) MoviePlayer->Close();
	if (MovieSound) MovieSound->Stop();
	bMovieStarted = false;
	if (Camera) Camera->GetCameraComponent()->bConstrainAspectRatio = true;
}

bool ABRRFrontEndGameMode::IsMoviePlaying()
{
	if (!MoviePlayer) return false;
	if (MoviePlayer->IsPlaying())
	{
		bMovieStarted = true;
		return true;
	}
	// Opening: not started yet. Started and stopped: the end.
	return !bMovieStarted && (MoviePlayer->IsPreparing() || !MoviePlayer->GetUrl().IsEmpty());
}

UTexture* ABRRFrontEndGameMode::MovieTexture() const
{
	return MovieOut;
}

void ABRRFrontEndGameMode::ShowFrame(const FBRRImage& Frame)
{
	if (!BackdropMaterial || Frame.Width <= 0) return;
	if (!FrameTexture || FrameTexture->GetSizeX() != Frame.Width || FrameTexture->GetSizeY() != Frame.Height)
	{
		FrameTexture = MakeTexture(Frame);
		BackdropMaterial->SetTextureParameterValue(TEXT("Atlas"), FrameTexture);
		BackdropMaterial->SetTextureParameterValue(TEXT("AtlasHD"), FrameTexture);
		return;
	}
	FColor* Copy = new FColor[Frame.Pixels.Num()];
	FMemory::Memcpy(Copy, Frame.Pixels.GetData(), Frame.Pixels.Num() * sizeof(FColor));
	FUpdateTextureRegion2D* Region = new FUpdateTextureRegion2D(0, 0, 0, 0, Frame.Width, Frame.Height);
	FrameTexture->UpdateTextureRegions(0, 1, Region, Frame.Width * sizeof(FColor), sizeof(FColor), reinterpret_cast<uint8*>(Copy),
		[](uint8* Data, const FUpdateTextureRegion2D* Regions)
		{
			delete[] reinterpret_cast<FColor*>(Data);
			delete Regions;
		});
	BackdropMaterial->SetTextureParameterValue(TEXT("Atlas"), FrameTexture);
	BackdropMaterial->SetTextureParameterValue(TEXT("AtlasHD"), FrameTexture);
}

FVector ABRRFrontEndGameMode::StagePoint() const { return FVector(BackdropDistance * 0.45, 0, 0); }

void ABRRFrontEndGameMode::Push(TSharedRef<FBRRMenuScreen> Screen)
{
	Screen->Menu = this;
	if (!Screen->Load()) return;
	if (Screen->SpriteTextures.IsEmpty())
	{
		for (const FBRRImage& I : Screen->Scn.Sprites) Screen->SpriteTextures.Add(I.Width > 0 ? MakeTexture(I, true) : nullptr);
	}
	if (TSharedPtr<FBRRMenuScreen> Old = Top()) Old->Leave();
	Stack.Add(Screen);
	ShowPicture(Screen->Scn.Background);
	Screen->Enter();
}

void ABRRFrontEndGameMode::Pop()
{
	if (Stack.Num() <= 1) return;
	Stack.Last()->Leave();
	Stack.Pop();
	ShowPicture(Stack.Last()->Scn.Background);
	Stack.Last()->Enter();
}

void ABRRFrontEndGameMode::Replace(TSharedRef<FBRRMenuScreen> Screen)
{
	Screen->Menu = this;
	if (!Screen->Load()) return;
	if (Screen->SpriteTextures.IsEmpty())
	{
		for (const FBRRImage& I : Screen->Scn.Sprites) Screen->SpriteTextures.Add(I.Width > 0 ? MakeTexture(I, true) : nullptr);
	}
	if (!Stack.IsEmpty())
	{
		Stack.Last()->Leave();
		Stack.Pop();
	}
	Stack.Add(Screen);
	ShowPicture(Screen->Scn.Background);
	Screen->Enter();
}

void ABRRFrontEndGameMode::PopToRoot()
{
	while (Stack.Num() > 1)
	{
		Stack.Last()->Leave();
		Stack.Pop();
	}
	if (!Stack.IsEmpty())
	{
		ShowPicture(Stack.Last()->Scn.Background);
		Stack.Last()->Enter();
	}
}

void ABRRFrontEndGameMode::Click()
{
	BRRAudio::PlayOnce(DataDir(), TEXT("front"), TEXT("BEEP2"));
}

int32 ABRRFrontEndGameMode::BoxAt(const FBRRMenuScreen& S, FVector2D At) const
{
	for (const FBRRScnBox& B : S.Scn.Boxes)
	{
		if (B.W <= 2 || B.H <= 2) continue; // the hidden "back" corner
		if (At.X >= B.X && At.X < B.X + B.W && At.Y >= B.Y && At.Y < B.Y + B.H) return B.Id;
	}
	return INDEX_NONE;
}

void ABRRFrontEndGameMode::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	Clock += DeltaSeconds;
	if (bArtSmooth != BRRArt::SmoothWanted())
	{
		// The look changed (the graphics menu): the fonts and the screens' sprites in its art.
		bArtSmooth = BRRArt::SmoothWanted();
		LoadFonts();
		for (const TSharedRef<FBRRMenuScreen>& S : Stack)
		{
			S->SpriteTextures.Reset();
			for (const FBRRImage& I : S->Scn.Sprites) S->SpriteTextures.Add(I.Width > 0 ? MakeTexture(I, true) : nullptr);
		}
	}
	HandleInput();
	UpdateWalk();
	if (TSharedPtr<FBRRMenuScreen> S = Top()) S->Tick(DeltaSeconds);
}

void ABRRFrontEndGameMode::HandleInput()
{
	TSharedPtr<FBRRMenuScreen> S = Top();
	APlayerController* PC = GetWorld()->GetFirstPlayerController();
	if (!S || !PC) return;
	float MX = 0, MY = 0;
	if (PC->GetMousePosition(MX, MY) && Scale.X > 0)
	{
		const FVector2D Mouse(MX, MY);
		bMouseMoved = Mouse != LastMouse;
		LastMouse = Mouse;
		Pointer = (Mouse - Origin) / Scale;
		if (bMouseMoved)
		{
			const int32 Over = BoxAt(*S, Pointer);
			const int32 Index = S->FocusOrder.IndexOfByKey(Over);
			if (Index != INDEX_NONE) S->Focus = Index;
		}
		if (PC->WasInputKeyJustPressed(EKeys::LeftMouseButton))
		{
			const int32 Box = BoxAt(*S, Pointer);
			if (Box != INDEX_NONE)
			{
				Click();
				S->OnBox(Box);
				return;
			}
		}
	}
	if (PC->WasInputKeyJustPressed(EKeys::Escape) || PC->WasInputKeyJustPressed(EKeys::Gamepad_FaceButton_Right))
	{
		Click();
		S->OnBack();
		return;
	}
	if (!S->FocusOrder.IsEmpty())
	{
		if (PC->WasInputKeyJustPressed(EKeys::Down) || PC->WasInputKeyJustPressed(EKeys::Gamepad_DPad_Down)) S->Focus = (S->Focus + 1) % S->FocusOrder.Num();
		if (PC->WasInputKeyJustPressed(EKeys::Up) || PC->WasInputKeyJustPressed(EKeys::Gamepad_DPad_Up)) S->Focus = (S->Focus + S->FocusOrder.Num() - 1) % S->FocusOrder.Num();
		if (PC->WasInputKeyJustPressed(EKeys::Enter) || PC->WasInputKeyJustPressed(EKeys::SpaceBar) || PC->WasInputKeyJustPressed(EKeys::Gamepad_FaceButton_Bottom))
		{
			Click();
			S->OnBox(S->FocusOrder[FMath::Clamp(S->Focus, 0, S->FocusOrder.Num() - 1)]);
			return;
		}
	}
	if (PC->WasInputKeyJustPressed(EKeys::Left) || PC->WasInputKeyJustPressed(EKeys::Gamepad_DPad_Left)) S->OnLeftRight(-1);
	if (PC->WasInputKeyJustPressed(EKeys::Right) || PC->WasInputKeyJustPressed(EKeys::Gamepad_DPad_Right)) S->OnLeftRight(1);
	// The script's own shortcuts, in the player's language.
	for (const FBRRScnKey& K : S->Scn.Keys)
	{
		const FKey Key = KeyForScanCode(K.ScanCode);
		if (!Key.IsValid() || Key == EKeys::Escape || Key == EKeys::Enter || Key == EKeys::SpaceBar) continue;
		if (PC->WasInputKeyJustPressed(Key))
		{
			Click();
			S->OnBox(K.Id);
			return;
		}
	}
}

void ABRRFrontEndGameMode::Draw(UCanvas* Canvas)
{
	TSharedPtr<FBRRMenuScreen> S = Top();
	if (!Canvas || !S) return;
	// The 4:3 picture, centred, as the camera constrains it.
	const double VW = Canvas->ClipX, VH = Canvas->ClipY;
	const double W = FMath::Min(VW, VH * 4.0 / 3.0), H = W * 0.75;
	Origin = FVector2D((VW - W) * 0.5, (VH - H) * 0.5);
	Scale = FVector2D(W / 320.0, H / 200.0);
	FBRRMenuPainter P;
	P.Canvas = Canvas;
	P.Menu = this;
	P.Origin = Origin;
	P.Scale = Scale;

	const int32 Frame = int32(Clock / (S->Scn.SpritePeriod * TimerTick)) % FMath::Max(S->Scn.SpriteFrames, 1);
	const TArray<UTexture2D*>& Sheet = S->SpriteTextures;
	for (const FBRRScnSprite& A : S->Scn.AniSprites)
	{
		if (S->HiddenGroups.Contains(A.Group)) continue;
		const int32 G = A.First + Frame;
		if (Sheet.IsValidIndex(G) && Sheet[G]) P.Image(Sheet[G], A.X, A.Y);
		else if (Sheet.IsValidIndex(A.First) && Sheet[A.First]) P.Image(Sheet[A.First], A.X, A.Y);
	}

	const int32 Focused = S->FocusOrder.IsValidIndex(S->Focus) ? S->FocusOrder[S->Focus] : INDEX_NONE;
	const FBRRScnBox* FocusBox = Focused != INDEX_NONE ? S->Scn.FindBox(Focused) : nullptr;
	int32 Row = 0;
	for (const FBRRScnText& T : S->Scn.Texts)
	{
		if (S->HiddenGroups.Contains(T.Group) || S->HiddenKeys.Contains(T.Key)) continue;
		// The focused choice's lettering turns yellow (ramp 5), as ORR rebuilds it.
		const TArray<FBRRTextRun> Runs = Game()->Text.Layout(T.Key);
		const bool bFocused = FocusBox && !Runs.IsEmpty() && Runs[0].X >= FocusBox->X && Runs[0].X < FocusBox->X + FocusBox->W
			&& Runs[0].Y >= FocusBox->Y - 4 && Runs[0].Y < FocusBox->Y + FocusBox->H;
		P.Entry(T.Key, T.Font, S->Scn.Palette.Num() == 256 ? S->Scn.Palette : DefaultPalette, FVector2D::ZeroVector, bFocused ? 5 : -2, (Row++ & 1) * 2);
	}
	S->Draw(P);
}
