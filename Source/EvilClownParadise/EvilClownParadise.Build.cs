// Fill out your copyright notice in the Description page of Project Settings.

using UnrealBuildTool;

public class EvilClownParadise : ModuleRules
{
	public EvilClownParadise(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
	
		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "InputCore", "AIModule", "NavigationSystem", "StateTreeModule", "GameplayTasks", "EnhancedInput" });

		PrivateDependencyModuleNames.AddRange(new string[] { "GameplayStateTreeModule" });

		// Steam 在线服务：模块加载由 .uproject 里的 OnlineSubsystemSteam 插件声明负责，
		// 这里不需要也不应写入 DynamicallyLoadedModuleNames（那是给非链接加载的程序目标用的）。
		// 若日后 C++ 直接引用 OnlineSubsystemSteam 的类型，再把它加入 PrivateDependencyModuleNames。
	}
}
