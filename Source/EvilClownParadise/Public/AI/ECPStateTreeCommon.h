// Fill out your copyright notice in the Description page of Project Settings.

#pragma once
#include "AIController.h"
#include "StateTreeExecutionContext.h"
#include "Characters/ECPMonsterBase.h"

namespace ECPTree
{
	inline AECPMonsterBase* GetMonster(const FStateTreeExecutionContext& Context)
	{
		if (const AAIController* AIController = Cast<AAIController>(Context.GetOwner()))
		{
			return AIController ->GetPawn<AECPMonsterBase>();
		}
		return nullptr;
	}
}
