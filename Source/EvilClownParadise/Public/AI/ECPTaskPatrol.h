// Fill out your copyright notice in the Description page of Project Settings.

#pragma once
#include "StateTreeTaskBase.h"
#include "ECPTaskPatrol.generated.h"

/**
 * 
 */

USTRUCT()
struct FECPTaskPatrolInstanceData
{
	GENERATED_BODY()
	
	/** 当前巡逻目标点 */
	UPROPERTY(EditAnywhere, Category= "ECP|AI")
	FVector CurrentTarget = FVector::ZeroVector;
	
	/** 停顿结束的时间（到达后 = 到达时间 + PauseDuration） */
	float NextPickTime = 0.0f;
	
	/** 已发出移动请求、正在前往 CurrentTarget */
	bool bHeadingToTarget = false;
};

USTRUCT()
struct FECPTaskPatrol : public FStateTreeTaskCommonBase
{
	GENERATED_BODY()
	
	using FInstanceDataType = FECPTaskPatrolInstanceData;
	
	/** 到达巡逻点后的停顿时间（秒） */
	UPROPERTY(EditAnywhere, Category= "ECP|AI")
	float PauseDuration = 0.5f;
	
	/** 认定"已到达"的距离阈值；MoveTo 的停止距离会叠加胶囊半径，所以到达判定放宽到本值的 2 倍 */
	UPROPERTY(EditAnywhere, Category= "ECP|AI")
	float AcceptanceRadius = 100.f;
	
	virtual const UStruct* GetInstanceDataType() const override { return FInstanceDataType::StaticStruct(); };
	
	virtual EStateTreeRunStatus EnterState(FStateTreeExecutionContext& Context, const FStateTreeTransitionResult& Transition) const override;
	virtual EStateTreeRunStatus Tick(FStateTreeExecutionContext& Context, const float DeltaTime) const override;
	virtual void ExitState(FStateTreeExecutionContext& Context, const FStateTreeTransitionResult& Transition) const override;
};
