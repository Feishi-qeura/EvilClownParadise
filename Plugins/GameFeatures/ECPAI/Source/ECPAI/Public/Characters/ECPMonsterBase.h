// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Characters/ECPCharBase.h"
#include "ECPMonsterBase.generated.h"

/**
 * 
 */
UCLASS()
class ECPAI_API AECPMonsterBase : public AECPCharBase
{
	GENERATED_BODY()
public:
	AECPMonsterBase();

	/** 怪物始终报告怪物阵营；只读本端类默认契约，不产生网络请求。 */
	virtual uint8 GetCharacterTeamId() const override { return ECPTeam::Monster; }

	void SetTarget(AActor* NewTarget) {TargetActor = NewTarget;};
	
	/** 读取 AI 当前目标。
	 * @return 服务器感知维护的目标，可为空；该目标字段不复制到客户端。
	 */
	UFUNCTION(BlueprintPure, Category = "ECP|AI")
	AActor* GetTarget() const {return TargetActor;};
	
	/** 检查 AI 目标引用是否仍有效。
	 * @return 目标存在且未销毁时返回 true。
	 * 网络：供服务器 StateTree 查询，不会触发 RPC。
	 */
	UFUNCTION(BlueprintPure, Category = "ECP|AI")
	bool HasTarget() const {return IsValid(TargetActor);};
	
	/** 读取本局怪物巡逻中心。
	 * @return 服务器 BeginPlay 记录的位置；客户端不应据此运行 AI。
	 */
	UFUNCTION(BlueprintPure, Category = "ECP|AI")
	FVector GetPatrolOrigin() const {return PatrolOrigin;};
	
	/** 检查目标是否位于配置的攻击距离内。
	 * @return 目标有效且距离满足 AttackRange 时返回 true。
	 * 网络：仅查询本端数据，实际攻击仍由服务器校验。
	 */
	UFUNCTION(BlueprintPure, Category = "ECP|AI")
	bool IsTargetInAttackRange() const;
	
	/** 设置当前 AI 状态使用的移动速度。
	 * @param NewSpeed 目标最大行走速度，单位 cm/s。
	 * 网络：服务器 StateTree 调用；函数本身不发送 RPC。
	 */
	UFUNCTION(BlueprintCallable, Category = "ECP|AI")
	void SetMaxWalkSpeed(float NewSpeed);
	
	/** 按距离和攻击间隔尝试攻击当前目标。
	 * 网络：仅服务器结算伤害；非权威端调用不会造成伤害。
	 */
	UFUNCTION(BlueprintCallable, Category = "ECP|AI")
	void PerformAttack();

protected:
	
	virtual void BeginPlay() override;
	
public:
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "ECP|AI")
	float PatrolRadius = 1500.f;
	
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "ECP|AI")
	float PatrolSpeed = 200.f;
	
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "ECP|AI")
	float ChaseSpeed = 450.f;
	
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "ECP|AI")
	float AttackRange = 150.f;
	
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "ECP|AI")
	float AttackDamage = 10.f;
	
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "ECP|AI")
	float AttackInterval = 1.5f;
	
private:
	// 追逐目标
	TObjectPtr<AActor> TargetActor;
	// 巡逻的中心
	FVector PatrolOrigin = FVector::ZeroVector;
	
	float LastAttackTime = -FLT_MAX;
};
