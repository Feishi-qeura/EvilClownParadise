// Fill out your copyright notice in the Description page of Project Settings.

#pragma once
#include "StateTreeEvaluatorBase.h"
#include "ECPEvaluatorTarget.generated.h"

USTRUCT()
struct FECPEvaluatorTargetInstanceData
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Output")
	bool bHasTarget = false;

	UPROPERTY(EditAnywhere, Category = "Output")
	bool bTargetInAttackRange = false;
};

USTRUCT()
struct FECPEvaluatorTarget : public FStateTreeEvaluatorCommonBase
{
	GENERATED_BODY()

	using FInstanceDataType = FECPEvaluatorTargetInstanceData;

	virtual const UStruct* GetInstanceDataType() const override { return FInstanceDataType::StaticStruct(); };

	virtual void Tick(FStateTreeExecutionContext& Context, const float DeltaTime) const override;
};
