// Fill out your copyright notice in the Description page of Project Settings.

#pragma once
#include "StateTreeTaskBase.h"

#include "ECPTaskChase.generated.h"

/**
 *
 */
USTRUCT()
struct FECPTaskChaseInstanceData
{
	GENERATED_BODY()
};

USTRUCT()
struct FECPTaskChase : public FStateTreeTaskCommonBase
{
	GENERATED_BODY()

	using FInstanceDataType = FECPTaskChaseInstanceData;

	// 追击的停止距离不在这里配置：由怪物自身的 AttackRange 派生（见 ECPTaskChase.cpp）

	virtual const UStruct* GetInstanceDataType() const override { return FInstanceDataType::StaticStruct(); };

	virtual EStateTreeRunStatus EnterState(FStateTreeExecutionContext& Context,
	                                       const FStateTreeTransitionResult& Transition) const override;
	virtual void ExitState(FStateTreeExecutionContext& Context,
	                       const FStateTreeTransitionResult& Transition) const override;
};