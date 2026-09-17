// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "StateTreeTaskBase.h"
#include "ECPTaskAttack.generated.h"

/**
 * 攻击状态。
 * 攻击间隔在 AECPMonsterBase::PerformAttack 内部节流，本任务只负责
 * "进状态站定、每次 Tick 请求一次攻击、退状态解除站定"。
 */
USTRUCT()
struct FECPTaskAttackInstanceData
{
	GENERATED_BODY()
};

USTRUCT()
struct FECPTaskAttack : public FStateTreeTaskCommonBase
{
	GENERATED_BODY()

	using FInstanceDataType = FECPTaskAttackInstanceData;
	virtual const UStruct* GetInstanceDataType() const override { return FInstanceDataType::StaticStruct(); }

	virtual EStateTreeRunStatus EnterState(FStateTreeExecutionContext& Context,
	                                       const FStateTreeTransitionResult& Transition) const override;
	virtual EStateTreeRunStatus Tick(FStateTreeExecutionContext& Context, const float DeltaTime) const override;
	// 必须在 ExitState 解除"站定"：否则下一个状态若不自带 SetMaxWalkSpeed，
	// 怪物会永久卡在速度 0（状态机契约要求每个状态自己收拾干净）
	virtual void ExitState(FStateTreeExecutionContext& Context,
	                       const FStateTreeTransitionResult& Transition) const override;
};
