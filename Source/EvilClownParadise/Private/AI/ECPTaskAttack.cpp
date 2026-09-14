// Fill out your copyright notice in the Description page of Project Settings.


#include "AI/ECPTaskAttack.h"

#include "AI/ECPStateTreeCommon.h"
#include "Characters/ECPMonsterBase.h"
#include "Data/ECPAIController.h"

EStateTreeRunStatus FECPTaskAttack::EnterState(FStateTreeExecutionContext& Context,
                                               const FStateTreeTransitionResult& Transition) const
{
	AECPMonsterBase* Monster = ECPTree::GetMonster(Context);
	AECPAIController* AIController = Monster ? Cast<AECPAIController>(Monster->GetController()) : nullptr;
	if (!Monster || !AIController)
	{
		return EStateTreeRunStatus::Failed;
	}
	
	Monster -> SetMaxWalkSpeed(0.f);
	AIController -> StopMovement();
	
	Monster -> PerformAttack();
	
	return EStateTreeRunStatus::Running;
}

EStateTreeRunStatus FECPTaskAttack::Tick(FStateTreeExecutionContext& Context, const float DeltaTime) const
{
	AECPMonsterBase* Monster = ECPTree::GetMonster(Context);
	if (!Monster)
	{
		return EStateTreeRunStatus::Failed;
	}
	Monster -> PerformAttack();
	
	return EStateTreeRunStatus::Running;
}
