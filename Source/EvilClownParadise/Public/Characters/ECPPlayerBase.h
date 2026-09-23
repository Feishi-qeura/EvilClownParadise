// 玩家逻辑从 BP_ECPlayertest 迁入这里；蓝图仅保留模型、相机等组件和资源设置。
#pragma once

#include "CoreMinimal.h"
#include "Actors/ECPPickupTypes.h"
#include "Characters/ECPCharBase.h"
#include "ECPPlayerBase.generated.h"

class AECPPickupItem;
class UCameraComponent;
class UPrimitiveComponent;
class USpringArmComponent;
class UUserWidget;

/** 玩家专属表现和拾取；布娃娃、移动锁定由 ECPCharBase 统一管理。 */
UCLASS()
class EVILCLOWNPARADISE_API AECPPlayerBase : public AECPCharBase
{
	GENERATED_BODY()

public:
	AECPPlayerBase();
	virtual void Tick(float DeltaSeconds) override;

	// SpringArm 通过这个入口读取视角；远端代理没有 Controller，需要从内建复制俯仰恢复视角。
	virtual FRotator GetViewRotation() const override;

	// F 只发起一次请求；服务器重新射线，绝不把半秒前的高亮缓存当成拾取依据。
	UFUNCTION(BlueprintCallable, Category = "ECP|Pickup")
	void RequestPickup();

	UFUNCTION(BlueprintPure, Category = "ECP|Pickup")
	AActor* GetHeldPickup() const;

	UFUNCTION(BlueprintPure, Category = "ECP|Pickup")
	bool IsInspectingPickup() const;

	UFUNCTION(BlueprintPure, Category = "ECP|Pickup")
	AECPPickupItem* GetInspectedPickup() const;

	UFUNCTION(BlueprintPure, Category = "ECP|Pickup")
	AECPPickupItem* GetEquippedPickup() const;

	UFUNCTION(BlueprintPure, Category = "ECP|Pickup")
	int32 GetStoredPickupCount() const { return StoredPickups.Num(); }

	// 这些入口只表达玩家意图；客户端不能指定目标、释放位置或投掷冲量。
	UFUNCTION(BlueprintCallable, Category = "ECP|Pickup")
	void RequestEquipPickup();

	UFUNCTION(BlueprintCallable, Category = "ECP|Pickup")
	void RequestStorePickup();

	UFUNCTION(BlueprintCallable, Category = "ECP|Pickup")
	void RequestPlacePickup();

	UFUNCTION(BlueprintCallable, Category = "ECP|Pickup")
	void RequestDropPickup();

	UFUNCTION(BlueprintCallable, Category = "ECP|Pickup")
	void RequestRotatePickup(AECPPickupItem* Item, const FECPInspectionRotationSample& Sample);
	void RequestFinalizePickupRotation(AECPPickupItem* Item, const FECPInspectionRotationSample& Sample);

	// 死亡、布娃娃和销毁都走同一幂等清理，确保库存不会随角色网络通道一起消失。
	UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category = "ECP|Pickup")
	void DropAllOwnedPickups();
	// 物品在下一服务器帧结算竞争时必须重新确认射线，不能沿用请求到达时的旧命中。
	bool CanServerPickUpItem(const AECPPickupItem* Item) const;
	void CompleteQueuedPickup(AECPPickupItem* Item);
	UCameraComponent* GetPickupInspectionComponent() const { return PlayerCamera.Get(); }

	// 物品只读取这些组件/插槽接口，不需要访问已删除的蓝图成员变量。
	USkeletalMeshComponent* GetPickupAttachmentComponent() const { return CarryMesh; }
	FName GetPickupAttachmentSocket() const { return PickupSocketName; }

	// 保留原小物品类型约束，可在派生类默认值换成另一种可拾取 Actor。
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
	UFUNCTION() void OnRep_InspectedPickup();
	UFUNCTION() void OnRep_EquippedPickup();

	void ResolveBlueprintComponents();
	void PublishViewPitch();
	void RefreshLocalPresentation();
	void UpdatePickupDebugTrace();
	void UpdatePickupFocus();
	void SetFocusedPickup(AECPPickupItem* NewItem);
	bool TracePickup(FHitResult& OutHit, bool bDrawTrace = false) const;
	void TryPickupOnServer();
	void SetInspectionMovementLocked(bool bLocked);
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

	// 缓存原 SCS 实例，不能创建同名原生组件，否则现有蓝图插槽与相机会连接到两套对象。
	UPROPERTY(Transient) TObjectPtr<UCameraComponent> PlayerCamera;
	UPROPERTY(Transient) TObjectPtr<USpringArmComponent> PlayerSpringArm;
	UPROPERTY(Transient) TObjectPtr<USkeletalMeshComponent> CarryMesh;
	UPROPERTY(Transient) TObjectPtr<UUserWidget> NetworkDebugWidget;
	// 只由服务器维护当前唯一瞄准对象；可见性和锁定归属复制在物品上，客户端不保存第二套锁。
	TWeakObjectPtr<AECPPickupItem> FocusedPickup;
	FVector StandingCarryMeshLocation = FVector::ZeroVector;
	FVector CarryBlendStart = FVector::ZeroVector;
	FVector CarryBlendTarget = FVector::ZeroVector;
	double CarryBlendStartedAt = 0.0;
	double LastPickupRequestAt = -1.0;
	bool bCarryLocationCached = false;
	bool bLocalPresentationReady = false;
	bool bInspectionMovementLocked = false;
	bool bWasRagdollActive = false;
	uint8 SavedInspectionMovementMode = 0;
	uint8 SavedInspectionCustomMovementMode = 0;
	FTimerHandle ViewPitchTimer;
	FTimerHandle PickupDebugTraceTimer;
	FTimerHandle PickupFocusTimer;
	FTimerHandle CarryMeshBlendTimer;
};
