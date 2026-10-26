using UnrealBuildTool;

public class BRRImporter : ModuleRules
{
	public BRRImporter(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine" });
		PrivateDependencyModuleNames.AddRange(new[]
		{
			"AssetRegistry",
			"BRRFormats",
			"MaterialEditor",
			"MeshDescription",
			"Slate",
			"SlateCore",
			"StaticMeshDescription",
			"ToolMenus",
			"UnrealEd",
		});
	}
}
