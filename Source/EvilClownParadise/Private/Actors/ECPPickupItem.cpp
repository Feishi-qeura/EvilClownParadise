#include "Actors/ECPPickupItem.h"

#include "Characters/ECPPlayerBase.h"
#include "Data/ECPPlayerController.h"
#include "Camera/CameraComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/PlayerState.h"
#include "CoreGlobals.h"
#include "Net/UnrealNetwork.h"
#include "TimerManager.h"

AECPPickupItem::AECPPickupItem()
{
	PrimaryActorTick.bCanEverTick = false;
	bReplicates = true;
	SetReplicateMovement(true);
}

void AECPPickupItem::BeginPlay()
{
	Super::BeginPlay();
	OriginalNetUpdateFrequency = GetNetUpdateFrequency();
	bOriginalAlwaysRelevant = bAlwaysRelevant;
	bOriginalOwnerRelevancy = bNetUseOwnerRelevancy;
	TInlineComponentArray<UPrimitiveComponent*> Components(this);
	for (UPrimitiveComponent* Component : Components)
	{
		if (Component->GetFName() == TEXT("Cube"))
		{
			// Cube 只由物品焦点 RepNotify 控制；缓存前禁复制，释放时也不能恢复为原蓝图的 true。
			FocusVisual = Component;
			Component->SetIsReplicated(false);
			Component->SetOnlyOwnerSee(false);
			Component->SetOwnerNoSee(false);
		}
		FECPPickupComponentDefaults& Defaults = ComponentDefaults.AddDefaulted_GetRef();
		Defaults.Name = Component->GetFName();
		Defaults.CollisionEnabled = Component->GetCollisionEnabled();
		Defaults.bReplicated = Component->GetIsReplicated();
	}
	// 本项目物理体明确为 Sphere1 根组件，避免对非根物理物体假定 Actor 移动复制能同步它。
	UPrimitiveComponent* Body = Cast<UPrimitiveComponent>(GetRootComponent());
	if (!Body || Body->GetFName() != TEXT("Sphere1"))
	{
		UE_LOG(LogTemp, Error, TEXT("%s: ECPPickupItem requires the Sphere1 primitive root."), *GetName());
		return;
	}
	bInitialized = true;
	// 原 BP BeginPlay 开启物理移到这里；先缓存默认碰撞，再依据复制状态决定是否模拟。
	OnRep_AttachmentState();
}

void AECPPickupItem::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AECPPickupItem, AttachmentState);
}

bool AECPPickupItem::CanBePickedUp() const
{
	return bInitialized && AttachmentState.State == EECPPickupState::World && !IsActorBeingDestroyed()
		&& !IsValid(GetOwner()) && !IsValid(GetAttachParentActor());
}

bool AECPPickupItem::CanBePickedUpBy(const AECPPlayerBase* Player) const
{
	// 焦点只负责高亮；真正竞争在 F 请求队列中完成，不能让先扫到的玩家垄断拾取资格。
	return IsValid(Player) && CanBePickedUp();
}

bool AECPPickupItem::IsFocusedBy(const AECPPlayerBase* Player) const
{
	return Player && IsFocused() && AttachmentState.FocusOwner == Player;
}

bool AECPPickupItem::TryAcquireFocus(AECPPlayerBase* Player)
{
	// 所有请求在服务器游戏线程串行竞争；已有其他所有者时绝不覆盖，先成功的玩家保留锁。
	if (!HasAuthority() || !CanBePickedUpBy(Player) || !Player->GetController()
		|| Player->IsMovementLocked() || Player->IsDead() || Player->GetHeldPickup())
	{
		return false;
	}
	if (IsFocusedBy(Player))
	{
		return true; // 服务器每 0.1 秒确认同一目标时，不重复发布状态或重建材质。
	}
	// 同时瞄准时保留第一个高亮所有者，但其他玩家仍可通过权威请求队列竞争物品。
	if (AttachmentState.bFocused)
	{
		return false;
	}
	AttachmentState.bFocused = true;
	AttachmentState.FocusOwner = Player;
	AttachmentState.FocusPlayerState = Player->GetPlayerState();
	BoundFocusOwner = Player;
	Player->OnDestroyed.AddUniqueDynamic(this, &ThisClass::OnFocusOwnerDestroyed);
	// 焦点只改变元数据，不递增物理 Revision，也不改变 Actor Owner 或网络相关性。
	OnRep_AttachmentState();
	FlushNetDormancy();
	ForceNetUpdate();
	return true;
}

void AECPPickupItem::ReleaseFocus(const AECPPlayerBase* Player)
{
	// 旧角色的延迟清理只允许释放自己的锁，不能撤销物品后来交给另一位玩家的新锁。
	if (!HasAuthority() || !Player || !AttachmentState.bFocused || AttachmentState.FocusOwner != Player)
	{
		return;
	}
	ClearFocusState();
	OnRep_AttachmentState();
	FlushNetDormancy();
	ForceNetUpdate();
}

void AECPPickupItem::ClearFocusState()
{
	if (AECPPlayerBase* Previous = BoundFocusOwner.Get())
	{
		Previous->OnDestroyed.RemoveDynamic(this, &ThisClass::OnFocusOwnerDestroyed);
	}
	BoundFocusOwner.Reset();
	AttachmentState.bFocused = false;
	AttachmentState.FocusOwner = nullptr;
	AttachmentState.FocusPlayerState = nullptr;
}

void AECPPickupItem::OnFocusOwnerDestroyed(AActor* DestroyedActor)
{
	// 销毁期间对象可能已被标记无效，但指针仍可比较；不能用 IsValid 提前跳过最后一次解锁。
	if (HasAuthority() && AttachmentState.FocusOwner == DestroyedActor)
	{
		ReleaseFocus(Cast<AECPPlayerBase>(DestroyedActor));
	}
}

void AECPPickupItem::ApplyFocusPresentation()
{
	if (!FocusVisual)
	{
		return; // BeginPlay 记录 SCS 后会再次应用，同样支持早到的初始复制状态。
	}
	// 可见性由显式状态决定，不依赖 Pawn/PlayerState 网络引用已解析；晚加入可先显示默认材质。
	const bool bVisible = IsFocused();
	APlayerState* FocusingPlayerState = bVisible ? AttachmentState.FocusPlayerState.Get() : nullptr;
	// 同时归一可见性和 HiddenInGame，避免模板隐藏标志让已锁定的 Cube 仍然不可见。
	FocusVisual->SetVisibility(bVisible, false);
	FocusVisual->SetHiddenInGame(!bVisible, false);
	if (!bFocusPresentationInitialized || bLastPresentedFocused != bVisible
		|| LastPresentedFocusPlayerState.Get() != FocusingPlayerState)
	{
		bFocusPresentationInitialized = true;
		bLastPresentedFocused = bVisible;
		LastPresentedFocusPlayerState = FocusingPlayerState;
		OnFocusPresentationChanged(bVisible, FocusingPlayerState);
	}
}

bool AECPPickupItem::IsHeldBy(const AECPPlayerBase* Player) const
{
	return IsOwnedBy(Player) && (AttachmentState.State == EECPPickupState::Pickup
		|| AttachmentState.State == EECPPickupState::Equipped);
}

bool AECPPickupItem::IsOwnedBy(const AECPPlayerBase* Player) const
{
	return Player && AttachmentState.State != EECPPickupState::World && AttachmentState.Holder == Player;
}

FRotator AECPPickupItem::GetInspectionRotation() const
{
	return bHasPredictedInspectionRotation && AttachmentState.State == EECPPickupState::Pickup
		&& ECPPickupRules::ShouldRetainInspectionPrediction(AttachmentState.Inspection, PredictedInspection)
		? PredictedInspection.Rotation : AttachmentState.Inspection.Rotation;
}

FRotator AECPPickupItem::ScaleInspectionRotationDelta(FRotator RotationDelta) const
{
	// 元数据限制只约束编辑器输入；运行时仍夹紧倍率，避免动态改值造成反向旋转或极端网络增量。
	return RotationDelta * FMath::Clamp(InspectionRotationSpeedMultiplier, 0.1f, 10.0f);
}

bool AECPPickupItem::TryPickUp(AECPPlayerBase* Player)
{
	// 兼容旧蓝图/C++ 入口；新 F 流程使用队列，最终仍进入同一权威转换函数。
	return TryEnterPickup(Player);
}

bool AECPPickupItem::QueuePickupRequest(AECPPlayerBase* Player)
{
	if (!HasAuthority() || !bInitialized || !CanBePickedUpBy(Player) || !GetWorld())
	{
		return false;
	}
	// 同一玩家在结算前重复触发只保留第一次到达序号，不能靠按键连发提升排序机会。
	for (const FECPPendingPickupRequest& Request : PendingPickupRequests)
	{
		if (Request.Player == Player)
		{
			return true;
		}
	}
	// 记录服务器帧而不是客户端时间；跨帧先到者始终排在更晚主机/低 Ping 请求之前。
	PendingPickupRequests.Add({Player, GFrameCounter, NextPickupArrivalOrder++});
	if (!bPickupResolveScheduled)
	{
		bPickupResolveScheduled = true;
		GetWorldTimerManager().SetTimerForNextTick(this, &ThisClass::ResolvePickupRequests);
	}
	return true;
}

void AECPPickupItem::ResolvePickupRequests()
{
	bPickupResolveScheduled = false;
	TArray<FECPPendingPickupRequest> Requests = MoveTemp(PendingPickupRequests);
	PendingPickupRequests.Reset();
	if (!HasAuthority() || AttachmentState.State != EECPPickupState::World)
	{
		return;
	}
	// 在排序前用当前物品重新执行服务器射线复核；失效玩家和已移开视角的请求直接丢弃。
	Requests = Requests.FilterByPredicate([this](const FECPPendingPickupRequest& Request)
	{
		AECPPlayerBase* Player = Request.Player.Get();
		return IsValid(Player) && Player->CanServerPickUpItem(this);
	});
	Requests.Sort([](const FECPPendingPickupRequest& Left, const FECPPendingPickupRequest& Right)
	{
		const AECPPlayerController* LeftController = Cast<AECPPlayerController>(Left.Player->GetController());
		const AECPPlayerController* RightController = Cast<AECPPlayerController>(Right.Player->GetController());
		const FECPPickupPriority LeftPriority = LeftController
			? LeftController->GetPickupPriority(Left.ServerFrame, Left.ArrivalOrder)
			: FECPPickupPriority{Left.ServerFrame, false, MAX_flt, MAX_int32, Left.ArrivalOrder};
		const FECPPickupPriority RightPriority = RightController
			? RightController->GetPickupPriority(Right.ServerFrame, Right.ArrivalOrder)
			: FECPPickupPriority{Right.ServerFrame, false, MAX_flt, MAX_int32, Right.ArrivalOrder};
		return ECPPickupRules::IsHigherPriority(LeftPriority, RightPriority);
	});
	for (const FECPPendingPickupRequest& Request : Requests)
	{
		AECPPlayerBase* Player = Request.Player.Get();
		if (IsValid(Player) && TryEnterPickup(Player))
		{
			Player->CompleteQueuedPickup(this);
			break; // 一个 World 版本只能产生一个赢家，后续请求不能抢占。
		}
	}
}

bool AECPPickupItem::TryEnterPickup(AECPPlayerBase* Player)
{
	USceneComponent* Parent = IsValid(Player) ? Player->GetPickupInspectionComponent() : nullptr;
	if (!HasAuthority() || !CanBePickedUpBy(Player) || !Parent || !Player->GetController()
		|| Player->IsMovementLocked() || Player->IsDead() || Player->GetHeldPickup()
		|| !ECPPickupRules::CanTransition(AttachmentState.State, EECPPickupState::Pickup))
	{
		return false;
	}
	ClearFocusState();
	// 必须在附到相机前采集世界缩放；附着后的相对缩放已经受父组件缩放影响，不能再作为原值。
	AttachmentState.PreservedWorldScale = GetActorScale3D();
	++AttachmentState.Revision;
	AttachmentState.State = EECPPickupState::Pickup;
	AttachmentState.Holder = Player;
	// 每次重新进入 Pickup 都生成新代际；之前的可靠尾包即使迟到，也不能旋转这次新持有。
	uint32 NextGeneration = AttachmentState.Inspection.Generation + 1;
	if (NextGeneration == 0) ++NextGeneration;
	AttachmentState.Inspection = FECPInspectionRotationSample();
	AttachmentState.Inspection.Generation = NextGeneration;
	LastInspectionNetworkPublishTime = -1.0;
	SetOwner(Player);
	BoundHolder = Player;
	Player->OnDestroyed.AddUniqueDynamic(this, &ThisClass::OnHolderDestroyed);
	bAlwaysRelevant = false;
	bNetUseOwnerRelevancy = true;
	PublishStateChange();
	return true;
}

bool AECPPickupItem::TryEquip(AECPPlayerBase* Player)
{
	USkeletalMeshComponent* Parent = IsValid(Player) ? Player->GetPickupAttachmentComponent() : nullptr;
	if (!HasAuthority() || !IsOwnedBy(Player) || AttachmentState.State != EECPPickupState::Pickup
		|| !Parent || !Parent->DoesSocketExist(Player->GetPickupAttachmentSocket())
		|| !ECPPickupRules::CanTransition(AttachmentState.State, EECPPickupState::Equipped))
	{
		return false;
	}
	++AttachmentState.Revision;
	AttachmentState.State = EECPPickupState::Equipped;
	PublishStateChange();
	return true;
}

bool AECPPickupItem::TryStore(AECPPlayerBase* Player)
{
	if (!HasAuthority() || !IsOwnedBy(Player)
		|| !ECPPickupRules::CanTransition(AttachmentState.State, EECPPickupState::Stored))
	{
		return false;
	}
	++AttachmentState.Revision;
	AttachmentState.State = EECPPickupState::Stored;
	PublishStateChange();
	return true;
}

bool AECPPickupItem::ApplyInspectionRotation(AECPPlayerBase* Player, const FECPInspectionRotationSample& Sample)
{
	// 所有权、检查态和代际共同隔离不同物品/不同次持有；序号则阻止重复与乱序包反向覆盖。
	if (!HasAuthority() || !IsOwnedBy(Player) || AttachmentState.State != EECPPickupState::Pickup
		|| !ECPPickupRules::CanAcceptInspectionSample(AttachmentState.Inspection, Sample))
	{
		return false;
	}
	// 接收的是完整目标而非增量。中间包丢失时下一个包自动追上，不能再按单包角度截断累积。
	AttachmentState.Inspection = Sample;
	AttachmentState.Inspection.Rotation.Normalize();
	PublishInspectionRotation();
	return true;
}

void AECPPickupItem::PreviewInspectionRotation(const FECPInspectionRotationSample& Sample)
{
	// 只有非权威拥有端需要预测；主机直接写入同一权威样本，避免预览与权威角度轮流覆盖。
	if (HasAuthority() || AttachmentState.State != EECPPickupState::Pickup
		|| Sample.Generation != AttachmentState.Inspection.Generation || Sample.Rotation.ContainsNaN()
		|| Sample.Session == 0 || Sample.Sequence == 0)
	{
		return;
	}
	PredictedInspection = Sample;
	PredictedInspection.Rotation.Normalize();
	bHasPredictedInspectionRotation = true;
	ApplyInspectionPresentation();
}

void AECPPickupItem::CancelInspectionPrediction()
{
	// Pawn 切换、物品切换和本地中断都显式结束旧预测，不能把残留角度用于另一轮持有。
	bHasPredictedInspectionRotation = false;
	ApplyInspectionPresentation();
}

void AECPPickupItem::PublishInspectionRotation()
{
	// 旋转不会改变碰撞/附着关系，不递增物理 Revision，也不反复 SetIsReplicated 或 Attach。
	OnRep_AttachmentState();
	const double Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0;
	// 主机输入可逐帧直接提交权威姿态；网络仍以 20 Hz 发布，可靠 Final 必须立即确认。
	if (AttachmentState.Inspection.bFinal || LastInspectionNetworkPublishTime < 0.0
		|| Now - LastInspectionNetworkPublishTime >= 1.0 / 20.0)
	{
		LastInspectionNetworkPublishTime = Now;
		FlushNetDormancy();
		ForceNetUpdate();
	}
}

bool AECPPickupItem::TryDrop(AECPPlayerBase* Player, const FTransform& ReleaseTransform, const FVector& ThrowImpulse)
{
	if (!HasAuthority() || !IsOwnedBy(Player)
		|| !ECPPickupRules::CanTransition(AttachmentState.State, EECPPickupState::World)
		|| !Cast<UPrimitiveComponent>(GetRootComponent()))
	{
		return false;
	}
	Release(ReleaseTransform, Player->GetVelocity(), ThrowImpulse);
	return true;
}

bool AECPPickupItem::DropFromInventory(AECPPlayerBase* Player, const FTransform& ReleaseTransform, const FVector& InitialVelocity)
{
	if (!HasAuthority() || !IsOwnedBy(Player) || AttachmentState.State != EECPPickupState::Stored)
	{
		return false;
	}
	Release(ReleaseTransform, InitialVelocity, FVector::ZeroVector);
	return true;
}

void AECPPickupItem::ReleaseFrom(AECPPlayerBase* Player)
{
	if (HasAuthority() && IsOwnedBy(Player))
	{
		Release(GetActorTransform(), Player ? Player->GetVelocity() : FVector::ZeroVector, FVector::ZeroVector);
	}
}

void AECPPickupItem::OnHolderDestroyed(AActor* DestroyedActor)
{
	// 释放状态留在物品上，不依赖一个即将关闭网络通道的角色 RPC。
	if (HasAuthority() && AttachmentState.State != EECPPickupState::World && AttachmentState.Holder == DestroyedActor)
	{
		Release(GetActorTransform(), FVector::ZeroVector, FVector::ZeroVector);
	}
}

void AECPPickupItem::Release(const FTransform& ReleaseTransform, const FVector& InitialVelocity, const FVector& ThrowImpulse)
{
	if (AECPPlayerBase* Previous = BoundHolder.Get())
	{
		Previous->OnDestroyed.RemoveDynamic(this, &ThisClass::OnHolderDestroyed);
	}
	BoundHolder.Reset();
	++AttachmentState.Revision;
	// 玩家只提交释放位置和朝向；缩放继续由服务器保存的物品状态决定，不能信任默认单位缩放。
	AttachmentState.ReleaseTransform = ReleaseTransform;
	AttachmentState.ReleaseTransform.SetScale3D(AttachmentState.PreservedWorldScale);
	AttachmentState.ReleaseVelocity = InitialVelocity;
	AttachmentState.State = EECPPickupState::World;
	AttachmentState.Holder = nullptr;
	SetOwner(nullptr);
	bAlwaysRelevant = bOriginalAlwaysRelevant;
	bNetUseOwnerRelevancy = bOriginalOwnerRelevancy;
	FlushNetDormancy();
	SetNetDormancy(DORM_Awake);
	PublishStateChange();
	// Chaos 已按质量计算冲量后的速度变化；这里绝不再除以 KG，避免质量平方反比。
	if (!ThrowImpulse.IsNearlyZero())
	{
		if (UPrimitiveComponent* Body = Cast<UPrimitiveComponent>(GetRootComponent()))
		{
			Body->AddImpulse(ThrowImpulse);
		}
	}
}

void AECPPickupItem::PublishStateChange()
{
	OnRep_AttachmentState();
	FlushNetDormancy();
	ForceNetUpdate();
	// 先强制发布 Stored 状态再休眠；爆出会 FlushNetDormancy 并恢复 Awake。
	SetNetDormancy(AttachmentState.State == EECPPickupState::Stored ? DORM_DormantAll : DORM_Awake);
}

void AECPPickupItem::OnRep_AttachmentState()
{
	// 只用确认令牌判断预测是否已处理；角度相等可能只是绕了一圈，不能确认一个较新的输入。
	if (AttachmentState.State != EECPPickupState::Pickup
		|| (bHasPredictedInspectionRotation
			&& !ECPPickupRules::ShouldRetainInspectionPrediction(AttachmentState.Inspection, PredictedInspection)))
	{
		bHasPredictedInspectionRotation = false;
	}
	// 焦点表现与 Holder 的引用映射独立；等待附着时也必须立即关掉已拾取物品的 Cube。
	ApplyFocusPresentation();
	// 仅焦点/颜色元数据改变时不重开物理、不重接附着，静止物品也不会因被瞄准而反复重建物理状态。
	if (bAttachmentPresentationInitialized && LastAppliedRevision == AttachmentState.Revision)
	{
		ApplyInspectionPresentation();
		return;
	}
	// RepNotify 可以早于 BeginPlay 或角色引用映射；状态不丢弃，待 SCS 和拥有者准备好后再附着。
	if (!ApplyAttachmentState() && GetWorld())
	{
		GetWorldTimerManager().SetTimer(AttachmentRetryTimer, this, &ThisClass::RetryAttachment, 0.05f, true);
	}
	else if (GetWorld())
	{
		GetWorldTimerManager().ClearTimer(AttachmentRetryTimer);
	}
}

void AECPPickupItem::RetryAttachment()
{
	if (ApplyAttachmentState())
	{
		GetWorldTimerManager().ClearTimer(AttachmentRetryTimer);
	}
}

FTransform AECPPickupItem::BuildInspectionRelativeTransform(
	const USceneComponent* Parent, const FRotator& InspectionRotation) const
{
	FTransform RelativeTransform = InspectionRelativeTransform;
	RelativeTransform.SetRotation(InspectionRotation.Quaternion()
		* InspectionRelativeTransform.GetRotation());
	// 保存的是世界缩放；除以父组件世界缩放后再写相对 Transform，才能在非单位相机层级下保持外观尺寸。
	const FVector ParentWorldScale = Parent
		? Parent->GetComponentTransform().GetScale3D()
		: FVector::OneVector;
	RelativeTransform.SetScale3D(AttachmentState.PreservedWorldScale
		* FTransform::GetSafeScaleReciprocal(ParentWorldScale));
	return RelativeTransform;
}

void AECPPickupItem::ApplyInspectionPresentation()
{
	// 相机引用未映射时由附着重试负责建立层级；旋转不能临时写成世界 Transform。
	if (AttachmentState.State != EECPPickupState::Pickup || !GetRootComponent()) return;
	AECPPlayerBase* Holder = AttachmentState.Holder;
	USceneComponent* Parent = IsValid(Holder) ? Holder->GetPickupInspectionComponent() : nullptr;
	if (!Parent || GetRootComponent()->GetAttachParent() != Parent) return;
	const FRotator DisplayRotation = !HasAuthority() && Holder->IsLocallyControlled()
		? GetInspectionRotation() : AttachmentState.Inspection.Rotation;
	SetActorRelativeTransform(BuildInspectionRelativeTransform(Parent, DisplayRotation));
}

bool AECPPickupItem::ApplyAttachmentState()
{
	UPrimitiveComponent* Body = Cast<UPrimitiveComponent>(GetRootComponent());
	if (!bInitialized || !Body)
	{
		return false;
	}
	const bool bNewRevision = LastAppliedRevision != AttachmentState.Revision;
	if (HasAuthority())
	{
		// 主机逐帧权威输入不等于逐帧网络广播；离开检查态恢复世界物品原有的物理复制频率。
		SetNetUpdateFrequency(AttachmentState.State == EECPPickupState::Pickup ? 20.f : OriginalNetUpdateFrequency);
	}
	TInlineComponentArray<UPrimitiveComponent*> Components(this);
	if (AttachmentState.State != EECPPickupState::World)
	{
		// 即使 Holder 尚未映射也先停物理；不能把暂时为空的网络对象引用当作 World。
		SetReplicateMovement(false);
		for (UPrimitiveComponent* Component : Components)
		{
			Component->SetSimulatePhysics(false);
			Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			Component->SetIsReplicated(false);
		}
		bPresentedAsHeld = true;
		SetActorHiddenInGame(AttachmentState.State == EECPPickupState::Stored);
		if (AttachmentState.State == EECPPickupState::Stored)
		{
			DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
			LastAppliedRevision = AttachmentState.Revision;
			bAttachmentPresentationInitialized = true;
			return true;
		}
		AECPPlayerBase* Holder = AttachmentState.Holder;
		if (!IsValid(Holder))
		{
			return false;
		}
		if (AttachmentState.State == EECPPickupState::Pickup)
		{
			USceneComponent* Parent = Holder->GetPickupInspectionComponent();
			if (!Parent || !AttachToComponent(Parent, FAttachmentTransformRules::KeepRelativeTransform))
			{
				return false;
			}
			ApplyInspectionPresentation();
		}
		else
		{
			USkeletalMeshComponent* Parent = Holder->GetPickupAttachmentComponent();
			if (!Parent || !Parent->DoesSocketExist(Holder->GetPickupAttachmentSocket()))
			{
				return false;
			}
			// 装备态保持旧工程的 Snap 语义，同时不焊接，避免把物品质量并入角色。
			const FAttachmentTransformRules Rules(EAttachmentRule::SnapToTarget, EAttachmentRule::SnapToTarget,
				EAttachmentRule::KeepWorld, false);
			if (!AttachToComponent(Parent, Rules, Holder->GetPickupAttachmentSocket()))
			{
				return false;
			}
		}
	}
	else
	{
		const bool bApplyReleaseTransition = bNewRevision && bPresentedAsHeld;
		bPresentedAsHeld = false;
		SetActorHiddenInGame(false);
		DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
		// 晚加入时 ReleaseTransform 可能已是很久以前的落点；只给本机持有→释放过渡定位。
		// 首次收到 Released 保留当前 FRepMovement 位置/速度，也不让重复回调重置已经开始滚动的物品。
		if (bApplyReleaseTransition)
		{
			SetActorTransform(AttachmentState.ReleaseTransform, false, nullptr, ETeleportType::TeleportPhysics);
		}
		for (UPrimitiveComponent* Component : Components)
		{
			for (const FECPPickupComponentDefaults& Defaults : ComponentDefaults)
			{
				if (Defaults.Name == Component->GetFName())
				{
					// CollisionEnabled 不包含在 FRepMovement 内，必须由物品 RepNotify 各端显式恢复。
					Component->SetCollisionEnabled(Defaults.CollisionEnabled);
					Component->SetIsReplicated(Component == FocusVisual ? false : Defaults.bReplicated);
					break;
				}
			}
		}
		SetReplicateMovement(true);
		Body->SetSimulatePhysics(true);
		if (bApplyReleaseTransition)
		{
			Body->SetPhysicsLinearVelocity(AttachmentState.ReleaseVelocity);
		}
	}
	LastAppliedRevision = AttachmentState.Revision;
	bAttachmentPresentationInitialized = true;
	return true;
}

void AECPPickupItem::OnRep_ReplicateMovement()
{
	Super::OnRep_ReplicateMovement();
	bAttachmentPresentationInitialized = false; // 引擎移动属性可能覆盖物理开关，必须重申当前持有态。
	// 引擎移动属性与自定义状态到达顺序不保证一致，当前持有状态始终是物理开关的最终依据。
	OnRep_AttachmentState();
}

void AECPPickupItem::OnRep_AttachmentReplication()
{
	Super::OnRep_AttachmentReplication();
	bAttachmentPresentationInitialized = false; // 引擎附着更新同样需要按当前物品状态校正。
	OnRep_AttachmentState();
}

void AECPPickupItem::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	GetWorldTimerManager().ClearTimer(AttachmentRetryTimer);
	// 下一帧仲裁委托会随 Actor 销毁失效，同时清空弱引用，避免调试时误读残留请求。
	PendingPickupRequests.Reset();
	bPickupResolveScheduled = false;
	if (AECPPlayerBase* PreviousFocusOwner = BoundFocusOwner.Get())
	{
		PreviousFocusOwner->OnDestroyed.RemoveDynamic(this, &ThisClass::OnFocusOwnerDestroyed);
	}
	BoundFocusOwner.Reset();
	if (AECPPlayerBase* Previous = BoundHolder.Get())
	{
		Previous->OnDestroyed.RemoveDynamic(this, &ThisClass::OnHolderDestroyed);
	}
	Super::EndPlay(EndPlayReason);
}
