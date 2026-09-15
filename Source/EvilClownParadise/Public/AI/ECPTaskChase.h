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
	
	float LastMoveTime = 0.0f;
};

USTRUCT()
struct FECPTaskChase : public FStateTreeTaskCommonBase
{
	GENERATED_BODY()
	
	using FInstanceDataType = FECPTaskChaseInstanceData;
	
	UPROPERTY(EditAnywhere, Category= "ECP|AI")
	float AcceptanceRadius = 50.f;
	
	virtual const UStruct* GetInstanceDataType() const override { return FECPTaskChaseInstanceData::StaticStruct(); };
	
	virtual EStateTreeRunStatus EnterState(FStateTreeExecutionContext& Context, const FStateTreeTransitionResult& Transition) const override;
	virtual void ExitState(FStateTreeExecutionContext& Context, const FStateTreeTransitionResult& Transition) const override;
	
};
