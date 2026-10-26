#pragma once

#include "Commandlets/Commandlet.h"

#include "BRRImportCommandlet.generated.h"

// Headless import:
//   UnrealEditor-Cmd.exe BiggerRedRacing.uproject -run=BRRImport -Level=DIRT1 [-Vehicles=JEEP,ATV] [-Colour=1]
//   UnrealEditor-Cmd.exe BiggerRedRacing.uproject -run=BRRImport -All
UCLASS()
class UBRRImportCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UBRRImportCommandlet();
	virtual int32 Main(const FString& Params) override;
};
