using UnrealBuildTool;

// 功能实现只引用声明的公共基础层，避免宿主与 Feature 形成反向模块依赖。
public class ECPAI : ModuleRules
{
    public ECPAI(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine", "ECPCharacter", "AIModule", "StateTreeModule" });
        PrivateDependencyModuleNames.AddRange(new[] { "ECPFoundation", "GameplayStateTreeModule", "NavigationSystem", "GameplayTasks" });
    }
}
