using UnrealBuildTool;

// 宿主只保留启动依赖；具体玩法由 Foundation 的唯一功能清单注册。
public class EvilClownParadise : ModuleRules
{
	public EvilClownParadise(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine", "ECPFoundation" });
	}
}
