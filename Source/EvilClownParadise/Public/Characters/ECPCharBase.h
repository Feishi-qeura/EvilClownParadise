#pragma once

#include "CoreMinimal.h"
#include "Animation/PoseSnapshot.h"
#include "Characters/ECPRagdollTypes.h"
#include "GameFramework/Character.h"
#include "ECPCharBase.generated.h"

class UAnimMontage;
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FECPOnHealthChanged, float, OldHealth, float, NewHealth);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FECPOnDied, AActor*, Killer);

// 玩家和敌人共享服务器物理、姿态复制及恢复锁；具体输入和拾取由玩家层负责。
UCLASS()
class EVILCLOWNPARADISE_API AECPCharBase : public ACharacter
{
	GENERATED_BODY()
public:
	AECPCharBase();
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="ECP|Health") float MaxHealth = 100.f;
	UPROPERTY(BlueprintAssignable, Category="ECP|Health") FECPOnHealthChanged OnHealthChanged;
	UPROPERTY(BlueprintAssignable, Category="ECP|Health") FECPOnDied OnDied;
	UFUNCTION(BlueprintPure, Category="ECP|Health") float GetCurrentHealth() const { return CurrentHealth; }
	UFUNCTION(BlueprintPure, Category="ECP|Health") bool IsDead() const { return bIsDead; }
	virtual float TakeDamage(float DamageAmount, const FDamageEvent& DamageEvent, AController* EventInstigator, AActor* DamageCauser) override;

	// 拥有角色的客户端只提出请求；服务器和 AI 可以直接调用同一入口。
	UFUNCTION(BlueprintCallable, Category="ECP|Ragdoll") void RequestRagdollToggle();
	UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category="ECP|Ragdoll") void StartRagdoll();
	UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category="ECP|Ragdoll") bool TryRecoverFromRagdoll();
	UFUNCTION(BlueprintPure, Category="ECP|Ragdoll") bool IsRagdollActive() const { return bRagdollActive; }
	UFUNCTION(BlueprintPure, Category="ECP|Movement") bool IsMovementLocked() const { return bMovementLocked || bIsDead; }
	UFUNCTION(BlueprintCallable, Category="ECP|Movement") void SetRunning(bool bRunning);
	UFUNCTION(BlueprintPure, Category="ECP|Movement") bool IsRunning() const { return bRunningRequested; }

	// 链接动画实例在 NativeUpdateAnimation 读取同一显示时间，避免 Tick 顺序造成一帧停顿。
	bool BuildRagdollSnapshot(FPoseSnapshot& OutPose);
	virtual void Tick(float DeltaSeconds) override;
	virtual void Jump() override;
	virtual void Landed(const FHitResult& Hit) override;
	virtual void OnMovementModeChanged(EMovementMode PrevMovementMode, uint8 PreviousCustomMode=0) override;

	// 骨盆名因骨架不同而不同，敌人可在类默认值中覆盖，不硬编码玩家骨架。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="ECP|Ragdoll") FName RagdollPelvisBone = TEXT("Hips");
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="ECP|Ragdoll", meta=(ClampMin="10",ClampMax="60")) float RagdollPoseRate = 30.f;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="ECP|Ragdoll", meta=(ClampMin="0.02",ClampMax="0.15")) float RagdollInterpolationDelay = .04f;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="ECP|Ragdoll") float RecoveryMaximumSpeed = 160.f;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="ECP|Movement") float WalkSpeed = 180.f;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="ECP|Movement") float RunSpeed = 450.f;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="ECP|Movement") float CrouchSpeed = 160.f;
	// 沿用 AMT_Jump 的 Default / DefaultLoop / Landing；可给敌人单独配置蒙太奇。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="ECP|Animation") TObjectPtr<UAnimMontage> JumpMontage;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void PostInitializeComponents() override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual bool CanJumpInternal_Implementation() const override;
	virtual void OnRep_IsCrouched() override;
	UFUNCTION(BlueprintNativeEvent, Category="ECP|Health") void HandleDied(AActor* Killer);
	virtual void HandleDied_Implementation(AActor* Killer);
	UFUNCTION() void OnRep_CurrentHealth();
	UFUNCTION() void OnRep_IsDead();
	void BroadcastHealthChange(float OldHealth, float NewHealth);
	UPROPERTY(ReplicatedUsing=OnRep_CurrentHealth) float CurrentHealth = 100.f;
	UPROPERTY(ReplicatedUsing=OnRep_IsDead) bool bIsDead = false;

private:
	// 阶段、轮次、时间和完整最新姿态放在一个复制结构，支持丢包后恢复及中途加入。
	UPROPERTY(ReplicatedUsing=OnRep_MotionState) FECPRagdollNetState MotionState;
	UPROPERTY(ReplicatedUsing=OnRep_Running) bool bRunningRequested = false;
	UFUNCTION() void OnRep_MotionState();
	UFUNCTION() void OnRep_Running();
	UFUNCTION(Server,Reliable) void ServerRequestRagdollToggle();
	UFUNCTION(Server,Reliable) void ServerSetRunning(bool bRunning);
	void ApplyMotionState();
	void EnterRagdollPresentation();
	void RestoreRagdollPresentation(const FTransform& RecoveryTransform);
	void CaptureRagdollPose();
	void CacheSkeleton();
	void NormalizeStandingCapsule();
	void CacheAndRequirePoseTick();
	void RestorePoseTick();
	void SetMovementLocked(bool bLocked);
	void UpdateTickEnabled();
	void PlayAirMontage();
	void BeginLanding(bool bFromRagdoll);
	void FinishLanding(uint32 LandingGeneration);
	void PublishNormalState();
	double GetServerTime() const;

	TArray<FECPRagdollNetState> PoseBuffer;
	TArray<FName> CachedBoneNames;
	uint32 CachedSkeletonSignature = 0;
	uint32 AppliedSequence = 0;
	uint32 LandingGeneration = 0;
	// 自主客户端的预测落地必须得到新轮次确认，不能把旧 Normal 当作本轮已经结束。
	uint32 PredictedLandingBaseSequence = 0;
	EECPMotionPhase AppliedPhase = EECPMotionPhase::Normal;
	FTransform MeshRelativeBeforeRagdoll;
	FTransform PoseFrame;
	FTimerHandle LandingFallbackTimer;
	TWeakObjectPtr<AController> LockedController;
	double NextPoseSampleTime = 0;
	double LastRagdollRequestTime = -1;
	float SavedMaxAcceleration = 0;
	float SavedWalkSpeed = 180.f;
	float SavedCrouchSpeed = 160.f;
	int32 SavedForcedLOD = 0;
	uint8 SavedPoseTickMode = 0;
	ECollisionEnabled::Type SavedMeshCollision = ECollisionEnabled::QueryOnly;
	ECollisionEnabled::Type SavedCapsuleCollision = ECollisionEnabled::QueryAndPhysics;
	FName SavedMeshCollisionProfile;
	bool bRagdollActive = false;
	bool bMovementLocked = false;
	bool bLocalLandingPlaying = false;
	bool bPredictedLandingPending = false;
	bool bPredictedLandingFinished = false;
	bool bPoseTickModeCached = false;
	bool bDeferredMotionStateApply = false;
	bool bSavedMovementTick = true;
	bool bSavedReplicateMovement = true;
	bool bSavedMeshReplicated = false;
	bool bSavedUpdateRateOptimizations = false;
	bool bSavedIgnoreClientCorrections = false;
	bool bSavedIgnoreServerCorrections = false;
	bool bApplyingMotionState = false;
};
