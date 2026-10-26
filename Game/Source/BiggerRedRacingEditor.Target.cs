using UnrealBuildTool;

public class BiggerRedRacingEditorTarget : TargetRules
{
	public BiggerRedRacingEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.AddRange(new[] { "BRRFormats", "BRRGame" });
	}
}
