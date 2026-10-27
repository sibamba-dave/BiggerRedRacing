#include "BRRGameInstance.h"

#include "BRRArchive.h"
#include "BRRAudio.h"
#include "HAL/IConsoleManager.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/PackageName.h"

namespace
{
	const TCHAR* ConfigSection = TEXT("BRR");
	const TCHAR* FrontEndMap = TEXT("/Engine/Maps/Entry");
}

void UBRRGameInstance::Init()
{
	Super::Init();
	DataDir = BRR::GetDataDir();
	if (!DataDir.IsEmpty())
	{
		FString Error;
		BRR::LoadTournament(DataDir, Tournament, Error);
		Text.Load(DataDir);
	}
	GConfig->GetFloat(ConfigSection, TEXT("SoundVolume"), SoundVolume, GGameUserSettingsIni);
	GConfig->GetFloat(ConfigSection, TEXT("MusicVolume"), MusicVolume, GGameUserSettingsIni);
	GConfig->GetBool(ConfigSection, TEXT("Commentary"), bCommentary, GGameUserSettingsIni);
	GConfig->GetFloat(ConfigSection, TEXT("MasterVolume"), MasterVolume, GGameUserSettingsIni);
	GConfig->GetFloat(ConfigSection, TEXT("EngineVolume"), EngineVolume, GGameUserSettingsIni);
	GConfig->GetFloat(ConfigSection, TEXT("SpeechVolume"), SpeechVolume, GGameUserSettingsIni);
	GConfig->GetBool(ConfigSection, TEXT("MusicDuck"), bMusicDuck, GGameUserSettingsIni);
	GConfig->GetBool(ConfigSection, TEXT("TunnelEcho"), bTunnelEcho, GGameUserSettingsIni);
	GConfig->GetInt(ConfigSection, TEXT("Look"), Look, GGameUserSettingsIni);
	GConfig->GetInt(ConfigSection, TEXT("ViewCells"), ViewCells, GGameUserSettingsIni);
	GConfig->GetBool(ConfigSection, TEXT("NativeResolution"), bNativeResolution, GGameUserSettingsIni);
	GConfig->GetBool(ConfigSection, TEXT("FrameGeneration"), bFrameGeneration, GGameUserSettingsIni);
	GConfig->GetBool(ConfigSection, TEXT("MotionBlur"), bMotionBlur, GGameUserSettingsIni);
	GConfig->GetInt(ConfigSection, TEXT("Weather"), Weather, GGameUserSettingsIni);
	GConfig->GetBool(ConfigSection, TEXT("Crowds"), bCrowds, GGameUserSettingsIni);
	GConfig->GetInt(ConfigSection, TEXT("GroundDetail"), GroundDetail, GGameUserSettingsIni);
	GConfig->GetBool(ConfigSection, TEXT("Effects"), bEffects, GGameUserSettingsIni);
	GConfig->GetBool(ConfigSection, TEXT("SmoothArt"), bSmoothArt, GGameUserSettingsIni);
	ApplySettings();
}

void UBRRGameInstance::ApplySettings() const
{
	auto Set = [](const TCHAR* Name, const FString& Value)
	{
		if (IConsoleVariable* V = IConsoleManager::Get().FindConsoleVariable(Name)) V->Set(*Value, ECVF_SetByGameSetting);
	};
	// The master volume rides both buses (the race re-sends them every frame from these).
	Set(TEXT("brr.SoundVolume"), FString::SanitizeFloat(SoundVolume * MasterVolume));
	Set(TEXT("brr.MusicVolume"), FString::SanitizeFloat(MusicVolume * MasterVolume));
	Set(TEXT("brr.TunnelEcho"), bTunnelEcho ? TEXT("1") : TEXT("0"));
	BRRAudio::SetMix(EngineVolume, SpeechVolume, bMusicDuck);
	Set(TEXT("brr.Commentary"), bCommentary ? TEXT("1") : TEXT("0"));
	Set(TEXT("brr.Look"), FString::FromInt(Look));
	Set(TEXT("brr.ViewCells"), FString::FromInt(ViewCells));
	Set(TEXT("brr.DLAA"), bNativeResolution ? TEXT("1") : TEXT("0"));
	Set(TEXT("brr.FrameGeneration"), bFrameGeneration ? TEXT("1") : TEXT("0"));
	Set(TEXT("r.MotionBlurQuality"), bMotionBlur ? TEXT("4") : TEXT("0"));
	Set(TEXT("brr.Weather"), FString::FromInt(Weather));
	Set(TEXT("brr.Crowds"), bCrowds ? TEXT("1") : TEXT("0"));
	Set(TEXT("brr.GroundDetail"), FString::FromInt(GroundDetail));
	Set(TEXT("brr.Effects"), bEffects ? TEXT("1") : TEXT("0"));
	Set(TEXT("brr.SmoothArt"), bSmoothArt ? TEXT("1") : TEXT("0"));
	BRRAudio::SetVolumes(SoundVolume * MasterVolume, MusicVolume * MasterVolume);
}

void UBRRGameInstance::RecordTimes(FBRRRaceResult& R, const FString& InVehicle) const
{
	const TCHAR* Records = TEXT("BRR.Records");
	const FString Key = FString::Printf(TEXT("%s.%s"), *R.Level.ToUpper(), *InVehicle.ToUpper());
	FString Lap, Race;
	GConfig->GetString(Records, *(Key + TEXT(".Lap")), Lap, GGameUserSettingsIni);
	GConfig->GetString(Records, *(Key + TEXT(".Race")), Race, GGameUserSettingsIni);
	R.BestLap = Lap.IsEmpty() ? 0.0 : FCString::Atod(*Lap);
	R.BestRace = Race.IsEmpty() ? 0.0 : FCString::Atod(*Race);
	for (double T : R.LapTimes)
	{
		if (T > 0 && (R.BestLap <= 0 || T < R.BestLap))
		{
			R.BestLap = T;
			R.bNewBestLap = true;
		}
	}
	if (R.Seconds > 0 && (R.BestRace <= 0 || R.Seconds < R.BestRace))
	{
		R.BestRace = R.Seconds;
		R.bNewBestRace = true;
	}
	if (R.bNewBestLap) GConfig->SetString(Records, *(Key + TEXT(".Lap")), *FString::SanitizeFloat(R.BestLap), GGameUserSettingsIni);
	if (R.bNewBestRace) GConfig->SetString(Records, *(Key + TEXT(".Race")), *FString::SanitizeFloat(R.BestRace), GGameUserSettingsIni);
	if (R.bNewBestLap || R.bNewBestRace) GConfig->Flush(false, GGameUserSettingsIni);
}

void UBRRGameInstance::SaveSettings() const
{
	GConfig->SetFloat(ConfigSection, TEXT("SoundVolume"), SoundVolume, GGameUserSettingsIni);
	GConfig->SetFloat(ConfigSection, TEXT("MusicVolume"), MusicVolume, GGameUserSettingsIni);
	GConfig->SetBool(ConfigSection, TEXT("Commentary"), bCommentary, GGameUserSettingsIni);
	GConfig->SetFloat(ConfigSection, TEXT("MasterVolume"), MasterVolume, GGameUserSettingsIni);
	GConfig->SetFloat(ConfigSection, TEXT("EngineVolume"), EngineVolume, GGameUserSettingsIni);
	GConfig->SetFloat(ConfigSection, TEXT("SpeechVolume"), SpeechVolume, GGameUserSettingsIni);
	GConfig->SetBool(ConfigSection, TEXT("MusicDuck"), bMusicDuck, GGameUserSettingsIni);
	GConfig->SetBool(ConfigSection, TEXT("TunnelEcho"), bTunnelEcho, GGameUserSettingsIni);
	GConfig->SetInt(ConfigSection, TEXT("Look"), Look, GGameUserSettingsIni);
	GConfig->SetInt(ConfigSection, TEXT("ViewCells"), ViewCells, GGameUserSettingsIni);
	GConfig->SetBool(ConfigSection, TEXT("NativeResolution"), bNativeResolution, GGameUserSettingsIni);
	GConfig->SetBool(ConfigSection, TEXT("FrameGeneration"), bFrameGeneration, GGameUserSettingsIni);
	GConfig->SetBool(ConfigSection, TEXT("MotionBlur"), bMotionBlur, GGameUserSettingsIni);
	GConfig->SetInt(ConfigSection, TEXT("Weather"), Weather, GGameUserSettingsIni);
	GConfig->SetBool(ConfigSection, TEXT("Crowds"), bCrowds, GGameUserSettingsIni);
	GConfig->SetInt(ConfigSection, TEXT("GroundDetail"), GroundDetail, GGameUserSettingsIni);
	GConfig->SetBool(ConfigSection, TEXT("Effects"), bEffects, GGameUserSettingsIni);
	GConfig->SetBool(ConfigSection, TEXT("SmoothArt"), bSmoothArt, GGameUserSettingsIni);
	GConfig->Flush(false, GGameUserSettingsIni);
	ApplySettings();
}

bool UBRRGameInstance::IsImported(const FString& Level)
{
	return FPackageName::DoesPackageExist(FString::Printf(TEXT("/Game/BRR/%s/L_%s"), *Level, *Level));
}

TArray<FString> UBRRGameInstance::RaceableLevels() const
{
	TArray<FString> Out;
	for (const FString& L : Tournament.SingleRaceOrder)
	{
		if (IsImported(L)) Out.Add(L);
	}
	return Out;
}

void UBRRGameInstance::StartRace(const FString& InLevel, const FString& InVehicle, bool bInPreview)
{
	Level = InLevel;
	Vehicle = InVehicle;
	bPreview = bInPreview;
	bRaceFromMenu = true;
	LastResult = FBRRRaceResult();
	FString Options = InVehicle.IsEmpty() ? FString() : FString::Printf(TEXT("Vehicle=%s"), *InVehicle);
	if (bInPreview) Options += Options.IsEmpty() ? TEXT("Preview") : TEXT("?Preview");
	UGameplayStatics::OpenLevel(this, FName(*FString::Printf(TEXT("/Game/BRR/%s/L_%s"), *InLevel, *InLevel)), true, Options);
}

void UBRRGameInstance::ReturnToFrontEnd()
{
	UGameplayStatics::OpenLevel(this, FName(FrontEndMap), true);
}

void UBRRGameInstance::StartChampionship()
{
	Champ = FBRRChampionship();
	Champ.bActive = true;
	// Rounds whose circuits are all missing are skipped, as ORR drops circuits it cannot load.
	while (CurrentRound() && RoundLevels().IsEmpty()) ++Champ.Round;
	Champ.bActive = CurrentRound() != nullptr;
}

const FBRRTournamentRound* UBRRGameInstance::CurrentRound() const
{
	return Tournament.Rounds.IsValidIndex(Champ.Round) ? &Tournament.Rounds[Champ.Round] : nullptr;
}

bool UBRRGameInstance::IsFinalRound() const
{
	return Champ.Round == Tournament.Rounds.Num() - 1;
}

int32 UBRRGameInstance::RoundNeed() const
{
	const FBRRTournamentRound* R = CurrentRound();
	if (!R) return 0;
	int32 Available = 0;
	for (const FString& L : R->Levels) Available += IsImported(L);
	return FMath::Clamp(R->Need, 1, FMath::Max(Available, 1));
}

TArray<FString> UBRRGameInstance::RoundLevels() const
{
	TArray<FString> Out;
	if (const FBRRTournamentRound* R = CurrentRound())
	{
		for (const FString& L : R->Levels)
		{
			if (IsImported(L) && !Champ.Raced.Contains(L)) Out.Add(L);
		}
	}
	return Out;
}

FBRRChampionship::EOutcome UBRRGameInstance::RecordChampionshipRace(const FString& RacedLevel, int32 Place)
{
	using EOutcome = FBRRChampionship::EOutcome;
	Champ.Raced.AddUnique(RacedLevel);
	if (Place == 1) ++Champ.Wins;
	const int32 Need = RoundNeed();
	EOutcome Out = EOutcome::Racing;
	if (Champ.Wins >= Need && IsFinalRound()) Out = EOutcome::Champion;
	else if (Champ.Wins >= Need) Out = EOutcome::NextRound;
	else if (RoundLevels().Num() + Champ.Wins < Need) Out = EOutcome::Eliminated;
	if (Out == EOutcome::NextRound)
	{
		++Champ.Round;
		Champ.Wins = 0;
		Champ.Raced.Reset();
		while (CurrentRound() && RoundLevels().IsEmpty() && !IsFinalRound()) ++Champ.Round;
	}
	if (Out == EOutcome::Champion || Out == EOutcome::Eliminated) Champ.bActive = false;
	Champ.Last = Out;
	return Out;
}
