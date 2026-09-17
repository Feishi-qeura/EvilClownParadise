// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "GenericTeamAgentInterface.h"
#include "ECPCharBase.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FECPOnHealthChanged, float, OldHealth, float, NewHealth);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FECPOnDied, AActor*, Killer);

/**
 * 角色基类。
 * 同时实现 IGenericTeamAgentInterface：AI 感知判定"是不是敌人"时会直接对
 * Pawn 调 GetTeamIdentifier / GetTeamAttitudeTowards，而这些函数不会回退去问
 * Controller（详见 ECPCore.h 里 ECPTeam 的说明）。
 */
UCLASS()
class EVILCLOWNPARADISE_API AECPCharBase : public ACharacter, public IGenericTeamAgentInterface
{
	GENERATED_BODY()

public:
	AECPCharBase(const FObjectInitializer& ObjectInitializer);

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

	/** 感知阵营：与对方阵营不同即互为敌人。子类在构造函数里指定，蓝图可逐类覆盖。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "ECP|AI")
	uint8 TeamId = 255; // FGenericTeamId::NoTeam

	/** IGenericTeamAgentInterface */
	virtual FGenericTeamId GetGenericTeamId() const override { return FGenericTeamId(TeamId); }

	/**
	 * 伤害只在服务器结算，武器开火逻辑必须跑在服务器。
	 * 返回值是"实际生效的伤害"（过量击杀、夹取会小于请求值），
	 * 伤害数字与统计一律用返回值，不要用请求值。
	 */
	virtual float TakeDamage(float DamageAmount, const struct FDamageEvent& DamageEvent,
	                         class AController* EventInstigator, AActor* DamageCauser) override;

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

	/**
	 * 死亡表现（关碰撞、向客户端广播死亡）。
	 * 服务器在结算伤害时直接调用，远程客户端由 OnRep_IsDead 调用——
	 * 两端共用同一段逻辑，避免"手动调 OnRep、函数内部再判一次 HasAuthority"的脆写法。
	 */
	void ApplyDeathState();

	UPROPERTY(ReplicatedUsing = OnRep_CurrentHealth)
	float CurrentHealth = 100.f;

	UPROPERTY(ReplicatedUsing = OnRep_IsDead)
	bool bIsDead = false;
};
