#pragma once

#include "Actors/ECPPickupTypes.h"
#include "CoreMinimal.h"
#include "Engine/EngineTypes.h"
#include "GameFramework/Actor.h"
#include "Interaction/ECPCarryTarget.h"
#include "ECPPickupItem.generated.h"

class AECPPlayerBase;
class APlayerState;
class UPrimitiveComponent;
class USceneComponent;
class UStaticMeshComponent;
class UMaterialInterface;
class UWidgetComponent;
class APlayerController;
class UUserWidget;
class SWidget;

/** 状态属于物品本身：角色销毁或退出相关范围后，客户端仍能收到正确的释放状态。 */
USTRUCT()
struct ECPINTERACTION_API FECPPickupNetState
{
	GENERATED_BODY()
	UPROPERTY() uint32 Revision = 0;
	UPROPERTY() EECPPickupState State = EECPPickupState::World;
	UPROPERTY() TObjectPtr<AECPPlayerBase> Holder = nullptr;
	// 绝对旋转和确认序号一起复制，拥有客户端据此确认预测，而非比较角度是否碰巧相同。
	UPROPERTY() FECPInspectionRotationSample Inspection;
	// 检查态相对相机的 X 距离及滚轮确认序号，随物品状态同步给所有观察者。
	UPROPERTY() float InspectionDistance = 120.f;
	UPROPERTY() uint32 InspectionDistanceSequence = 0;
	// 由服务器在附着前保存并随状态复制，确保检查、装备、入库和释放都沿用物品原本的世界缩放。
	UPROPERTY() FVector PreservedWorldScale = FVector::OneVector;
	// 焦点所有者与物理持有者分开；不能借 SetOwner 锁定焦点，否则会改变 RPC/相关性。
	UPROPERTY() bool bFocused = false;
	UPROPERTY() TObjectPtr<AECPPlayerBase> FocusOwner = nullptr;
	UPROPERTY() TObjectPtr<APlayerState> FocusPlayerState = nullptr;
	UPROPERTY() FTransform ReleaseTransform = FTransform::Identity;
	UPROPERTY() FVector ReleaseVelocity = FVector::ZeroVector;
	// Actual server physics pose travels with the lifecycle revision, never with a predicted input target.
	UPROPERTY() FTransform HeldTransform = FTransform::Identity;
	// 与物理姿态在同一帧采样的持有者视点；拥有客户端补偿本地预测移动，不运行第二套物理。
	UPROPERTY() FTransform HeldReference = FTransform::Identity;
	UPROPERTY() bool bHasHeldReference = false;
};

// 这些是各端从同一 SCS 模板记录的本地默认值，不需要在每次拾取时通过网络重复发送。
struct FECPPickupComponentDefaults
{
	FName Name;
	ECollisionEnabled::Type CollisionEnabled = ECollisionEnabled::NoCollision;
	bool bReplicated = false;
};

/** BP_Smallitems 的原生父类；物理根组件负责移动复制，静态网格体叠加描边。 */
UCLASS()
class ECPINTERACTION_API AECPPickupItem : public AActor, public IECPCarryTarget
{
	GENERATED_BODY()
public:
	AECPPickupItem();
	virtual void Tick(float DeltaSeconds) override;
	/** 读取已确认的物品重量。@return 千克值，入库后仍保留；各端只读，不发送 RPC。 */
	UFUNCTION(BlueprintPure, Category="ECP|Pickup|Mass") float GetPickupMassKg() const { return AuthoritativeMassKg; }
	/** 刷新物理或配置重量，重量变化时通知持有者；仅服务器执行，无参数、无返回值。 */
	UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category="ECP|Pickup|Mass") void RefreshPickupMass();
	/** 读取按重量计算的跟随时间常数。@return 秒数；各端只读，不发送 RPC。 */
	UFUNCTION(BlueprintPure, Category="ECP|Pickup|Lag") float GetPickupResponseSeconds() const;
	FTransform GetInspectionTargetWorldTransform() const;
	virtual UPrimitiveComponent* GetCarryBody() const override;
	virtual float GetCarryMassKg() const override { return GetPickupMassKg(); }
	virtual APawn* GetCarryHolder() const override;
	virtual bool CanBeCarriedBy(const APawn* Holder) const override;
	virtual bool TryBeginCarry(APawn* Holder) override;
	virtual void EndCarry(APawn* Holder) override;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="ECP|Pickup|Mass", meta=(ClampMin="0.1", Units="kg")) float DefaultMassKg = 10.f;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="ECP|Pickup|Mass", meta=(ClampMin="0.1", Units="kg")) float MinimumMassKg = 0.1f;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="ECP|Pickup|Mass", meta=(ClampMin="0.1", Units="kg")) float MaximumMassKg = 100000.f;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="ECP|Pickup") bool bSupportsEquipment = true;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="ECP|Pickup") bool bSupportsStorage = true;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="ECP|Pickup|Lag", meta=(ClampMin="0.01", Units="s")) float LagResponseAt50Kg = 0.12f;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="ECP|Pickup|Lag", meta=(ClampMin="0.01", Units="s")) float LagResponseAt120Kg = 0.45f;
	virtual bool CanBeBaseForCharacter(APawn* Pawn) const override { return false; }
	/** 自动管理物品价值控件：检查态显示，屏幕朝向及本地遮挡独立于物理根组件。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="ECP|Pickup|Value") bool bManageValueWidgets=true;
	/** 留空时管理本 Actor 的所有控件组件；有其他用途的控件时可用组件标签限定价值控件。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="ECP|Pickup|Value") FName ValueWidgetComponentTag;
	/**
	 * @brief 查询指定视点能否看到检查中的物品价值，按本端场景验证遮挡。
	 * @param Viewer 同一世界的玩家控制器，使用其实际相机视点。
	 * @return 物品处于检查态、在相机前方且无遮挡时为 true。
	 * @note 各端本地只读计算，不发送 RPC、不复制界面可见性。
	 */
	UFUNCTION(BlueprintPure, Category="ECP|Pickup|Value") bool CanShowPickupValueTo(const APlayerController* Viewer) const;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "ECP|Pickup|Focus")
	TObjectPtr<UMaterialInterface> FocusOutlineMaterial;

	/**
	 * @brief 读取该物品最近一次同步的生命周期状态，供界面区分世界、检查、装备和库存。
	 * @return 当前生命周期枚举；客户端读到的是最近复制的状态。
	 * @note 服务器决定真实状态，各端均可只读查询。
	 */
	UFUNCTION(BlueprintPure, Category = "ECP|Pickup")
	EECPPickupState GetPickupState() const { return AttachmentState.State; }
	uint32 GetStateRevision() const { return AttachmentState.Revision; }
	AECPPlayerBase* GetPickupOwner() const { return AttachmentState.Holder.Get(); }
	UFUNCTION(BlueprintPure, Category = "ECP|Pickup")
	virtual bool SupportsEquipment() const { return bSupportsEquipment; }
	UFUNCTION(BlueprintPure, Category = "ECP|Pickup")
	virtual bool SupportsStorage() const { return bSupportsStorage; }
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
	float GetInspectionDistance() const;
	float GetInspectionScrollStep() const { return FMath::Max(0.1f, InspectionScrollStep); }
	float ClampInspectionDistance(float Distance) const;
	uint32 GetNextInspectionDistanceSequence() const;
	bool ApplyInspectionDistance(AECPPlayerBase* Player, uint32 Generation, uint32 Sequence, float Distance);
	void PreviewInspectionDistance(uint32 Generation, uint32 Sequence, float Distance);
	// 控制器先应用物品倍率，再累计为绝对目标；本地预测和服务器因此使用同一最终姿态。
	/**
	 * @brief 按物品独立倍率缩放检查旋转输入，使不同模型可使用各自的旋转速度。
	 * @param RotationDelta 原始鼠标旋转增量。
	 * @return 乘以 InspectionRotationSpeedMultiplier 后的旋转增量。
	 * @note 各端均可计算；本函数不提交状态，服务器仍校验最终旋转样本。
	 */
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

	/**
	 * @brief 判断世界中的物品是否已被玩家瞄准，用于高亮和提示。
	 * @return 物品处于 World 状态且存在焦点标记时返回 true。
	 * @note 服务器维护焦点；客户端只读取最近同步的结果。
	 */
	UFUNCTION(BlueprintPure, Category = "ECP|Pickup|Focus")
	bool IsFocused() const { return AttachmentState.bFocused && AttachmentState.State == EECPPickupState::World; }

	/**
	 * @brief 读取当前瞄准该世界物品的玩家角色，便于界面显示焦点归属。
	 * @return 焦点有效时返回玩家角色，否则返回 nullptr。
	 * @note 各端只读；服务器负责授予和释放焦点。
	 */
	UFUNCTION(BlueprintPure, Category = "ECP|Pickup|Focus")
	AECPPlayerBase* GetFocusOwner() const { return IsFocused() ? AttachmentState.FocusOwner.Get() : nullptr; }

	/**
	 * @brief 读取焦点玩家的 PlayerState，以便蓝图按玩家身份绘制高亮。
	 * @return 焦点有效时返回对应 PlayerState，否则返回 nullptr。
	 * @note 各端只读最近同步的焦点，不能借此改变物品持有者。
	 */
	UFUNCTION(BlueprintPure, Category = "ECP|Pickup|Focus")
	APlayerState* GetFocusPlayerState() const { return IsFocused() ? AttachmentState.FocusPlayerState.Get() : nullptr; }

	// 蓝图可按 PlayerState 进一步定制描边颜色。
	/**
	 * @brief 在焦点表现发生变化时通知物品蓝图更新本机颜色或提示。
	 * @param bFocused 当前是否处于世界焦点状态。
	 * @param FocusingPlayerState 当前焦点玩家，可为空。
	 * @return 无返回值。
	 * @note 服务器和相关客户端根据同步状态触发本机表现；蓝图不得在此决定拾取归属。
	 */
	UFUNCTION(BlueprintImplementableEvent, Category = "ECP|Pickup|Focus")
	void OnFocusPresentationChanged(bool bFocused, APlayerState* FocusingPlayerState);
	bool IsHeldBy(const AECPPlayerBase* Player) const;
	bool TryPickUp(AECPPlayerBase* Player);
	void ReleaseFrom(AECPPlayerBase* Player);

	// 检查态相对相机的位置可由物品蓝图覆盖，不要求所有模型拥有相同尺寸和轴向。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "ECP|Pickup|Inspection")
	FTransform InspectionRelativeTransform = FTransform(FRotator::ZeroRotator, FVector(120.f, 0.f, 0.f));

	// 仅限制 InspectionRelativeTransform 的 X 距离；蓝图配置的 Y/Z 偏移保持不变。
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "ECP|Pickup|Inspection|Distance",
		meta = (ClampMin = "0.0", UIMin = "0.0", UIMax = "300.0"))
	float InspectionDistanceMin = 80.f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "ECP|Pickup|Inspection|Distance",
		meta = (ClampMin = "0.0", UIMin = "0.0", UIMax = "300.0"))
	float InspectionDistanceMax = 150.f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "ECP|Pickup|Inspection|Distance",
		meta = (ClampMin = "0.1", UIMin = "0.1", UIMax = "50.0"))
	float InspectionScrollStep = 10.f;

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
	// 子类只改变表现，不改权威目标、预测序号或物品状态协议。
	virtual void PrepareForPickup() {}
	virtual void OnPickupPresentationStateChanged() {}
	virtual void PresentInspectionTransform(const FTransform& RelativeTransform, USceneComponent* Parent);
	void ApplyInspectionPresentation();

private:
	UPROPERTY(ReplicatedUsing=OnRep_Mass) float AuthoritativeMassKg = 10.f;
	UFUNCTION() void OnRep_Mass();
	UFUNCTION() void OnRep_AttachmentState();
	UFUNCTION() void OnHolderDestroyed(AActor* DestroyedActor);
	UFUNCTION() void OnFocusOwnerDestroyed(AActor* DestroyedActor);
	void ClearFocusState();
	void ApplyFocusPresentation();
	FTransform BuildInspectionRelativeTransform(const USceneComponent* Parent,
		const FRotator& InspectionRotation) const;
	bool ApplyAttachmentState();
	void PublishInspectionRotation();
	void RetryAttachment();
	void ResolvePickupRequests();
	void Release(const FTransform& ReleaseTransform, const FVector& InitialVelocity, const FVector& ThrowImpulse);
	void PublishStateChange();
	void InitializeValueWidgets();
	void UpdateValueWidgets();
	void HideValueWidgets();
	void CaptureHeldPose();
	FTransform LimitOwnerDisplayPrediction(const FTransform& Predicted) const;
	void UpdateOwnerDisplayMesh(const FTransform* Display);
	UPROPERTY(Transient) TObjectPtr<UStaticMeshComponent> OwnerDisplayMesh;
	bool bRootVisibleBeforeOwnerDisplay=true;
	bool bOwnerDisplayActive=false;
	FTransform OwnerRelativeDisplay;
	TWeakObjectPtr<AECPPlayerBase> PresentedLocalHolder;
	uint32 OwnerDisplayRevision=0;
	bool bOwnerRelativeInitialized=false;
	struct FValueWidgetPresentation
	{
		TWeakObjectPtr<UWidgetComponent> Widget;
		FVector WorldOffset=FVector::ZeroVector;
		TWeakObjectPtr<UUserWidget> PresentedUserWidget;
		TWeakPtr<SWidget> PresentedSlateWidget;
		uint8 OriginalUserVisibility=0;
		uint8 OriginalSlateVisibility=0;
	};
	void SetValueWidgetVisibility(FValueWidgetPresentation& Presentation,bool bVisible);
	TArray<FValueWidgetPresentation> ValueWidgets;

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
	UPROPERTY(Transient) TObjectPtr<UStaticMeshComponent> FocusMesh;
	UPROPERTY(Transient) TObjectPtr<UMaterialInterface> OriginalOverlayMaterial;
	bool bOriginalForceDisableNanite = false;
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
	float PredictedInspectionDistance = 120.f;
	uint32 PredictedInspectionDistanceGeneration = 0;
	uint32 PredictedInspectionDistanceSequence = 0;
	bool bHasPredictedInspectionDistance = false;
	float OriginalNetUpdateFrequency = 100.f;
	double LastInspectionNetworkPublishTime = -1.0;
	bool bOriginalAlwaysRelevant = false;
	bool bOriginalOwnerRelevancy = false;
};
