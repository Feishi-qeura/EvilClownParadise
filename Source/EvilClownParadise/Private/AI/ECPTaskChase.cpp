// Fill out your copyright notice in the Description page of Project Settings.

#include "AI/ECPTaskChase.h"

// 合并保留 SANC 的 AI 控制器及原 Data 路径，追击任务继续使用同一控制器接口。
#include "Data/ECPAIController.h"
#include "AI/ECPStateTreeCommon.h"
#include "Characters/ECPMonsterBase.h"
#include "Navigation/PathFollowingComponent.h"

EStateTreeRunStatus FECPTaskChase::EnterState(FStateTreeExecutionContext& Context,
                                              const FStateTreeTransitionResult& Transition) const
{
	AECPMonsterBase* Monster = ECPTree::GetMonster(Context);
	AECPAIController* AIController = Monster ? Cast<AECPAIController>(Monster->GetController()) : nullptr;
	if (!Monster || !AIController)
	{
		return EStateTreeRunStatus::Failed;
	}

	Monster->SetMaxWalkSpeed(Monster->ChaseSpeed);

	// 追到攻击范围的一半就停，保证停下时已经在攻击圈内（真实停止距离 = 本值 + 胶囊半径）
	const EPathFollowingRequestResult::Type MoveResult =
	    AIController->MoveToActor(Monster->GetTarget(), Monster->AttackRange * 0.5f);

	// 请求失败 = 目标不可达（玩家站在无导航网格处 / 被完全挡住）。
	// 不判的话任务会永远停在 Running，状态机再也回不到巡逻。
	if (MoveResult == EPathFollowingRequestResult::Failed)
	{
		return EStateTreeRunStatus::Failed;
	}

	return EStateTreeRunStatus::Running;
}

void FECPTaskChase::ExitState(FStateTreeExecutionContext& Context, const FStateTreeTransitionResult& Transition) const
{
	if (AECPMonsterBase* Monster = ECPTree::GetMonster(Context))
	{
		if (AECPAIController* AIController = Cast<AECPAIController>(Monster->GetController()))
		{
			AIController->StopMovement();
		}
	}
}
