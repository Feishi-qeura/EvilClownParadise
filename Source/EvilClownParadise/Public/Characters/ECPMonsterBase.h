// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Characters/ECPCharBase.h"
#include "ECPMonsterBase.generated.h"

/**
 * 
 */
UCLASS()
class EVILCLOWNPARADISE_API AECPMonsterBase : public AECPCharBase
{
	GENERATED_BODY()
public:
	AECPMonsterBase(const FObjectInitializer& ObjectInitializer);

	void SetTarget(AActor* NewTarget) {TargetActor = NewTarget;};
	
	UFUNCTION(BlueprintPure, Category = "ECP|AI")
	AActor* GetTarget() const {return TargetActor;};
	
	UFUNCTION(BlueprintPure, Category = "ECP|AI")
	bool HasTarget() const;
	
	UFUNCTION(BlueprintPure, Category = "ECP|AI")
	FVector GetPatrolOrigin() const {return PatrolOrigin;};
	
	UFUNCTION(BlueprintPure, Category = "ECP|AI")
	bool IsTargetInAttackRange() const;
	
	UFUNCTION(BlueprintCallable, Category = "ECP|AI")
	void SetMaxWalkSpeed(float NewSpeed);
	
	UFUNCTION(BlueprintCallable, Category = "ECP|AI")
	void PerformAttack();

protected:
	
	virtual void BeginPlay() override;
	
public:
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "ECP|AI")
	float PatrolRadius = 1500.f;
	
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "ECP|AI")
	float PatrolSpeed = 200.f;
	
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "ECP|AI")
	float ChaseSpeed = 550.f;
	
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "ECP|AI")
	float AttackRange = 150.f;	// 攻击距离：按双方 Actor 原点（胶囊中心）计算，不是表面距离
	
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "ECP|AI")
	float AttackDamage = 10.f;
	
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "ECP|AI")
	float AttackInterval = 1.5f;
	
private:
	// 追逐目标
	TObjectPtr<AActor> TargetActor;
	// 巡逻的中心
	FVector PatrolOrigin = FVector::ZeroVector;
	
	float LastAttackTime = -FLT_MAX;
};
