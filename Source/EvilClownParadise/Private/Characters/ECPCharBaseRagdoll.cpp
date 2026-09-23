#include "Characters/ECPCharBase.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/Controller.h"
#include "Misc/Crc.h"
#include "TimerManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogECPRagdoll, Log, All);

double AECPCharBase::GetServerTime() const
{
	// 和推动物共享服务器世界时间，网络到包时刻不能当作物理发生时刻。
	const AGameStateBase* State = GetWorld()->GetGameState();
	return State ? State->GetServerWorldTimeSeconds() : GetWorld()->GetTimeSeconds();
}

void AECPCharBase::CacheSkeleton()
{
	CachedBoneNames.Reset();
	CachedSkeletonSignature = 0;
	if (const USkeletalMesh* Asset = GetMesh()->GetSkeletalMeshAsset())
	{
		const FReferenceSkeleton& Skeleton = Asset->GetRefSkeleton();
		for (int32 Index = 0; Index < Skeleton.GetNum(); ++Index)
		{
			const FName Name = Skeleton.GetBoneName(Index);
			CachedBoneNames.Add(Name);
			CachedSkeletonSignature = FCrc::StrCrc32(*Name.ToString(), CachedSkeletonSignature);
			const int32 Parent = Skeleton.GetParentIndex(Index);
			CachedSkeletonSignature = FCrc::MemCrc32(&Parent, sizeof(Parent), CachedSkeletonSignature);
		}
	}
}

void AECPCharBase::RequestRagdollToggle()
{
	// 非拥有端不得替其他玩家提交 RPC；AI 直接在服务器调用即可。
	if (HasAuthority()) ServerRequestRagdollToggle_Implementation();
	else if (IsLocallyControlled()) ServerRequestRagdollToggle();
}

void AECPCharBase::ServerRequestRagdollToggle_Implementation()
{
	const double Now = GetWorld()->GetTimeSeconds();
	if (Now - LastRagdollRequestTime < .15 || bIsDead) return;
	LastRagdollRequestTime = Now;
	if (MotionState.Phase == EECPMotionPhase::Ragdoll) TryRecoverFromRagdoll();
	else if (MotionState.Phase == EECPMotionPhase::Normal && !bMovementLocked) StartRagdoll();
}

void AECPCharBase::StartRagdoll()
{
	// 允许敌人死亡逻辑主动调用；没有物理资产或骨盆时保持原状态而不是锁死角色。
	if (!HasAuthority() || bRagdollActive || !GetMesh()->GetPhysicsAsset() || GetMesh()->GetBoneIndex(RagdollPelvisBone) == INDEX_NONE) return;
	CacheSkeleton();
	if (CachedBoneNames.IsEmpty() || CachedBoneNames.Num() > 256) return;
	++LandingGeneration;
	GetWorldTimerManager().ClearTimer(LandingFallbackTimer);
	bLocalLandingPlaying = false;
	MotionState.Sequence++;
	MotionState.Phase = EECPMotionPhase::Ragdoll;
	MotionState.Frame = GetMesh()->GetComponentTransform();
	MotionState.LinearVelocity = GetVelocity();
	MotionState.ServerTime = GetServerTime();
	MotionState.SkeletonSignature = CachedSkeletonSignature;
	MotionState.LocalTransforms.Reset();
	ApplyMotionState();
	CaptureRagdollPose();
	NextPoseSampleTime = GetWorld()->GetTimeSeconds() + 1.f / FMath::Clamp(RagdollPoseRate, 10.f, 60.f);
	ForceNetUpdate();
}

void AECPCharBase::NormalizeStandingCapsule()
{
	UCharacterMovementComponent* Movement = GetCharacterMovement();
	UCapsuleComponent* Capsule = GetCapsuleComponent();
	const UCapsuleComponent* DefaultCapsule = GetClass()->GetDefaultObject<AECPCharBase>()->GetCapsuleComponent();
	Movement->bWantsToCrouch = false;
	if (IsCrouched() || !FMath::IsNearlyEqual(Capsule->GetUnscaledCapsuleHalfHeight(), DefaultCapsule->GetUnscaledCapsuleHalfHeight())
		|| !FMath::IsNearlyEqual(Capsule->GetUnscaledCapsuleRadius(), DefaultCapsule->GetUnscaledCapsuleRadius()))
	{
		// 调用者已关闭胶囊碰撞；布娃娃开始不要求倒地位置能站直，空间检测统一留到服务器恢复。
		// ACharacter::UnCrouch 只排队下一次移动更新，因此这里立即执行 CMC 的表现恢复路径。
		Movement->UnCrouch(true);
		SetIsCrouched(false);
		Capsule->SetCapsuleSize(DefaultCapsule->GetUnscaledCapsuleRadius(), DefaultCapsule->GetUnscaledCapsuleHalfHeight(), false);
	}
}

void AECPCharBase::OnRep_IsCrouched()
{
	if (bRagdollActive || MotionState.Phase == EECPMotionPhase::Ragdoll)
	{
		// 蹲姿与 MotionState 是不同复制属性；迟到的 OnEndCrouch 不能改写已分离 Mesh 的世界坐标。
		GetCharacterMovement()->bWantsToCrouch = false;
		SetIsCrouched(false);
		return;
	}
	Super::OnRep_IsCrouched();
}

void AECPCharBase::CacheAndRequirePoseTick()
{
	if (!bPoseTickModeCached)
	{
		SavedPoseTickMode = static_cast<uint8>(GetMesh()->VisibilityBasedAnimTickOption);
		bPoseTickModeCached = true;
	}
	// 从 Landing 再被击倒时复用第一次缓存，不能把临时 AlwaysTick 当作原设置永久保留下来。
	GetMesh()->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
}

void AECPCharBase::RestorePoseTick()
{
	if (bPoseTickModeCached)
	{
		GetMesh()->VisibilityBasedAnimTickOption = static_cast<EVisibilityBasedAnimTickOption>(SavedPoseTickMode);
		bPoseTickModeCached = false;
	}
}

void AECPCharBase::EnterRagdollPresentation()
{
	USkeletalMeshComponent* MeshComponent = GetMesh();
	UCharacterMovementComponent* Movement = GetCharacterMovement();
	CacheSkeleton();
	++LandingGeneration;
	GetWorldTimerManager().ClearTimer(LandingFallbackTimer);
	bLocalLandingPlaying = false;
	bPredictedLandingPending = false;
	bPredictedLandingFinished = false;
	SetMovementLocked(true);
	StopJumping();
	if (UAnimInstance* Anim = MeshComponent->GetAnimInstance()) Anim->Montage_Stop(.1f, JumpMontage);
	bSavedMovementTick = Movement->IsComponentTickEnabled();
	if (HasAuthority())
	{
		// 复制开关本身也参与网络复制，只能由服务器保存进入前的原值。
		// 客户端的 OnRep 顺序不固定，此时可能已经收到 rag 的 false；缓存它会在恢复时
		// 覆盖服务器新发来的 true，导致 CMC 不再发送移动 RPC，下次 rag 又回到服务器旧位置。
		bSavedReplicateMovement = IsReplicatingMovement();
		bSavedMeshReplicated = MeshComponent->GetIsReplicated();
	}
	bSavedUpdateRateOptimizations = MeshComponent->bEnableUpdateRateOptimizations;
	bSavedIgnoreClientCorrections = Movement->bIgnoreClientMovementErrorChecksAndCorrection;
	bSavedIgnoreServerCorrections = Movement->bClientIgnoreMovementCorrections;
	SavedForcedLOD = MeshComponent->GetForcedLOD();
	SavedMeshCollision = MeshComponent->GetCollisionEnabled();
	SavedMeshCollisionProfile = MeshComponent->GetCollisionProfileName();
	SavedCapsuleCollision = GetCapsuleComponent()->GetCollisionEnabled();
	GetCapsuleComponent()->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	NormalizeStandingCapsule();
	// 缓存站立后的局部偏移，恢复时不能再次附上进入前的蹲姿高度。
	MeshRelativeBeforeRagdoll = MeshComponent->GetRelativeTransform();
	bRagdollActive = true;
	PoseFrame = MotionState.Frame;
	PoseBuffer.Reset();
	// 各端停止本地 CharacterMovement；复制元数据由服务器发布，避免表现回调争改收到的网络值。
	Movement->StopMovementImmediately();
	Movement->DisableMovement();
	Movement->SetComponentTickEnabled(false);
	Movement->bIgnoreClientMovementErrorChecksAndCorrection = true;
	Movement->bClientIgnoreMovementCorrections = true;
	if (HasAuthority())
	{
		SetReplicateMovement(false);
		MeshComponent->SetIsReplicated(false);
	}
	MeshComponent->DetachFromComponent(FDetachmentTransformRules::KeepWorldTransform);
	MeshComponent->SetWorldTransform(PoseFrame, false, nullptr, ETeleportType::TeleportPhysics);
	// 全骨架快照要求固定 LOD；屏幕外与专服也必须刷新骨骼。
	MeshComponent->SetForcedLOD(1);
	MeshComponent->bEnableUpdateRateOptimizations = false;
	CacheAndRequirePoseTick();
	if (HasAuthority())
	{
		MeshComponent->SetCollisionProfileName(TEXT("Ragdoll"));
		MeshComponent->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		MeshComponent->SetAllBodiesSimulatePhysics(true);
		MeshComponent->SetAllBodiesPhysicsBlendWeight(1.f);
		MeshComponent->SetAllPhysicsLinearVelocity(MotionState.LinearVelocity);
		MeshComponent->WakeAllRigidBodies();
	}
	else
	{
		// 客户端只显示服务器姿态；不能和推动物再次解算出另一套落地姿势。
		MeshComponent->SetAllBodiesSimulatePhysics(false);
		MeshComponent->SetAllBodiesPhysicsBlendWeight(0.f);
		MeshComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	}
	UpdateTickEnabled();
}

void AECPCharBase::CaptureRagdollPose()
{
	if (!HasAuthority() || !bRagdollActive) return;
	FPoseSnapshot Snapshot;
	GetMesh()->SnapshotPose(Snapshot);
	if (!Snapshot.bIsValid || Snapshot.LocalTransforms.Num() != CachedBoneNames.Num()) return;
	// Root 转到进入布娃娃时的固定参考系，根组件跟随骨盆也不会重复位移身体。
	Snapshot.LocalTransforms[0] = (Snapshot.LocalTransforms[0] * GetMesh()->GetComponentTransform()).GetRelativeTransform(PoseFrame);
	MotionState.LocalTransforms = MoveTemp(Snapshot.LocalTransforms);
	MotionState.LinearVelocity = GetMesh()->GetPhysicsLinearVelocity(RagdollPelvisBone);
	MotionState.ServerTime = GetServerTime();
	MotionState.Frame = PoseFrame;
	ForceNetUpdate();
}

void AECPCharBase::OnRep_MotionState()
{
	if (!HasActorBegunPlay())
	{
		// 初始属性可早于 BeginPlay 到达；先让组件初始偏移/骨架缓存完成，避免把分离后的世界变换当作恢复偏移。
		if (!bDeferredMotionStateApply)
		{
			bDeferredMotionStateApply = true;
			GetWorldTimerManager().SetTimerForNextTick(FTimerDelegate::CreateWeakLambda(this, [this]()
			{
				bDeferredMotionStateApply = false;
				OnRep_MotionState();
			}));
		}
		return;
	}
	ApplyMotionState();
}

void AECPCharBase::ApplyMotionState()
{
	TGuardValue<bool> Guard(bApplyingMotionState, true);
	const bool bNewSequence = AppliedSequence != MotionState.Sequence;
	const bool bNewPhase = bNewSequence || AppliedPhase != MotionState.Phase;
	AppliedSequence = MotionState.Sequence;
	AppliedPhase = MotionState.Phase;
	if (MotionState.Phase == EECPMotionPhase::Ragdoll)
	{
		if (!bRagdollActive) EnterRagdollPresentation();
		else if (bNewSequence)
		{
			// 丢包可能跳过恢复和 Normal，直接看见下一轮 rag；只重建坐标系，不覆盖第一次保存的恢复设置。
			++LandingGeneration;
			GetWorldTimerManager().ClearTimer(LandingFallbackTimer);
			bLocalLandingPlaying = false;
			bPredictedLandingPending = false;
			bPredictedLandingFinished = false;
			CacheSkeleton();
			PoseBuffer.Reset();
			PoseFrame = MotionState.Frame;
			if (!HasAuthority()) GetMesh()->SetWorldTransform(PoseFrame, false, nullptr, ETeleportType::TeleportPhysics);
		}
		// 同一属性保留最新静止帧，后加入或重获相关性的客户端也能立即显示。
		if (!HasAuthority() && MotionState.SkeletonSignature == CachedSkeletonSignature && MotionState.LocalTransforms.Num() == CachedBoneNames.Num())
		{
			if (PoseBuffer.IsEmpty() || MotionState.ServerTime > PoseBuffer.Last().ServerTime)
			{
				PoseBuffer.Add(MotionState);
				if (PoseBuffer.Num() > 8) PoseBuffer.RemoveAt(0, PoseBuffer.Num()-8, EAllowShrinking::No);
			}
		}
	}
	else if (bNewPhase)
	{
		const bool bWasRagdoll = bRagdollActive;
		if (bWasRagdoll) RestoreRagdollPresentation(MotionState.Frame);
		if (MotionState.Phase == EECPMotionPhase::RagdollLanding)
		{
			// 中途加入可能直接看见恢复阶段，仍须立起胶囊；恢复坐标已经通过服务器完整站立空间检测。
			SetActorTransform(MotionState.Frame, false, nullptr, ETeleportType::TeleportPhysics);
			if (!bWasRagdoll)
			{
				const ECollisionEnabled::Type Collision = GetCapsuleComponent()->GetCollisionEnabled();
				GetCapsuleComponent()->SetCollisionEnabled(ECollisionEnabled::NoCollision);
				NormalizeStandingCapsule();
				GetCapsuleComponent()->SetCollisionEnabled(bIsDead ? ECollisionEnabled::NoCollision : Collision);
			}
			BeginLanding(true);
		}
		else if (MotionState.Phase == EECPMotionPhase::JumpLanding)
		{
			// 自主客户端可能早已预测并完成同一次 Landing；迟到的阶段只确认，不重新播放整段。
			if (!bPredictedLandingPending || !bPredictedLandingFinished) BeginLanding(false);
		}
		else
		{
			if (bPredictedLandingPending && MotionState.Sequence != PredictedLandingBaseSequence)
			{
				// 即使中间 JumpLanding 被合并掉，较新的 Normal 也能确认这一轮已由服务器结束。
				bPredictedLandingPending = false;
				bPredictedLandingFinished = false;
			}
			if (!bLocalLandingPlaying)
			{
				// 若中间恢复阶段整段丢失，直接从 rag 收到 Normal 也要归还骨骼刷新设置。
				RestorePoseTick();
				if (!bPredictedLandingPending) SetMovementLocked(false);
			}
		}
	}
	UpdateTickEnabled();
}

bool AECPCharBase::BuildRagdollSnapshot(FPoseSnapshot& OutPose)
{
	if (HasAuthority() || !bRagdollActive || PoseBuffer.IsEmpty()) return false;
	if (!ECPRagdoll::SamplePose(PoseBuffer, GetServerTime() - RagdollInterpolationDelay, OutPose.LocalTransforms)) return false;
	OutPose.BoneNames = CachedBoneNames;
	OutPose.SkeletalMeshName = GetMesh()->GetSkeletalMeshAsset()->GetFName();
	OutPose.SnapshotName = TEXT("ECP_ServerRagdoll");
	OutPose.bIsValid = OutPose.LocalTransforms.Num() == OutPose.BoneNames.Num();
	return OutPose.bIsValid;
}

bool AECPCharBase::TryRecoverFromRagdoll()
{
	if (!HasAuthority() || !bRagdollActive || bIsDead) return false;
	const FVector Pelvis = GetMesh()->GetSocketLocation(RagdollPelvisBone);
	if (GetMesh()->GetPhysicsLinearVelocity(RagdollPelvisBone).Size() > RecoveryMaximumSpeed) return false;
	FCollisionQueryParams Query(SCENE_QUERY_STAT(ECPRagdollRecovery), false, this);
	FHitResult Floor;
	if (!GetWorld()->LineTraceSingleByChannel(Floor, Pelvis+FVector(0,0,40), Pelvis-FVector(0,0,500), ECC_Visibility, Query) || Floor.ImpactNormal.Z < GetCharacterMovement()->GetWalkableFloorZ()) return false;
	// 不能用进入前的蹲姿尺寸验证恢复空间，敌人和玩家都取各自类默认站立胶囊并应用实例缩放。
	const UCapsuleComponent* DefaultCapsule = GetClass()->GetDefaultObject<AECPCharBase>()->GetCapsuleComponent();
	const float CapsuleScale = GetCapsuleComponent()->GetShapeScale();
	const float HalfHeight = DefaultCapsule->GetUnscaledCapsuleHalfHeight() * CapsuleScale;
	const float Radius = DefaultCapsule->GetUnscaledCapsuleRadius() * CapsuleScale;
	const FVector RecoveryLocation = Floor.ImpactPoint + FVector(0,0,HalfHeight+4.f);
	// 起身前检测完整胶囊，防止推动物上方或墙内恢复导致穿透瞬移。
	if (GetWorld()->OverlapBlockingTestByChannel(RecoveryLocation, FQuat::Identity, ECC_Pawn, FCollisionShape::MakeCapsule(Radius,HalfHeight), Query)) return false;
	MotionState.Sequence++;
	MotionState.Phase = EECPMotionPhase::RagdollLanding;
	MotionState.Frame = FTransform(FRotator(0,GetActorRotation().Yaw,0),RecoveryLocation,GetActorScale3D());
	MotionState.ServerTime = GetServerTime();
	MotionState.LocalTransforms.Reset();
	ApplyMotionState();
	ForceNetUpdate();
	return true;
}

void AECPCharBase::RestoreRagdollPresentation(const FTransform& RecoveryTransform)
{
	USkeletalMeshComponent* MeshComponent = GetMesh();
	UCharacterMovementComponent* Movement = GetCharacterMovement();
	MeshComponent->SetAllBodiesSimulatePhysics(false);
	MeshComponent->SetAllBodiesPhysicsBlendWeight(0.f);
	SetActorTransform(RecoveryTransform,false,nullptr,ETeleportType::TeleportPhysics);
	MeshComponent->AttachToComponent(GetCapsuleComponent(),FAttachmentTransformRules::KeepRelativeTransform);
	MeshComponent->SetRelativeTransform(MeshRelativeBeforeRagdoll,false,nullptr,ETeleportType::TeleportPhysics);
	NormalizeStandingCapsule();
	MeshComponent->SetCollisionProfileName(SavedMeshCollisionProfile);
	MeshComponent->SetCollisionEnabled(SavedMeshCollision);
	GetCapsuleComponent()->SetCollisionEnabled(bIsDead ? ECollisionEnabled::NoCollision : SavedCapsuleCollision);
	if (HasAuthority())
	{
		// 客户端保留服务器已经送达的组件复制值，不用本地缓存覆盖较新的属性。
		MeshComponent->SetIsReplicated(bSavedMeshReplicated);
	}
	MeshComponent->SetForcedLOD(SavedForcedLOD);
	MeshComponent->bEnableUpdateRateOptimizations = bSavedUpdateRateOptimizations;
	// Landing 仍需屏幕外完成蒙太奇，原可见性优化留到真正解锁才恢复。
	Movement->SetComponentTickEnabled(bSavedMovementTick);
	Movement->bIgnoreClientMovementErrorChecksAndCorrection = bSavedIgnoreClientCorrections;
	Movement->bClientIgnoreMovementCorrections = bSavedIgnoreServerCorrections;
	if (HasAuthority())
	{
		// RagdollLanding 与 bReplicateMovement 谁先到客户端都成立：只有服务器还原复制开关。
		SetReplicateMovement(bSavedReplicateMovement);
	}
	bRagdollActive = false;
	PoseBuffer.Reset();
}

void AECPCharBase::SetMovementLocked(bool bLocked)
{
	UCharacterMovementComponent* Movement = GetCharacterMovement();
	// 本地预测完成不等于服务器已经确认，本类的其他调用也不能用旧 Normal 提前解锁。
	if (!bLocked && bPredictedLandingPending && !HasAuthority()) return;
	if (bMovementLocked == bLocked) return;
	bMovementLocked = bLocked;
	if (bLocked)
	{
		SavedMaxAcceleration = Movement->MaxAcceleration;
		SavedWalkSpeed = Movement->MaxWalkSpeed;
		SavedCrouchSpeed = Movement->MaxWalkSpeedCrouched;
		Movement->MaxAcceleration = 0;
		Movement->MaxWalkSpeed = 0;
		Movement->MaxWalkSpeedCrouched = 0;
		Movement->StopMovementImmediately();
		ConsumeMovementInputVector();
		StopJumping();
		if (Controller)
		{
			Controller->SetIgnoreMoveInput(true);
			LockedController = Controller;
		}
	}
	else
	{
		Movement->MaxAcceleration = SavedMaxAcceleration;
		Movement->MaxWalkSpeed = SavedWalkSpeed;
		Movement->MaxWalkSpeedCrouched = SavedCrouchSpeed;
		if (Movement->MovementMode == MOVE_None && !bIsDead) Movement->SetMovementMode(MOVE_Walking);
		ConsumeMovementInputVector();
		StopJumping();
		// 仅撤销本类加的一层锁，不能重置 UI 或其他玩法加的输入锁。
		if (AController* Previous = LockedController.Get()) Previous->SetIgnoreMoveInput(false);
		LockedController.Reset();
	}
	UpdateTickEnabled();
}

void AECPCharBase::BeginLanding(bool bFromRagdoll)
{
	(void)bFromRagdoll;
	SetMovementLocked(true);
	// 拥有端和模拟代理常会先从落地移动状态开始表现，收到服务器阶段时保留同一段 Landing。
	if (bLocalLandingPlaying) return;
	bLocalLandingPlaying = true;
	const uint32 Generation = ++LandingGeneration;
	// 先缓存再检查蒙太奇；无蒙太奇 AI、直接收到恢复阶段和再次击倒都必须能还原原可见性设置。
	CacheAndRequirePoseTick();
	UAnimInstance* Anim = GetMesh()->GetAnimInstance();
	if (!Anim || !JumpMontage || JumpMontage->GetSectionIndex(TEXT("Landing")) == INDEX_NONE)
	{
		// 通用敌人可以没有起身蒙太奇；用下一帧完成，避免从复制回调递归改状态。
		GetWorldTimerManager().SetTimerForNextTick(FTimerDelegate::CreateWeakLambda(this,[this,Generation](){FinishLanding(Generation);}));
		return;
	}
	const int32 Section = JumpMontage->GetSectionIndex(TEXT("Landing"));
	const float Start = JumpMontage->GetAnimCompositeSection(Section).GetTime();
	// 普通落地复用空中实例再切段；重复 Play 且不停止旧实例会留下持续贡献姿势的 DefaultLoop。
	// 布娃娃起身等没有活动实例的路径，才从 Landing 起点启动播放。
	if (!Anim->Montage_IsActive(JumpMontage))
	{
		Anim->Montage_Play(JumpMontage,1.f,EMontagePlayReturnType::MontageLength,Start,false);
	}
	// 空中阶段把 DefaultLoop 配成了自循环；落地时同时改写后继段并显式跳段，避免动画线程沿旧循环继续评估。
	Anim->Montage_SetNextSection(TEXT("Default"),TEXT("Landing"),JumpMontage);
	Anim->Montage_SetNextSection(TEXT("DefaultLoop"),TEXT("Landing"),JumpMontage);
	Anim->Montage_SetNextSection(TEXT("Landing"),NAME_None,JumpMontage);
	Anim->Montage_JumpToSection(TEXT("Landing"),JumpMontage);
	FOnMontageEnded EndDelegate;
	EndDelegate.BindWeakLambda(this,[this,Generation](UAnimMontage*,bool bInterrupted)
	{
		// 外部打断不能提前恢复移动；由原 Landing 时长的保护计时器完成。
		if (!bInterrupted) FinishLanding(Generation);
	});
	Anim->Montage_SetEndDelegate(EndDelegate,JumpMontage);
	// 保护 AI、专服与被其他系统中断的动画；正常流程由含混出结束的委托提前清理计时器。
	const float Duration = JumpMontage->GetPlayLength()-Start+JumpMontage->BlendOut.GetBlendTime()+.1f;
	GetWorldTimerManager().SetTimer(LandingFallbackTimer,FTimerDelegate::CreateWeakLambda(this,[this,Generation](){FinishLanding(Generation);}),Duration,false);
	UpdateTickEnabled();
}

void AECPCharBase::FinishLanding(uint32 Generation)
{
	if (Generation != LandingGeneration || !bLocalLandingPlaying || bRagdollActive) return;
	bLocalLandingPlaying = false;
	GetWorldTimerManager().ClearTimer(LandingFallbackTimer);
	RestorePoseTick();
	// 服务器等待自己的 Landing 完整结束；AI 无需玩家 RPC，自主客户端还要等新的服务器阶段确认。
	if (HasAuthority()) PublishNormalState();
	else if (bPredictedLandingPending)
	{
		bPredictedLandingFinished = true;
		if (MotionState.Phase == EECPMotionPhase::Normal && MotionState.Sequence != PredictedLandingBaseSequence)
		{
			bPredictedLandingPending = false;
			bPredictedLandingFinished = false;
			SetMovementLocked(false);
		}
	}
	else if (MotionState.Phase == EECPMotionPhase::Normal) SetMovementLocked(false);
	UpdateTickEnabled();
}

void AECPCharBase::PublishNormalState()
{
	MotionState.Phase = EECPMotionPhase::Normal;
	MotionState.Sequence++;
	MotionState.ServerTime = GetServerTime();
	MotionState.LocalTransforms.Reset();
	ApplyMotionState();
	ForceNetUpdate();
}

void AECPCharBase::Landed(const FHitResult& Hit)
{
	Super::Landed(Hit);
	if (bRagdollActive || bApplyingMotionState || bLocalLandingPlaying || bIsDead) return;
	if (HasAuthority())
	{
		MotionState.Phase = EECPMotionPhase::JumpLanding;
		MotionState.Sequence++;
		MotionState.ServerTime = GetServerTime();
		ApplyMotionState();
		ForceNetUpdate();
	}
	else if (IsLocallyControlled())
	{
		// 记住预测开始前的服务器轮次，区分“服务器尚未知道落地”和“本轮已经结束”两个 Normal。
		bPredictedLandingPending = true;
		bPredictedLandingFinished = false;
		PredictedLandingBaseSequence = MotionState.Sequence;
		BeginLanding(false);
	}
}

void AECPCharBase::PlayAirMontage()
{
	if (IsMovementLocked() || !JumpMontage) return;
	if (UAnimInstance* Anim = GetMesh()->GetAnimInstance())
	{
		// 网络移动纠正可能再次进入空中；沿用活动实例，避免同一跳跃资源累积多个循环。
		if (!Anim->Montage_IsActive(JumpMontage))
		{
			Anim->Montage_Play(JumpMontage,1.f,EMontagePlayReturnType::MontageLength,0.f,false);
		}
		Anim->Montage_SetNextSection(TEXT("Default"),TEXT("DefaultLoop"),JumpMontage);
		Anim->Montage_SetNextSection(TEXT("DefaultLoop"),TEXT("DefaultLoop"),JumpMontage);
		Anim->Montage_JumpToSection(GetVelocity().Z > 10 ? TEXT("Default") : TEXT("DefaultLoop"),JumpMontage);
	}
}

void AECPCharBase::OnMovementModeChanged(EMovementMode Previous, uint8 PreviousCustom)
{
	Super::OnMovementModeChanged(Previous,PreviousCustom);
	if (!bApplyingMotionState && !bRagdollActive)
	{
		if (GetCharacterMovement()->IsFalling() && Previous != MOVE_Falling)
		{
			// 服务器纠正了错误的本地落地预测时，不能继续等待一个服务器从未进入的 Landing。
			if (bPredictedLandingPending && MotionState.Phase == EECPMotionPhase::Normal)
			{
				++LandingGeneration;
				GetWorldTimerManager().ClearTimer(LandingFallbackTimer);
				bLocalLandingPlaying = false;
				bPredictedLandingPending = false;
				bPredictedLandingFinished = false;
				RestorePoseTick();
				SetMovementLocked(false);
			}
			PlayAirMontage();
		}
		else if (Previous == MOVE_Falling && GetCharacterMovement()->IsMovingOnGround()
			&& !bLocalLandingPlaying && !bIsDead)
		{
			// 移动平台或直接修正移动模式可能跳过碰撞 Landed；所有角色都要在地面转换时收尾空中循环。
			// 正常 Landed 已设置 bLocalLandingPlaying，上面的保护条件会避免重复播放和发布轮次。
			if (HasAuthority())
			{
				// 服务器必须发布 JumpLanding，才能让其他端也进入同一轮落地锁。
				Landed(FHitResult());
			}
			else
			{
				// 客户端只补表现，不等待新轮次：服务器可能早已结束本轮并处于 Normal。
				BeginLanding(false);
			}
		}
	}
	UpdateTickEnabled();
}

void AECPCharBase::Jump()
{
	if (!IsMovementLocked()) Super::Jump();
}

bool AECPCharBase::CanJumpInternal_Implementation() const
{
	return !IsMovementLocked() && Super::CanJumpInternal_Implementation();
}

void AECPCharBase::SetRunning(bool bRunning)
{
	if (!HasAuthority() && !IsLocallyControlled()) return;
	bRunningRequested = bRunning && !IsMovementLocked();
	OnRep_Running();
	if (!HasAuthority()) ServerSetRunning(bRunningRequested);
	else ForceNetUpdate();
}

void AECPCharBase::ServerSetRunning_Implementation(bool bRunning)
{
	bRunningRequested = bRunning && !IsMovementLocked();
	OnRep_Running();
	ForceNetUpdate();
}

void AECPCharBase::OnRep_Running()
{
	const float Speed = bRunningRequested ? RunSpeed : WalkSpeed;
	if (bMovementLocked) SavedWalkSpeed = Speed;
	else GetCharacterMovement()->MaxWalkSpeed = Speed;
}

void AECPCharBase::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (bRagdollActive)
	{
		// 根仅跟随骨盆，为镜头和网络相关性提供位置；已分离的 Mesh 不受根移动影响。
		SetActorLocation(GetMesh()->GetSocketLocation(RagdollPelvisBone),false,nullptr,ETeleportType::TeleportPhysics);
		if (HasAuthority() && GetWorld()->GetTimeSeconds() >= NextPoseSampleTime)
		{
			CaptureRagdollPose();
			NextPoseSampleTime = GetWorld()->GetTimeSeconds()+1.f/FMath::Clamp(RagdollPoseRate,10.f,60.f);
		}
	}
	else if (bMovementLocked)
	{
		// 服务器也禁止旧的移动输入继续累积，避免起身后跳到隐藏移动的位置。
		GetCharacterMovement()->StopMovementImmediately();
		ConsumeMovementInputVector();
		StopJumping();
	}
}

void AECPCharBase::UpdateTickEnabled()
{
	SetActorTickEnabled(bRagdollActive || bMovementLocked || (GetCharacterMovement() && GetCharacterMovement()->IsFalling()));
}

void AECPCharBase::EndPlay(const EEndPlayReason::Type Reason)
{
	GetWorldTimerManager().ClearTimer(LandingFallbackTimer);
	bPredictedLandingPending = false;
	bPredictedLandingFinished = false;
	if (AController* Previous = LockedController.Get()) Previous->SetIgnoreMoveInput(false);
	LockedController.Reset();
	Super::EndPlay(Reason);
}
