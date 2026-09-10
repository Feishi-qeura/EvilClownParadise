using UnrealBuildTool;

public class FUOnlineSession : ModuleRules
{
    public FUOnlineSession(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "OnlineSubsystem",
            "OnlineSubsystemUtils",
            "DeveloperSettings"
        });
        
      
        DynamicallyLoadedModuleNames.AddRange(new[]
        {
            "OnlineSubsystemSteam",
            "OnlineSubsystemNull"
        });
    }
}
