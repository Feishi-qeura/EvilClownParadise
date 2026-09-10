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

        PrivateDependencyModuleNames.AddRange(new[]
        {
            // SteamSockets 必须在 Runtime 初始化阶段可解析，不能依赖后续动态加载才准备传输层。
            "SteamSockets",
            "Sockets",
            "Slate",
            "SlateCore"
        });

        DynamicallyLoadedModuleNames.AddRange(new[]
        {
            "OnlineSubsystemSteam",
            "OnlineSubsystemNull"
        });
    }
}
