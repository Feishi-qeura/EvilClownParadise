// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Logging/LogMacros.h"

/**
 * 项目级基础声明。放在 Public/ 下才能被模块内任意文件包含
 * （模块根目录不在 UBT 的 include 路径里，只有 Public/ 与 Private/ 在）。
 */

/**
 * 项目统一日志类别。
 * 业务代码禁止裸用 LogTemp：分类日志可以单独过滤、单独提级，
 * 打包后也能用 -LogCmds 开关，LogTemp 做不到这些。
 */
DECLARE_LOG_CATEGORY_EXTERN(LogECP, Log, All);

/**
 * AI 感知阵营。
 *
 * 必须实现在 Pawn（AECPCharBase）上，不能只实现在 Controller 上：
 * FGenericTeamId::GetTeamIdentifier 只检查传入 Actor 自身是否实现
 * IGenericTeamAgentInterface，不会回退去问它的 Controller。
 * 而 UGameplayStatics 之外的感知判定走 FAISenseAffiliationFilter::ShouldSenseTeam，
 * 它调用 GetTeamAttitudeTowards(TargetActor) 并要求双方都能解析出阵营，
 * 否则一律算 Neutral —— 阵营没实现在 Pawn 上时，勾了"只检测敌人"的怪物
 * 会完全看不见玩家（不会报任何错，只是永远不触发感知回调）。
 */
namespace ECPTeam
{
	inline constexpr uint8 Player = 0;
	inline constexpr uint8 Monster = 1;
}
