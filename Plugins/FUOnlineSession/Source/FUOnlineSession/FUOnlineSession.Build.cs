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
            "OnlineSubsystemNull",

            // 【FU 修复：Steam 传输依赖】
            // Lobby 的创建/搜索属于 OnlineSubsystemSteam；steam.<SteamId> 的实际连接由 SteamSockets 解析。
            "SteamSockets"
        });
    }
}
