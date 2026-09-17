// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "ECPGameMode.generated.h"

/**
 * 游戏模式基类。
 *
 * 目前是空实现，但已被 /Game/_Game/Data/GM_InGame 继承，因此保留：
 * 它的存在意义是给"默认 Pawn / 默认 PlayerController的替换点"提供一个 C++ 落点，
 * 关卡与蓝图里的 GM_InGame 通过它接住后续的规则（回合、计分、复活）。
 * 具体规则请加在这里而不是蓝图里，蓝图只做资源指定。
 */
UCLASS()
class EVILCLOWNPARADISE_API AECPGameMode : public AGameModeBase
{
	GENERATED_BODY()
};
