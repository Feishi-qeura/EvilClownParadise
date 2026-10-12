#include "Data/ECPPlayerController.h"
#include "Engine/World.h"

#include "Actors/ECPPickupItem.h"
#include "Characters/ECPCharBase.h"
#include "Characters/ECPPlayerBase.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "Engine/LocalPlayer.h"
#include "Engine/GameViewportClient.h"
#include "Blueprint/UserWidget.h"
#include "Framework/Application/IInputProcessor.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Application/SlateUser.h"
#include "Widgets/SViewport.h"
#include "GameFramework/Character.h"
#include "GameFramework/PlayerState.h"
#include "InputAction.h"
#include "InputActionValue.h"
#include "InputCoreTypes.h"
#include "InputMappingContext.h"
#include "Kismet/KismetSystemLibrary.h"

DEFINE_LOG_CATEGORY_STATIC(LogECPPlayerInput, Log, All);

namespace
{
// GameAndUI 下按钮会吞掉 Tab 作为焦点导航；仅在此玩家的调试界面打开期间接管该键。
class FECPNetworkDebugTabProcessor final : public IInputProcessor
{
public:
	explicit FECPNetworkDebugTabProcessor(AECPPlayerController* InController) : Controller(InController) {}
	virtual void Tick(float, FSlateApplication&, TSharedRef<ICursor>) override {}
	virtual bool HandleKeyDownEvent(FSlateApplication& SlateApp, const FKeyEvent& Event) override
	{
		AECPPlayerController* Owner = Controller.Get();
		if (!Owner || !Owner->IsNetworkDebugVisible() || Event.GetKey() != EKeys::Tab) return false;
		ULocalPlayer* LocalPlayer = Owner->GetLocalPlayer();
		const TSharedPtr<FSlateUser> User = LocalPlayer ? LocalPlayer->GetSlateUser() : nullptr;
		if (!User || User->GetUserIndex() != Event.GetUserIndex()) return false;
		AECPPlayerBase* Player = Cast<AECPPlayerBase>(Owner->GetPawn());
		UUserWidget* Widget = Player ? Player->GetNetworkDebugWidget() : nullptr;
		const TSharedPtr<SWidget> Root = Widget ? Widget->GetCachedWidget() : nullptr;
		UGameViewportClient* ViewportClient = LocalPlayer->ViewportClient;
		const TSharedPtr<SViewport> Viewport = ViewportClient ? ViewportClient->GetGameViewportWidget() : nullptr;
		const int32 UserIndex = User->GetUserIndex();
		const bool bDebugFocused = Root && (Root->HasUserFocus(UserIndex).IsSet()
			|| SlateApp.HasUserFocusedDescendants(Root.ToSharedRef(), UserIndex));
		const bool bViewportFocused = Viewport && (Viewport->HasUserFocus(UserIndex).IsSet()
			|| SlateApp.HasUserFocusedDescendants(Viewport.ToSharedRef(), UserIndex));
		// 编辑器其他面板/其他 PIE 窗口有焦点时不抢按键。
		if (!bDebugFocused && !bViewportFocused) return false;
		if (!Event.IsRepeat()) Owner->ToggleNetworkDebug();
		return true;
	}
private:
	TWeakObjectPtr<AECPPlayerController> Controller;
};
}

void AECPPlayerController::ToggleNetworkDebug()
{
	if (!IsLocalController()) return;
	if (bNetworkDebugInputModeActive)
	{
		HideNetworkDebug();
		return;
	}
	AECPPlayerBase* InputPlayer = Cast<AECPPlayerBase>(GetPawn());
	if (!IsValid(InputPlayer) || !InputPlayer->SetNetworkDebugWidgetVisible(true)) return;
	// 打开前可靠结束正在拖动的物品，避免界面鼠标动作继续提交旋转。
	EndRotatePickup();
	NetworkDebugPlayer = InputPlayer;
	bNetworkDebugInputModeActive = true;
	if (bPickupInputModeActive) bSavedCursorVisible = false;
	bShowMouseCursor = true;
	FInputModeGameAndUI Mode;
	Mode.SetHideCursorDuringCapture(false);
	Mode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
	InputPlayer->GetNetworkDebugWidget()->SetIsFocusable(true);
	Mode.SetWidgetToFocus(InputPlayer->GetNetworkDebugWidget()->TakeWidget());
	SetInputMode(Mode);
	if (FSlateApplication::IsInitialized())
	{
		NetworkDebugTabProcessor = MakeShared<FECPNetworkDebugTabProcessor>(this);
		FSlateApplication::Get().RegisterInputPreProcessor(NetworkDebugTabProcessor);
	}
}

void AECPPlayerController::HideNetworkDebug()
{
	if (NetworkDebugTabProcessor && FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().UnregisterInputPreProcessor(NetworkDebugTabProcessor);
	}
	NetworkDebugTabProcessor.Reset();
	if (AECPPlayerBase* InputPlayer = NetworkDebugPlayer.Get()) InputPlayer->SetNetworkDebugWidgetVisible(false);
	NetworkDebugPlayer.Reset();
	if (!bNetworkDebugInputModeActive) return;
	bNetworkDebugInputModeActive = false;
	bSavedCursorVisible = false;
	bShowMouseCursor = false;
	if (IsLocalController() && GetWorld()) SetInputMode(FInputModeGameOnly());
}

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
	InputComponent->BindKey(EKeys::MouseScrollUp, IE_Pressed, this, &ThisClass::MoveInspectedPickupCloser);
	InputComponent->BindKey(EKeys::MouseScrollDown, IE_Pressed, this, &ThisClass::MoveInspectedPickupFarther);
	InputComponent->BindKey(EKeys::Tab, IE_Pressed, this, &ThisClass::ToggleNetworkDebug);
}

void AECPPlayerController::Move(const FInputActionValue& Value)
{
	APawn* ControlledPawn = GetPawn();
	if (!IsValid(ControlledPawn))
	{
		return;
	}
	// 布娃娃和 Landing 都拒绝输入积累；服务器上的角色移动锁也会独立校验。
	if (const AECPCharBase* InputCharacter = Cast<AECPCharBase>(ControlledPawn))
	{
		if (InputCharacter->IsMovementLocked() || InputCharacter->IsDead() || InputCharacter->IsOverloaded())
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
	// 只有按住左键并进入物品旋转会话时，才把鼠标拖动专用于物品。
	// 拾取检查期间未按左键，鼠标输入仍用于转动第一人称视角。
	if (bRotatingPickup || bNetworkDebugInputModeActive)
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
	ACharacter* InputCharacter = GetCharacter();
	if (!IsValid(InputCharacter))
	{
		return;
	}
	if (const AECPCharBase* ECPCharacter = Cast<AECPCharBase>(InputCharacter))
	{
		if (ECPCharacter->IsMovementLocked() || ECPCharacter->IsDead() || ECPCharacter->IsOverloaded())
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
	if (AECPCharBase* InputCharacter = Cast<AECPCharBase>(GetPawn()))
	{
		// 检查物品时也允许切换布娃娃；服务器进入布娃娃后会统一释放角色持有的物品。
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
	if (bNetworkDebugInputModeActive) return;
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
	if (!IsLocalController() || bNetworkDebugInputModeActive) return;
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

void AECPPlayerController::MoveInspectedPickupCloser()
{
	AdjustInspectionDistance(-1.f);
}

void AECPPlayerController::MoveInspectedPickupFarther()
{
	AdjustInspectionDistance(1.f);
}

void AECPPlayerController::AdjustInspectionDistance(float Direction)
{
	if (!IsLocalController() || bNetworkDebugInputModeActive) return;
	AECPPlayerBase* InputPlayer = Cast<AECPPlayerBase>(GetPawn());
	AECPPickupItem* Item = IsValid(InputPlayer) ? InputPlayer->GetInspectedPickup() : nullptr;
	if (!IsValid(Item) || Item->GetInspectionGeneration() == 0) return;

	const float CurrentDistance = Item->GetInspectionDistance();
	const float TargetDistance = Item->ClampInspectionDistance(
		CurrentDistance + Direction * Item->GetInspectionScrollStep());
	if (FMath::IsNearlyEqual(CurrentDistance, TargetDistance)) return;

	const uint32 Generation = Item->GetInspectionGeneration();
	const uint32 Sequence = Item->GetNextInspectionDistanceSequence();
	// 本地即时预览，服务器再按物品配置的范围夹紧并同步给其他玩家。
	Item->PreviewInspectionDistance(Generation, Sequence, TargetDistance);
	InputPlayer->RequestSetInspectionDistance(Item, Generation, Sequence, TargetDistance);
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
	// Pawn 切换或控件被外部隐藏时清理输入所有权，不能让光标留到新角色。
	if (bNetworkDebugInputModeActive)
	{
		AECPPlayerBase* DebugPlayer = NetworkDebugPlayer.Get();
		UUserWidget* Widget = DebugPlayer ? DebugPlayer->GetNetworkDebugWidget() : nullptr;
		if (GetPawn() != DebugPlayer || !Widget || !Widget->IsVisible()) HideNetworkDebug();
	}
	if (bShouldInspect && bPickupInputModeActive)
	{
		// 调试面板打开时优先持有鼠标；关闭后继续沿用检查态隐藏规则。
		bShowMouseCursor = bNetworkDebugInputModeActive;
		return;
	}
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
	bSavedCursorVisible = bNetworkDebugInputModeActive ? false : bShowMouseCursor;
	bPickupInputModeActive = true;
	bShowMouseCursor = bNetworkDebugInputModeActive;
	StopJump();
	StopRunning();
	// 检查时鼠标增量直接用于拖动物品；GameOnly 保持鼠标隐藏并让角色输入继续生效。
	if (!bNetworkDebugInputModeActive) SetInputMode(FInputModeGameOnly());
}

void AECPPlayerController::RestorePickupInputMode()
{
	if (!bPickupInputModeActive)
	{
		return;
	}
	// Pawn 切换、取消持有和 EndPlay 都走这里，确保鼠标不会永久留在屏幕上。
	bPickupInputModeActive = false;
	bShowMouseCursor = bNetworkDebugInputModeActive || bSavedCursorVisible;
	EndRotatePickup();
	if (!bNetworkDebugInputModeActive) SetInputMode(FInputModeGameOnly());
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
	const FRotator RawRotationDelta(MouseY * PickupRotationSensitivity, -MouseX * PickupRotationSensitivity, 0.f);
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
	HideNetworkDebug();
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
