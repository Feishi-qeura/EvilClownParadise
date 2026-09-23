#include "Characters/ECPCharBase.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/Engine.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Net/UnrealNetwork.h"

AECPCharBase::AECPCharBase()
{
	// 只在布娃娃、空中或落地锁定期间开 Tick；采样在本帧物理解算之后。
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = false;
	PrimaryActorTick.TickGroup = TG_PostPhysics;
	bReplicates = true;
	GetMesh()->bEnablePhysicsOnDedicatedServer = true;
}

void AECPCharBase::BeginPlay()
{
	Super::BeginPlay();
	MeshRelativeBeforeRagdoll = GetMesh()->GetRelativeTransform();
	CacheSkeleton();
	UpdateTickEnabled();
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
	DOREPLIFETIME(AECPCharBase, MotionState);
	DOREPLIFETIME(AECPCharBase, bRunningRequested);
}

float AECPCharBase::TakeDamage(float DamageAmount, const FDamageEvent& DamageEvent,
	AController* EventInstigator, AActor* DamageCauser)
{
	// 伤害只由服务器结算，避免每个客户端分别扣血和重复死亡。
	if (!HasAuthority() || bIsDead || DamageAmount <= 0.f) return 0.f;
	const float OldHealth = CurrentHealth;
	CurrentHealth = FMath::Clamp(CurrentHealth - DamageAmount, 0.f, MaxHealth);
	BroadcastHealthChange(OldHealth, CurrentHealth);
	if (CurrentHealth <= 0.f)
	{
		bIsDead = true;
		OnRep_IsDead();
		AActor* Killer = (EventInstigator && EventInstigator->GetPawn()) ? EventInstigator->GetPawn() : DamageCauser;
		UE_LOG(LogTemp, Warning, TEXT("%s 死亡，凶手：%s"), *GetName(), Killer ? *Killer->GetName() : TEXT("未知"));
		HandleDied(Killer);
	}
	else if (GEngine)
	{
		// 专用服务器没有屏幕调试引擎实例，保留原反馈但不解引用空指针。
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
	if (!bIsDead) return;
	GetCapsuleComponent()->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	if (!HasAuthority()) OnDied.Broadcast(nullptr);
}

void AECPCharBase::BroadcastHealthChange(float OldHealth, float NewHealth)
{
	OnHealthChanged.Broadcast(OldHealth, NewHealth);
	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(1, 5.f, FColor::Yellow, FString::Printf(TEXT("血量：%.1f / %.1f"), CurrentHealth, MaxHealth));
	}
}

void AECPCharBase::OnRep_CurrentHealth()
{
	// 客户端继续使用现有生命值通知接口，迁移布娃娃不改变生命系统契约。
	OnHealthChanged.Broadcast(CurrentHealth, CurrentHealth);
}
