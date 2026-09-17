// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Characters/ECPCharBase.h"
#include "ECPMonsterBase.generated.h"

/**
 * 怪物基类：血量/死亡继承自 AECPCharBase，这里只放 AI 需要的目标、巡逻与攻击参数。
 * 参数目前是类默认值，多怪物类型出现前应迁移到 DataAsset（见 docs/tech-debt.md）。
 */
UCLASS()
class EVILCLOWNPARADISE_API AECPMonsterBase : public AECPCharBase
{
	GENERATED_BODY()

public:
	AECPMonsterBase(const FObjectInitializer& ObjectInitializer);

	void SetTarget(AActor* NewTarget) { TargetActor = NewTarget; }

	UFUNCTION(BlueprintPure, Category = "ECP|AI")
	AActor* GetTarget() const { return TargetActor.Get(); }

	UFUNCTION(BlueprintPure, Category = "ECP|AI")
	bool HasTarget() const;

	UFUNCTION(BlueprintPure, Category = "ECP|AI")
	FVector GetPatrolOrigin() const { return PatrolOrigin; }

	UFUNCTION(BlueprintPure, Category = "ECP|AI")
	bool IsTargetInAttackRange() const;

	UFUNCTION(BlueprintCallable, Category = "ECP|AI")
	void SetMaxWalkSpeed(float NewSpeed);

	UFUNCTION(BlueprintCallable, Category = "ECP|AI")
	void PerformAttack();

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "ECP|AI")
	float PatrolRadius = 1500.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "ECP|AI")
	float PatrolSpeed = 200.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "ECP|AI")
	float ChaseSpeed = 550.f;

	/** 攻击距离：按双方 Actor 原点（胶囊中心）计算，不是表面距离 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "ECP|AI")
	float AttackRange = 150.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "ECP|AI")
	float AttackDamage = 10.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "ECP|AI")
	float AttackInterval = 1.5f;

protected:
	virtual void BeginPlay() override;

private:
	/**
	 * 追逐目标。用弱引用而不是 UPROPERTY 强引用：
	 * 目标只是"当前盯上谁"的辅助信息，不该让怪物持有玩家 Actor 的生命周期，
	 * 玩家被销毁后这里自动失效，HasTarget() 直接返回 false。
	 */
	TWeakObjectPtr<AActor> TargetActor;

	/** 巡逻的中心，BeginPlay 时取初始位置（仅服务器） */
	FVector PatrolOrigin = FVector::ZeroVector;

	float LastAttackTime = -FLT_MAX;
};
