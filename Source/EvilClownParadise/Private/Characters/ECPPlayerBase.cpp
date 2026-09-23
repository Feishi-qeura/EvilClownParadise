#include "Characters/ECPPlayerBase.h"
#include "Actors/ECPPickupItem.h"

#include "Animation/AnimInstance.h"
#include "Blueprint/UserWidget.h"
#include "Camera/CameraComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/SpringArmComponent.h"
#include "Net/UnrealNetwork.h"
#include "TimerManager.h"
#include "UObject/ConstructorHelpers.h"
#include "UObject/StructOnScope.h"
#include "UObject/UnrealType.h"

namespace ECPPickup
{
	// SCS 模板名字可能带 _GEN_VARIABLE，运行时组件使用原始名字；统一后才可跨端按名恢复。
	FName InstanceName(const USceneComponent* Component)
	{
		FString Name = Component ? Component->GetName() : FString();
		Name.RemoveFromEnd(TEXT("_GEN_VARIABLE"));
		return FName(*Name);
	}

	template <typename T>
	T* FindComponent(const AActor* Actor, FName Name)
	{
		// 组件来自用户保留的 SCS，不依赖蓝图已删除的变量 getter。
		if (!IsValid(Actor))
		{
			return nullptr;
		}
		TInlineComponentArray<T*> Components;
		Actor->GetComponents(Components);
		for (T* Component : Components)
		{
			if (InstanceName(Component) == Name)
			{
				return Component;
			}
		}
		return nullptr;
	}
}

AECPPlayerBase::AECPPlayerBase()
{
	// 这些仅是现有资产默认值，后续项目可在派生蓝图中更换类型，不把玩法写回蓝图变量。
	static ConstructorHelpers::FClassFinder<AActor> ItemClass(TEXT("/Game/zxx/BluePrint/thing/BP_Smallitems"));
	PickupActorClass = ItemClass.Class;
	static ConstructorHelpers::FClassFinder<UUserWidget> DebugClass(TEXT("/Game/zxx/Widget/BP_NetworkDebug"));
	NetworkDebugWidgetClass = DebugClass.Class;
}

void AECPPlayerBase::BeginPlay()
{
	Super::BeginPlay();
	ResolveBlueprintComponents();
	bWasRagdollActive = IsRagdollActive();
	bLocalPresentationReady = true;
	RefreshLocalPresentation();
	if (HasAuthority())
	{
		// 基类正常行走时关闭 Actor Tick，因此不能依赖 Pawn Tick 持续发布 RemoteViewPitch。
		// 复用内建 16 位 pitch，只有实际角度变化才提前请求复制，不额外复制 Camera/武器世界变换。
		GetWorldTimerManager().SetTimer(ViewPitchTimer, this, &ThisClass::PublishViewPitch, 1.f / 30.f, true, 0.f);
	}

	// 迟加入可能在 BeginPlay 前已收到蹲下/持有状态，初始化后再应用一次是幂等的。
	if (bIsCrouched)
	{
		BeginCarryMeshCrouchBlend(true);
	}
}

void AECPPlayerBase::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	// 基类只在布娃娃/落地阶段开启 Tick；检测进入边沿一次，避免每帧重复生成库存物理状态。
	if (HasAuthority())
	{
		const bool bRagdollNow = IsRagdollActive();
		if (bRagdollNow && !bWasRagdollActive)
		{
			DropAllOwnedPickups();
		}
		bWasRagdollActive = bRagdollNow;
	}
}

FRotator AECPPlayerBase::GetViewRotation() const
{
	// 本地拥有者、服务器和未被控制的本地角色继续使用引擎默认视角。
	if (GetController() || GetLocalRole() != ROLE_SimulatedProxy)
	{
		return Super::GetViewRotation();
	}
	// 读取 GetBaseAimRotation 使用的内建 RemoteViewPitch16，直接解码以避免虚函数互调递归。
	// 仅补回角色朝向中原本为零的俯仰；保留非零 Actor Pitch、Yaw、Roll 的已有含义。
	FRotator ViewRotation = GetActorRotation();
	if (FMath::IsNearlyZero(ViewRotation.Pitch))
	{
		ViewRotation.Pitch = FRotator::DecompressAxisFromShort(GetRemoteViewPitch());
	}
	return ViewRotation;
}

void AECPPlayerBase::PublishViewPitch()
{
	if (!HasAuthority() || !GetController())
	{
		return;
	}
	const uint16 PreviousPitch = GetRemoteViewPitch();
	SetRemoteViewPitch(GetController()->GetControlRotation().Pitch);
	if (GetRemoteViewPitch() != PreviousPitch)
	{
		ForceNetUpdate();
	}
}

void AECPPlayerBase::ResolveBlueprintComponents()
{
	PlayerCamera = ECPPickup::FindComponent<UCameraComponent>(this, TEXT("Camera"));
	PlayerSpringArm = ECPPickup::FindComponent<USpringArmComponent>(this, TEXT("SpringArm"));
	CarryMesh = ECPPickup::FindComponent<USkeletalMeshComponent>(this, TEXT("SkeletalMesh"));

	// 起始位置来自现有组件设置，保留用户调整；只记录一次，避免把蹲下中间值当站立位置。
	if (CarryMesh && !bCarryLocationCached)
	{
		StandingCarryMeshLocation = CarryMesh->GetRelativeLocation();
		bCarryLocationCached = true;
	}
}

void AECPPlayerBase::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AECPPlayerBase, InspectedPickup);
	DOREPLIFETIME(AECPPlayerBase, EquippedPickup);
}

void AECPPlayerBase::PossessedBy(AController* NewController)
{
	Super::PossessedBy(NewController);
	RefreshLocalPresentation();
}

void AECPPlayerBase::UnPossessed()
{
	Super::UnPossessed();
	RefreshLocalPresentation();
}

void AECPPlayerBase::PawnClientRestart()
{
	Super::PawnClientRestart();
	RefreshLocalPresentation();
}

void AECPPlayerBase::OnRep_Controller()
{
	Super::OnRep_Controller();
	RefreshLocalPresentation();
}

void AECPPlayerBase::RefreshLocalPresentation()
{
	// PossessedBy 可能早于 BeginPlay；组件就绪之后再开始扫描和本地表现。
	if (!bLocalPresentationReady || !GetWorld())
	{
		return;
	}
	APlayerController* PlayerController = Cast<APlayerController>(GetController());
	if (HasAuthority())
	{
		// 服务器替每个玩家检查真实视角；包括远端客户端，不依赖其本地 SetVisibility 或 Actor Tick。
		if (PlayerController)
		{
			if (!GetWorldTimerManager().IsTimerActive(PickupFocusTimer))
			{
				GetWorldTimerManager().SetTimer(PickupFocusTimer, this, &ThisClass::UpdatePickupFocus,
					FMath::Clamp(PickupFocusScanInterval, 0.05f, 1.f), true, 0.f);
			}
		}
		else
		{
			// 失去控制权立即交还锁；不能依赖客户端再发送一次“取消瞄准”。
			GetWorldTimerManager().ClearTimer(PickupFocusTimer);
			SetFocusedPickup(nullptr);
		}
	}
	const bool bIsLocalPlayer = PlayerController && PlayerController->IsLocalController() && IsLocallyControlled();
	if (bIsLocalPlayer)
	{
#if ENABLE_DRAW_DEBUG
		// 开发时保留红色射线；正式关闭调试绘制的构建不需要额外进行本地扫描。
		if (!GetWorldTimerManager().IsTimerActive(PickupDebugTraceTimer))
		{
			GetWorldTimerManager().SetTimer(PickupDebugTraceTimer, this, &ThisClass::UpdatePickupDebugTrace,
				FMath::Max(0.05f, PickupTraceInterval), true, 0.f);
		}
#endif
		if (!NetworkDebugWidget && NetworkDebugWidgetClass)
		{
			NetworkDebugWidget = CreateWidget<UUserWidget>(PlayerController, NetworkDebugWidgetClass);
			if (NetworkDebugWidget)
			{
				NetworkDebugWidget->AddToPlayerScreen();
			}
		}
	}
	else
	{
		// 本地控制结束只清理本地调试；其他玩家拥有的物品提示不能被这个角色隐藏。
		GetWorldTimerManager().ClearTimer(PickupDebugTraceTimer);
		if (NetworkDebugWidget)
		{
			NetworkDebugWidget->RemoveFromParent();
			NetworkDebugWidget = nullptr;
		}
	}
}

bool AECPPlayerBase::TracePickup(FHitResult& OutHit, bool bDrawTrace) const
{
	// 沿用原 SkeletalMesh 的 Up 方向。该武器模型的朝前轴是 Up，换成 Actor Forward 会改变瞄准。
	if (!CarryMesh || !GetWorld() || IsMovementLocked() || IsDead() || GetHeldPickup())
	{
		return false;
	}
	const FVector Start = CarryMesh->GetComponentLocation();
	FVector Direction = CarryMesh->GetUpVector();
	if (PlayerSpringArm && PlayerCamera && PlayerCamera->GetAttachParent() == PlayerSpringArm
		&& CarryMesh->GetAttachParent() == PlayerCamera)
	{
		// RPC 可能先于 SpringArm 本帧 Tick 到达；直接用当前权威视角和保留的局部轴计算方向。
		// 相同计算也用于服务器瞄准锁，不接受客户端传入的任意射线或目标，原 250cm 起点/距离保持不变。
		const FQuat CameraAim = PlayerSpringArm->GetTargetRotation().Quaternion() * PlayerCamera->GetRelativeRotation().Quaternion();
		Direction = CameraAim.RotateVector(CarryMesh->GetRelativeRotation().Quaternion().GetUpVector()).GetSafeNormal();
	}
	const FVector End = Start + Direction * FMath::Max(1.f, PickupDistance);

	FCollisionQueryParams Query(SCENE_QUERY_STAT(ECPPickup), false, this);
	FCollisionObjectQueryParams Objects;
	Objects.AddObjectTypesToQuery(ECC_GameTraceChannel1); // 项目 thing 是 Object Channel，不是 Trace Channel。
	const bool bHit = GetWorld()->LineTraceSingleByObjectType(OutHit, Start, End, Objects, Query);
#if ENABLE_DRAW_DEBUG
	// ImpactPoint 只有检测命中后才有效；未命中也画出完整射线，方便检查方向和拾取距离。
	// 复用本地拾取扫描定时器，不依赖按需关闭的 Actor Tick，也不绘制其他玩家的服务器校验线。
	if (bDrawTrace && IsLocallyControlled())
	{
		DrawDebugLine(GetWorld(), Start, bHit ? OutHit.ImpactPoint : End, FColor::Red, false, 2.0f, 0, 2.0f);
	}
#endif
	// 这里只判断真实几何命中；占用由物品仲裁，不能把自己的锁误判成“未命中”造成闪烁。
	// 也不能忽略被别人锁定的首个物品，穿透它去抢后面的物品。
	if (!bHit || !IsValid(OutHit.GetActor()) || !PickupActorClass || !OutHit.GetActor()->IsA(PickupActorClass))
	{
		return false;
	}

	// 只筛 thing 的射线会穿过墙；再检查 Visibility 遮挡，墙后的合法物品也必须拒绝。
	FHitResult Obstruction;
	if (GetWorld()->LineTraceSingleByChannel(Obstruction, Start, OutHit.ImpactPoint, ECC_Visibility, Query)
		&& Obstruction.GetActor() != OutHit.GetActor())
	{
		return false;
	}

	return OutHit.Distance <= FMath::Max(1.f, PickupDistance) + KINDA_SMALL_NUMBER;
}

bool AECPPlayerBase::CanServerPickUpItem(const AECPPickupItem* Item) const
{
	// 下一帧队列结算再次走服务器相机、距离和遮挡，玩家移开视角后不能靠旧 RPC 赢得物品。
	if (!HasAuthority() || !IsValid(Item) || IsDead() || IsMovementLocked() || GetHeldPickup())
	{
		return false;
	}
	FHitResult Hit;
	return TracePickup(Hit) && Hit.GetActor() == Item;
}

void AECPPlayerBase::CompleteQueuedPickup(AECPPickupItem* Item)
{
	// 只有物品已经权威切到本玩家的 Pickup 态才回写角色引用，避免两边状态分叉。
	if (!HasAuthority() || !IsValid(Item) || Item->GetPickupState() != EECPPickupState::Pickup
		|| !Item->IsOwnedBy(this))
	{
		return;
	}
	InspectedPickup = Item;
	EquippedPickup = nullptr;
	SetInspectionMovementLocked(true);
	SetFocusedPickup(nullptr);
	ForceNetUpdate();
}

void AECPPlayerBase::UpdatePickupDebugTrace()
{
	// 红线只反映本机射线，绝不直接改变 Cube；全员提示由物品复制的锁定状态统一控制。
	if (IsLocallyControlled())
	{
		FHitResult Hit;
		TracePickup(Hit, true);
	}
}

void AECPPlayerBase::UpdatePickupFocus()
{
	if (!HasAuthority())
	{
		return;
	}
	FHitResult Hit;
	// 移开、超距、遮挡、死亡、布娃娃/落地锁、持有物品时都让旧锁失效。
	const bool bValidAim = IsValid(Cast<APlayerController>(GetController())) && TracePickup(Hit);
	SetFocusedPickup(bValidAim ? Cast<AECPPickupItem>(Hit.GetActor()) : nullptr);
}

void AECPPlayerBase::SetFocusedPickup(AECPPickupItem* NewItem)
{
	if (!HasAuthority())
	{
		return;
	}
	AECPPickupItem* Previous = FocusedPickup.Get();
	if (Previous != NewItem)
	{
		// 一个玩家最多锁一件物品；先交还旧目标，不能继续垄断已经不再瞄准的物品。
		if (IsValid(Previous))
		{
			Previous->ReleaseFocus(this);
		}
		FocusedPickup.Reset();
	}
	if (IsValid(NewItem) && NewItem->TryAcquireFocus(this))
	{
		// 物品在服务器串行仲裁；重复扫描自己的锁不会改变状态或重复发送网络更新。
		FocusedPickup = NewItem;
	}
	else
	{
		// 目标被拿走或自己失去资格时只能释放自己的锁，不能清掉其他玩家新取得的锁。
		if (Previous == NewItem && IsValid(Previous))
		{
			Previous->ReleaseFocus(this);
		}
		FocusedPickup.Reset();
	}
}

void AECPPlayerBase::RequestPickup()
{
	// 本地只预筛射线；复制锁可能尚未更新，不能用旧锁吞掉一次有效的 F 请求。
	// 服务器仍即时检查射线、移动状态与锁定者，客户端无法借此抢走别人的物品。
	FHitResult Hit;
	if (!IsLocallyControlled() || !TracePickup(Hit, true))
	{
		return;
	}
	if (HasAuthority())
	{
		TryPickupOnServer();
	}
	else
	{
		ServerRequestPickup();
	}
}

void AECPPlayerBase::ServerRequestPickup_Implementation()
{
	TryPickupOnServer();
}

void AECPPlayerBase::TryPickupOnServer()
{
	// 可靠 RPC 只用于按键；节流同时阻止同一帧重复 F 触发多次状态修改。
	if (!HasAuthority() || !GetWorld() || !PlayerCamera)
	{
		return;
	}
	const double Now = GetWorld()->GetTimeSeconds();
	if (Now - LastPickupRequestAt < 0.1)
	{
		return;
	}
	LastPickupRequestAt = Now;
	FHitResult Hit;
	if (!TracePickup(Hit))
	{
		SetFocusedPickup(nullptr);
		return;
	}
	AECPPickupItem* Item = Cast<AECPPickupItem>(Hit.GetActor());
	// 高亮锁不决定胜负；同帧 F 请求由主机、服务器观测 Ping、加入与到达顺序依次仲裁。
	SetFocusedPickup(Item);
	if (Item)
	{
		Item->QueuePickupRequest(this);
	}
}

AActor* AECPPlayerBase::GetHeldPickup() const
{
	// 只把检查态和装备态视为活跃手持；Stored 不阻止继续拾取新的世界物品。
	if (AECPPickupItem* Item = GetInspectedPickup())
	{
		return Item;
	}
	return GetEquippedPickup();
}

bool AECPPlayerBase::IsInspectingPickup() const
{
	return GetInspectedPickup() != nullptr;
}

AECPPickupItem* AECPPlayerBase::GetInspectedPickup() const
{
	return IsValid(InspectedPickup) && InspectedPickup->IsOwnedBy(this)
		&& InspectedPickup->GetPickupState() == EECPPickupState::Pickup ? InspectedPickup.Get() : nullptr;
}

AECPPickupItem* AECPPlayerBase::GetEquippedPickup() const
{
	return IsValid(EquippedPickup) && EquippedPickup->IsOwnedBy(this)
		&& EquippedPickup->GetPickupState() == EECPPickupState::Equipped ? EquippedPickup.Get() : nullptr;
}

void AECPPlayerBase::RequestEquipPickup()
{
	if (!IsLocallyControlled())
	{
		return;
	}
	if (HasAuthority())
	{
		ServerRequestEquipPickup_Implementation();
	}
	else
	{
		ServerRequestEquipPickup();
	}
}

void AECPPlayerBase::RequestStorePickup()
{
	if (!IsLocallyControlled())
	{
		return;
	}
	if (HasAuthority())
	{
		ServerRequestStorePickup_Implementation();
	}
	else
	{
		ServerRequestStorePickup();
	}
}

void AECPPlayerBase::RequestPlacePickup()
{
	if (!IsLocallyControlled())
	{
		return;
	}
	if (HasAuthority())
	{
		ServerRequestPlacePickup_Implementation();
	}
	else
	{
		ServerRequestPlacePickup();
	}
}

void AECPPlayerBase::RequestDropPickup()
{
	if (!IsLocallyControlled())
	{
		return;
	}
	if (HasAuthority())
	{
		ServerRequestDropPickup_Implementation();
	}
	else
	{
		ServerRequestDropPickup();
	}
}

void AECPPlayerBase::RequestRotatePickup(AECPPickupItem* Item, const FECPInspectionRotationSample& Sample)
{
	// 固定为按下鼠标时捕获的物品；换物品后不能把上一轮拖动重新解释为新物品的输入。
	if (!IsLocallyControlled() || !IsValid(Item) || GetInspectedPickup() != Item || Sample.Rotation.ContainsNaN())
	{
		return;
	}
	FECPInspectionRotationSample StreamSample = Sample;
	StreamSample.bFinal = false;
	if (HasAuthority())
	{
		ServerRequestRotatePickup_Implementation(Item, StreamSample);
	}
	else
	{
		ServerRequestRotatePickup(Item, StreamSample);
	}
}

void AECPPlayerBase::RequestFinalizePickupRotation(AECPPickupItem* Item, const FECPInspectionRotationSample& Sample)
{
	if (!IsLocallyControlled() || !IsValid(Item) || GetInspectedPickup() != Item || Sample.Rotation.ContainsNaN())
	{
		return;
	}
	FECPInspectionRotationSample FinalSample = Sample;
	FinalSample.bFinal = true;
	if (HasAuthority())
	{
		ServerFinalizePickupRotation_Implementation(Item, FinalSample);
	}
	else
	{
		ServerFinalizePickupRotation(Item, FinalSample);
	}
}

void AECPPlayerBase::ServerRequestEquipPickup_Implementation()
{
	AECPPickupItem* Item = GetInspectedPickup();
	if (!HasAuthority() || IsDead() || IsRagdollActive() || !Item || !Item->TryEquip(this))
	{
		return;
	}
	InspectedPickup = nullptr;
	EquippedPickup = Item;
	SetInspectionMovementLocked(false);
	ForceNetUpdate();
}

void AECPPlayerBase::ServerRequestStorePickup_Implementation()
{
	AECPPickupItem* Item = GetInspectedPickup();
	const bool bWasInspecting = Item != nullptr;
	if (!Item)
	{
		Item = GetEquippedPickup();
	}
	if (!HasAuthority() || IsDead() || IsRagdollActive() || !Item || !Item->TryStore(this))
	{
		return;
	}
	// 库存数组只在物品成功切换 Stored 后写入，避免角色和物品产生两套真相。
	StoredPickups.AddUnique(Item);
	if (bWasInspecting)
	{
		InspectedPickup = nullptr;
		SetInspectionMovementLocked(false);
	}
	else
	{
		EquippedPickup = nullptr;
	}
	ForceNetUpdate();
}

void AECPPlayerBase::ServerRequestPlacePickup_Implementation()
{
	AECPPickupItem* Item = GetInspectedPickup();
	if (!HasAuthority() || !Item || !Item->TryDrop(this, BuildPickupReleaseTransform(), FVector::ZeroVector))
	{
		return;
	}
	InspectedPickup = nullptr;
	SetInspectionMovementLocked(false);
	ForceNetUpdate();
}

void AECPPlayerBase::ServerRequestDropPickup_Implementation()
{
	AECPPickupItem* Item = GetEquippedPickup();
	if (!HasAuthority() || IsDead() || IsRagdollActive() || !Item)
	{
		return;
	}
	// 投掷冲量只由服务器用角色视角和力量生成；Chaos 再按物体质量计算实际速度。
	const float ImpulseStrength = ECPPickupRules::ExpectedThrowImpulse(ThrowStrength, PlayerStrength);
	const FVector ThrowImpulse = GetViewRotation().Vector().GetSafeNormal() * ImpulseStrength;
	if (!Item->TryDrop(this, BuildPickupReleaseTransform(), ThrowImpulse))
	{
		return;
	}
	EquippedPickup = nullptr;
	ForceNetUpdate();
}

void AECPPlayerBase::ServerRequestRotatePickup_Implementation(AECPPickupItem* Item, const FECPInspectionRotationSample& Sample)
{
	// 指定目标只能收紧校验，不能替代权威槽位；否则旧物品包可能误作用到刚换到的新物品。
	if (HasAuthority() && !IsDead() && !IsRagdollActive() && IsValid(Item)
		&& GetInspectedPickup() == Item && !Sample.bFinal)
	{
		Item->ApplyInspectionRotation(this, Sample);
	}
}

void AECPPlayerBase::ServerFinalizePickupRotation_Implementation(AECPPickupItem* Item, const FECPInspectionRotationSample& Sample)
{
	// 与流包共用代际/会话裁决；可靠只保证交付，不代表迟到的旧 Final 可以覆盖新拖动。
	if (HasAuthority() && !IsDead() && !IsRagdollActive() && IsValid(Item)
		&& GetInspectedPickup() == Item && Sample.bFinal)
	{
		Item->ApplyInspectionRotation(this, Sample);
	}
}

FTransform AECPPlayerBase::BuildPickupReleaseTransform(float ForwardDistance) const
{
	const FVector ViewLocation = PlayerCamera ? PlayerCamera->GetComponentLocation() : GetActorLocation();
	const FRotator ViewRotation = GetViewRotation();
	return FTransform(ViewRotation, ViewLocation + ViewRotation.Vector() * FMath::Max(0.f, ForwardDistance));
}

void AECPPlayerBase::SetInspectionMovementLocked(bool bLocked)
{
	UCharacterMovementComponent* Movement = GetCharacterMovement();
	if (!Movement || bInspectionMovementLocked == bLocked)
	{
		return;
	}
	if (bLocked)
	{
		// 保存进入检查态前的模式，装备、入库或放下后精确恢复游泳/飞行等派生模式。
		SavedInspectionMovementMode = static_cast<uint8>(Movement->MovementMode);
		SavedInspectionCustomMovementMode = Movement->CustomMovementMode;
		bInspectionMovementLocked = true;
		Movement->StopMovementImmediately();
		Movement->DisableMovement();
		return;
	}
	bInspectionMovementLocked = false;
	// 死亡或布娃娃拥有更高优先级，清理检查态时不能把角色错误恢复为可行走。
	if (!IsDead() && !IsRagdollActive())
	{
		Movement->SetMovementMode(static_cast<EMovementMode>(SavedInspectionMovementMode), SavedInspectionCustomMovementMode);
	}
}

void AECPPlayerBase::OnRep_InspectedPickup()
{
	// 输入模式在拥有者控制器统一刷新；此处只保证组件/调试表现已完成延迟初始化。
	RefreshLocalPresentation();
}

void AECPPlayerBase::OnRep_EquippedPickup()
{
	RefreshLocalPresentation();
}

void AECPPlayerBase::DropAllOwnedPickups()
{
	if (!HasAuthority())
	{
		return;
	}

	// 先释放活跃槽，再清空引用；重复调用时所有槽为空，因此天然幂等。
	if (AECPPickupItem* Item = GetInspectedPickup())
	{
		Item->TryDrop(this, BuildPickupReleaseTransform(60.f), FVector::ZeroVector);
	}
	if (AECPPickupItem* Item = GetEquippedPickup())
	{
		Item->TryDrop(this, BuildPickupReleaseTransform(80.f), FVector::ZeroVector);
	}
	InspectedPickup = nullptr;
	EquippedPickup = nullptr;
	SetInspectionMovementLocked(false);

	const int32 ItemCount = StoredPickups.Num();
	const FVector Origin = GetActorLocation() + FVector(0.f, 0.f, 50.f);
	for (int32 Index = 0; Index < ItemCount; ++Index)
	{
		AECPPickupItem* Item = StoredPickups[Index];
		if (!IsValid(Item) || !Item->IsOwnedBy(this))
		{
			continue;
		}
		// 围绕尸体均匀爆出，避免所有 Stored 物品在同一点互相穿插并产生巨大物理解算冲量。
		const float Angle = ItemCount > 0 ? 2.f * PI * static_cast<float>(Index) / static_cast<float>(ItemCount) : 0.f;
		const FVector Radial(FMath::Cos(Angle), FMath::Sin(Angle), 0.f);
		const FTransform ReleaseTransform(GetActorRotation(), Origin + Radial * FMath::Max(0.f, InventoryDropRadius));
		const FVector InitialVelocity = GetVelocity() + Radial * 80.f
			+ FVector::UpVector * FMath::Max(0.f, InventoryDropUpwardVelocity);
		Item->DropFromInventory(this, ReleaseTransform, InitialVelocity);
	}
	StoredPickups.Reset();
	ForceNetUpdate();
}

void AECPPlayerBase::HandleDied_Implementation(AActor* Killer)
{
	// 先把物品变回世界态，再广播死亡；监听者即使立刻销毁角色也不会丢失库存。
	DropAllOwnedPickups();
	Super::HandleDied_Implementation(Killer);
}

void AECPPlayerBase::OnStartCrouch(float HalfHeightAdjust, float ScaledHalfHeightAdjust)
{
	Super::OnStartCrouch(HalfHeightAdjust, ScaledHalfHeightAdjust);
	NotifyAnimationCrouch(true);
	BeginCarryMeshCrouchBlend(true);
}

void AECPPlayerBase::OnEndCrouch(float HalfHeightAdjust, float ScaledHalfHeightAdjust)
{
	Super::OnEndCrouch(HalfHeightAdjust, ScaledHalfHeightAdjust);
	NotifyAnimationCrouch(false);
	BeginCarryMeshCrouchBlend(false);
}

void AECPPlayerBase::NotifyAnimationCrouch(bool bCrouching)
{
	UAnimInstance* AnimInstance = GetMesh() ? GetMesh()->GetAnimInstance() : nullptr;
	UFunction* CrouchEvent = AnimInstance ? AnimInstance->FindFunction(TEXT("EventAnimationCrouch")) : nullptr;
	if (!CrouchEvent)
	{
		return;
	}
	// 现有动画蓝图实现的是蓝图接口；按真实参数布局写入 bool，保留模块化动画而不依赖旧角色变量。
	FStructOnScope Parameters(CrouchEvent);
	for (TFieldIterator<FBoolProperty> It(CrouchEvent); It; ++It)
	{
		if (It->HasAnyPropertyFlags(CPF_Parm) && !It->HasAnyPropertyFlags(CPF_ReturnParm))
		{
			It->SetPropertyValue_InContainer(Parameters.GetStructMemory(), bCrouching);
			break;
		}
	}
	AnimInstance->ProcessEvent(CrouchEvent, Parameters.GetStructMemory());
}

void AECPPlayerBase::BeginCarryMeshCrouchBlend(bool bCrouching)
{
	ResolveBlueprintComponents();
	if (!CarryMesh || !GetWorld())
	{
		return;
	}
	CarryBlendStart = CarryMesh->GetRelativeLocation();
	CarryBlendTarget = bCrouching ? CrouchedCarryMeshLocation : StandingCarryMeshLocation;
	CarryBlendStartedAt = GetWorld()->GetTimeSeconds();
	// 从当前值重新平滑，连续蹲起可立刻反向，不沿用旧 Timeline 的布尔门锁而漏掉站起事件。
	if (CarryMeshCrouchBlendDuration <= KINDA_SMALL_NUMBER)
	{
		CarryMesh->SetRelativeLocation(CarryBlendTarget);
		GetWorldTimerManager().ClearTimer(CarryMeshBlendTimer);
		return;
	}
	GetWorldTimerManager().SetTimer(CarryMeshBlendTimer, this, &ThisClass::UpdateCarryMeshCrouchBlend, 1.f / 60.f, true, 0.f);
}

void AECPPlayerBase::UpdateCarryMeshCrouchBlend()
{
	if (!CarryMesh)
	{
		GetWorldTimerManager().ClearTimer(CarryMeshBlendTimer);
		return;
	}
	const float Alpha = FMath::Clamp(static_cast<float>((GetWorld()->GetTimeSeconds() - CarryBlendStartedAt)
		/ FMath::Max(KINDA_SMALL_NUMBER, CarryMeshCrouchBlendDuration)), 0.f, 1.f);
	const float SmoothAlpha = Alpha * Alpha * (3.f - 2.f * Alpha);
	CarryMesh->SetRelativeLocation(FMath::Lerp(CarryBlendStart, CarryBlendTarget, SmoothAlpha));
	// 完成立即清理；不依赖 Actor Tick，避免与基类布娃娃的按需 Tick 开关争用。
	if (Alpha >= 1.f)
	{
		GetWorldTimerManager().ClearTimer(CarryMeshBlendTimer);
	}
}

void AECPPlayerBase::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// 服务器上的退出、换 Pawn 和关卡销毁都释放物品；物品函数幂等，可与死亡/布娃娃边沿相邻调用。
	if (HasAuthority())
	{
		DropAllOwnedPickups();
	}
	bLocalPresentationReady = false;
	GetWorldTimerManager().ClearTimer(ViewPitchTimer);
	GetWorldTimerManager().ClearTimer(PickupDebugTraceTimer);
	GetWorldTimerManager().ClearTimer(PickupFocusTimer);
	GetWorldTimerManager().ClearTimer(CarryMeshBlendTimer);
	// 退出游戏/换角色也立即释放瞄准锁；物品仍负责把新状态同步给留下的玩家。
	SetFocusedPickup(nullptr);
	if (NetworkDebugWidget)
	{
		NetworkDebugWidget->RemoveFromParent();
		NetworkDebugWidget = nullptr;
	}
	Super::EndPlay(EndPlayReason);
}
