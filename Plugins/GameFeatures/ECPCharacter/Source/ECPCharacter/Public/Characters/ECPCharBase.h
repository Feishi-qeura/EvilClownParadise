#pragma once

#include "CoreMinimal.h"
#include "ECPCore.h"
#include "Animation/PoseSnapshot.h"
#include "Characters/ECPRagdollTypes.h"
#include "Interaction/ECPCarryTarget.h"
#include "GameFramework/Character.h"
#include "ECPCharBase.generated.h"

class UAnimMontage;
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FECPOnHealthChanged, float, OldHealth, float, NewHealth);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FECPOnDied, AActor*, Killer);

// 玩家和敌人共享服务器物理、姿态复制及恢复锁；具体输入和拾取由玩家层负责。
UCLASS()
class ECPCHARACTER_API AECPCharBase : public ACharacter, public IECPCarryTarget
{
	GENERATED_BODY()
public:
	AECPCharBase();

	/** 读取本端最近同步的总负重。@return 千克重量；服务器结算，客户端只读，不发送 RPC。 */
	UFUNCTION(BlueprintPure, Category="ECP|Load") float GetCarriedWeightKg() const { return CarriedWeightKg; }
	/** 读取负重运动倍率。@return 0～1；各端按同步重量计算，不发送 RPC。 */
	UFUNCTION(BlueprintPure, Category="ECP|Load") float GetLoadMultiplier() const;
	/** 查询是否超重。@return 达到承重上限时为 true；各端只读，交互卸重仍可使用。 */
	UFUNCTION(BlueprintPure, Category="ECP|Load") bool IsOverloaded() const { return GetLoadMultiplier() <= 0.f; }
	/** 修改总负重。@param WeightKg 有限、非负千克值；仅服务器执行并同步运动限制，无返回值。 */
	UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category="ECP|Load") void SetCarriedWeightKg(float WeightKg);
	/** 扩展承重上限。@param CapacityKg 有限、正数千克值；仅服务器执行并同步，无返回值。 */
	UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category="ECP|Load") void SetMaxCarryWeightKg(float CapacityKg);
	/** 修改身体重量。@param WeightKg 有限、正数千克值；仅服务器执行并刷新搬运者负重，无返回值。 */
	UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category="ECP|Load") void SetBodyWeightKg(float WeightKg);
	UPROPERTY(EditAnywhere, BlueprintReadOnly, ReplicatedUsing=OnRep_Load, Category="ECP|Load", meta=(ClampMin="0.1", Units="kg"))
	float MaxCarryWeightKg = 200.f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, ReplicatedUsing=OnRep_BodyWeight, Category="ECP|Load", meta=(ClampMin="0.1", Units="kg"))
	float BodyWeightKg = 80.f;

	virtual UPrimitiveComponent* GetCarryBody() const override;
	virtual FName GetCarryBone() const override { return RagdollPelvisBone; }
	virtual float GetCarryMassKg() const override;
	virtual APawn* GetCarryHolder() const override { return CarryHolder.Get(); }
	virtual bool CanBeCarriedBy(const APawn* Holder) const override;
	virtual bool TryBeginCarry(APawn* Holder) override;
	virtual void EndCarry(APawn* Holder) override;

	/** 读取角色所属阵营，供 AI 在不依赖玩家交互类时筛选目标。
	 * @return 静态角色类型决定的阵营编号；默认中立，派生玩家和怪物分别覆盖。
	 * 网络：只读本端类型契约，不发送 RPC，也不允许客户端改变权威阵营。
	 */
	UFUNCTION(BlueprintPure, Category="ECP|Team")
	virtual uint8 GetCharacterTeamId() const { return ECPTeam::Neutral; }

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="ECP|Health") float MaxHealth = 100.f;
	UPROPERTY(BlueprintAssignable, Category="ECP|Health") FECPOnHealthChanged OnHealthChanged;
	UPROPERTY(BlueprintAssignable, Category="ECP|Health") FECPOnDied OnDied;
	/** 读取当前生命值。
	 * @return 本端最近已应用的生命值；服务器为权威值，客户端可能等待属性复制。
	 */
	UFUNCTION(BlueprintPure, Category="ECP|Health") float GetCurrentHealth() const { return CurrentHealth; }
	/** 读取死亡状态。
	 * @return 本端最近已应用的死亡标记；仅服务器改变权威状态。
	 */
	UFUNCTION(BlueprintPure, Category="ECP|Health") bool IsDead() const { return bIsDead; }
	virtual float TakeDamage(float DamageAmount, const FDamageEvent& DamageEvent, AController* EventInstigator, AActor* DamageCauser) override;

	// 拥有角色的客户端只提出请求；服务器和 AI 可以直接调用同一入口。
	/** 请求切换布娃娃状态。
	 * 网络：拥有角色的客户端发送服务器 RPC；服务器直接校验执行，其他客户端不能代为请求。
	 */
	UFUNCTION(BlueprintCallable, Category="ECP|Ragdoll") void RequestRagdollToggle();
	/** 令角色进入布娃娃阶段。
	 * 网络：仅服务器可调用；完整姿态与阶段通过属性复制同步。
	 */
	UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category="ECP|Ragdoll") void StartRagdoll();
	/** 尝试在满足恢复条件时结束布娃娃并进入落地阶段。
	 * @return 服务器接受恢复返回 true，条件不满足返回 false。
	 * 网络：仅服务器可调用，客户端通过复制观察结果。
	 */
	UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category="ECP|Ragdoll") bool TryRecoverFromRagdoll();
	/** 读取本端是否正在显示布娃娃。
	 * @return 本端表现状态；客户端在复制阶段应用后更新。
	 */
	UFUNCTION(BlueprintPure, Category="ECP|Ragdoll") bool IsRagdollActive() const { return bRagdollActive; }
	/** 读取角色是否因布娃娃、落地或死亡而禁止移动。
	 * @return 任一移动锁或死亡条件成立时返回 true。
	 * 网络：本地输入可据此屏蔽操作，服务器仍独立校验。
	 */
	UFUNCTION(BlueprintPure, Category="ECP|Movement") bool IsMovementLocked() const { return bMovementLocked || bIsDead; }
	/** 请求开始或结束跑步。
	 * @param bRunning true 请求跑步，false 取消跑步。
	 * 网络：拥有客户端先预测再发送服务器 RPC；服务器独立校验最终状态并复制。
	 */
	UFUNCTION(BlueprintCallable, Category="ECP|Movement") void SetRunning(bool bRunning);
	/** 读取已应用的跑步请求状态。
	 * @return 本端已应用的跑步标记，可能包含拥有客户端等待服务器确认的预测；不等同于实际速度。
	 */
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
	/** 处理权威死亡后的表现与派生类清理。
	 * @param Killer 导致死亡的伤害来源 Actor，可为空。
	 * 网络：死亡结算由服务器发起；本事件本身不执行跨端 RPC。
	 */
	UFUNCTION(BlueprintNativeEvent, Category="ECP|Health") void HandleDied(AActor* Killer);
	virtual void HandleDied_Implementation(AActor* Killer);
	UFUNCTION() void OnRep_CurrentHealth();
	UFUNCTION() void OnRep_IsDead();
	void BroadcastHealthChange(float OldHealth, float NewHealth);
	// All load, run and recovery paths must use the same base values, never compound a previous multiplier.
	void RefreshLoadMovement();
	UPROPERTY(ReplicatedUsing=OnRep_CurrentHealth) float CurrentHealth = 100.f;
	UPROPERTY(ReplicatedUsing=OnRep_IsDead) bool bIsDead = false;

private:
	UPROPERTY(ReplicatedUsing=OnRep_Load) float CarriedWeightKg = 0.f;
	UPROPERTY(Replicated) TObjectPtr<APawn> CarryHolder;
	UFUNCTION() void OnRep_Load();
	UFUNCTION() void OnRep_BodyWeight();
	UFUNCTION() void OnCarryHolderDestroyed(AActor* DestroyedActor);
	float BaseJumpZVelocity = 420.f;
	float BaseMaxAcceleration = 2048.f;
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
