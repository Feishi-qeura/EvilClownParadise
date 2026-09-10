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
	
private:
	TObjectPtr<AActor> TargetActor;
};
