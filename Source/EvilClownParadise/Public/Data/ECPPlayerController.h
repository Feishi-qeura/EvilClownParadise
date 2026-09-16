// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "ECPPlayerController.generated.h"

class ACharacter;
class UInputAction;
class UInputMappingContext;
class UEnhancedInputLocalPlayerSubsystem;
struct FInputActionValue;

// 每个输入动作使用独立属性；蓝图配置资源，C++ 逐项绑定处理函数。
UCLASS()
class EVILCLOWNPARADISE_API AECPPlayerController : public APlayerController
{
	GENERATED_BODY()

public:
	// 复用现有 IMC，允许蓝图更换映射而无需改动 C++ 路径。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "ECP|Input")
	TObjectPtr<UInputMappingContext> InputMapping;

	// 移动使用 Axis2D，沿用 X 前后、Y 左右的现有轴约定。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "ECP|Input")
	TObjectPtr<UInputAction> MoveAction;

	// 视角使用 Axis2D，分别读取鼠标水平和垂直位移。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "ECP|Input")
	TObjectPtr<UInputAction> LookAction;

	// 下蹲在开始按下时切换一次，按住不重复切换。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "ECP|Input")
	TObjectPtr<UInputAction> CrouchAction;

	// 跳跃开始时提出请求，松开或取消时停止持续跳跃；未配置时不启用。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "ECP|Input")
	TObjectPtr<UInputAction> JumpAction;

	// 默认与原 AddMappingContext 节点一致，数值越大优先级越高。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "ECP|Input")
	int32 InputMappingPriority = 0;

	// 沿用蓝图的 0.2 倍率；负值可反转对应视角轴。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "ECP|Input")
	FVector2D LookSensitivity = FVector2D(0.2, 0.2);

	// 保留原移动轴打印，并允许通过蓝图关闭调试输出。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "ECP|Input|Debug")
	bool bPrintMovementInput = true;

protected:
	// 为当前本地玩家启用映射，不依赖蓝图缓存 Pawn 的时机。
	virtual void BeginPlay() override;

	// 控制器的输入绑定入口，保留父类初始化。
	virtual void SetupInputComponent() override;

	// 退出时只移除本类安装的映射，保留 UI 等其他上下文。
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	// 回调始终获取当前 Pawn，避免重生后继续访问旧角色。
	void Move(const FInputActionValue& Value);
	void Look(const FInputActionValue& Value);
	void ToggleCrouch();

	// 跳跃开始与结束分开处理，支持按住跳得更高的 Character 配置。
	void StartJump();
	void StopJump();

	// 记录开始跳跃的角色；切换 Pawn 后松开按键时仍清理原角色的跳跃请求。
	TWeakObjectPtr<ACharacter> JumpingCharacter;

	// 记录本类实际安装的资源，避免清理其他系统的输入映射。
	UPROPERTY(Transient)
	TObjectPtr<UInputMappingContext> InstalledInputMapping;

	// 本地玩家先销毁时弱引用自动失效，清理阶段不访问悬空子系统。
	TWeakObjectPtr<UEnhancedInputLocalPlayerSubsystem> InputSubsystem;
};
