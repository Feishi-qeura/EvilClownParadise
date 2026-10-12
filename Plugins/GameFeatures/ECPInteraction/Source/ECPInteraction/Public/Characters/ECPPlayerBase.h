// 玩家逻辑从 BP_ECPlayertest 迁入这里；蓝图仅保留模型、相机等组件和资源设置。
#pragma once

#include "CoreMinimal.h"
#include "ECPCore.h"
#include "Actors/ECPPickupTypes.h"
#include "Characters/ECPCharBase.h"
#include "ECPPlayerBase.generated.h"

class AECPPickupItem;
class UCameraComponent;
class UPrimitiveComponent;
class USpringArmComponent;
class UUserWidget;
class UECPPhysicalCarryComponent;
class UMaterialInterface;

/** 玩家专属表现和拾取；布娃娃、移动锁定由 ECPCharBase 统一管理。 */
UCLASS()
class ECPINTERACTION_API AECPPlayerBase : public AECPCharBase
{
	GENERATED_BODY()

public:
	AECPPlayerBase();
	UECPPhysicalCarryComponent* GetPhysicalCarryComponent() const { return PhysicalCarry.Get(); }
	void RecalculateCarriedWeight();
	void NotifyOwnedPickupChanged(AActor* Target);
	void HandlePhysicalCarryLost(AActor* Target);
	/** 读取本端最近同步的搬运队友。@return 队友角色或 nullptr；各端只读，不发送 RPC。 */
	UFUNCTION(BlueprintPure, Category="ECP|Carry") AECPCharBase* GetCarriedTeammate() const { return CarriedTeammate.Get(); }
	/** 查询当前视线命中的可搬运目标。@return 物品、布娃娃队友或 nullptr；各端本地射线，不提交拾取。 */
	UFUNCTION(BlueprintPure, Category="ECP|Carry") AActor* GetScannedCarryTarget() const;
	void ReleaseCarriedTeammate();
	/** 本地瞄准可搬运队友时使用的叠加描边；服务器不复制材质或本地焦点。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="ECP|Carry")
	TObjectPtr<UMaterialInterface> TeammateOutlineMaterial;

	/**
	 * @brief 通过公共角色契约提供玩家阵营，让怪物感知无需依赖交互模块。
	 * @return ECPTeam::Player，对所有玩家实例保持一致。
	 * @note 各端只读常量；不新增复制字段或改变网络协议。
	 */
	virtual uint8 GetCharacterTeamId() const override { return ECPTeam::Player; }
	virtual void Tick(float DeltaSeconds) override;

	// SpringArm 通过这个入口读取视角；远端代理没有 Controller，需要从内建复制俯仰恢复视角。
	virtual FRotator GetViewRotation() const override;

	// F 只发起一次请求；服务器重新射线，绝不把半秒前的高亮缓存当成拾取依据。
	/**
	 * @brief 请求拾取当前视线命中的物品，服务器重新射线并仲裁同帧竞争。
	 * @return 无返回值；结果通过物品和玩家槽位的复制状态回传。
	 * @note 仅本地受控玩家发起；客户端发送 RPC，服务器拥有最终裁决权。
	 */
	UFUNCTION(BlueprintCallable, Category = "ECP|Pickup")
	void RequestPickup();

	/**
	 * @brief 读取当前活跃手持物品，优先检查槽，再读取装备槽，忽略库存物品。
	 * @return 正在检查或装备的 Actor；两槽均无有效物品时返回 nullptr。
	 * @note 各端均可只读查询，客户端结果基于最近复制的槽位状态。
	 */
	UFUNCTION(BlueprintPure, Category = "ECP|Pickup")
	AActor* GetHeldPickup() const;

	/**
	 * @brief 判断玩家是否正在检查自己持有的有效物品，供输入与界面切换模式。
	 * @return 有效检查物品存在时返回 true，否则返回 false。
	 * @note 各端均可只读查询，服务器决定检查态。
	 */
	UFUNCTION(BlueprintPure, Category = "ECP|Pickup")
	bool IsInspectingPickup() const;

	/**
	 * @brief 读取归属当前玩家且处于 Pickup 状态的检查物品。
	 * @return 有效检查物品；槽位为空、归属变化或状态不符时返回 nullptr。
	 * @note 各端只读最近同步的状态，不改变物品占用。
	 */
	UFUNCTION(BlueprintPure, Category = "ECP|Pickup")
	AECPPickupItem* GetInspectedPickup() const;

	/**
	 * @brief 读取归属当前玩家且处于 Equipped 状态的装备物品。
	 * @return 有效装备物品；槽位为空、归属变化或状态不符时返回 nullptr。
	 * @note 各端只读最近同步的状态，装备切换由服务器处理。
	 */
	UFUNCTION(BlueprintPure, Category = "ECP|Pickup")
	AECPPickupItem* GetEquippedPickup() const;

	/**
	 * @brief 读取服务器确认的库存数量，同步给拥有该角色的客户端。
	 * @return 最近确认的库存数量。
	 * @note 库存 Actor 数组仍仅由服务器维护。
	 */
	UFUNCTION(BlueprintPure, Category = "ECP|Pickup")
	int32 GetStoredPickupCount() const { return StoredPickupCount; }

	UFUNCTION(BlueprintPure, Category = "ECP|Pickup")
	AECPPickupItem* GetScannedPickup() const;

	// 这些入口只表达玩家意图；客户端不能指定目标、释放位置或投掷冲量。
	/**
	 * @brief 请求把当前检查物品切换到装备槽，并在成功后解除检查移动锁。
	 * @return 无返回值；是否装备成功以服务器复制状态为准。
	 * @note 仅本地受控玩家发起，服务器校验死亡、布娃娃和物品归属后执行。
	 */
	UFUNCTION(BlueprintCallable, Category = "ECP|Pickup")
	void RequestEquipPickup();

	/**
	 * @brief 请求把当前检查或装备物品收进库存，检查槽具有优先级。
	 * @return 无返回值；库存仅在服务器确认 Stored 状态后更新。
	 * @note 仅本地受控玩家发起，服务器校验角色和物品状态。
	 */
	UFUNCTION(BlueprintCallable, Category = "ECP|Pickup")
	void RequestStorePickup();

	/**
	 * @brief 请求将正在检查的物品放回世界，释放位置由服务器角色视角计算。
	 * @return 无返回值；成功后由复制状态解除检查槽和移动锁。
	 * @note 仅本地受控玩家发起，服务器决定释放变换且不施加投掷冲量。
	 */
	UFUNCTION(BlueprintCallable, Category = "ECP|Pickup")
	void RequestPlacePickup();

	/**
	 * @brief 请求投掷当前装备物品，冲量由服务器按角色视角和力量生成。
	 * @return 无返回值；成功释放后通过复制状态清空装备槽。
	 * @note 仅本地受控玩家发起，客户端不能指定投掷位置、方向或冲量。
	 */
	UFUNCTION(BlueprintCallable, Category = "ECP|Pickup")
	void RequestDropPickup();

	/**
	 * @brief 提交检查物品的绝对旋转流样本，保持物品、代际和拖动会话绑定。
	 * @param Item 鼠标按下时捕获的检查物品。
	 * @param Sample 包含代际、会话、序号和绝对旋转的流样本。
	 * @return 无返回值；服务器接纳后复制权威旋转确认。
	 * @note 仅本地受控玩家发起；客户端走不可靠 RPC，服务器验证归属、代际和序号。
	 */
	UFUNCTION(BlueprintCallable, Category = "ECP|Pickup")
	void RequestRotatePickup(AECPPickupItem* Item, const FECPInspectionRotationSample& Sample);
	void RequestFinalizePickupRotation(AECPPickupItem* Item, const FECPInspectionRotationSample& Sample);
	void RequestSetInspectionDistance(AECPPickupItem* Item, uint32 Generation, uint32 Sequence, float Distance);

	// 死亡、布娃娃和销毁都走同一幂等清理，确保库存不会随角色网络通道一起消失。
	/**
	 * @brief 在死亡、布娃娃或退出时幂等释放检查、装备和库存中的所有物品。
	 * @return 无返回值；释放后清理拥有关系及检查移动锁。
	 * @note 仅服务器可执行，客户端调用不会取得库存修改权。
	 */
	UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category = "ECP|Pickup")
	void DropAllOwnedPickups();
	// 物品在下一服务器帧结算竞争时必须重新确认射线，不能沿用请求到达时的旧命中。
	bool CanServerPickUpItem(const AECPPickupItem* Item) const;
	void CompleteQueuedPickup(AECPPickupItem* Item);
	UCameraComponent* GetPickupInspectionComponent() const { return PlayerCamera.Get(); }

	// 物品只读取这些组件/插槽接口，不需要访问已删除的蓝图成员变量。
	USkeletalMeshComponent* GetPickupAttachmentComponent() const { return CarryMesh; }
	FName GetPickupAttachmentSocket() const { return PickupSocketName; }

	// 兼容旧蓝图的扫描类型；原生 AECPPickupItem 派生类统一支持，无需绑定某个小物品蓝图。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "ECP|Pickup")
	TSubclassOf<AActor> PickupActorClass;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "ECP|Pickup", meta = (ClampMin = "1.0"))
	float PickupDistance = 250.f;

	// 仅控制本地调试射线刷新，不参与服务器锁定或 Cube 可见性。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "ECP|Pickup", meta = (ClampMin = "0.05"))
	float PickupTraceInterval = 0.5f;

	// 服务器检查所有受控玩家的瞄准；只在锁定归属变化时复制状态，不发送逐次扫描 RPC。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "ECP|Pickup", meta = (ClampMin = "0.05", ClampMax = "1.0"))
	float PickupFocusScanInterval = 0.1f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "ECP|Pickup")
	FName PickupSocketName = TEXT("HitScanSocket2");

	// Chaos 的 AddImpulse 会自动按质量换算速度，因此固定冲量天然满足 KG 越大飞得越近。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "ECP|Pickup|Throw", meta = (ClampMin = "0.0"))
	float ThrowStrength = 150000.f;

	// 玩家力量是投掷基础冲量的倍率，可由角色蓝图/后续属性系统按角色配置。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "ECP|Pickup|Throw", meta = (ClampMin = "0.0"))
	float PlayerStrength = 1.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "ECP|Pickup|Inventory", meta = (ClampMin = "0.0"))
	float InventoryDropRadius = 120.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "ECP|Pickup|Inventory", meta = (ClampMin = "0.0"))
	float InventoryDropUpwardVelocity = 150.f;

	// 对应旧 NewVar；它移动额外 SkeletalMesh（及其子组件），保留原蹲下布局。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "ECP|Camera")
	FVector CrouchedCarryMeshLocation = FVector(-20.f, 20.f, 90.f);

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "ECP|Camera", meta = (ClampMin = "0.0"))
	float CarryMeshCrouchBlendDuration = 0.2f;

	// 网络调试 UI 仅由本地拥有该角色的 PlayerController 创建，避免双人 PIE 重复叠加。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "ECP|Debug")
	TSubclassOf<UUserWidget> NetworkDebugWidgetClass;

	/** 是否绘制本地角色的红色拾取调试射线；关闭绘制不会停止真实拾取检测。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ECP|Debug")
	bool bDrawPickupDebugTrace = false;

	// 由本地控制器管理 Tab 和鼠标模式，角色只管理控件实例。
	bool SetNetworkDebugWidgetVisible(bool bVisible);
	UUserWidget* GetNetworkDebugWidget() const { return NetworkDebugWidget.Get(); }

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void PossessedBy(AController* NewController) override;
	virtual void UnPossessed() override;
	virtual void PawnClientRestart() override;
	virtual void OnRep_Controller() override;
	virtual void OnStartCrouch(float HalfHeightAdjust, float ScaledHalfHeightAdjust) override;
	virtual void OnEndCrouch(float HalfHeightAdjust, float ScaledHalfHeightAdjust) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual void HandleDied_Implementation(AActor* Killer) override;

private:
	UPROPERTY(VisibleAnywhere, Category="ECP|Carry") TObjectPtr<UECPPhysicalCarryComponent> PhysicalCarry;
	UPROPERTY(Replicated) TObjectPtr<AECPCharBase> CarriedTeammate;
	struct FTeammateCarryRequest
	{
		TWeakObjectPtr<AECPPlayerBase> Player;
		uint64 Frame = 0;
		uint32 Arrival = 0;
	};
	TArray<FTeammateCarryRequest> PendingTeammateRequests;
	bool bTeammateResolveScheduled = false;
	uint32 NextTeammateArrival = 0;
	void QueueTeammateCarryRequest(AECPPlayerBase* Player);
	void ResolveTeammateCarryRequests();
	bool TraceCarryTarget(FHitResult& OutHit) const;
	bool TryDropLastStoredPickup();
	bool FindSafePickupRelease(AECPPickupItem* Item, FTransform& OutTransform) const;
	// RPC 不接收客户端目标、起点或方向，服务器只使用自己的角色组件重新做命中与遮挡检查。
	UFUNCTION(Server, Reliable)
	void ServerRequestPickup();
	UFUNCTION(Server, Reliable) void ServerRequestEquipPickup();
	UFUNCTION(Server, Reliable) void ServerRequestStorePickup();
	UFUNCTION(Server, Reliable) void ServerRequestPlacePickup();
	UFUNCTION(Server, Reliable) void ServerRequestDropPickup();
	// 流包是带物品、代际及序号的绝对目标；中间丢包不会阻塞或永久少转。
	UFUNCTION(Server, Unreliable) void ServerRequestRotatePickup(AECPPickupItem* Item, const FECPInspectionRotationSample& Sample);
	// 每次拖动都可靠封口，包括绕一圈回到零；后到的旧流包不能覆盖已确认的最终姿态。
	UFUNCTION(Server, Reliable) void ServerFinalizePickupRotation(AECPPickupItem* Item, const FECPInspectionRotationSample& Sample);
	UFUNCTION(Server, Reliable) void ServerRequestSetInspectionDistance(
		AECPPickupItem* Item, uint32 Generation, uint32 Sequence, float Distance);
	UFUNCTION() void OnRep_InspectedPickup();
	UFUNCTION() void OnRep_EquippedPickup();

	void ResolveBlueprintComponents();
	void PublishViewPitch();
	void RefreshLocalPresentation();
	void UpdatePickupDebugTrace();
	void UpdatePickupFocus();
	void UpdateTeammateFocus();
	void ClearTeammateFocus();
	void SetFocusedPickup(AECPPickupItem* NewItem);
	bool TracePickup(FHitResult& OutHit, bool bDrawTrace = false) const;
	void TryPickupOnServer();
	void BeginCarryMeshCrouchBlend(bool bCrouching);
	void UpdateCarryMeshCrouchBlend();
	void NotifyAnimationCrouch(bool bCrouching);
	FTransform BuildPickupReleaseTransform(float ForwardDistance = 100.f) const;

	// 活跃槽复制给所有端用于一致表现；库存内容只由服务器保存，客户端无权伪造或选择物品。
	UPROPERTY(ReplicatedUsing = OnRep_InspectedPickup)
	TObjectPtr<AECPPickupItem> InspectedPickup;
	UPROPERTY(ReplicatedUsing = OnRep_EquippedPickup)
	TObjectPtr<AECPPickupItem> EquippedPickup;
	UPROPERTY(Transient)
	TArray<TObjectPtr<AECPPickupItem>> StoredPickups;
	UPROPERTY(Replicated) int32 StoredPickupCount = 0;

	// 缓存原 SCS 实例，不能创建同名原生组件，否则现有蓝图插槽与相机会连接到两套对象。
	UPROPERTY(Transient) TObjectPtr<UCameraComponent> PlayerCamera;
	UPROPERTY(Transient) TObjectPtr<USpringArmComponent> PlayerSpringArm;
	UPROPERTY(Transient) TObjectPtr<USkeletalMeshComponent> CarryMesh;
	UPROPERTY(Transient) TObjectPtr<UUserWidget> NetworkDebugWidget;
	UPROPERTY(Transient) TObjectPtr<UUserWidget> PickupStatusWidget;
	// 只由服务器维护当前唯一瞄准对象；可见性和锁定归属复制在物品上，客户端不保存第二套锁。
	TWeakObjectPtr<AECPPickupItem> FocusedPickup;
	TWeakObjectPtr<USkeletalMeshComponent> FocusedTeammateMesh;
	UPROPERTY(Transient) TObjectPtr<UMaterialInterface> OriginalTeammateOverlay;
	FVector StandingCarryMeshLocation = FVector::ZeroVector;
	FVector CarryBlendStart = FVector::ZeroVector;
	FVector CarryBlendTarget = FVector::ZeroVector;
	double CarryBlendStartedAt = 0.0;
	double LastPickupRequestAt = -1.0;
	bool bCarryLocationCached = false;
	bool bLocalPresentationReady = false;
	bool bWasRagdollActive = false;
	FTimerHandle ViewPitchTimer;
	FTimerHandle PickupDebugTraceTimer;
	FTimerHandle PickupFocusTimer;
	FTimerHandle TeammateFocusTimer;
	FTimerHandle CarryMeshBlendTimer;
};
