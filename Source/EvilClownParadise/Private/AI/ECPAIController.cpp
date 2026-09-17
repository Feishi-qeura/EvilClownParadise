// Fill out your copyright notice in the Description page of Project Settings.

#include "AI/ECPAIController.h"

#include "Characters/ECPMonsterBase.h"
#include "Characters/ECPPlayerBase.h"
#include "Components/StateTreeAIComponent.h"
#include "Debug/DebugHelper.h"
#include "ECPCore.h"
#include "Perception/AIPerceptionComponent.h"
#include "Perception/AIPerceptionTypes.h"
#include "Perception/AISenseConfig_Sight.h"

AECPAIController::AECPAIController()
{
	// 决策：StateTreeAI组件跑在AIC上，附身后自动开始
	StateTreeAIComp = CreateDefaultSubobject<UStateTreeAIComponent>(TEXT("StateTreeAIComp"));

	// 眼睛：视觉感知配置
	SightConfig = CreateDefaultSubobject<UAISenseConfig_Sight>(TEXT("SightConfig"));
	SightConfig->SightRadius = 1000.f;                // 视野范围10m
	SightConfig->LoseSightRadius = 1200.f;            // 视野丢失范围 > 视野范围，避免在边界抽风
	SightConfig->PeripheralVisionAngleDegrees = 60.f; // 视野角度
	SightConfig->SetMaxAge(3.f);                      // 丢失视野后会继续追击3秒

	// 只感知敌人：阵营由 GetGenericTeamId 提供，中立/友方不再触发回调，
	// 省掉一次每 Pawn 的回调 + Cast 过滤。将来做 AI 对 AI 时这条是前提。
	SightConfig->DetectionByAffiliation.bDetectEnemies = true;
	SightConfig->DetectionByAffiliation.bDetectFriendlies = false;
	SightConfig->DetectionByAffiliation.bDetectNeutrals = false;

	AIPerceptionComp = CreateDefaultSubobject<UAIPerceptionComponent>(TEXT("AIPerceptionComp"));
	AIPerceptionComp->ConfigureSense(*SightConfig);
	AIPerceptionComp->SetDominantSense(SightConfig->GetSenseImplementation());
	PerceptionComponent = AIPerceptionComp;
}

FGenericTeamId AECPAIController::GetGenericTeamId() const { return FGenericTeamId(ECPTeam::Monster); }

void AECPAIController::PostInitializeComponents()
{
	Super::PostInitializeComponents();

	// 事件绑定
	AIPerceptionComp->OnTargetPerceptionUpdated.AddDynamic(this, &AECPAIController::OnTargetPerceptionUpdated);
}

void AECPAIController::OnTargetPerceptionUpdated(AActor* Actor, struct FAIStimulus Stimulus)
{
	AECPMonsterBase* Monster = GetPawn<AECPMonsterBase>();
	AECPPlayerBase* Player = Cast<AECPPlayerBase>(Actor);

	if (!Monster || !Player)
	{
		return;
	}

	if (Stimulus.WasSuccessfullySensed() && !Player->IsDead())
	{
		Monster->SetTarget(Player);
		DebugHelper::Print(FString::Printf(TEXT("%s看见了玩家：%s"), *Monster->GetName(), *Player->GetName()), 2.f,
		                   FColor::Cyan);
	}
	// 丢失视野：只清自己当前盯着的目标。
	// 玩家死亡不会走到这里——尸体仍算"看得见"，且视觉感知只在成功/失败翻转时才回调，
	// "目标已死"由 HasTarget() 每次查询时判掉
	else if (Actor == Monster->GetTarget())
	{
		Monster->SetTarget(nullptr);
	}
}
