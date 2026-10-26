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
			"ImageCore",
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
