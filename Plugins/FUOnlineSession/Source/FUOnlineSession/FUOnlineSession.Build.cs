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
            // 诊断报告读取已加载插件的 VersionName；只读元数据，不写入项目或引擎配置。
            "Projects",
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
