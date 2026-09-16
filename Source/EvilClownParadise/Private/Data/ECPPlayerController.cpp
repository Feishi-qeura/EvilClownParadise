// Fill out your copyright notice in the Description page of Project Settings.

#include "Data/ECPPlayerController.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "Engine/LocalPlayer.h"
#include "GameFramework/Character.h"
#include "InputAction.h"
#include "InputActionValue.h"
#include "InputMappingContext.h"
#include "Kismet/KismetSystemLibrary.h"

// 单独的日志分类便于定位资源配置和输入组件错误。
DEFINE_LOG_CATEGORY_STATIC(LogECPPlayerInput, Log, All);

// IMC 属于本地玩家；服务器上的远程控制器不安装本机输入。
void AECPPlayerController::BeginPlay()
{
	Super::BeginPlay();

	// 避免把远程玩家的映射装到玩家 0，使用当前控制器的本地玩家。
	if (!IsLocalController())
	{
		return;
	}

	ULocalPlayer* LocalPlayer = GetLocalPlayer();
	// 资源未配置时明确报告，方便在蓝图类默认值中补齐。
	if (!LocalPlayer || !InputMapping)
	{
		UE_LOG(LogECPPlayerInput, Warning, TEXT("%s: LocalPlayer or InputMapping is missing."), *GetName());
		return;
	}

	UEnhancedInputLocalPlayerSubsystem* Subsystem = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(LocalPlayer);
	// 非游戏环境下子系统可能不可用，不能解引用空对象。
	if (!Subsystem)
	{
		UE_LOG(LogECPPlayerInput, Warning, TEXT("%s: Enhanced Input subsystem is unavailable."), *GetName());
		return;
	}

	// 不接管其他系统已经注册的同一映射，也不清空其他输入上下文。
	if (!Subsystem->HasMappingContext(InputMapping.Get()))
	{
		FModifyContextOptions Options;
		Options.bIgnoreAllPressedKeysUntilRelease = true;
		Subsystem->AddMappingContext(InputMapping.Get(), InputMappingPriority, Options);
		InstalledInputMapping = InputMapping;
		InputSubsystem = Subsystem;
	}
}

// 逐项绑定四个输入动作，直接展示资源、触发事件和回调函数的对应关系。
void AECPPlayerController::SetupInputComponent()
{
	Super::SetupInputComponent();

	UEnhancedInputComponent* EnhancedInput = Cast<UEnhancedInputComponent>(InputComponent);
	// 普通 InputComponent 无法绑定增强输入，需要检查项目默认输入组件。
	if (!EnhancedInput)
	{
		UE_LOG(LogECPPlayerInput, Error, TEXT("%s: Default Input Component Class must be EnhancedInputComponent."), *GetName());
		return;
	}

	// 单独验证移动资源，避免错误的值类型进入 FVector2D 读取。
	if (MoveAction && MoveAction->ValueType == EInputActionValueType::Axis2D)
	{
		EnhancedInput->BindAction(MoveAction.Get(), ETriggerEvent::Triggered, this, &AECPPlayerController::Move);
	}
	else
	{
		UE_LOG(LogECPPlayerInput, Warning, TEXT("%s: MoveAction must reference an Axis2D action."), *GetName());
	}

	// 视角保持持续触发，并使用蓝图配置的二维鼠标动作。
	if (LookAction && LookAction->ValueType == EInputActionValueType::Axis2D)
	{
		EnhancedInput->BindAction(LookAction.Get(), ETriggerEvent::Triggered, this, &AECPPlayerController::Look);
	}
	else
	{
		UE_LOG(LogECPPlayerInput, Warning, TEXT("%s: LookAction must reference an Axis2D action."), *GetName());
	}

	// 切换下蹲只处理 Started，避免按住时每帧蹲起。
	if (CrouchAction)
	{
		EnhancedInput->BindAction(CrouchAction.Get(), ETriggerEvent::Started, this, &AECPPlayerController::ToggleCrouch);
	}
	else
	{
		UE_LOG(LogECPPlayerInput, Warning, TEXT("%s: CrouchAction is missing."), *GetName());
	}

	// 跳跃允许暂不配置；配置后同时处理按下、松开和取消，避免残留按住状态。
	if (JumpAction)
	{
		EnhancedInput->BindAction(JumpAction.Get(), ETriggerEvent::Started, this, &AECPPlayerController::StartJump);
		EnhancedInput->BindAction(JumpAction.Get(), ETriggerEvent::Completed, this, &AECPPlayerController::StopJump);
		EnhancedInput->BindAction(JumpAction.Get(), ETriggerEvent::Canceled, this, &AECPPlayerController::StopJump);
	}
}

// 按 Pawn 自身的前方和右方移动，保留原蓝图 X 前后、Y 左右的轴约定。
void AECPPlayerController::Move(const FInputActionValue& Value)
{
	APawn* ControlledPawn = GetPawn();
	// 控制器可能在角色生成前或销毁后收到输入，此时安全忽略。
	if (!IsValid(ControlledPawn))
	{
		return;
	}

	const FVector2D Axis = Value.Get<FVector2D>();
	ControlledPawn->AddMovementInput(ControlledPawn->GetActorForwardVector(), Axis.X);
	ControlledPawn->AddMovementInput(ControlledPawn->GetActorRightVector(), Axis.Y);

	// 对应原 PrintString 节点，继续打印屏幕和日志以便比较迁移前后的轴值。
	if (bPrintMovementInput)
	{
		UKismetSystemLibrary::PrintString(this,FString::Printf(TEXT("X: %s          Y: %s"),*FString::SanitizeFloat(Axis.X), *FString::SanitizeFloat(Axis.Y)),true, true, FLinearColor(0.0f, 0.66f, 1.0f), 2.0f);
	}
}

// 鼠标输入是帧位移，沿用原倍率，不额外乘帧时长。
void AECPPlayerController::Look(const FInputActionValue& Value)
{
	APawn* ControlledPawn = GetPawn();
	// 每次读取当前 Pawn，避免重生或换角色后视角失效。
	if (!IsValid(ControlledPawn))
	{
		return;
	}

	const FVector2D Axis = Value.Get<FVector2D>();
	ControlledPawn->AddControllerYawInput(static_cast<float>(Axis.X * LookSensitivity.X));
	ControlledPawn->AddControllerPitchInput(static_cast<float>(Axis.Y * LookSensitivity.Y));
}

// 请求 Character 下蹲，角色原有 OnStart/OnEndCrouch 继续通过接口驱动动画。
void AECPPlayerController::ToggleCrouch()
{
	ACharacter* ControlledCharacter = GetCharacter();
	// 旁观或控制非 Character 的 Pawn 时不执行角色专属操作。
	if (!IsValid(ControlledCharacter))
	{
		return;
	}

	// 使用实际下蹲状态，保持原分支；能否站起仍由 CharacterMovement 检测。
	if (ControlledCharacter->bIsCrouched)
	{
		ControlledCharacter->UnCrouch();
	}
	else
	{
		ControlledCharacter->Crouch();
	}
}

//Character负责检查能否跳跃；控制器只提出跳跃请求。
void AECPPlayerController::StartJump()
{
	ACharacter* ControlledCharacter = GetCharacter();
	//没有角色或正在旁观时不能请求 Character 跳跃。
	if (!IsValid(ControlledCharacter))
	{
		return;
	}

	//清理上一次记录的跳跃请求，避免换角色后原角色仍保持按住状态。
	StopJump();
	JumpingCharacter = ControlledCharacter;
	ControlledCharacter->Jump();
}

//松开或取消动作都清理按住状态，允许 Character 结束可变高度跳跃。
void AECPPlayerController::StopJump()
{
	//使用开始跳跃时记录的角色，不把旧按键的松开事件发送给新 Pawn。
	if (ACharacter* JumpingPawn = JumpingCharacter.Get())
	{
		JumpingPawn->StopJumping();
	}

	JumpingCharacter.Reset();
}

//控制器退出时释放本类安装的映射，防止返回菜单后残留角色输入。
void AECPPlayerController::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	//退出时不一定再收到输入结束事件，因此主动清理跳跃请求。
	StopJump();

	//本地玩家可能已先销毁，只有弱引用仍有效时才操作子系统。
	if (UEnhancedInputLocalPlayerSubsystem* Subsystem = InputSubsystem.Get())
	{
		//仅移除本类记录的映射，不影响其他功能的输入上下文。
		if (InstalledInputMapping)
		{
			Subsystem->RemoveMappingContext(InstalledInputMapping.Get());
		}
	}

	InstalledInputMapping = nullptr;
	InputSubsystem.Reset();
	Super::EndPlay(EndPlayReason);
}
