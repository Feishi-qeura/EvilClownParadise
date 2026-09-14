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
	InstanceData.NextPickTime = 0.f;         // 进场立刻出发去第一个点
	InstanceData.bHeadingToTarget = false;
	
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
	
	if (InstanceData.bHeadingToTarget)
	{
		// 到点判定：移动已结束，且已在目标附近。
		// 距离阈值放宽到接受半径的两倍——停止时接受半径会额外加上胶囊体半径，留足余量。
		const bool bMoveEnded = AIController -> GetMoveStatus() != EPathFollowingStatus::Moving;
		const bool bCloseEnough = FVector::DistSquared(Monster -> GetActorLocation(), InstanceData.CurrentTarget)
			<= FMath::Square(AcceptanceRadius * 2.f);
		if (bMoveEnded && bCloseEnough)
		{
			InstanceData.bHeadingToTarget = false;
			InstanceData.NextPickTime = now + PauseDuration;   // 到点后停顿
		}
	}
	else if (now >= InstanceData.NextPickTime)
	{
		// 停顿结束（或刚进场）：选下一个巡逻点并出发
		if (UNavigationSystemV1* NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(Monster -> GetWorld()))
		{
			FNavLocation NavLoc;
			if (NavSys -> GetRandomReachablePointInRadius(Monster ->GetPatrolOrigin(), Monster -> PatrolRadius, NavLoc))
			{
				InstanceData.CurrentTarget = NavLoc.Location;
				// 请求失败（Failed）时保持"未出发"，下一帧自动重试选点
				const EPathFollowingRequestResult::Type MoveResult = AIController -> MoveToLocation(NavLoc.Location, AcceptanceRadius);
				InstanceData.bHeadingToTarget = (MoveResult != EPathFollowingRequestResult::Failed);
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
