using UnrealBuildTool;

// UE-facing access to Big Red Racing's original data files and rules.
//
// Private/ORR holds OpenRedRacing's formats/sim/scene code vendored verbatim (Tools/sync_orr.py).
// It is plain standard C++ that throws std exceptions, so this module enables exceptions, skips the
// shared PCH and unity builds (so upstream files compile exactly as in their own tree), and catches
// at its public boundary: nothing ORR-typed or throwing escapes to other modules.
public class BRRFormats : ModuleRules
{
	public BRRFormats(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.NoPCHs;
		bUseUnity = false;
		bEnableExceptions = true;
		CppStandard = CppStandardVersion.Cpp20;
		PrivateIncludePaths.Add(System.IO.Path.Combine(ModuleDirectory, "Private", "ORR"));
		PublicDependencyModuleNames.Add("Core");
		CppCompileWarningSettings.ShadowVariableWarningLevel = WarningLevel.Off;
		CppCompileWarningSettings.UndefinedIdentifierWarningLevel = WarningLevel.Off;
	}
}
