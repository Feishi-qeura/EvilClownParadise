// Fill out your copyright notice in the Description page of Project Settings.

using UnrealBuildTool;
using System.Collections.Generic;

public class EvilClownParadiseEditorTarget : TargetRules
{
	public EvilClownParadiseEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.V7;

		// 与 Game 目标保持一致：两边 include 顺序不同会造成"编辑器能编、打包编不过"
		IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_8;

		ExtraModuleNames.AddRange( new string[] { "EvilClownParadise" } );
	}
}
