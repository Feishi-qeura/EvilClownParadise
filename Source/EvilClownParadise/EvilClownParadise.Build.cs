// Fill out your copyright notice in the Description page of Project Settings.

using UnrealBuildTool;

public class EvilClownParadise : ModuleRules
{
	public EvilClownParadise(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
	
		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "InputCore" });

		PrivateDependencyModuleNames.AddRange(new string[] {  });
		
		//显式加载Steam在线服务模块
		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			DynamicallyLoadedModuleNames.Add("OnlineSubsystemSteam");
		}

		// Uncomment if you are using Slate UI
		// PrivateDependencyModuleNames.AddRange(new string[] { "Slate", "SlateCore" });
		
		// Uncomment if you are using online features
		// PrivateDependencyModuleNames.Add("OnlineSubsystem");

		// To include OnlineSubsystemSteam, add it to the plugins section in your uproject file with the Enabled attribute set to true
	}
}
