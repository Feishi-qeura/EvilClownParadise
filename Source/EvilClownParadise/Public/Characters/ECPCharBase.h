// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "ECPCharBase.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FECPOnHealthChanged, float, OldHealth, float, NewHealth);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FECPOnDied, AActor*, Killer);

UCLASS()
class EVILCLOWNPARADISE_API AECPCharBase : public ACharacter
{
	GENERATED_BODY()

public:
	AECPCharBase();

	/** 属性升级商店会改这个值 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "ECP|Health")
	float MaxHealth = 100.f;

	UPROPERTY(BlueprintAssignable, Category = "ECP|Health")
	FECPOnHealthChanged OnHealthChanged;

	/** 死亡广播：怪物掉玩偶、玩家死亡UI都从这里接 */
	UPROPERTY(BlueprintAssignable, Category = "ECP|Health")
	FECPOnDied OnDied;

	UFUNCTION(BlueprintPure, Category = "ECP|Health")
	float GetCurrentHealth() const { return CurrentHealth; }

	UFUNCTION(BlueprintPure, Category = "ECP|Health")
	bool IsDead() const { return bIsDead; }

	/** 伤害只在服务器结算，武器开火逻辑必须跑在服务器 */
	virtual float TakeDamage(float DamageAmount, const struct FDamageEvent& DamageEvent, class AController* EventInstigator, AActor* DamageCauser) override;

protected:
	virtual void BeginPlay() override;
	virtual void PostInitializeComponents() override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/** 服务器专属死亡逻辑：基类只广播，怪物子类=掉玩偶+击杀加成，玩家子类=死亡反馈+复活 */
	UFUNCTION(BlueprintNativeEvent, Category = "ECP|Health")
	void HandleDied(AActor* Killer);
	virtual void HandleDied_Implementation(AActor* Killer);

	UFUNCTION()
	void OnRep_CurrentHealth();

	UFUNCTION()
	void OnRep_IsDead();

	void BroadcastHealthChange(float OldHealth, float NewHealth);

	UPROPERTY(ReplicatedUsing = OnRep_CurrentHealth)
	float CurrentHealth = 100.f;

	UPROPERTY(ReplicatedUsing = OnRep_IsDead)
	bool bIsDead = false;
};
