using UnrealBuildTool;

public class BRRGame : ModuleRules
{
	public BRRGame(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine", "BRRFormats" });
	}
}
