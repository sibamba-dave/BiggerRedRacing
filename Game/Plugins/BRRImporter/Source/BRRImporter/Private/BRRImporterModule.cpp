#include "BRRImporter.h"

#include "FileHelpers.h"
#include "Misc/MessageDialog.h"
#include "Misc/ScopedSlowTask.h"
#include "Modules/ModuleManager.h"
#include "ToolMenus.h"

#define LOCTEXT_NAMESPACE "BRRImporter"

namespace
{
	void ImportLevels(const TArray<FString>& Levels, bool bOpenMap)
	{
		FScopedSlowTask Task(Levels.Num(), LOCTEXT("Importing", "Importing Big Red Racing levels..."));
		Task.MakeDialog();
		FString LastMap;
		TArray<FString> Errors;
		for (const FString& Level : Levels)
		{
			Task.EnterProgressFrame(1, FText::FromString(Level));
			FBRRImportOptions Options;
			Options.Level = Level;
			FString Error;
			if (!BRRImport::ImportLevel(Options, LastMap, Error)) Errors.Add(Level + TEXT(": ") + Error);
		}
		if (!Errors.IsEmpty())
		{
			FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(FString::Join(Errors, TEXT("\n"))));
		}
		else if (bOpenMap && !LastMap.IsEmpty())
		{
			UEditorLoadingAndSavingUtils::LoadMap(LastMap);
		}
	}

	void FillLevelMenu(UToolMenu* Menu)
	{
		FToolMenuSection& Section = Menu->AddSection("BRRLevels", LOCTEXT("Levels", "Import level"));
		const TArray<FString> Levels = BRRImport::FindLevels();
		if (Levels.IsEmpty())
		{
			Section.AddMenuEntry("BRRNoData", LOCTEXT("NoData", "Game data not found (set BRR_DATA_DIR)"), FText(), FSlateIcon(), FUIAction());
			return;
		}
		Section.AddMenuEntry("BRRImportAll", LOCTEXT("ImportAll", "Import all levels"),
			LOCTEXT("ImportAllTip", "Bake every level and its vehicles into /Game/BRR"), FSlateIcon(),
			FUIAction(FExecuteAction::CreateLambda([Levels] { ImportLevels(Levels, false); })));
		for (const FString& Level : Levels)
		{
			Section.AddMenuEntry(FName(*Level), FText::FromString(Level),
				FText::Format(LOCTEXT("ImportOneTip", "Bake {0} and its vehicles into /Game/BRR/{0} and open it"), FText::FromString(Level)),
				FSlateIcon(), FUIAction(FExecuteAction::CreateLambda([Level] { ImportLevels({ Level }, true); })));
		}
	}
}

class FBRRImporterModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateLambda([this]
		{
			FToolMenuOwnerScoped Owner(this);
			UToolMenu* Tools = UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu.Tools");
			FToolMenuSection& Section = Tools->FindOrAddSection("BRR", LOCTEXT("BRRSection", "Bigger Red Racing"));
			Section.AddSubMenu("BRRImport", LOCTEXT("BRRMenu", "Big Red Racing"),
				LOCTEXT("BRRMenuTip", "Import the original game's levels from your own copy"),
				FNewToolMenuDelegate::CreateStatic(&FillLevelMenu));
		}));
	}

	virtual void ShutdownModule() override
	{
		UToolMenus::UnRegisterStartupCallback(this);
		UToolMenus::UnregisterOwner(this);
	}
};

IMPLEMENT_MODULE(FBRRImporterModule, BRRImporter);

#undef LOCTEXT_NAMESPACE
