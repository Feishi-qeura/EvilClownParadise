// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Engine/Engine.h"

/**
 * 开发期屏幕调试输出。
 *
 * 只在非 Shipping 构建中产生代码：Shipping 下两个函数都退化为空内联函数，
 * 调用点无需自行包 #if，忘记删除也不会进包。需要彻底移除时直接找调用点删行。
 *
 * 注意：编辑器命令行（-run=... / Cook）下 GEngine 可能不存在，
 * 所有输出必须先判空，否则打包流程会崩在调试代码上。
 */
namespace DebugHelper
{
/** 打印一行独立消息，Duration 秒后自动消失。 */
inline void Print(const FString& Message, float Duration = 2.0f, const FColor& Color = FColor::Black)
{
#if !UE_BUILD_SHIPPING
	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(-1, Duration, Color, Message);
	}
#endif
}

/** 固定在 Key 对应行刷新，用于常驻显示（同一 Key 覆盖同一行）。 */
inline void PrintPersistent(int32 Key, const FString& Message, float Duration = 5.0f,
                            const FColor& Color = FColor::Yellow)
{
#if !UE_BUILD_SHIPPING
	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(Key, Duration, Color, Message);
	}
#endif
}
}
