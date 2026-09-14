// Fill out your copyright notice in the Description page of Project Settings.


#include "AI/ECPEvaluatorTarget.h"

#include "StateTreeExecutionContext.h"
#include "AI/ECPStateTreeCommon.h"
#include "Characters/ECPMonsterBase.h"

void FECPEvaluatorTarget::Tick(FStateTreeExecutionContext& Context, const float DeltaTime) const
{
	FInstanceDataType& InstanceData = Context.GetInstanceData(*this);
	
	if (const AECPMonsterBase* Monster = ECPTree::GetMonster(Context))
	{
		InstanceData.bHasTarget = Monster->HasTarget();
		InstanceData.bTargetInAttackRange = Monster->IsTargetInAttackRange();
	}
	else
	{
		InstanceData.bHasTarget = false;
		InstanceData.bTargetInAttackRange = false;
	}
}
