using UnrealBuildTool;

public class BiggerRedRacingTarget : TargetRules
{
	public BiggerRedRacingTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.AddRange(new[] { "BRRFormats", "BRRGame" });
	}
}
