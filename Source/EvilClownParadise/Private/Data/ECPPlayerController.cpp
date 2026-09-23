#include "Data/ECPPlayerController.h"
#include "Engine/World.h"

#include "Actors/ECPPickupItem.h"
#include "Characters/ECPCharBase.h"
#include "Characters/ECPPlayerBase.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "Engine/LocalPlayer.h"
#include "GameFramework/Character.h"
#include "GameFramework/PlayerState.h"
#include "InputAction.h"
#include "InputActionValue.h"
#include "InputCoreTypes.h"
#include "InputMappingContext.h"
#include "Kismet/KismetSystemLibrary.h"

DEFINE_LOG_CATEGORY_STATIC(LogECPPlayerInput, Log, All);

void AECPPlayerController::SetPickupJoinOrder(int32 InJoinOrder, bool bInListenServerHost)
{
	// 只有服务器生成排名；防止客户端本地改值被误用为权威竞争结果。
	if (!HasAuthority())
	{
		return;
	}
	PickupJoinOrder = FMath::Max(0, InJoinOrder);
	bPickupListenServerHost = bInListenServerHost;
}

FECPPickupPriority AECPPlayerController::GetPickupPriority(uint64 ServerFrame, uint32 ArrivalOrder) const
{
	// Ping 取服务器持有的 PlayerState 数据，客户端不能把自报的低延迟塞进拾取 RPC。
	const APlayerState* ECPPlayerState = GetPlayerState<APlayerState>();
	const float MeasuredPing = ECPPlayerState ? ECPPlayerState->GetPingInMilliseconds() : MAX_flt;
	const float SafePing = FMath::IsFinite(MeasuredPing) && MeasuredPing >= 0.f ? MeasuredPing : MAX_flt;
	return {ServerFrame, bPickupListenServerHost, SafePing, PickupJoinOrder, ArrivalOrder};
}

void AECPPlayerController::BeginPlay()
{
	Super::BeginPlay();
	// IMC 属于具体的本地玩家，服务器上的远程控制器不能安装到玩家 0。
	if (!IsLocalController())
	{
		return;
	}
	ULocalPlayer* LocalPlayer = GetLocalPlayer();
	if (!LocalPlayer || !InputMapping)
	{
		UE_LOG(LogECPPlayerInput, Warning, TEXT("%s: LocalPlayer or InputMapping is missing."), *GetName());
		return;
	}
	UEnhancedInputLocalPlayerSubsystem* Subsystem = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(LocalPlayer);
	if (!Subsystem)
	{
		UE_LOG(LogECPPlayerInput, Warning, TEXT("%s: Enhanced Input subsystem is unavailable."), *GetName());
		return;
	}
	// 不清空 UI 等其他上下文，也不接管另一个系统已经注册的同一映射。
	if (!Subsystem->HasMappingContext(InputMapping.Get()))
	{
		FModifyContextOptions Options;
		Options.bIgnoreAllPressedKeysUntilRelease = true;
		Subsystem->AddMappingContext(InputMapping.Get(), InputMappingPriority, Options);
		InstalledInputMapping = InputMapping;
		InputSubsystem = Subsystem;
	}
}

void AECPPlayerController::SetupInputComponent()
{
	Super::SetupInputComponent();
	UEnhancedInputComponent* EnhancedInput = Cast<UEnhancedInputComponent>(InputComponent);
	if (!EnhancedInput)
	{
		UE_LOG(LogECPPlayerInput, Error, TEXT("%s: Default Input Component Class must be EnhancedInputComponent."), *GetName());
		return;
	}
	// Axis2D 先校验类型，避免错误资源进入 FVector2D 读取。
	if (MoveAction && MoveAction->ValueType == EInputActionValueType::Axis2D)
	{
		EnhancedInput->BindAction(MoveAction.Get(), ETriggerEvent::Triggered, this, &ThisClass::Move);
	}
	else
	{
		UE_LOG(LogECPPlayerInput, Warning, TEXT("%s: MoveAction must reference an Axis2D action."), *GetName());
	}
	if (LookAction && LookAction->ValueType == EInputActionValueType::Axis2D)
	{
		EnhancedInput->BindAction(LookAction.Get(), ETriggerEvent::Triggered, this, &ThisClass::Look);
	}
	else
	{
		UE_LOG(LogECPPlayerInput, Warning, TEXT("%s: LookAction must reference an Axis2D action."), *GetName());
	}
	if (CrouchAction)
	{
		EnhancedInput->BindAction(CrouchAction.Get(), ETriggerEvent::Started, this, &ThisClass::ToggleCrouch);
	}
	else
	{
		UE_LOG(LogECPPlayerInput, Warning, TEXT("%s: CrouchAction is missing."), *GetName());
	}
	// 松开与取消都清理跳跃，避免按住 Space 落地后残留一次新的跳跃。
	if (JumpAction)
	{
		EnhancedInput->BindAction(JumpAction.Get(), ETriggerEvent::Started, this, &ThisClass::StartJump);
		EnhancedInput->BindAction(JumpAction.Get(), ETriggerEvent::Completed, this, &ThisClass::StopJump);
		EnhancedInput->BindAction(JumpAction.Get(), ETriggerEvent::Canceled, this, &ThisClass::StopJump);
	}
	if (RunAction)
	{
		EnhancedInput->BindAction(RunAction.Get(), ETriggerEvent::Started, this, &ThisClass::StartRunning);
		EnhancedInput->BindAction(RunAction.Get(), ETriggerEvent::Completed, this, &ThisClass::StopRunning);
		EnhancedInput->BindAction(RunAction.Get(), ETriggerEvent::Canceled, this, &ThisClass::StopRunning);
	}
	if (RagdollAction)
	{
		EnhancedInput->BindAction(RagdollAction.Get(), ETriggerEvent::Started, this, &ThisClass::ToggleRagdoll);
	}
	if (PickupAction)
	{
		EnhancedInput->BindAction(PickupAction.Get(), ETriggerEvent::Started, this, &ThisClass::Pickup);
	}

	// 固定玩法键直接绑定，不要求修改已有 IMC/IA 资产，也避免测试关卡漏装新 Mapping Context。
	InputComponent->BindKey(EKeys::Two, IE_Pressed, this, &ThisClass::EquipPickup);
	InputComponent->BindKey(EKeys::G, IE_Pressed, this, &ThisClass::DropPickup);
	InputComponent->BindKey(EKeys::V, IE_Pressed, this, &ThisClass::StorePickup);
	InputComponent->BindKey(EKeys::LeftMouseButton, IE_Pressed, this, &ThisClass::BeginRotatePickup);
	InputComponent->BindKey(EKeys::LeftMouseButton, IE_Released, this, &ThisClass::EndRotatePickup);
	InputComponent->BindKey(EKeys::RightMouseButton, IE_Pressed, this, &ThisClass::PlacePickup);
}

void AECPPlayerController::Move(const FInputActionValue& Value)
{
	APawn* ControlledPawn = GetPawn();
	if (!IsValid(ControlledPawn))
	{
		return;
	}
	if (IsInspectingPickup())
	{
		return;
	}
	// 布娃娃和 Landing 都拒绝输入积累；服务器上的角色移动锁也会独立校验。
	if (const AECPCharBase* InputCharacter = Cast<AECPCharBase>(ControlledPawn))
	{
		if (InputCharacter->IsMovementLocked() || InputCharacter->IsDead())
		{
			return;
		}
	}
	const FVector2D Axis = Value.Get<FVector2D>();
	ControlledPawn->AddMovementInput(ControlledPawn->GetActorForwardVector(), Axis.X);
	ControlledPawn->AddMovementInput(ControlledPawn->GetActorRightVector(), Axis.Y);
	if (bPrintMovementInput)
	{
		UKismetSystemLibrary::PrintString(this, FString::Printf(TEXT("X: %s          Y: %s"),
			*FString::SanitizeFloat(Axis.X), *FString::SanitizeFloat(Axis.Y)), true, true,
			FLinearColor(0.f, 0.66f, 1.f), 2.f);
	}
}

void AECPPlayerController::Look(const FInputActionValue& Value)
{
	APawn* ControlledPawn = GetPawn();
	if (!IsValid(ControlledPawn))
	{
		return;
	}
	// 检查态把鼠标拖动专用于物品旋转，不能同时改变角色视角。
	if (IsInspectingPickup())
	{
		return;
	}
	// 移动锁不锁死观察，布娃娃时仍可转动视角查看物理表现。
	const FVector2D Axis = Value.Get<FVector2D>();
	ControlledPawn->AddControllerYawInput(static_cast<float>(Axis.X * LookSensitivity.X));
	ControlledPawn->AddControllerPitchInput(static_cast<float>(Axis.Y * LookSensitivity.Y));
}

void AECPPlayerController::ToggleCrouch()
{
	if (IsInspectingPickup())
	{
		return;
	}
	ACharacter* InputCharacter = GetCharacter();
	if (!IsValid(InputCharacter))
	{
		return;
	}
	if (const AECPCharBase* ECPCharacter = Cast<AECPCharBase>(InputCharacter))
	{
		// 不能用下蹲改变布娃娃/落地恢复期间的胶囊体尺寸。
		if (ECPCharacter->IsMovementLocked() || ECPCharacter->IsDead())
		{
			return;
		}
	}
	if (InputCharacter->bIsCrouched)
	{
		InputCharacter->UnCrouch();
	}
	else
	{
		InputCharacter->Crouch();
	}
}

void AECPPlayerController::StartJump()
{
	if (IsInspectingPickup())
	{
		return;
	}
	ACharacter* InputCharacter = GetCharacter();
	if (!IsValid(InputCharacter))
	{
		return;
	}
	if (const AECPCharBase* ECPCharacter = Cast<AECPCharBase>(InputCharacter))
	{
		if (ECPCharacter->IsMovementLocked() || ECPCharacter->IsDead())
		{
			return;
		}
	}
	StopJump();
	JumpingCharacter = InputCharacter;
	InputCharacter->Jump();
}

void AECPPlayerController::StopJump()
{
	// 即使换 Pawn 也清理原角色，弱引用则安全处理已经销毁的原角色。
	if (ACharacter* InputCharacter = JumpingCharacter.Get())
	{
		InputCharacter->StopJumping();
	}
	JumpingCharacter.Reset();
}

void AECPPlayerController::StartRunning()
{
	if (IsInspectingPickup())
	{
		return;
	}
	AECPCharBase* InputCharacter = Cast<AECPCharBase>(GetPawn());
	if (!IsValid(InputCharacter) || InputCharacter->IsMovementLocked() || InputCharacter->IsDead())
	{
		return;
	}
	StopRunning();
	RunningCharacter = InputCharacter;
	InputCharacter->SetRunning(true);
}

void AECPPlayerController::StopRunning()
{
	if (AECPCharBase* InputCharacter = RunningCharacter.Get())
	{
		InputCharacter->SetRunning(false);
	}
	RunningCharacter.Reset();
}

void AECPPlayerController::ToggleRagdoll()
{
	if (IsInspectingPickup())
	{
		return;
	}
	if (AECPCharBase* InputCharacter = Cast<AECPCharBase>(GetPawn()))
	{
		// 这里不能用移动锁挡掉 X，否则进入布娃娃后再也无法提出起身请求。
		InputCharacter->RequestRagdollToggle();
	}
}

void AECPPlayerController::Pickup()
{
	if (IsInspectingPickup())
	{
		return;
	}
	if (AECPPlayerBase* InputCharacter = Cast<AECPPlayerBase>(GetPawn()))
	{
		InputCharacter->RequestPickup();
	}
}

void AECPPlayerController::EquipPickup()
{
	if (AECPPlayerBase* InputPlayer = Cast<AECPPlayerBase>(GetPawn()))
	{
		InputPlayer->RequestEquipPickup();
	}
}

void AECPPlayerController::StorePickup()
{
	if (AECPPlayerBase* InputPlayer = Cast<AECPPlayerBase>(GetPawn()))
	{
		InputPlayer->RequestStorePickup();
	}
}

void AECPPlayerController::PlacePickup()
{
	if (AECPPlayerBase* InputPlayer = Cast<AECPPlayerBase>(GetPawn()))
	{
		InputPlayer->RequestPlacePickup();
	}
}

void AECPPlayerController::DropPickup()
{
	if (AECPPlayerBase* InputPlayer = Cast<AECPPlayerBase>(GetPawn()))
	{
		InputPlayer->RequestDropPickup();
	}
}

void AECPPlayerController::BeginRotatePickup()
{
	// 重复按下先可靠结束原会话，不允许两个拖动共享同一个累计起点。
	if (!IsLocalController()) return;
	if (bRotatingPickup) EndRotatePickup();
	AECPPlayerBase* InputPlayer = Cast<AECPPlayerBase>(GetPawn());
	AECPPickupItem* Item = IsValid(InputPlayer) ? InputPlayer->GetInspectedPickup() : nullptr;
	if (!IsValid(Item) || Item->GetInspectionGeneration() == 0) return;

	bRotatingPickup = true;
	RotatingPickupPlayer = InputPlayer;
	RotatingPickupItem = Item;
	PickupRotationStart = Item->GetInspectionRotation();
	PickupRotationTotal = FRotator::ZeroRotator;
	PickupRotationSample = FECPInspectionRotationSample();
	PickupRotationSample.Generation = Item->GetInspectionGeneration();
	// 控制器重建但仍持有原物品时，从服务器最后确认的会话继续，避免计数归零后全部被当旧包拒绝。
	const uint32 AcceptedSession = Item->GetLastAcceptedInspectionSession();
	if (NextPickupRotationSession == 0 || ECPPickupRules::IsNewerInspectionSequence(AcceptedSession, NextPickupRotationSession))
	{
		NextPickupRotationSession = AcceptedSession;
	}
	if (++NextPickupRotationSession == 0) ++NextPickupRotationSession;
	PickupRotationSample.Session = NextPickupRotationSession;
	PickupRotationSample.Rotation = PickupRotationStart;
	LastPickupRotationSendTime = -1.0;
	LastSentPickupRotationSequence = 0;
}

void AECPPlayerController::EndRotatePickup()
{
	AECPPlayerBase* InputPlayer = RotatingPickupPlayer.Get();
	AECPPickupItem* Item = RotatingPickupItem.Get();
	if (bRotatingPickup && IsValid(InputPlayer) && IsValid(Item) && GetPawn() == InputPlayer
		&& InputPlayer->IsLocallyControlled() && InputPlayer->GetInspectedPickup() == Item
		&& Item->GetInspectionGeneration() == PickupRotationSample.Generation)
	{
		// Final 总是占用一个新序号，包括零位移和绕一圈回到起点；不能漏掉可靠封口。
		if (++PickupRotationSample.Sequence == 0) ++PickupRotationSample.Sequence;
		PickupRotationSample.bFinal = true;
		PickupRotationSample.Rotation = (PickupRotationStart + PickupRotationTotal).GetNormalized();
		if (!InputPlayer->HasAuthority()) Item->PreviewInspectionRotation(PickupRotationSample);
		InputPlayer->RequestFinalizePickupRotation(Item, PickupRotationSample);
	}
	else if (IsValid(Item))
	{
		// 中途失去所有权/换 Pawn 时不能向新物品发送旧 Final；等待原物品自己的复制状态即可。
		Item->CancelInspectionPrediction();
	}
	bRotatingPickup = false;
	RotatingPickupPlayer.Reset();
	RotatingPickupItem.Reset();
	PickupRotationSample = FECPInspectionRotationSample();
	PickupRotationStart = FRotator::ZeroRotator;
	PickupRotationTotal = FRotator::ZeroRotator;
	LastPickupRotationSendTime = -1.0;
	LastSentPickupRotationSequence = 0;
}

bool AECPPlayerController::IsInspectingPickup() const
{
	const AECPPlayerBase* InputPlayer = Cast<AECPPlayerBase>(GetPawn());
	return IsValid(InputPlayer) && InputPlayer->IsInspectingPickup();
}

void AECPPlayerController::RefreshPickupInputMode()
{
	if (!IsLocalController())
	{
		return;
	}
	const bool bShouldInspect = IsInspectingPickup();
	if (bShouldInspect == bPickupInputModeActive)
	{
		return;
	}
	if (!bShouldInspect)
	{
		RestorePickupInputMode();
		return;
	}

	// 只在进入边沿保存一次，避免每帧把我们自己设置的 true 当成玩家原始鼠标偏好。
	bSavedCursorVisible = bShowMouseCursor;
	bPickupInputModeActive = true;
	bShowMouseCursor = true;
	StopJump();
	StopRunning();
	FInputModeGameAndUI InputMode;
	InputMode.SetHideCursorDuringCapture(false);
	InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
	SetInputMode(InputMode);
}

void AECPPlayerController::RestorePickupInputMode()
{
	if (!bPickupInputModeActive)
	{
		return;
	}
	// Pawn 切换、取消持有和 EndPlay 都走这里，确保鼠标不会永久留在屏幕上。
	bPickupInputModeActive = false;
	bShowMouseCursor = bSavedCursorVisible;
	EndRotatePickup();
	SetInputMode(FInputModeGameOnly());
}

void AECPPlayerController::PlayerTick(float DeltaTime)
{
	Super::PlayerTick(DeltaTime);
	// 服务器上的远程控制器不读取本机鼠标，也不维护一份虚假的输入会话。
	if (!IsLocalController()) return;
	RefreshPickupInputMode();

	AECPPlayerBase* InputPlayer = RotatingPickupPlayer.Get();
	AECPPickupItem* Item = RotatingPickupItem.Get();
	if (!bRotatingPickup) return;
	if (!IsValid(InputPlayer) || !IsValid(Item) || GetPawn() != InputPlayer
		|| InputPlayer->GetInspectedPickup() != Item
		|| Item->GetInspectionGeneration() != PickupRotationSample.Generation)
	{
		EndRotatePickup();
		return;
	}

	float MouseX = 0.f;
	float MouseY = 0.f;
	GetInputMouseDelta(MouseX, MouseY);
	const FRotator RawRotationDelta(-MouseY * PickupRotationSensitivity, MouseX * PickupRotationSensitivity, 0.f);
	// 保留物品自己的 2.0 默认倍率；倍率只在本地累计前应用一次，网络传最终绝对姿态。
	const FRotator RotationDelta = Item->ScaleInspectionRotationDelta(RawRotationDelta);
	if (!RotationDelta.IsNearlyZero())
	{
		PickupRotationTotal = (PickupRotationTotal + RotationDelta).GetNormalized();
		if (++PickupRotationSample.Sequence == 0) ++PickupRotationSample.Sequence;
		PickupRotationSample.Rotation = (PickupRotationStart + PickupRotationTotal).GetNormalized();
		if (InputPlayer->HasAuthority())
		{
			// 主机没有往返延迟，逐帧写同一权威状态；物品自身把网络发布限制在 20 Hz。
			InputPlayer->RequestRotatePickup(Item, PickupRotationSample);
		}
		else
		{
			Item->PreviewInspectionRotation(PickupRotationSample);
		}
	}

	if (InputPlayer->HasAuthority()) return;
	const UWorld* World = GetWorld();
	const double Now = World ? World->GetTimeSeconds() : 0.0;
	const double SendInterval = 1.0 / FMath::Clamp(PickupRotationSendRate, 1.f, 60.f);
	// 即使这一帧鼠标已停，也要发送尚未发送的最新样本；丢失的旧包无需重放。
	if (PickupRotationSample.Sequence != 0 && PickupRotationSample.Sequence != LastSentPickupRotationSequence
		&& (LastPickupRotationSendTime < 0.0 || Now - LastPickupRotationSendTime >= SendInterval))
	{
		InputPlayer->RequestRotatePickup(Item, PickupRotationSample);
		LastSentPickupRotationSequence = PickupRotationSample.Sequence;
		LastPickupRotationSendTime = Now;
	}
}

void AECPPlayerController::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	RestorePickupInputMode();
	StopJump();
	StopRunning();
	// 只移除本类确实安装过的映射，保留 UI 和其他玩法的上下文。
	if (UEnhancedInputLocalPlayerSubsystem* Subsystem = InputSubsystem.Get())
	{
		if (InstalledInputMapping)
		{
			Subsystem->RemoveMappingContext(InstalledInputMapping.Get());
		}
	}
	InstalledInputMapping = nullptr;
	InputSubsystem.Reset();
	Super::EndPlay(EndPlayReason);
}
