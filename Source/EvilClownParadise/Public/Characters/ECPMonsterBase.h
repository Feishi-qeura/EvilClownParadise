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
	AECPMonsterBase();

	void SetTarget(AActor* NewTarget) {TargetActor = NewTarget;};
	
	UFUNCTION(BlueprintPure, Category = "ECP|AI")
	AActor* GetTarget() const {return TargetActor;};
	
	UFUNCTION(BlueprintPure, Category = "ECP|AI")
	bool HasTarget() const {return IsValid(TargetActor);};
	
	UFUNCTION(BlueprintPure, Category = "ECP|AI")
	FVector GetPatrolOrigin() const {return PatrolOrigin;};
	
	UFUNCTION(BlueprintPure, Category = "ECP|AI")
	bool IsTargetInAttackRange() const;
	
	UFUNCTION(BlueprintCallable, Category = "ECP|AI")
	void SetMaxWalkSpeed(float NewSpeed);
	
	UFUNCTION(BlueprintCallable, Category = "ECP|AI")
	void PerformAttack();

protected:
	
	UFUNCTION()
	virtual void BeginPlay() override;
	
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
