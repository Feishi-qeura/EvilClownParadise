// Fill out your copyright notice in the Description page of Project Settings.


#include "ECPTaskChase.h"

#include "AI/ECPStateTreeCommon.h"
#include "Characters/ECPMonsterBase.h"
#include "Data/ECPAIController.h"

EStateTreeRunStatus FECPTaskChase::EnterState(FStateTreeExecutionContext& Context,
                                             const FStateTreeTransitionResult& Transition) const
{
	AECPMonsterBase* Monster = ECPTree::GetMonster(Context);
	AECPAIController* AIController = Cast<AECPAIController>(Monster -> GetController());
	if (!Monster || !AIController)
	{
		return EStateTreeRunStatus::Failed;
	}
	
	Monster -> SetMaxWalkSpeed(Monster -> ChaseSpeed);
	
	AIController -> MoveToActor(Monster -> GetTarget(), AcceptanceRadius);
	
	return EStateTreeRunStatus::Running;
}

void FECPTaskChase::ExitState(FStateTreeExecutionContext& Context, const FStateTreeTransitionResult& Transition) const
{
	if (AECPMonsterBase* Monster = ECPTree::GetMonster(Context))
	{
		if (AECPAIController* AIController = Cast<AECPAIController>(Monster -> GetController()))
		{
			AIController -> StopMovement();
		}
	}
}
