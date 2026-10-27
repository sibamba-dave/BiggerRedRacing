#include "BRRMenuScreens.h"

#include "BRRAudio.h"
#include "BRRFrontEnd.h"
#include "BRRGameInstance.h"
#include "BRRVideo.h"
#include "BRRWorld.h"
#include "Misc/Paths.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Engine/Engine.h"
#include "GameFramework/GameUserSettings.h"
#include "HAL/IConsoleManager.h"

// The original's screens, each on its own .SCN (the scripts are quoted where a box id matters).
// Their order is the game's: CONFSCN -> GLOBESCN -> TRAKSCN (-> CARSCN) -> the race -> RESULSCN.

namespace
{
	int32 GMusicLoop = -1; // what the front end last asked the music for: not restarted on every screen

	void Music(int32 Intro, int32 Loop)
	{
		if (Loop == GMusicLoop) return;
		GMusicLoop = Loop;
		BRRAudio::PlayMusic(Intro, Loop);
	}

	FString Clock(double Seconds)
	{
		const int32 Hundredths = FMath::RoundToInt(Seconds * 100.0);
		return FString::Printf(TEXT("%d:%02d.%02d"), Hundredths / 6000, (Hundredths / 100) % 60, Hundredths % 100);
	}

	const FBRRTournamentTrack* TrackOf(const UBRRGameInstance& G, const FString& Level)
	{
		return G.Tournament.Tracks.FindByPredicate([&](const FBRRTournamentTrack& T) { return T.Level.Equals(Level, ESearchCase::IgnoreCase); });
	}

	bool LoadScreen(FBRRMenuScreen& S, const TCHAR* Name, const FString& PicArchive = FString(), const FString& PicMember = FString())
	{
		FString Error;
		if (BRR::LoadScn(S.Menu->DataDir(), Name, TEXT("ENG"), S.Scn, Error, PicArchive, PicMember)) return true;
		UE_LOG(LogTemp, Error, TEXT("Front end: %s: %s"), Name, *Error);
		return false;
	}

	TSharedRef<FBRRMenuScreen> MakeGlobe(bool bChampionship, const FString& Select = FString());
	TSharedRef<FBRRMenuScreen> MakeTrack(const FString& Level);
	TSharedRef<FBRRMenuScreen> MakeOptions();

	// --- a video: the intro (VIDEO/INTROSND.RPL, when the game starts) and VICTORY.MVI (the
	// championship won). Its sound is its clock; any key or click skips it.
	class FVideoScreen : public FBRRMenuScreen
	{
	public:
		FString Path;
		TFunction<void(ABRRFrontEndGameMode&)> Then;
		FBRRVideo Video;
		double Elapsed = 0;

		virtual bool Load() override
		{
			FString Error;
			if (!Video.Open(Path, Error))
			{
				UE_LOG(LogTemp, Warning, TEXT("Front end: %s"), *Error);
				return false;
			}
			Scn.Boxes.Add({ 0, 0, 0, 320, 200 }); // anywhere: skip
			return true;
		}
		virtual void Enter() override
		{
			Elapsed = 0;
			GMusicLoop = -1;
			BRRAudio::StopMusic();
			BRRAudio::PlayTrack(Video.Sound(), Video.SoundRate());
			if (Video.Advance()) Menu->ShowFrame(Video.Picture());
		}
		virtual void Tick(float DeltaSeconds) override
		{
			Elapsed += DeltaSeconds;
			const int32 Want = int32(Elapsed * Video.FramesPerSecond());
			bool bShown = false;
			while (Video.FrameIndex() < Want)
			{
				if (!Video.Advance()) { Finish(); return; }
				bShown = true;
			}
			if (bShown) Menu->ShowFrame(Video.Picture());
		}
		void Finish()
		{
			BRRAudio::StopTrack();
			ABRRFrontEndGameMode& M = *Menu;
			TFunction<void(ABRRFrontEndGameMode&)> Next = MoveTemp(Then);
			M.Pop();
			if (Next) Next(M);
		}
		virtual void OnBox(int32) override { Finish(); }
		virtual void OnBack() override { Finish(); }
	};

	TSharedRef<FBRRMenuScreen> MakeVideo(const FString& Path, TFunction<void(ABRRFrontEndGameMode&)> Then = nullptr)
	{
		TSharedRef<FVideoScreen> S = MakeShared<FVideoScreen>();
		S->Path = Path;
		S->Then = MoveTemp(Then);
		return S;
	}

	// --- CONFSCN: the main menu ------------------------------------------------------------
	// mbox 0..4 at y 40/70/100/140/170: single race, championship, multi player, load recording,
	// options. Recordings were never built (ORR neither); that line is QUIT here.
	class FMainScreen : public FBRRMenuScreen
	{
	public:
		virtual bool Load() override
		{
			FocusOrder = { 0, 1, 3, 4 };
			HiddenKeys = { TEXT("CONFIG_LOAD"), TEXT("CONFIG_2P") };
			return LoadScreen(*this, TEXT("CONFSCN"));
		}
		virtual void Enter() override { Music(2, 2); }
		virtual void Draw(FBRRMenuPainter& P) override
		{
			const int32 Focused = FocusOrder.IsValidIndex(Focus) ? FocusOrder[Focus] : -1;
			P.Text(TEXT("QUIT"), 50, 140, 5, Focused == 3 ? 5 : -1, Scn.Palette, 2);
		}
		virtual void OnBox(int32 Id) override
		{
			UBRRGameInstance* G = Menu->Game();
			switch (Id)
			{
			case 0: Menu->Push(MakeGlobe(false)); break;
			case 1:
				G->StartChampionship();
				if (G->Champ.bActive) Menu->Push(MakeGlobe(true));
				break;
			case 3: OnBack(); break;
			case 4: Menu->Push(MakeOptions()); break;
			default: break;
			}
		}
		virtual void OnBack() override
		{
			UKismetSystemLibrary::QuitGame(Menu, nullptr, EQuitPreference::Quit, false);
		}
	};

	// --- GLOBESCN: choose the circuit ------------------------------------------------------
	// mbox 0 the passport (OK), 1 the corner (back), 2/3 the arrows. The original turns a 3D
	// globe; here the circuit's own picture stands where the globe does, its place name over it.
	class FGlobeScreen : public FBRRMenuScreen
	{
	public:
		bool bChampionship = false;
		FString Select;
		TArray<FString> Levels;
		int32 Index = 0;
		TMap<FString, UTexture2D*> Views;

		virtual bool Load() override
		{
			FocusOrder = { 0 };
			// 4..8 the championship's headings, 9 single race, 11..13 the multi-player picks.
			HiddenGroups = { 4, 5, 6, 7, 8, 11, 12, 13 };
			if (bChampionship) HiddenGroups.Add(9);
			return LoadScreen(*this, TEXT("GLOBESCN"));
		}
		virtual void Enter() override
		{
			UBRRGameInstance* G = Menu->Game();
			Music(2, 2);
			Levels = bChampionship ? G->RoundLevels() : G->RaceableLevels();
			if (bChampionship)
			{
				HiddenGroups.Remove(4);
				HiddenGroups.Remove(G->IsFinalRound() ? 8 : 6);
				HiddenKeys = { G->IsFinalRound() ? TEXT("GLB_HD_RND") : TEXT("GLB_HD_FIN"), G->IsFinalRound() ? TEXT("GLB_HD_TOQUAL") : TEXT("GLB_HD_TOCHAMP") };
			}
			const int32 Want = Levels.IndexOfByKey(Select.IsEmpty() ? G->Level : Select);
			Index = Want != INDEX_NONE ? Want : 0;
		}
		FString Level() const { return Levels.IsValidIndex(Index) ? Levels[Index] : FString(); }
		UTexture2D* View(const FString& L)
		{
			if (UTexture2D** T = Views.Find(L)) return *T;
			FBRRImage Picture;
			FString Error;
			UTexture2D* T = BRR::LoadScreen(Menu->DataDir(), L + TEXT(".LEV"), TEXT("VIEW.RAW"), Picture, Error) ? Menu->MakeTexture(Picture) : nullptr;
			Views.Add(L, T);
			return T;
		}
		virtual void Draw(FBRRMenuPainter& P) override
		{
			UBRRGameInstance* G = Menu->Game();
			if (bChampionship)
			{
				// "ROUND n:" and "Win n races to qualify": the game writes the numbers in.
				P.Text(FString::FromInt(G->Champ.Round + 1), 150, 3, 5, -1, Scn.Palette, 1);
				P.Text(FString::FromInt(G->RoundNeed() - G->Champ.Wins), 85, 30, 1, -1, Scn.Palette);
			}
			const FString L = Level();
			if (L.IsEmpty()) return;
			if (UTexture2D* T = View(L)) P.Image(T, 20, 50, 160, 100);
			if (const FBRRTournamentTrack* Track = TrackOf(*G, L))
			{
				const FString Place = G->Text.Text(Track->GlobeText, L);
				P.Text(Place, 100 - FBRRMenuPainter::TextWidth(Menu, Place, 5) / 2, 152, 5, 5, Scn.Palette);
				double Y = 182;
				for (const FString& Line : G->Text.Lines(Track->ViewText))
				{
					P.Text(Line, 8, Y, 0, -1, Scn.Palette);
					Y += 9;
				}
			}
		}
		virtual void OnLeftRight(int32 Dir) override
		{
			if (Levels.IsEmpty()) return;
			Index = (Index + Dir + Levels.Num()) % Levels.Num();
			Menu->Click();
		}
		virtual void OnBox(int32 Id) override
		{
			if (Id == 0 && !Level().IsEmpty()) Menu->Push(MakeTrack(Level()));
			else if (Id == 1) OnBack();
			else if (Id == 2) OnLeftRight(-1);
			else if (Id == 3) OnLeftRight(1);
		}
		virtual void OnBack() override
		{
			if (bChampionship) Menu->Game()->Champ.bActive = false;
			Menu->PopToRoot();
		}
	};

	// --- CARSCN: choose the vehicle -------------------------------------------------------
	// mbox 0 next, 1 OK, 2 the corner. The picture is the theme's CARSEL.RAW; the vehicle turns
	// in front of it, as the original's does.
	class FCarScreen : public FBRRMenuScreen
	{
	public:
		FString Level;
		TArray<FString> Cars;
		int32 Index = 0;
		FBRRWorld World;    // the circuit's vehicles: where each one's wheels go
		double Spin = 0;

		virtual bool Load() override
		{
			FocusOrder = { 1, 0 };
			HiddenGroups = { 11, 12 };
			const FString Dir = Menu->DataDir();
			Cars = BRR::LevelCars(Dir, Level);
			FString Error;
			FBRRWorld::Load(Dir, Level, Cars, 1, World, Error);
			return LoadScreen(*this, TEXT("CARSCN"), BRR::LevelEnvironment(Level) + TEXT(".ENV"), TEXT("CARSEL.RAW"));
		}
		virtual void Enter() override
		{
			const int32 Have = Cars.IndexOfByKey(Menu->Game()->Vehicle);
			Index = Have != INDEX_NONE ? Have : 0;
			ShowCar();
		}
		virtual void Leave() override
		{
			if (Menu->Stage) Menu->Stage->Destroy();
			Menu->Stage = nullptr;
		}
		void ShowCar()
		{
			Leave();
			if (!Cars.IsValidIndex(Index)) return;
			const FString Car = Cars[Index];
			const FBRRVehicleMesh* Data = World.Vehicles.FindByPredicate([&](const FBRRVehicleMesh& V) { return V.Name == Car; });
			const FString Dir = FString::Printf(TEXT("/Game/BRR/%s/Meshes/"), *Level);
			UStaticMesh* Body = LoadObject<UStaticMesh>(nullptr, *(Dir + FString::Printf(TEXT("SM_%s_Body"), *Car)), nullptr, LOAD_NoWarn | LOAD_Quiet);
			if (!Body || !Data) return;
			AActor* Stage = Menu->GetWorld()->SpawnActor<AActor>();
			USceneComponent* Root = NewObject<USceneComponent>(Stage, TEXT("Root"));
			Stage->SetRootComponent(Root);
			Root->RegisterComponent();
			auto Add = [&](UStaticMesh* Mesh, const FVector& At, const TCHAR* Name)
			{
				UStaticMeshComponent* C = NewObject<UStaticMeshComponent>(Stage, Name);
				C->SetStaticMesh(Mesh);
				C->SetupAttachment(Root);
				C->SetRelativeLocation(At);
				C->SetCastShadow(false);
				C->RegisterComponent();
			};
			// Centred on the body, the wheels at their sockets.
			const FVector Centre = FVector(Data->Body.GetBounds().GetCenter());
			Add(Body, -Centre, TEXT("Body"));
			for (int32 W = 0; W < Data->Wheels.Num(); ++W)
			{
				if (UStaticMesh* Wheel = LoadObject<UStaticMesh>(nullptr, *(Dir + FString::Printf(TEXT("SM_%s_Wheel%d"), *Car, W)), nullptr, LOAD_NoWarn | LOAD_Quiet))
				{
					Add(Wheel, Data->Wheels[W].Socket - Centre, *FString::Printf(TEXT("Wheel%d"), W));
				}
			}
			const double Size = FMath::Max(double(Data->Body.GetBounds().GetSize().Size()), 1.0);
			Stage->SetActorScale3D(FVector(140.0 / Size));
			Stage->SetActorLocation(Menu->StagePoint() + FVector(0, 25, -40));
			Menu->Stage = Stage;
		}
		virtual void Tick(float DeltaSeconds) override
		{
			Spin = FMath::Fmod(Spin + DeltaSeconds * 40.0, 360.0);
			if (Menu->Stage) Menu->Stage->SetActorRotation(FRotator(-12, Spin, 0));
		}
		virtual void Draw(FBRRMenuPainter& P) override
		{
			if (!Cars.IsValidIndex(Index)) return;
			UBRRGameInstance* G = Menu->Game();
			P.Entry(TEXT("INF_") + Cars[Index], 0, Scn.Palette);
			P.Entry(TEXT("VHC_") + Cars[Index], 0, Scn.Palette, FVector2D(0, 0));
		}
		virtual void OnLeftRight(int32 Dir) override
		{
			if (Cars.IsEmpty()) return;
			Index = (Index + Dir + Cars.Num()) % Cars.Num();
			Menu->Click();
			ShowCar();
		}
		virtual void OnBox(int32 Id) override
		{
			if (Id == 0) OnLeftRight(1);
			else if (Id == 1)
			{
				if (Cars.IsValidIndex(Index)) Menu->Game()->Vehicle = Cars[Index];
				Menu->Pop();
			}
			else OnBack();
		}
		virtual void OnBack() override { Menu->Pop(); }
	};

	// --- TRAKSCN: the circuit -------------------------------------------------------------
	// Its picture is the circuit's VIEW.RAW. mbox 1 the vehicle, 2 a preview lap, 3 race; 0 back.
	// The circuit's own music starts here: its intro once, then its loop (TOURN.TXT).
	class FTrackScreen : public FBRRMenuScreen
	{
	public:
		FString Level;
		virtual bool Load() override
		{
			FocusOrder = { 3, 1, 2 };
			HiddenGroups = { 3 };
			return LoadScreen(*this, TEXT("TRAKSCN"), Level + TEXT(".LEV"), TEXT("VIEW.RAW"));
		}
		virtual void Enter() override
		{
			UBRRGameInstance* G = Menu->Game();
			G->Level = Level;
			const TArray<FString> Cars = BRR::LevelCars(Menu->DataDir(), Level);
			if (!Cars.Contains(G->Vehicle) && !Cars.IsEmpty()) G->Vehicle = Cars[0];
			int32 Intro = 0, Loop = 0;
			BRRAudio::CircuitMusic(Menu->DataDir(), Level, Intro, Loop);
			Music(Intro, Loop);
		}
		virtual void Draw(FBRRMenuPainter& P) override
		{
			// The vehicle chosen, under its plate.
			P.Text(Menu->Game()->Vehicle, 4, 136, 0, -1, Scn.Palette);
		}
		virtual void OnBox(int32 Id) override
		{
			UBRRGameInstance* G = Menu->Game();
			if (Id == 1)
			{
				TSharedRef<FCarScreen> Car = MakeShared<FCarScreen>();
				Car->Level = Level;
				Menu->Push(Car);
			}
			else if (Id == 2) G->StartRace(Level, G->Vehicle, true);
			else if (Id == 3) G->StartRace(Level, G->Vehicle, false);
			else OnBack();
		}
		virtual void OnBack() override
		{
			GMusicLoop = -1;
			Menu->Pop();
		}
	};

	// --- RESULSCN: after the race -----------------------------------------------------------
	// mbox 1 OK. Groups: 31 "Results", 3 "Races won:", 5/6/7 "Qualify:"/"Enter next round:"/
	// "be Champion:", 8/9/10 GAME OVER / THE CHAMPION / QUALIFIED; 2 the series score, 30 SAVE
	// recording and 20 the network wait are not this game's.
	class FResultsScreen : public FBRRMenuScreen
	{
	public:
		FBRRChampionship::EOutcome Outcome = FBRRChampionship::EOutcome::Racing;
		bool bChampionship = false;

		virtual bool Load() override
		{
			FocusOrder = { 1 };
			HiddenGroups = { 2, 3, 5, 6, 7, 8, 9, 10, 20, 30 };
			if (bChampionship)
			{
				using EOutcome = FBRRChampionship::EOutcome;
				HiddenGroups.Remove(3);
				if (Outcome == EOutcome::Eliminated) { HiddenGroups.Remove(8); HiddenGroups.Add(31); }
				else if (Outcome == EOutcome::Champion) { HiddenGroups.Remove(9); HiddenGroups.Add(31); }
				else if (Outcome == EOutcome::NextRound) { HiddenGroups.Remove(10); HiddenGroups.Add(31); }
				else HiddenGroups.Remove(Menu->Game()->IsFinalRound() ? 7 : 5);
			}
			return LoadScreen(*this, TEXT("RESULSCN"));
		}
		virtual void Draw(FBRRMenuPainter& P) override
		{
			UBRRGameInstance* G = Menu->Game();
			const FBRRRaceResult& R = G->LastResult;
			if (const FBRRTournamentTrack* Track = TrackOf(*G, R.Level))
			{
				P.Text(G->Text.Text(Track->GlobeText, R.Level), 10, 30, 0, 5, Scn.Palette);
			}
			double Y = 44;
			TArray<FBRRResultRow> Rows = R.Rows;
			Rows.Sort([](const FBRRResultRow& A, const FBRRResultRow& B) { return A.Place < B.Place; });
			for (const FBRRResultRow& Row : Rows)
			{
				const int32 Ramp = Row.bPlayer ? 5 : -1;
				P.Text(FString::Printf(TEXT("%d"), Row.Place), 10, Y, 0, Ramp, Scn.Palette);
				P.Text(Row.bPlayer ? FString(TEXT("YOU")) : Row.Name, 30, Y, 0, Ramp, Scn.Palette);
				P.Text(Row.bFinished ? Clock(Row.Seconds) : FString(TEXT("--")), 110, Y, 0, Ramp, Scn.Palette);
				Y += 10;
			}
			double LY = 44;
			for (int32 L = 0; L < R.LapTimes.Num(); ++L)
			{
				P.Text(FString::Printf(TEXT("LAP %d %s"), L + 1, *Clock(R.LapTimes[L])), 190, LY, 0, -1, Scn.Palette);
				LY += 10;
			}
			if (R.BestLap > 0)
			{
				P.Text(FString::Printf(TEXT("BEST LAP %s%s"), *Clock(R.BestLap), R.bNewBestLap ? TEXT(" NEW") : TEXT("")), 190, LY + 6, 0, R.bNewBestLap ? 5 : -1, Scn.Palette);
				LY += 10;
			}
			if (R.BestRace > 0)
			{
				P.Text(FString::Printf(TEXT("BEST RACE %s%s"), *Clock(R.BestRace), R.bNewBestRace ? TEXT(" NEW") : TEXT("")), 190, LY + 6, 0, R.bNewBestRace ? 5 : -1, Scn.Palette);
			}
			if (bChampionship)
			{
				P.Text(FString::Printf(TEXT("%d"), G->Champ.Wins), 180, 90, 1, 5, Scn.Palette);
				P.Text(FString::Printf(TEXT("%d"), FMath::Max(G->RoundNeed() - G->Champ.Wins, 0)), 200, 130, 1, 5, Scn.Palette);
			}
		}
		virtual void OnBox(int32 Id) override
		{
			UBRRGameInstance* G = Menu->Game();
			using EOutcome = FBRRChampionship::EOutcome;
			if (!bChampionship) Menu->Replace(MakeTrack(G->LastResult.Level));
			else if (Outcome == EOutcome::Racing || Outcome == EOutcome::NextRound) Menu->Replace(MakeGlobe(true));
			else if (Outcome == EOutcome::Champion)
			{
				// The championship won: the victory video, then the title.
				Menu->PopToRoot();
				Menu->Push(MakeVideo(Menu->DataDir() / TEXT("VICTORY.MVI")));
			}
			else Menu->PopToRoot();
		}
		virtual void OnBack() override { OnBox(1); }
	};

	// --- SOUNDSCN: the volumes and the commentary --------------------------------------------
	// mbox 0/1 the CD (music) volume -/+, 2/3 the effects', 10..17 and 20..27 their eight steps,
	// 28 the commentary, 4 OK (and the corner).
	class FSoundScreen : public FBRRMenuScreen
	{
	public:
		virtual bool Load() override
		{
			FocusOrder = { 4, 28 };
			if (!LoadScreen(*this, TEXT("SOUNDSCN"))) return false;
			// The script places no sprites: the game draws BUTTONS.SPR's on the boxes (frames 0
			// plus, 5 minus, 10 OK).
			Scn.AniSprites.Add({ 1, 5, 103, 50 });
			Scn.AniSprites.Add({ 1, 0, 236, 50 });
			Scn.AniSprites.Add({ 1, 5, 103, 93 });
			Scn.AniSprites.Add({ 1, 0, 236, 93 });
			Scn.AniSprites.Add({ 1, 10, 265, 155 });
			return true;
		}
		static int32 Step(float V) { return FMath::Clamp(FMath::RoundToInt(V * 8.f), 0, 8); }
		virtual void Draw(FBRRMenuPainter& P) override
		{
			UBRRGameInstance* G = Menu->Game();
			const int32 Music = Step(G->MusicVolume), Effects = Step(G->SoundVolume);
			for (const FBRRScnBox& B : Scn.Boxes)
			{
				const bool bMusicStep = B.Id >= 10 && B.Id <= 17 && B.Id - 10 < Music;
				const bool bEffectStep = B.Id >= 20 && B.Id <= 27 && B.Id - 20 < Effects;
				if (bMusicStep || bEffectStep) P.Box(B.X + 1, B.Y + B.H - 6, B.W - 2, 4, FLinearColor(1.f, 0.85f, 0.1f, 0.85f));
			}
			P.Text(G->bCommentary ? TEXT("ON") : TEXT("OFF"), 200, 140, 5, 5, Scn.Palette);
		}
		virtual void OnBox(int32 Id) override
		{
			UBRRGameInstance* G = Menu->Game();
			auto Nudge = [](float& V, int32 By) { V = FMath::Clamp((FMath::RoundToInt(V * 8.f) + By) / 8.f, 0.f, 1.f); };
			if (Id == 0) Nudge(G->MusicVolume, -1);
			else if (Id == 1) Nudge(G->MusicVolume, 1);
			else if (Id == 2) Nudge(G->SoundVolume, -1);
			else if (Id == 3) Nudge(G->SoundVolume, 1);
			else if (Id >= 10 && Id <= 17) G->MusicVolume = (Id - 9) / 8.f;
			else if (Id >= 20 && Id <= 27) G->SoundVolume = (Id - 19) / 8.f;
			else if (Id == 28) G->bCommentary = !G->bCommentary;
			else if (Id == 4) { OnBack(); return; }
			G->ApplySettings();
		}
		virtual void OnLeftRight(int32 Dir) override { OnBox(Dir < 0 ? 0 : 1); }
		virtual void OnBack() override
		{
			Menu->Game()->SaveSettings();
			Menu->Pop();
		}
	};

	// --- the graphics: a list of its own on LANDSCN's picture -----------------------------------
	// The original's display screen had two sliders; the remaster has more to set. Each row is a
	// box: up/down walk them, left/right (or Enter, or a click) change the focused one. The window
	// and quality rows are UE's GameUserSettings; the rest the game instance's [BRR] settings.
	class FGraphicsScreen : public FBRRMenuScreen
	{
	public:
		enum ERow { Look, Window, Resolution, VSync, FrameLimit, Quality, Upscaling, FrameGen, MotionBlur, Hdr, ViewDistance, Count };
		static constexpr int32 OkId = 100;
		static constexpr double Top = 34, Pitch = 12, LabelX = 28, ValueX = 172;
		TArray<FIntPoint> Resolutions;

		virtual bool Load() override
		{
			if (!LoadScreen(*this, TEXT("LANDSCN"))) return false;
			Scn.Texts.Reset();
			Scn.AniSprites.Reset();
			Scn.Keys.Reset();
			Scn.Boxes.Reset();
			for (int32 Row = 0; Row < Count; ++Row)
			{
				Scn.Boxes.Add({ Row, 20, int32(Top + Row * Pitch) - 2, 280, int32(Pitch) });
				FocusOrder.Add(Row);
			}
			Scn.Boxes.Add({ OkId, 140, int32(Top + int32(Count) * Pitch) + 4, 40, 12 });
			FocusOrder.Add(OkId);
			UKismetSystemLibrary::GetSupportedFullscreenResolutions(Resolutions);
			return true;
		}
		static UGameUserSettings* Settings() { return GEngine ? GEngine->GetGameUserSettings() : nullptr; }
		static bool HasFrameGeneration() { return IConsoleManager::Get().FindConsoleVariable(TEXT("r.Streamline.DLSSG.Enable")) != nullptr; }
		static const TArray<float>& FrameLimits() { static const TArray<float> L = { 0.f, 30.f, 60.f, 120.f, 144.f, 165.f, 240.f }; return L; }

		FString Label(int32 Row) const
		{
			static const TCHAR* Labels[Count] = { TEXT("LOOK"), TEXT("DISPLAY"), TEXT("RESOLUTION"), TEXT("VSYNC"), TEXT("FRAME LIMIT"),
				TEXT("QUALITY"), TEXT("UPSCALING"), TEXT("FRAME GENERATION"), TEXT("MOTION BLUR"), TEXT("HDR"), TEXT("VIEW DISTANCE") };
			return Labels[Row];
		}
		FString Value(int32 Row) const
		{
			UBRRGameInstance* G = Menu->Game();
			UGameUserSettings* S = Settings();
			auto OnOff = [](bool b) { return FString(b ? TEXT("ON") : TEXT("OFF")); };
			switch (Row)
			{
			case Look: return G->Look ? TEXT("REMASTERED") : TEXT("CLASSIC");
			case Window:
			{
				const EWindowMode::Type M = S ? S->GetFullscreenMode() : EWindowMode::WindowedFullscreen;
				return M == EWindowMode::Fullscreen ? TEXT("FULLSCREEN") : M == EWindowMode::Windowed ? TEXT("WINDOWED") : TEXT("BORDERLESS");
			}
			case Resolution:
			{
				if (!S) return TEXT("-");
				if (S->GetFullscreenMode() == EWindowMode::WindowedFullscreen) return TEXT("DESKTOP");
				const FIntPoint R = S->GetScreenResolution();
				return FString::Printf(TEXT("%d X %d"), R.X, R.Y);
			}
			case VSync: return OnOff(S && S->IsVSyncEnabled());
			case FrameLimit:
			{
				const float L = S ? S->GetFrameRateLimit() : 0.f;
				return L <= 0.f ? FString(TEXT("NONE")) : FString::Printf(TEXT("%d FPS"), FMath::RoundToInt(L));
			}
			case Quality:
			{
				static const TCHAR* Levels[] = { TEXT("LOW"), TEXT("MEDIUM"), TEXT("HIGH"), TEXT("EPIC"), TEXT("CINEMATIC") };
				const int32 L = S ? S->GetOverallScalabilityLevel() : -1;
				return L >= 0 && L <= 4 ? Levels[L] : TEXT("CUSTOM");
			}
			case Upscaling: return G->bNativeResolution ? TEXT("NATIVE") : TEXT("QUALITY");
			case FrameGen: return HasFrameGeneration() ? OnOff(G->bFrameGeneration) : TEXT("N/A");
			case MotionBlur: return OnOff(G->bMotionBlur);
			case Hdr: return S && S->SupportsHDRDisplayOutput() ? OnOff(S->IsHDREnabled()) : TEXT("N/A");
			default: return FString();
			}
		}
		virtual void Draw(FBRRMenuPainter& P) override
		{
			UBRRGameInstance* G = Menu->Game();
			const int32 Focused = FocusOrder.IsValidIndex(Focus) ? FocusOrder[Focus] : -1;
			P.Box(16, 4, 288, 192, FLinearColor(0.f, 0.f, 0.f, 0.6f));
			P.Text(TEXT("GRAPHICS"), 160 - FBRRMenuPainter::TextWidth(Menu, TEXT("GRAPHICS"), 3) * 0.5, 8, 3, -1, Scn.Palette, 1);
			for (int32 Row = 0; Row < Count; ++Row)
			{
				const double Y = Top + Row * Pitch;
				const int32 Ramp = Row == Focused ? FBRRMenuPainter::FocusRamp : -1;
				P.Text(Label(Row), LabelX, Y, 0, Ramp, Scn.Palette);
				if (Row == ViewDistance)
				{
					const double Fill = (G->ViewCells - BRR::ViewCellsMin) / double(BRR::ViewCellsMax - BRR::ViewCellsMin);
					P.Box(ValueX, Y + 1, 100, 6, FLinearColor(1.f, 1.f, 1.f, 0.25f));
					P.Box(ValueX, Y + 1, 100 * Fill, 6, FLinearColor(1.f, 0.85f, 0.1f, 0.9f));
				}
				else
				{
					const FString V = Value(Row);
					P.Text(Row == Focused ? FString::Printf(TEXT("< %s >"), *V) : V, Row == Focused ? ValueX - 16 : ValueX, Y, 0, Ramp, Scn.Palette);
				}
			}
			P.Text(TEXT("OK"), 152, Top + int32(Count) * Pitch + 6, 0, Focused == OkId ? FBRRMenuPainter::FocusRamp : -1, Scn.Palette);
			// What the focused row is for, where it does nothing in the look now chosen.
			const TCHAR* Note = nullptr;
			if (Focused == ViewDistance && G->Look) Note = TEXT("THE CLASSIC LOOK'S VIEW DISTANCE");
			else if ((Focused == Upscaling || Focused == FrameGen || Focused == MotionBlur) && !G->Look) Note = TEXT("THE REMASTERED LOOK ONLY");
			else if (Focused == Quality && Settings() && Settings()->GetOverallScalabilityLevel() >= 0 && Settings()->GetOverallScalabilityLevel() < 2) Note = TEXT("LUMEN NEEDS HIGH OR ABOVE");
			else if (Focused == Resolution && Settings() && Settings()->GetFullscreenMode() == EWindowMode::WindowedFullscreen) Note = TEXT("BORDERLESS USES THE DESKTOP'S");
			if (Note) P.Text(Note, 160 - FBRRMenuPainter::TextWidth(Menu, Note, 0) * 0.5, 186, 0, -1, Scn.Palette);
		}
		void Change(int32 Row, int32 Dir)
		{
			UBRRGameInstance* G = Menu->Game();
			UGameUserSettings* S = Settings();
			auto Step = [Dir](int32 Index, int32 Num) { return Num > 0 ? (Index + Dir + Num) % Num : 0; };
			switch (Row)
			{
			case Look: G->Look = G->Look ? 0 : 1; break;
			case Window:
				if (S)
				{
					// Borderless, fullscreen, windowed.
					static const EWindowMode::Type Modes[] = { EWindowMode::WindowedFullscreen, EWindowMode::Fullscreen, EWindowMode::Windowed };
					int32 Index = 0;
					for (int32 i = 0; i < 3; ++i) if (Modes[i] == S->GetFullscreenMode()) Index = i;
					const EWindowMode::Type Mode = Modes[Step(Index, 3)];
					S->SetFullscreenMode(Mode);
					if (Mode == EWindowMode::WindowedFullscreen) S->SetScreenResolution(S->GetDesktopResolution());
					else if (Mode == EWindowMode::Windowed && S->GetScreenResolution() == S->GetDesktopResolution() && !Resolutions.IsEmpty())
					{
						// A window the size of the desktop is borderless under another name: a size smaller.
						const FIntPoint D = S->GetDesktopResolution();
						FIntPoint Best = Resolutions[0];
						for (const FIntPoint& R : Resolutions) if (R.X < D.X && R.Y < D.Y && R.X * R.Y > Best.X * Best.Y) Best = R;
						S->SetScreenResolution(Best);
					}
				}
				break;
			case Resolution:
				if (S && S->GetFullscreenMode() != EWindowMode::WindowedFullscreen && !Resolutions.IsEmpty())
				{
					int32 Index = Resolutions.IndexOfByKey(S->GetScreenResolution());
					if (Index == INDEX_NONE) Index = Resolutions.Num() - 1;
					S->SetScreenResolution(Resolutions[Step(Index, Resolutions.Num())]);
				}
				break;
			case VSync: if (S) S->SetVSyncEnabled(!S->IsVSyncEnabled()); break;
			case FrameLimit:
				if (S)
				{
					const TArray<float>& L = FrameLimits();
					int32 Index = L.IndexOfByPredicate([S](float F) { return FMath::IsNearlyEqual(F, S->GetFrameRateLimit()); });
					S->SetFrameRateLimit(L[Step(FMath::Max(Index, 0), L.Num())]);
				}
				break;
			case Quality:
				if (S) S->SetOverallScalabilityLevel(Step(FMath::Clamp(S->GetOverallScalabilityLevel(), 0, 4), 5));
				break;
			case Upscaling: G->bNativeResolution = !G->bNativeResolution; break;
			case FrameGen: if (HasFrameGeneration()) G->bFrameGeneration = !G->bFrameGeneration; break;
			case MotionBlur: G->bMotionBlur = !G->bMotionBlur; break;
			case Hdr: if (S && S->SupportsHDRDisplayOutput()) S->EnableHDRDisplayOutput(!S->IsHDREnabled()); break;
			case ViewDistance: G->ViewCells = FMath::Clamp(G->ViewCells + Dir * 2, BRR::ViewCellsMin, BRR::ViewCellsMax); break;
			default: return;
			}
			if (S && (Row == Window || Row == Resolution || Row == VSync || Row == FrameLimit || Row == Quality)) S->ApplySettings(false);
			G->ApplySettings();
		}
		virtual void OnBox(int32 Id) override
		{
			if (Id == OkId) { OnBack(); return; }
			Change(Id, 1);
		}
		virtual void OnLeftRight(int32 Dir) override
		{
			const int32 Focused = FocusOrder.IsValidIndex(Focus) ? FocusOrder[Focus] : -1;
			if (Focused >= 0 && Focused < Count) Change(Focused, Dir);
		}
		virtual void OnBack() override
		{
			Menu->Game()->SaveSettings();
			Menu->Pop();
		}
	};

	// --- OPTSCN: the options -------------------------------------------------------------------
	// mbox 0 joysticks, 1 keys, 2 display, 3 sound, 4 the game (damage); 10 OK. The joystick and
	// key screens are UE's input, and damage is not modelled: those three lead nowhere.
	class FOptionsScreen : public FBRRMenuScreen
	{
	public:
		virtual bool Load() override
		{
			FocusOrder = { 2, 3, 10 };
			HiddenKeys = { TEXT("OPT_STICKS"), TEXT("OPT_KEYS"), TEXT("OPT_GAME") };
			return LoadScreen(*this, TEXT("OPTSCN"));
		}
		virtual void OnBox(int32 Id) override
		{
			if (Id == 2) Menu->Push(MakeShared<FGraphicsScreen>());
			else if (Id == 3) Menu->Push(MakeShared<FSoundScreen>());
			else if (Id == 10) OnBack();
		}
		virtual void OnBack() override { Menu->Pop(); }
	};
	
	TSharedRef<FBRRMenuScreen> MakeOptions() { return MakeShared<FOptionsScreen>(); }

	TSharedRef<FBRRMenuScreen> MakeGlobe(bool bChampionship, const FString& Select)
	{
		TSharedRef<FGlobeScreen> S = MakeShared<FGlobeScreen>();
		S->bChampionship = bChampionship;
		S->Select = Select;
		return S;
	}

	TSharedRef<FBRRMenuScreen> MakeTrack(const FString& Level)
	{
		TSharedRef<FTrackScreen> S = MakeShared<FTrackScreen>();
		S->Level = Level;
		return S;
	}
}

void BRRMenu::ShowVideo(ABRRFrontEndGameMode& Menu, bool bVictory)
{
	FString Path = bVictory ? Menu.DataDir() / TEXT("VICTORY.MVI") : Menu.DataDir() / TEXT("../VIDEO/INTROSND.RPL");
	FPaths::CollapseRelativeDirectories(Path);
	Menu.Push(MakeVideo(Path));
}

void BRRMenu::ShowSampleResults(ABRRFrontEndGameMode& Menu)
{
	UBRRGameInstance* G = Menu.Game();
	FBRRRaceResult& R = G->LastResult;
	R = FBRRRaceResult();
	R.bValid = true;
	R.Level = TEXT("DIRT1");
	R.Place = 2;
	R.LapTimes = { 41.27, 38.90, 39.42 };
	R.Seconds = 119.59;
	const TCHAR* Names[6] = { TEXT("ATV"), TEXT("JEEP"), TEXT("JEEP"), TEXT("ATV"), TEXT("JEEP"), TEXT("ATV") };
	for (int32 I = 0; I < 6; ++I) R.Rows.Add({ Names[I], I + 1, 115.0 + I * 3.1, I < 4, I == 1 });
	R.BestLap = 38.90;
	R.bNewBestLap = true;
	R.BestRace = 117.25;
	Menu.Push(MakeShared<FResultsScreen>());
}

void BRRMenu::Open(ABRRFrontEndGameMode& Menu)
{
	GMusicLoop = -1;
	Menu.Push(MakeShared<FMainScreen>());
	UBRRGameInstance* G = Menu.Game();
	if (!G) return;
	// The intro, once, as the original's VIDTHUMP played it before the game (-nointro skips it).
	if (!G->bIntroShown && !G->bRaceFromMenu)
	{
		G->bIntroShown = true;
		if (!FParse::Param(FCommandLine::Get(), TEXT("nointro")) && !FParse::Param(FCommandLine::Get(), TEXT("BRRMenuWalk")))
		{
			FString Intro = Menu.DataDir() / TEXT("../VIDEO/INTROSND.RPL");
			FPaths::CollapseRelativeDirectories(Intro);
			Menu.Push(MakeVideo(Intro));
		}
	}
	if (!G->bRaceFromMenu) return;
	G->bRaceFromMenu = false;
	if (G->LastResult.bValid && !G->bPreview)
	{
		TSharedRef<FResultsScreen> Results = MakeShared<FResultsScreen>();
		Results->bChampionship = G->Champ.bActive;
		if (G->Champ.bActive) Results->Outcome = G->RecordChampionshipRace(G->LastResult.Level, G->LastResult.Place);
		Menu.Push(Results);
	}
	else if (G->Champ.bActive)
	{
		Menu.Push(MakeGlobe(true, G->Level));
	}
	else if (!G->Level.IsEmpty())
	{
		Menu.Push(MakeGlobe(false, G->Level));
		Menu.Push(MakeTrack(G->Level));
	}
}
