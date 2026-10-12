using UnrealBuildTool;

// 公共层只依赖引擎，不引用具体功能实现，避免双向依赖和启动顺序不确定。
public class ECPFoundation : ModuleRules
{
    public ECPFoundation(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine", "DeveloperSettings", "GameFeatures" });
        PrivateDependencyModuleNames.Add("Projects");
    }
}
