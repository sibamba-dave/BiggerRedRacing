#pragma once

#include "CoreMinimal.h"
#include "BRRAssets.h"
#include "Engine/GameInstance.h"
#include "BRRGameInstance.generated.h"

// One line of a race's results, as RESULSCN lists them.
struct FBRRResultRow
{
	FString Name;          // the vehicle, as the original names a ghost
	int32 Place = 0;
	double Seconds = 0;    // total, when finished
	bool bFinished = false;
	bool bPlayer = false;
};

struct FBRRRaceResult
{
	bool bValid = false;
	FString Level;
	int32 Place = 0;
	double Seconds = 0;
	TArray<double> LapTimes;
	TArray<FBRRResultRow> Rows;
	// The circuit's records for this vehicle after the race (0: none yet), and whether it set them.
	double BestLap = 0, BestRace = 0;
	bool bNewBestLap = false, bNewBestRace = false;
};

// The championship as the original runs it (ORR game/championship.cpp): TOURN.TXT's `tourn 0..6`
// rounds; a round offers its circuits not yet raced, and `need` of them must be WON (first place,
// nothing else scores) to go on. Kept for the session only, as in the original.
struct FBRRChampionship
{
	enum class EOutcome { Racing, NextRound, Eliminated, Champion };
	bool bActive = false;
	int32 Round = 0;
	int32 Wins = 0;
	TArray<FString> Raced;      // this round's
	EOutcome Last = EOutcome::Racing;
};

// What carries across the front end and the races: the race asked for, its result, the
// championship, and the player's settings (saved in GameUserSettings.ini, [BRR]).
UCLASS()
class BRRGAME_API UBRRGameInstance : public UGameInstance
{
	GENERATED_BODY()

public:
	virtual void Init() override;

	FString DataDir;
	FBRRTournament Tournament;
	FBRRText Text;

	// The circuits a player can race: TOURN.TXT's single-race order, those imported.
	TArray<FString> RaceableLevels() const;
	static bool IsImported(const FString& Level);

	// Goes to a race; it returns to the front end when it is over (or Escape).
	void StartRace(const FString& Level, const FString& Vehicle, bool bPreview);
	void ReturnToFrontEnd();
	FString Level, Vehicle;
	bool bPreview = false;
	bool bRaceFromMenu = false;  // a race started here goes back here; one started directly does not
	bool bIntroShown = false;
	FBRRRaceResult LastResult;   // set by the race, read by RESULSCN

	// --- the championship
	FBRRChampionship Champ;
	void StartChampionship();
	const FBRRTournamentRound* CurrentRound() const;
	bool IsFinalRound() const;
	int32 RoundNeed() const;
	TArray<FString> RoundLevels() const; // this round's circuits not raced yet, imported
	// Scores the race just run and moves the championship on.
	FBRRChampionship::EOutcome RecordChampionshipRace(const FString& RacedLevel, int32 Place);

	// --- records: the best lap and the best race per circuit and vehicle, kept in
	// GameUserSettings.ini [BRR.Records]. Fills the result's Best* fields.
	void RecordTimes(FBRRRaceResult& Result, const FString& Vehicle) const;

	// --- settings
	float SoundVolume = 1.f, MusicVolume = 0.6f;
	bool bCommentary = true;
	int32 Look = 1;          // brr.Look
	int32 ViewCells = 16;    // brr.ViewCells (the classic look's view distance)
	bool bNativeResolution = false; // brr.DLAA: the remastered look at full resolution, not upscaled
	bool bFrameGeneration = true;   // brr.FrameGeneration
	bool bMotionBlur = true;        // r.MotionBlurQuality
	int32 Weather = 1;              // brr.Weather: 0 clear, 1 drawn for each race, 2 foul
	bool bCrowds = true;            // brr.Crowds
	int32 GroundDetail = 3;         // brr.GroundDetail: 0 none .. 3 all
	bool bEffects = true;           // brr.Effects: the wakes and the dust haze
	bool bSmoothArt = true;         // brr.SmoothArt
	void ApplySettings() const;
	void SaveSettings() const;
};
