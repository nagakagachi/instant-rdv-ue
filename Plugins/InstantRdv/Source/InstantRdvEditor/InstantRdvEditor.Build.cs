using UnrealBuildTool;

public class InstantRdvEditor : ModuleRules
{
    public InstantRdvEditor(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PrivateDependencyModuleNames.AddRange(
            new[]
            {
                "Core",
                "CoreUObject",
                "Engine",
                "Slate",
                "SlateCore",
                "InstantRdv",
                "WorkspaceMenuStructure",
                "LevelEditor",
                "UnrealEd",
            });
    }
}
