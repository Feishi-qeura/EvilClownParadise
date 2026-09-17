// Fill out your copyright notice in the Description page of Project Settings.

using UnrealBuildTool;

public class EvilClownParadise : ModuleRules
{
	public EvilClownParadise(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// Public = 本模块的头文件里直接出现的类型所属模块。
		// 放在这里会传染给所有依赖本模块的目标，所以只放真的进了头文件的那几个：
		//   AIModule         —— ECPPlayerController.h 的 FGenericTeamId、ECPAIController.h 的 AAIController
		//   StateTreeModule  —— ECPTask* / ECPEvaluatorTarget 头文件里的 FStateTreeTaskCommonBase
		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"AIModule",
			"StateTreeModule",
		});

		// Private = 只在 .cpp 里用到的模块。不要把只给实现用的依赖放进 Public：
		// 依赖列表应该和"谁能看见谁"一致，否则模块边界形同虚设。
		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"EnhancedInput",           // 仅 Framework/ECPPlayerController.cpp
			"GameplayStateTreeModule", // 仅 AI/ECPAIController.cpp（UStateTreeAIComponent）
			"InputCore",               // 输入键位类型
			"NavigationSystem",        // 仅 AI/ECPTaskPatrol.cpp
		});

		// Steam 在线服务：模块加载由 .uproject 里的 OnlineSubsystemSteam 插件声明负责，
		// 这里不需要也不应写入 DynamicallyLoadedModuleNames（那是给非链接加载的程序目标用的）。
		// 若日后 C++ 直接引用 OnlineSubsystemSteam 的类型，再把它加入 PrivateDependencyModuleNames。
	}
}
