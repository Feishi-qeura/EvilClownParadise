// Enhanced Input 由控制器集中绑定，角色负责校验状态和服务器权威玩法。
#pragma once

#include "Actors/ECPPickupTypes.h"
#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "ECPPlayerController.generated.h"

class ACharacter;
class AECPCharBase;
class AECPPlayerBase;
class AECPPickupItem;
class UInputAction;
class UInputMappingContext;
class UEnhancedInputLocalPlayerSubsystem;
struct FInputActionValue;

UCLASS()
class EVILCLOWNPARADISE_API AECPPlayerController : public APlayerController
{
	GENERATED_BODY()

public:
	// 排名只由服务器 GameMode 写入；客户端没有仲裁权，因此无需增加复制字段。
	void SetPickupJoinOrder(int32 InJoinOrder, bool bInListenServerHost);
	int32 GetPickupJoinOrder() const { return PickupJoinOrder; }
	bool IsPickupListenServerHost() const { return bPickupListenServerHost; }
	FECPPickupPriority GetPickupPriority(uint64 ServerFrame, uint32 ArrivalOrder) const;

	// 复制状态到达和 Pawn 切换后都可幂等刷新；公开入口也便于角色 RepNotify 主动通知。
	void RefreshPickupInputMode();
	bool IsPickupInputModeActive() const { return bPickupInputModeActive; }

	// 复用现有 IMC；资源由蓝图配置，不改变项目当前输入资源路径。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "ECP|Input")
	TObjectPtr<UInputMappingContext> InputMapping;

	// 沿用 X 前后、Y 左右的 Axis2D 约定。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "ECP|Input")
	TObjectPtr<UInputAction> MoveAction;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "ECP|Input")
	TObjectPtr<UInputAction> LookAction;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "ECP|Input")
	TObjectPtr<UInputAction> CrouchAction;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "ECP|Input")
	TObjectPtr<UInputAction> JumpAction;

	// Run 处理按下、松开、取消，避免失去焦点之后仍保持跑步。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "ECP|Input")
	TObjectPtr<UInputAction> RunAction;

	// Ragdoll 和 Pickup 仅处理 Started，一次按键只提出一次状态改变。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "ECP|Input")
	TObjectPtr<UInputAction> RagdollAction;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "ECP|Input")
	TObjectPtr<UInputAction> PickupAction;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "ECP|Input")
	int32 InputMappingPriority = 0;

	// 鼠标是每帧位移，不额外乘 DeltaSeconds；保留旧蓝图 0.2 倍率。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "ECP|Input")
	FVector2D LookSensitivity = FVector2D(0.2, 0.2);

	// 鼠标位移是每帧像素增量，不乘 DeltaSeconds；这是控制器基础值，物品蓝图还会叠加自己的速度倍率。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "ECP|Input|Pickup", meta = (ClampMin = "0.01"))
	float PickupRotationSensitivity = 0.25f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "ECP|Input|Pickup", meta = (ClampMin = "1.0", ClampMax = "60.0"))
	float PickupRotationSendRate = 20.f;

	// 高频打印会干扰网络/帧时间观察；调试时可临时开启，正常玩法默认关闭。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "ECP|Input|Debug")
	bool bPrintMovementInput = false;

protected:
	virtual void BeginPlay() override;
	virtual void SetupInputComponent() override;
	virtual void PlayerTick(float DeltaTime) override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	// 每次读取当前 Pawn，重生和切换角色后无需重建输入绑定。
	void Move(const FInputActionValue& Value);
	void Look(const FInputActionValue& Value);
	void ToggleCrouch();
	void StartJump();
	void StopJump();
	void StartRunning();
	void StopRunning();
	void ToggleRagdoll();
	void Pickup();
	void EquipPickup();
	void StorePickup();
	void PlacePickup();
	void DropPickup();
	void BeginRotatePickup();
	void EndRotatePickup();
	bool IsInspectingPickup() const;
	void RestorePickupInputMode();

	// 松开事件应清理原来按下时的角色，不能把旧按键误发给新 Pawn。
	TWeakObjectPtr<ACharacter> JumpingCharacter;
	TWeakObjectPtr<AECPCharBase> RunningCharacter;

	UPROPERTY(Transient)
	TObjectPtr<UInputMappingContext> InstalledInputMapping;
	TWeakObjectPtr<UEnhancedInputLocalPlayerSubsystem> InputSubsystem;
	int32 PickupJoinOrder = MAX_int32;
	bool bPickupListenServerHost = false;
	bool bPickupInputModeActive = false;
	bool bSavedCursorVisible = false;
	bool bRotatingPickup = false;
	double LastPickupRotationSendTime = -1.0;
	// 拖动绑定按下时的 Pawn/物品/代际；途中换 Pawn 或换物品只会取消，不能转发旧输入。
	TWeakObjectPtr<AECPPlayerBase> RotatingPickupPlayer;
	TWeakObjectPtr<AECPPickupItem> RotatingPickupItem;
	FECPInspectionRotationSample PickupRotationSample;
	uint32 NextPickupRotationSession = 0;
	uint32 LastSentPickupRotationSequence = 0;
	FRotator PickupRotationStart = FRotator::ZeroRotator;
	FRotator PickupRotationTotal = FRotator::ZeroRotator;
};
