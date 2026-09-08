// Fill out your copyright notice in the Description page of Project Settings.

#include "Characters/ECPCharBase.h"
#include "Components/CapsuleComponent.h"
#include "Engine/Engine.h"
#include "Kismet/GameplayStatics.h"
#include "Net/UnrealNetwork.h"

AECPCharBase::AECPCharBase()
{
	PrimaryActorTick.bCanEverTick = false;
}

void AECPCharBase::BeginPlay()
{
	Super::BeginPlay();
}

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
	BroadcastHealthChange(OldHealth, CurrentHealth);

	if (CurrentHealth <= 0.f)
	{
		bIsDead = true;
		OnRep_IsDead();	// OnRep 只在远程客户端自动触发，监听服务器本机要手动调一次才有死亡表现
		AActor* Killer = (EventInstigator && EventInstigator->GetPawn()) ? EventInstigator->GetPawn() : DamageCauser;
		UE_LOG(LogTemp, Warning, TEXT("%s 死亡，凶手：%s"), *GetName(), Killer ? *Killer->GetName() : TEXT("未知"));
		HandleDied(Killer);
	}
	else
	{
		GEngine->AddOnScreenDebugMessage(-1, 5.f, FColor::Cyan, FString::Printf(TEXT("%s 受到 %.1f 伤害，剩余血量 %.1f / %.1f"), *GetName(), DamageAmount, CurrentHealth, MaxHealth));
	}

	return DamageAmount;
}

void AECPCharBase::HandleDied_Implementation(AActor* Killer)
{
	OnDied.Broadcast(Killer);
}

void AECPCharBase::OnRep_IsDead()
{
	if (!bIsDead)
	{
		return;
	}
	GetCapsuleComponent()->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	if (!HasAuthority())
	{
		OnDied.Broadcast(nullptr);	// 远程客户端拿不到凶手，死亡UI表现够用
	}
}

void AECPCharBase::BroadcastHealthChange(float OldHealth, float NewHealth)
{
	OnHealthChanged.Broadcast(OldHealth, NewHealth);
	if (GEngine)
	{
		// key=1 会覆盖刷新同一行，形成常驻血量显示；正式血条UI做好后删掉
		GEngine->AddOnScreenDebugMessage(1, 5.f, FColor::Yellow, FString::Printf(TEXT("血量：%.1f / %.1f"), CurrentHealth, MaxHealth));
	}
}

void AECPCharBase::OnRep_CurrentHealth()
{
	// 客户端拿不到精确旧值，UI盯新值即可
	OnHealthChanged.Broadcast(CurrentHealth, CurrentHealth);
}
