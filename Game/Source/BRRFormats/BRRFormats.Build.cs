using UnrealBuildTool;

// Pure parsers for Big Red Racing's original data files. No engine/rendering dependencies so the
// same code serves the editor bake path and the runtime import-from-user's-copy path.
public class BRRFormats : ModuleRules
{
	public BRRFormats(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.AddRange(new[] { "Core" });
	}
}
