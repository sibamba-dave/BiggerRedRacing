using UnrealBuildTool;

public class BRRGame : ModuleRules
{
	public BRRGame(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine", "BRRFormats" });
		PrivateDependencyModuleNames.AddRange(new[] { "ChaosVehicles", "EnhancedInput", "InputCore", "PhysicsCore", "ProceduralMeshComponent", "AudioMixer", "Json", "MediaAssets" });
	}
}
