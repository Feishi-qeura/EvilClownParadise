// Fill out your copyright notice in the Description page of Project Settings.

#include "Characters/ECPCharacterMovementComponent.h"

#include "Characters/ECPPlayerBase.h"

UECPCharacterMovementComponent::UECPCharacterMovementComponent()
{
	NavAgentProps.bCanCrouch = true;

	MaxWalkSpeed = 320.f;
	MaxWalkSpeedCrouched = 160.f;
	MaxAcceleration = 2400.f;
	AirControl = 0.333f;

	// GroundFriction 不是"地面摩擦"，在 UE 里它是【改变方向的能力】：
	// CalcVelocity 用 `Velocity -= (Velocity - AccelDir*|V|) * min(dt*Friction, 1)`
	// 把垂直于输入方向的分量抹掉。设成 0 这一项就完全失效，转向只剩
	// "超过 MaxSpeed 就把速度整体缩放" 在拐方向 —— 结果是转向不掉速、像贴地滑冰。
	// Godot 是逐轴 move_toward(24 m/s²)，满速 320 时等价于速率 2400/320 = 7.5。
	GroundFriction = 7.5f;

	// 但 GroundFriction 也会被 ApplyVelocityBraking 当刹车用，而且还要乘
	// BrakingFrictionFactor（引擎默认 2.0）—— 那会让停止时间直接砍半。
	// 开这个开关让刹车只认 BrakingFriction，减速完全由 BrakingDecelerationWalking 决定。
	bUseSeparateBrakingFriction = true;
	BrakingFriction = 0.f;

	BrakingDecelerationWalking = 4000.f;

	JumpZVelocity = 480.f;
}

void UECPCharacterMovementComponent::CutJump()
{
	if (Velocity.Z > 0.f)
	{
		Velocity.Z *= JumpCutMultiplier;
	}
}

float UECPCharacterMovementComponent::GetGravityZ() const
{
	const float BaseGravityZ = Super::GetGravityZ();

	if (Velocity.Z < 0.f)
	{
		return BaseGravityZ * FallGravityScale;
	}

	return BaseGravityZ;
}

float UECPCharacterMovementComponent::GetMaxSpeed() const
{
	// 游泳 / 飞行 / 自定义模式交回引擎默认实现，免得以后加这些模式时
	// 被这里锁死在走路速度上（默认实现里它们各有各的 Max*Speed）。
	if (MovementMode != MOVE_Walking && MovementMode != MOVE_NavWalking && MovementMode != MOVE_Falling)
	{
		return Super::GetMaxSpeed();
	}

	float Base = MaxWalkSpeed;
	if (IsCrouching())
	{
		Base = MaxWalkSpeedCrouched;
	}
	// 冲刺倍率只在【地面】生效。空中上限要用 Godot 的 air_speed（= MaxWalkSpeed），
	// 否则按住 Shift+W 起跳后能在空中一路加速到 600（Godot 只能到 320）。
	// 冲刺跳本来就有的 600 水平速度不受影响：PhysFalling 用 FallingLateralFriction(0)
	// 和 BrakingDecelerationFalling(0) 调 CalcVelocity，刹车分支会直接 early-out。
	else if (bSprinting && IsMovingOnGround())
	{
		Base = MaxWalkSpeed * SprintSpeedScale;
	}
	return Base * GetLandControl();
}

void UECPCharacterMovementComponent::UpdateCharacterStateBeforeMovement(float DeltaSeconds)
{
	AECPPlayerBase* PlayerOwner = Cast<AECPPlayerBase>(CharacterOwner);

	if (PlayerOwner)
	{
		// 蹲着按跳 → 先站起来再跳。Godot 的 crouch_state 就是
		// `consume_jump() and can_stand()` 然后 set_crouching(false) + do_jump()。
		// 头顶没空间时 Super 里的 UnCrouch 会失败，bIsCrouched 保持 true，
		// 于是 IsCrouching() 仍为真 → 跳不出去，和 Godot 的 can_stand() 判定一致。
		// 用 JumpBufferRemaining 是为了让"落地前提前按跳"也能先站起来，不丢缓冲。
		const bool bJumpPending = PlayerOwner->bJumpRequested || JumpBufferRemaining > 0.f;
		bWantsToCrouch = PlayerOwner->bWantsToCrouch && !bJumpPending;
	}

	Super::UpdateCharacterStateBeforeMovement(DeltaSeconds);

	if (!PlayerOwner)
	{
		return;
	}

	bHasMoveInput = PlayerOwner->MoveInput.SizeSquared() > 0.01f;

	const bool bForwardInput = PlayerOwner->MoveInput.X > SprintForwardInputMin;
	// 注意：落地锁定【不】屏蔽冲刺。
	// Godot 的 land_state 是 `run_speed if wants_sprint() else walk_speed` 再乘 control，
	// 也就是锁定期间仍按冲刺档减速。这里屏蔽掉会让惩罚目标直接砍半。
	bSprinting = PlayerOwner->bWantsToSprint && bForwardInput && !PlayerOwner->bWantsToCrouch;

	if (IsMovingOnGround())
	{
		CoyoteRemaining = CoyoteTime;
	}
	else
	{
		CoyoteRemaining = FMath::Max((CoyoteRemaining - DeltaSeconds), 0.f);
	}

	if (PlayerOwner->bJumpRequested)
	{
		JumpBufferRemaining = JumpBufferTime;
		PlayerOwner->bJumpRequested = false;
	}
	else
	{
		JumpBufferRemaining = FMath::Max((JumpBufferRemaining - DeltaSeconds), 0.f);
	}

	// 落地锁定期间能不能跳：软落地全程可取消；中/硬落地只在剩余 40% 窗口内可取消。
	// 对应 Godot 的 land_state.gd：can_cancel = _kind == "soft" or _timer <= land_duration * 0.4
	bool bLandAllowsJump = true;
	if (LandLockRemaining > 0.f)
	{
		bLandAllowsJump =
		    (LandKind == EECPLandKind::Soft) || (LandLockRemaining <= GetLandLockDuration() * LandCancelWindowRatio);
	}

	const bool bCanJumpNow =
	    JumpBufferRemaining > 0.f && (IsMovingOnGround() || CoyoteRemaining > 0.f) && !IsCrouching() && bLandAllowsJump;

	if (bCanJumpNow)
	{
		Velocity.Z = JumpZVelocity;
		SetMovementMode(MOVE_Falling);
		bNotifyApex = true;

		JumpBufferRemaining = 0.0f;
		CoyoteRemaining = 0.0f;

		// 起跳也要清掉落地锁定 —— Godot 的 do_jump 里就写了 land_locked = false。
		// 不清的话已经在空中了，UpdateMovementState 还会因为 LandLockRemaining > 0
		// 一直报 Landing，动画和速度都会被拖住。
		LandLockRemaining = 0.0f;
		LandKind = EECPLandKind::None;
	}

	if (IsFalling())
	{
		PeakFallSpeed = FMath::Max(PeakFallSpeed, -Velocity.Z);
	}

	LandLockRemaining = FMath::Max(LandLockRemaining - DeltaSeconds, 0.0f);

	// 移动取消：Godot 的 land_state 里，只要玩家有移动输入且 can_cancel，
	// 就直接切回 walk/run —— 也就是【提前退出落地状态】，速度惩罚立即消失。
	//   软落地：can_cancel 恒为真 → 一动就取消（惩罚最多存在一帧）
	//   中落地：最后 40% 可取消
	//   硬落地：不能取消
	// 不实现这个的话，惩罚会持续满时长，软/中落地的体感会明显偏重。
	if (LandLockRemaining > 0.0f && LandKind != EECPLandKind::Hard && bHasMoveInput)
	{
		const bool bCanCancelMove =
		    (LandKind == EECPLandKind::Soft) || (LandLockRemaining <= GetLandLockDuration() * LandCancelWindowRatio);

		if (bCanCancelMove)
		{
			LandLockRemaining = 0.0f;
		}
	}

	if (LandLockRemaining <= 0.0f)
	{
		LandKind = EECPLandKind::None;
	}

	UpdateMovementState();
}

void UECPCharacterMovementComponent::OnMovementModeChanged(EMovementMode PreviousMovementMode, uint8 PreviousCustomMode)
{
	Super::OnMovementModeChanged(PreviousMovementMode, PreviousCustomMode);

	if (MovementMode == EMovementMode::MOVE_Falling && PreviousMovementMode != MOVE_Falling)
	{
		PeakFallSpeed = 0.0f;
	}

	if (PreviousMovementMode == EMovementMode::MOVE_Falling &&
	    (MovementMode == MOVE_Walking || MovementMode == MOVE_NavWalking))
	{
		ClassifyLanding();
	}

	// 移动模式变了，状态多半也变了，必须在这里补一次推导 ——
	// UpdateMovementState() 在物理步进【之前】就跑完了，而落地检测发生在步进【之中】，
	// 不补的话落地/起跳那一帧广播的会是旧状态（动画晚一帧）。
	UpdateMovementState();
}

void UECPCharacterMovementComponent::UpdateMovementState()
{
	EECPMovementState NewState;

	if (LandLockRemaining > 0.0f)
	{
		NewState = EECPMovementState::Landing;
	}
	else if (!IsMovingOnGround())
	{
		NewState = EECPMovementState::Air;
	}
	else if (IsCrouching())
	{
		NewState = EECPMovementState::Crouching;
	}
	else if (!bHasMoveInput)
	{
		NewState = EECPMovementState::Idle;
	}
	else if (bSprinting)
	{
		NewState = EECPMovementState::Running;
	}
	else
	{
		NewState = EECPMovementState::Walking;
	}

	if (NewState != CurrentMovementState)
	{
		const EECPMovementState OldState = CurrentMovementState;
		CurrentMovementState = NewState;
		OnMovementStateChanged.Broadcast(OldState, NewState);
	}
}

void UECPCharacterMovementComponent::ClassifyLanding()
{
	if (PeakFallSpeed >= HardLandSpeed)
	{
		LandKind = EECPLandKind::Hard;
		LandLockRemaining = HardLandTime;
	}
	else if (PeakFallSpeed >= MediumLandSpeed)
	{
		LandKind = EECPLandKind::Medium;
		LandLockRemaining = MediumLandTime;
	}
	else if (PeakFallSpeed >= SoftLandSpeed)
	{
		LandKind = EECPLandKind::Soft;
		LandLockRemaining = SoftLandTime;
	}
	else
	{
		LandKind = EECPLandKind::None;
		LandLockRemaining = 0.0f;
	}

	// 落地冲击（相机下压 + 俯仰）不在这里广播：相机层是自己在 ModifyCamera 里
	// 轮询 CurrentMovementState 的 Air 边沿，并按 PeakFallSpeed 缩放冲击强度的。
	// 这里只要保证 LandKind / LandLockRemaining 是对的即可。
}

float UECPCharacterMovementComponent::GetLandControl() const
{
	if (LandLockRemaining <= 0.0f)
	{
		return 1.0f;
	}

	switch (LandKind)
	{
	case EECPLandKind::Soft:
		return LandSoftControl;
	case EECPLandKind::Medium:
		return LandMediumControl;
	case EECPLandKind::Hard:
		return LandHardControl;
	default:
		return 1.0f;
	}
}

float UECPCharacterMovementComponent::GetLandLockDuration() const
{
	switch (LandKind)
	{
	case EECPLandKind::Soft:
		return SoftLandTime;
	case EECPLandKind::Medium:
		return MediumLandTime;
	case EECPLandKind::Hard:
		return HardLandTime;
	default:
		return 0.0f;
	}
}
