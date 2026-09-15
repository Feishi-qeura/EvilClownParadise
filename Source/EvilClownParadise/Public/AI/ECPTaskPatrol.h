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
	
	UPROPERTY(EditAnywhere, Category= "ECP|AI")
	FVector CurrentTarget = FVector::ZeroVector;
	
	float NextPickTime = 0.0f;
};

USTRUCT()
struct FECPTaskPatrol : public FStateTreeTaskCommonBase
{
	GENERATED_BODY()
	
	using FInstanceDataType = FECPTaskPatrolInstanceData;
	
	virtual const UStruct* GetInstanceDataType() const override { return FInstanceDataType::StaticStruct(); };
	
	virtual EStateTreeRunStatus EnterState(FStateTreeExecutionContext& Context, const FStateTreeTransitionResult& Transition) const override;
	virtual EStateTreeRunStatus Tick(FStateTreeExecutionContext& Context, const float DeltaTime) const override;
	virtual void ExitState(FStateTreeExecutionContext& Context, const FStateTreeTransitionResult& Transition) const override;
};
