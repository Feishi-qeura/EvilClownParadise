// Fill out your copyright notice in the Description page of Project Settings.

#include "Framework/ECPPlayerController.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "Engine/LocalPlayer.h"
#include "GameFramework/Character.h"
#include "InputAction.h"
#include "InputActionValue.h"
#include "InputMappingContext.h"
#include "Camera/ECPCameraModifier.h"
#include "Characters/ECPPlayerBase.h"

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

	if (PlayerCameraManager)
	{
		PlayerCameraManager->AddNewCameraModifier(UECPCameraModifier::StaticClass());
	}

	ULocalPlayer* LocalPlayer = GetLocalPlayer();
	// 资源未配置时明确报告，方便在蓝图类默认值中补齐。
	if (!LocalPlayer || !InputMapping)
	{
		UE_LOG(LogECPPlayerInput, Warning, TEXT("%s: LocalPlayer or InputMapping is missing."), *GetName());
		return;
	}

	UEnhancedInputLocalPlayerSubsystem* Subsystem =
	    ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(LocalPlayer);
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

// 逐项绑定输入动作，直接展示资源、触发事件和回调函数的对应关系。
// 每项都带"没配 / 类型不对"的告警：留空是允许的，但静默失效排查起来很费时间。
void AECPPlayerController::SetupInputComponent()
{
	Super::SetupInputComponent();

	UEnhancedInputComponent* EnhancedInput = Cast<UEnhancedInputComponent>(InputComponent);
	// 普通 InputComponent 无法绑定增强输入，需要检查项目默认输入组件。
	if (!EnhancedInput)
	{
		UE_LOG(LogECPPlayerInput, Error, TEXT("%s: Default Input Component Class must be EnhancedInputComponent."),
		       *GetName());
		return;
	}

	// 单独验证移动资源，避免错误的值类型进入 FVector2D 读取。
	if (MoveAction && MoveAction->ValueType == EInputActionValueType::Axis2D)
	{
		EnhancedInput->BindAction(MoveAction.Get(), ETriggerEvent::Triggered, this, &AECPPlayerController::Move);
		// 全部松开的帧只会来 Completed，用它把 MoveInput 归零（见 StopMove 的注释）。
		EnhancedInput->BindAction(MoveAction.Get(), ETriggerEvent::Completed, this, &AECPPlayerController::StopMove);
		EnhancedInput->BindAction(MoveAction.Get(), ETriggerEvent::Canceled, this, &AECPPlayerController::StopMove);
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

	// 按住蹲：Started 进蹲、松开或取消时站起（不是切换式）。
	if (CrouchAction)
	{
		EnhancedInput->BindAction(CrouchAction.Get(), ETriggerEvent::Started, this, &AECPPlayerController::StartCrouch);
		EnhancedInput->BindAction(CrouchAction.Get(), ETriggerEvent::Completed, this,
		                          &AECPPlayerController::StopCrouch);
		EnhancedInput->BindAction(CrouchAction.Get(), ETriggerEvent::Canceled, this, &AECPPlayerController::StopCrouch);
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
	else
	{
		UE_LOG(LogECPPlayerInput, Warning, TEXT("%s: JumpAction is missing."), *GetName());
	}

	if (SprintAction)
	{
		EnhancedInput->BindAction(SprintAction.Get(), ETriggerEvent::Started, this, &AECPPlayerController::StartSprint);
		EnhancedInput->BindAction(SprintAction.Get(), ETriggerEvent::Completed, this,
		                          &AECPPlayerController::StopSprint);
		EnhancedInput->BindAction(SprintAction.Get(), ETriggerEvent::Canceled, this, &AECPPlayerController::StopSprint);
	}
	else
	{
		UE_LOG(LogECPPlayerInput, Warning, TEXT("%s: SprintAction is missing."), *GetName());
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

	if (AECPPlayerBase* ECPPlayer = Cast<AECPPlayerBase>(ControlledPawn))
	{
		// 夹到长度 1：UE 的 2D 轴在斜向（W+D）会给 (1,1)，长度 1.41；
		// 而 Godot 的 Input.get_vector 是归一化的（给 0.707）。
		// 不夹的话侧移倾斜在斜向会大 41%。（移动本身不受影响，CMC 会把加速度钳到 1）
		ECPPlayer->MoveInput = Axis.GetClampedToMaxSize(1.0f);
	}
}

// Enhanced Input 在"所有键都松开"的那一帧只会发 Completed，不会补发一个值为 0 的
// Triggered（零值会让触发器状态直接变成 None）。所以必须在这里手动归零 ——
// 否则 MoveInput 会一直停在最后一次的值：状态机永远出不了 Idle，
// 落地锁定的"有移动输入就取消"也会被幻影输入触发。
void AECPPlayerController::StopMove()
{
	if (AECPPlayerBase* ECPPlayer = GetPawn<AECPPlayerBase>())
	{
		ECPPlayer->MoveInput = FVector2D::ZeroVector;
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

void AECPPlayerController::StartCrouch()
{
	if (AECPPlayerBase* ECPPlayer = GetPawn<AECPPlayerBase>())
	{
		ECPPlayer->bWantsToCrouch = true;
	}
}

void AECPPlayerController::StopCrouch()
{
	if (AECPPlayerBase* ECPPlayer = GetPawn<AECPPlayerBase>())
	{
		ECPPlayer->bWantsToCrouch = false;
	}
}

void AECPPlayerController::StartSprint()
{
	if (AECPPlayerBase* ECPPlayer = GetPawn<AECPPlayerBase>())
	{
		ECPPlayer->bWantsToSprint = true;
	}
}

void AECPPlayerController::StopSprint()
{
	if (AECPPlayerBase* ECPPlayer = GetPawn<AECPPlayerBase>())
	{
		ECPPlayer->bWantsToSprint = false;
	}
}

// Character负责检查能否跳跃；控制器只提出跳跃请求。
void AECPPlayerController::StartJump()
{
	AECPPlayerBase* ECPPlayer = GetPawn<AECPPlayerBase>();
	// 没有角色或正在旁观时不能请求 Character 跳跃。
	if (!IsValid(ECPPlayer))
	{
		return;
	}

	// 清理上一次记录的跳跃请求，避免换角色后原角色仍保持按住状态。
	JumpingCharacter.Reset();

	JumpingCharacter = ECPPlayer;
	ECPPlayer->RequestJump();
}

// 松开或取消动作都清理按住状态，允许 Character 结束可变高度跳跃。
void AECPPlayerController::StopJump()
{
	// 使用开始跳跃时记录的角色：换 Pawn 后松开按键，清理的是原角色，不会误伤新 Pawn。
	if (AECPPlayerBase* ECPPlayer = Cast<AECPPlayerBase>(JumpingCharacter.Get()))
	{
		ECPPlayer->ReleaseJump();
	}

	JumpingCharacter.Reset();
}

// 控制器退出时释放本类安装的映射，防止返回菜单后残留角色输入。
void AECPPlayerController::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// 退出时不一定再收到输入结束事件，因此主动清理跳跃请求。
	StopJump();

	// 本地玩家可能已先销毁，只有弱引用仍有效时才操作子系统。
	if (UEnhancedInputLocalPlayerSubsystem* Subsystem = InputSubsystem.Get())
	{
		// 仅移除本类记录的映射，不影响其他功能的输入上下文。
		if (InstalledInputMapping)
		{
			Subsystem->RemoveMappingContext(InstalledInputMapping.Get());
		}
	}

	InstalledInputMapping = nullptr;
	InputSubsystem.Reset();
	Super::EndPlay(EndPlayReason);
}
