// Fill out your copyright notice in the Description page of Project Settings.


#include "AI/ECPTaskPatrol.h"

#include "NavigationSystem.h"
#include "AI/ECPStateTreeCommon.h"
#include "Characters/ECPMonsterBase.h"
#include "Data/ECPAIController.h"
#include "Navigation/PathFollowingComponent.h"

EStateTreeRunStatus FECPTaskPatrol::EnterState(FStateTreeExecutionContext& Context, const FStateTreeTransitionResult& Transition) const
{
	AECPMonsterBase* Monster = ECPTree::GetMonster(Context);
	if (!Monster)
	{
		return EStateTreeRunStatus::Failed;
	}
	
	Monster -> SetMaxWalkSpeed(Monster -> PatrolSpeed);
	
	FInstanceDataType& InstanceData = Context.GetInstanceData(*this);
	InstanceData.NextPickTime = 0.f;
	
	return EStateTreeRunStatus::Running;
}

EStateTreeRunStatus FECPTaskPatrol::Tick(FStateTreeExecutionContext& Context, const float DeltaTime) const
{
	AECPMonsterBase* Monster = ECPTree::GetMonster(Context);
	AAIController* AIController = Monster ? Cast<AECPAIController>(Monster -> GetController()) : nullptr;
	
	if (!AIController || !Monster)
	{
		return EStateTreeRunStatus::Failed;
	}
	
	FInstanceDataType& InstanceData = Context.GetInstanceData(*this);
	const float now = Monster -> GetWorld()->GetTimeSeconds();
	
	const bool bMoveFinished = AIController -> GetMoveStatus() != EPathFollowingStatus::Moving;
	if (bMoveFinished && InstanceData.NextPickTime <= now)
	{
		if (UNavigationSystemV1* NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(Monster -> GetWorld()))
		{
			FNavLocation NavLoc;
			if (NavSys -> GetRandomReachablePointInRadius(Monster ->GetPatrolOrigin(), Monster -> PatrolRadius, NavLoc))
			{
				InstanceData.CurrentTarget = NavLoc.Location;
				AIController -> MoveToLocation(NavLoc.Location);
				InstanceData.NextPickTime = now + 0.5f;
			}
		}
	}
	return EStateTreeRunStatus::Running;
}

void FECPTaskPatrol::ExitState(FStateTreeExecutionContext& Context, const FStateTreeTransitionResult& Transition) const
{
	if (AECPMonsterBase* Monster = ECPTree::GetMonster(Context))
	{
		if (AECPAIController* AIController = Cast<AECPAIController>(Monster -> GetController()))
		{
			AIController -> StopMovement();
		}
	}
}
