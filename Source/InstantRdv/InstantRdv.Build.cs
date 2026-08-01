using UnrealBuildTool;
using System.IO;

public class InstantRdv : ModuleRules
{
    public InstantRdv(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicIncludePaths.AddRange(
            new[]
            {
                Path.Combine(EngineDirectory, "Source/Runtime/Renderer/Internal")
            });

        PrivateIncludePaths.AddRange(
            new[]
            {
                Path.Combine(EngineDirectory, "Source/Runtime/Renderer/Private")
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
                "Renderer",
            });
    }
}
