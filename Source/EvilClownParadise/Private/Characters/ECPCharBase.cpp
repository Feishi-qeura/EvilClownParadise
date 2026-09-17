// Fill out your copyright notice in the Description page of Project Settings.

#include "Characters/ECPCharBase.h"

#include "Components/CapsuleComponent.h"
#include "Debug/DebugHelper.h"
#include "ECPCore.h"
#include "Net/UnrealNetwork.h"

AECPCharBase::AECPCharBase(const FObjectInitializer& ObjectInitializer) : Super(ObjectInitializer)
{
	PrimaryActorTick.bCanEverTick = false;
}

void AECPCharBase::BeginPlay() { Super::BeginPlay(); }

void AECPCharBase::PostInitializeComponents()
{
	Super::PostInitializeComponents();
	CurrentHealth = MaxHealth;
}

void AECPCharBase::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AECPCharBase, CurrentHealth);
	DOREPLIFETIME(AECPCharBase, bIsDead);
}

float AECPCharBase::TakeDamage(float DamageAmount, const struct FDamageEvent& DamageEvent,
                               class AController* EventInstigator, AActor* DamageCauser)
{
	if (!HasAuthority() || bIsDead || DamageAmount <= 0.f)
	{
		return 0.f;
	}

	const float OldHealth = CurrentHealth;
	CurrentHealth = FMath::Clamp(CurrentHealth - DamageAmount, 0.f, MaxHealth);
	// 实际生效的伤害：血量夹取后与请求值不同（过量击杀、已满血时的部分伤害）
	const float AppliedDamage = OldHealth - CurrentHealth;
	BroadcastHealthChange(OldHealth, CurrentHealth);

	if (CurrentHealth <= 0.f)
	{
		bIsDead = true;
		ApplyDeathState();
		AActor* Killer = (EventInstigator && EventInstigator->GetPawn()) ? EventInstigator->GetPawn() : DamageCauser;
		UE_LOG(LogECP, Log, TEXT("%s 死亡，凶手：%s"), *GetName(), Killer ? *Killer->GetName() : TEXT("未知"));
		HandleDied(Killer);
	}
	else
	{
		DebugHelper::Print(FString::Printf(TEXT("%s 受到 %.1f 伤害，剩余血量 %.1f / %.1f"), *GetName(), AppliedDamage,
		                                   CurrentHealth, MaxHealth),
		                   5.f, FColor::Cyan);
	}

	return AppliedDamage;
}

void AECPCharBase::HandleDied_Implementation(AActor* Killer) { OnDied.Broadcast(Killer); }

void AECPCharBase::ApplyDeathState()
{
	// 死亡瞬间角色可能已被销毁（例如同帧内的多次伤害结算），胶囊体不一定还在
	if (UCapsuleComponent* Capsule = GetCapsuleComponent())
	{
		Capsule->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	}
	if (!HasAuthority())
	{
		OnDied.Broadcast(nullptr); // 远程客户端拿不到凶手，死亡UI表现够用
	}
}

void AECPCharBase::OnRep_IsDead()
{
	if (!bIsDead)
	{
		return;
	}
	ApplyDeathState();
}

void AECPCharBase::BroadcastHealthChange(float OldHealth, float NewHealth)
{
	OnHealthChanged.Broadcast(OldHealth, NewHealth);
	// key=1 覆盖刷新同一行，形成常驻血量显示；正式血条UI做好后删掉这一行
	DebugHelper::PrintPersistent(1, FString::Printf(TEXT("血量：%.1f / %.1f"), CurrentHealth, MaxHealth));
}

void AECPCharBase::OnRep_CurrentHealth()
{
	// 客户端拿不到精确旧值，UI盯新值即可
	OnHealthChanged.Broadcast(CurrentHealth, CurrentHealth);
}
