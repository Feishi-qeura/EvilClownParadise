// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "ECPCharacterMovementComponent.generated.h"

// 玩家的运动状态，用于驱动动画（对应 Godot 的 idle / walk / run / crouch / air / land 六态）
// 注意：移动参数不从这里取，速度走 GetMaxSpeed() 这一个出口
UENUM(BlueprintType)
enum class EECPMovementState : uint8
{
	Idle UMETA(DisplayName = "Idle"),
	Walking UMETA(DisplayName = "Walking"),
	Running UMETA(DisplayName = "Running"),
	Crouching UMETA(DisplayName = "Crouching"),
	Air UMETA(DisplayName = "Air"),
	Landing UMETA(DisplayName = "Landing"),
};

// 落地分级，对应 Godot 的三段落地表现
UENUM(BlueprintType)
enum class EECPLandKind : uint8
{
	None UMETA(DisplayName = "None"),
	Soft UMETA(DisplayName = "Soft"),
	Medium UMETA(DisplayName = "Medium"),
	Hard UMETA(DisplayName = "Hard"),
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FECPOnMovementStateChanged, EECPMovementState, OldState, EECPMovementState,
                                             NewState);

/**
 *
 */
UCLASS()
class EVILCLOWNPARADISE_API UECPCharacterMovementComponent : public UCharacterMovementComponent
{
	GENERATED_BODY()

public:
	UECPCharacterMovementComponent();

	// ==================== 重力 ====================

	// 下落加速度倍率。Godot：上升 9.8 m/s²，下落 ×1.7 = 16.66 m/s²
	UPROPERTY(EditAnywhere, Category = "ECP|3C|Gravity")
	float FallGravityScale = 1.7f;

	// ==================== 速度 ====================

	// 冲刺速度倍率。Godot 走 3.2 / 冲刺 6.0 → 1.875
	UPROPERTY(EditAnywhere, Category = "ECP|3C|Speed")
	float SprintSpeedScale = 1.875f;

	// 冲刺要求的最小向前输入。Godot 是 move_input.y < -0.15
	UPROPERTY(EditAnywhere, Category = "ECP|3C|Speed")
	float SprintForwardInputMin = 0.15f;

	// ==================== 跳跃 ====================

	// 土狼时间：离地后仍可起跳的窗口（秒）
	UPROPERTY(EditAnywhere, Category = "ECP|3C|Jump")
	float CoyoteTime = 0.12f;

	// 跳跃缓冲：落地前提前按跳仍然生效的窗口（秒）
	UPROPERTY(EditAnywhere, Category = "ECP|3C|Jump")
	float JumpBufferTime = 0.12f;

	// 松开跳跃键时上升速度乘这个系数（Godot = 0.42）
	UPROPERTY(EditAnywhere, Category = "ECP|3C|Jump")
	float JumpCutMultiplier = 0.42f;

	// 落地锁定的取消窗口：剩余时间比例小于它时允许起跳。
	// Godot 的 land_state.gd：软落地全程可取消，中/硬落地在剩余 40% 时可取消。
	UPROPERTY(EditAnywhere, Category = "ECP|3C|Jump")
	float LandCancelWindowRatio = 0.4f;

	// ==================== 落地分级 ====================

	// 分级门槛：空中峰值下落速度（cm/s，换算自 Godot 的 m/s）
	UPROPERTY(EditAnywhere, Category = "ECP|3C|Land")
	float SoftLandSpeed = 550.0f; // Godot 5.5 m/s
	UPROPERTY(EditAnywhere, Category = "ECP|3C|Land")
	float MediumLandSpeed = 900.0f; // Godot 9.0 m/s
	UPROPERTY(EditAnywhere, Category = "ECP|3C|Land")
	float HardLandSpeed = 1400.0f; // Godot 14.0 m/s

	// 三段落地的锁定时长（秒）
	UPROPERTY(EditAnywhere, Category = "ECP|3C|Land")
	float SoftLandTime = 0.10f;
	UPROPERTY(EditAnywhere, Category = "ECP|3C|Land")
	float MediumLandTime = 0.20f;
	UPROPERTY(EditAnywhere, Category = "ECP|3C|Land")
	float HardLandTime = 0.34f;

	// 三段落地期间允许的移动速度比例
	UPROPERTY(EditAnywhere, Category = "ECP|3C|Land")
	float LandSoftControl = 0.8f;
	UPROPERTY(EditAnywhere, Category = "ECP|3C|Land")
	float LandMediumControl = 0.5f;
	UPROPERTY(EditAnywhere, Category = "ECP|3C|Land")
	float LandHardControl = 0.18f;

	// ==================== 运行时状态 ====================
	// 这些是运行时值，用 VisibleInstanceOnly 而不是 EditAnywhere：
	// EditAnywhere 会让它们被存进蓝图/CDO，把运行值固化下来。
	// BlueprintReadOnly 是给动画蓝图读的。

	// 注意：不能叫 MovementState —— UNavMovementComponent 已占用该名字
	// （那是导航系统的移动能力 FMovementProperties，和这里无关）
	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category = "ECP|3C|Runtime")
	EECPMovementState CurrentMovementState = EECPMovementState::Idle;

	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category = "ECP|3C|Runtime")
	EECPLandKind LandKind = EECPLandKind::None;

	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category = "ECP|3C|Runtime")
	bool bHasMoveInput = false;

	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category = "ECP|3C|Runtime")
	bool bSprinting = false;

	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category = "ECP|3C|Runtime")
	float CoyoteRemaining = 0.0f;

	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category = "ECP|3C|Runtime")
	float JumpBufferRemaining = 0.0f;

	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category = "ECP|3C|Runtime")
	float LandLockRemaining = 0.0f;

	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category = "ECP|3C|Runtime")
	float PeakFallSpeed = 0.0f;

	// 状态变化时广播，给动画用
	UPROPERTY(BlueprintAssignable, Category = "ECP|3C")
	FECPOnMovementStateChanged OnMovementStateChanged;

	/* 接口部分 */
	void CutJump();

	virtual float GetGravityZ() const override;

	virtual float GetMaxSpeed() const override;

	virtual void UpdateCharacterStateBeforeMovement(float DeltaSeconds) override;

protected:
	virtual void OnMovementModeChanged(EMovementMode PreviousMovementMode, uint8 PreviousCustomMode) override;

private:
	// 所有状态算完之后，推导出运动状态并广播变化
	void UpdateMovementState();

	// 落地分级：按空中峰值下落速度判定并开启锁定。
	// 不叫 ProcessLanding —— 引擎有 ProcessLanded，差一个字母容易看错。
	void ClassifyLanding();

	// 当前落地锁定允许的移动速度比例
	float GetLandControl() const;

	// 当前落地分级对应的锁定时长（给取消窗口算比例用）
	float GetLandLockDuration() const;
};
