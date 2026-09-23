#pragma once

#include "Actors/ECPPickupTypes.h"
#include "CoreMinimal.h"
#include "Engine/EngineTypes.h"
#include "GameFramework/Actor.h"
#include "ECPPickupItem.generated.h"

class AECPPlayerBase;
class APlayerState;
class UPrimitiveComponent;
class USceneComponent;

/** 状态属于物品本身：角色销毁或退出相关范围后，客户端仍能收到正确的释放状态。 */
USTRUCT()
struct FECPPickupNetState
{
	GENERATED_BODY()
	UPROPERTY() uint32 Revision = 0;
	UPROPERTY() EECPPickupState State = EECPPickupState::World;
	UPROPERTY() TObjectPtr<AECPPlayerBase> Holder = nullptr;
	// 绝对旋转和确认序号一起复制，拥有客户端据此确认预测，而非比较角度是否碰巧相同。
	UPROPERTY() FECPInspectionRotationSample Inspection;
	// 由服务器在附着前保存并随状态复制，确保检查、装备、入库和释放都沿用物品原本的世界缩放。
	UPROPERTY() FVector PreservedWorldScale = FVector::OneVector;
	// 焦点所有者与物理持有者分开；不能借 SetOwner 锁定焦点，否则会改变 RPC/相关性。
	UPROPERTY() bool bFocused = false;
	UPROPERTY() TObjectPtr<AECPPlayerBase> FocusOwner = nullptr;
	UPROPERTY() TObjectPtr<APlayerState> FocusPlayerState = nullptr;
	UPROPERTY() FTransform ReleaseTransform = FTransform::Identity;
	UPROPERTY() FVector ReleaseVelocity = FVector::ZeroVector;
};

// 这些是各端从同一 SCS 模板记录的本地默认值，不需要在每次拾取时通过网络重复发送。
struct FECPPickupComponentDefaults
{
	FName Name;
	ECollisionEnabled::Type CollisionEnabled = ECollisionEnabled::NoCollision;
	bool bReplicated = false;
};

/** BP_Smallitems 的原生父类；保留 Sphere1 根组件、Sphere 检测体和 Cube 高亮。 */
UCLASS()
class EVILCLOWNPARADISE_API AECPPickupItem : public AActor
{
	GENERATED_BODY()
public:
	AECPPickupItem();

	UFUNCTION(BlueprintPure, Category = "ECP|Pickup")
	EECPPickupState GetPickupState() const { return AttachmentState.State; }
	uint32 GetStateRevision() const { return AttachmentState.Revision; }
	AECPPlayerBase* GetPickupOwner() const { return AttachmentState.Holder.Get(); }
	bool IsOwnedBy(const AECPPlayerBase* Player) const;
	bool QueuePickupRequest(AECPPlayerBase* Player);
	bool TryEnterPickup(AECPPlayerBase* Player);
	bool TryEquip(AECPPlayerBase* Player);
	bool TryStore(AECPPlayerBase* Player);
	bool TryDrop(AECPPlayerBase* Player, const FTransform& ReleaseTransform, const FVector& ThrowImpulse);
	bool ApplyInspectionRotation(AECPPlayerBase* Player, const FECPInspectionRotationSample& Sample);
	uint32 GetInspectionGeneration() const { return AttachmentState.Inspection.Generation; }
	uint32 GetLastAcceptedInspectionSession() const { return AttachmentState.Inspection.Session; }
	FRotator GetInspectionRotation() const;
	// 控制器先应用物品倍率，再累计为绝对目标；本地预测和服务器因此使用同一最终姿态。
	UFUNCTION(BlueprintPure, Category = "ECP|Pickup|Inspection")
	FRotator ScaleInspectionRotationDelta(FRotator RotationDelta) const;
	void PreviewInspectionRotation(const FECPInspectionRotationSample& Sample);
	void CancelInspectionPrediction();
	bool DropFromInventory(AECPPlayerBase* Player, const FTransform& ReleaseTransform, const FVector& InitialVelocity);

	bool CanBePickedUp() const;
	bool CanBePickedUpBy(const AECPPlayerBase* Player) const;
	bool IsFocusedBy(const AECPPlayerBase* Player) const;
	bool TryAcquireFocus(AECPPlayerBase* Player);
	void ReleaseFocus(const AECPPlayerBase* Player);

	UFUNCTION(BlueprintPure, Category = "ECP|Pickup|Focus")
	bool IsFocused() const { return AttachmentState.bFocused && AttachmentState.State == EECPPickupState::World; }

	UFUNCTION(BlueprintPure, Category = "ECP|Pickup|Focus")
	AECPPlayerBase* GetFocusOwner() const { return IsFocused() ? AttachmentState.FocusOwner.Get() : nullptr; }

	UFUNCTION(BlueprintPure, Category = "ECP|Pickup|Focus")
	APlayerState* GetFocusPlayerState() const { return IsFocused() ? AttachmentState.FocusPlayerState.Get() : nullptr; }

	// 将来可按 PlayerState 上色；原生当前只统一 Cube 可见性，不改用户现有材质。
	UFUNCTION(BlueprintImplementableEvent, Category = "ECP|Pickup|Focus")
	void OnFocusPresentationChanged(bool bFocused, APlayerState* FocusingPlayerState);
	bool IsHeldBy(const AECPPlayerBase* Player) const;
	bool TryPickUp(AECPPlayerBase* Player);
	void ReleaseFrom(AECPPlayerBase* Player);

	// 检查态相对相机的位置可由物品蓝图覆盖，不要求所有模型拥有相同尺寸和轴向。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "ECP|Pickup|Inspection")
	FTransform InspectionRelativeTransform = FTransform(FRotator::ZeroRotator, FVector(120.f, 0.f, 0.f));

	// 仅保留旧蓝图序列化兼容；绝对目标协议不再按单包增量限幅，所以不再暴露成可调玩法参数。
	UPROPERTY(meta = (DeprecatedProperty, DeprecationMessage = "Absolute inspection samples no longer use a per-packet delta clamp."))
	float MaxInspectionRotationDelta = 25.0f;

	// 每种物品蓝图可独立覆盖；默认 2.0 表示在玩家控制器基础灵敏度上提高到两倍。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "ECP|Pickup|Inspection",
		meta = (ClampMin = "0.1", ClampMax = "10.0", UIMin = "0.1", UIMax = "10.0"))
	float InspectionRotationSpeedMultiplier = 2.0f;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual void OnRep_ReplicateMovement() override;
	virtual void OnRep_AttachmentReplication() override;

private:
	UFUNCTION() void OnRep_AttachmentState();
	UFUNCTION() void OnHolderDestroyed(AActor* DestroyedActor);
	UFUNCTION() void OnFocusOwnerDestroyed(AActor* DestroyedActor);
	void ClearFocusState();
	void ApplyFocusPresentation();
	FTransform BuildInspectionRelativeTransform(const USceneComponent* Parent,
		const FRotator& InspectionRotation) const;
	bool ApplyAttachmentState();
	void ApplyInspectionPresentation();
	void PublishInspectionRotation();
	void RetryAttachment();
	void ResolvePickupRequests();
	void Release(const FTransform& ReleaseTransform, const FVector& InitialVelocity, const FVector& ThrowImpulse);
	void PublishStateChange();

	struct FECPPendingPickupRequest
	{
		TWeakObjectPtr<AECPPlayerBase> Player;
		uint64 ServerFrame = MAX_uint64;
		uint32 ArrivalOrder = 0;
	};

	UPROPERTY(ReplicatedUsing = OnRep_AttachmentState)
	FECPPickupNetState AttachmentState;
	TArray<FECPPickupComponentDefaults> ComponentDefaults;
	TWeakObjectPtr<AECPPlayerBase> BoundHolder;
	// 焦点与持有各自监听，拾取时解绑前者不能误删持有者的销毁清理。
	TWeakObjectPtr<AECPPlayerBase> BoundFocusOwner;
	UPROPERTY(Transient) TObjectPtr<UPrimitiveComponent> FocusVisual;
	TWeakObjectPtr<APlayerState> LastPresentedFocusPlayerState;
	bool bFocusPresentationInitialized = false;
	bool bLastPresentedFocused = false;
	FTimerHandle AttachmentRetryTimer;
	TArray<FECPPendingPickupRequest> PendingPickupRequests;
	uint32 NextPickupArrivalOrder = 0;
	bool bPickupResolveScheduled = false;
	uint32 LastAppliedRevision = 0;
	bool bInitialized = false;
	bool bAttachmentPresentationInitialized = false;
	// 仅本机真正经历持有→释放才采用释放瞬间的位置；晚加入必须保留当前 FRepMovement。
	bool bPresentedAsHeld = false;
	FECPInspectionRotationSample PredictedInspection;
	bool bHasPredictedInspectionRotation = false;
	float OriginalNetUpdateFrequency = 100.f;
	double LastInspectionNetworkPublishTime = -1.0;
	bool bOriginalAlwaysRelevant = false;
	bool bOriginalOwnerRelevancy = false;
};
