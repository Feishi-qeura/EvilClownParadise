using UnrealBuildTool;

// 玩家和物品在同一模块内共享复制状态，避免拆分过程中改变已有网络协议。
public class ECPInteraction : ModuleRules
{
	public ECPInteraction(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// 公开头文件暴露角色基类、阵营常量及引擎反射类型，调用方需要这些依赖。
		PublicDependencyModuleNames.AddRange(new[]
		{
			"Core", "CoreUObject", "Engine", "ECPFoundation", "ECPCharacter"
		});

		// 输入、界面与复制实现仅在本模块内部使用，不向其他玩法模块传播依赖。
		PrivateDependencyModuleNames.AddRange(new[]
		{
			"InputCore", "EnhancedInput", "UMG", "Slate", "SlateCore", "NetCore"
		});
		// Real multi-client PIE automation is editor-only; runtime features retain their original dependency graph.
		if (Target.bBuildEditor) PrivateDependencyModuleNames.AddRange(new[] { "UnrealEd", "BlueprintGraph" });
	}
}
