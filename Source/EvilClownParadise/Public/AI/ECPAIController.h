// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "AIController.h"
#include "ECPAIController.generated.h"

/**
 * 怪物 AI 控制器：持有 StateTree 决策组件与视觉感知组件。
 *
 * 阵营通过 GetGenericTeamId 暴露（AECPPlayerController 返回玩家阵营），
 * 感知只勾选"敌人"，玩家之外的 Pawn 不再触发回调。
 */
UCLASS()
class EVILCLOWNPARADISE_API AECPAIController : public AAIController
{
	GENERATED_BODY()

public:
	AECPAIController();

	/** 感知阵营：与玩家阵营不同即视为敌人（见 ECPTeam） */
	virtual FGenericTeamId GetGenericTeamId() const override;

protected:
	virtual void PostInitializeComponents() override;

private:
	UFUNCTION()
	void OnTargetPerceptionUpdated(AActor* Actor, struct FAIStimulus Stimulus);

	/**
	 * 视觉参数在构造函数里写入并已 ConfigureSense。
	 * 因此这里用 VisibleAnywhere：改成 EditAnywhere 会让编辑器里的改动被静默忽略。
	 * 多怪物类型需要差异化视野时，改为 DataAsset 驱动（见 docs/tech-debt.md）。
	 */
	UPROPERTY(VisibleAnywhere, Category = "ECP|AI")
	TObjectPtr<class UAISenseConfig_Sight> SightConfig;

	UPROPERTY(VisibleAnywhere, Category = "ECP|AI")
	TObjectPtr<class UStateTreeAIComponent> StateTreeAIComp;

	UPROPERTY(VisibleAnywhere, Category = "ECP|AI")
	TObjectPtr<class UAIPerceptionComponent> AIPerceptionComp;
};
