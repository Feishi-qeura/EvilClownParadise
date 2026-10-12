using UnrealBuildTool;

public class ECPOnline : ModuleRules
{
    public ECPOnline(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        // 蓝图公开返回现有 FU 子系统类型，保留其公开依赖以供项目蓝图和 C++ 正常解析。
        PublicDependencyModuleNames.AddRange(new[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "FUOnlineSession"
        });

        // Feature 只消费基础模块并查询激活状态，不改变后端的加载阶段或网络配置。
        PrivateDependencyModuleNames.AddRange(new[]
        {
            "ECPFoundation",
            "GameFeatures"
        });
    }
}
