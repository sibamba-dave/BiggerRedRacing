#include "BRRImportCommandlet.h"

#include "BRRImporter.h"
#include "Misc/Parse.h"

DEFINE_LOG_CATEGORY_STATIC(LogBRRImportCommandlet, Log, All);

UBRRImportCommandlet::UBRRImportCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 UBRRImportCommandlet::Main(const FString& Params)
{
	TArray<FString> Levels;
	FString Level;
	if (FParse::Param(*Params, TEXT("All")))
	{
		Levels = BRRImport::FindLevels();
	}
	else if (FParse::Value(*Params, TEXT("Level="), Level))
	{
		Levels.Add(Level);
	}
	if (Levels.IsEmpty())
	{
		UE_LOG(LogBRRImportCommandlet, Error, TEXT("Usage: -run=BRRImport -Level=<NAME> [-Vehicles=A,B] [-Colour=1] | -All"));
		return 1;
	}

	FBRRImportOptions Options;
	FString VehicleList;
	if (FParse::Value(*Params, TEXT("Vehicles="), VehicleList, false))
	{
		VehicleList.ParseIntoArray(Options.Vehicles, TEXT(","));
	}
	FParse::Value(*Params, TEXT("Colour="), Options.Colour);

	int32 Failures = 0;
	for (const FString& Name : Levels)
	{
		Options.Level = Name;
		FString MapPackage, Error;
		if (!BRRImport::ImportLevel(Options, MapPackage, Error))
		{
			UE_LOG(LogBRRImportCommandlet, Error, TEXT("%s: %s"), *Name, *Error);
			++Failures;
		}
	}
	UE_LOG(LogBRRImportCommandlet, Display, TEXT("Imported %d/%d levels"), Levels.Num() - Failures, Levels.Num());
	return Failures == 0 ? 0 : 1;
}
