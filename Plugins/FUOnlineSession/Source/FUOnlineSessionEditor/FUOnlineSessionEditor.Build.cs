using UnrealBuildTool;

public class FUOnlineSessionEditor : ModuleRules
{
    public FUOnlineSessionEditor(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PrivateDependencyModuleNames.AddRange(new[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "Projects",

            // 监听 UDeveloperSettings 的变更，并在编辑器中显示非阻塞通知。
            "DeveloperSettings",
            "Slate",
            "SlateCore",

            // Editor 模块可以依赖 Runtime 模块，
            // 但 Runtime 模块绝对不能反向依赖 Editor 模块。
            "FUOnlineSession"
        });
    }
}
