using UnrealBuildTool;

public class InstantRdv : ModuleRules
{
    public InstantRdv(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicIncludePaths.AddRange(
            new[]
            {
                "Runtime/Renderer/Internal"
            });

        PublicDependencyModuleNames.AddRange(
            new[]
            {
                "Core",
                "CoreUObject",
                "Engine"
            });

        PrivateDependencyModuleNames.AddRange(
            new[]
            {
                "Projects",
                "RHI",
                "RHICore",
                "RenderCore",
                "Renderer"
            });
    }
}
