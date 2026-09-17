// Fill out your copyright notice in the Description page of Project Settings.

using UnrealBuildTool;
using System.Collections.Generic;

public class EvilClownParadiseTarget : TargetRules
{
	public EvilClownParadiseTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.V7;

		// 显式钉住 include 顺序，不要靠默认值：UBT 的默认是 Oldest（5.8 下=5.6 顺序，
		// 且 5.6 顺序在 5.9 会被移除）。钉住之后升级引擎时 include 顺序的变化是可控的，
		// 而不是某天突然冒出一堆编译错误。
		IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_8;

		ExtraModuleNames.AddRange( new string[] { "EvilClownParadise" } );
	}
}
