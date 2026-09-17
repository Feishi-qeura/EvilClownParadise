// Fill out your copyright notice in the Description page of Project Settings.

#include "Camera/ECPCameraModifier.h"

#include "Camera/CameraTypes.h"
#include "Camera/PlayerCameraManager.h"
#include "Characters/ECPCharacterMovementComponent.h"
#include "Characters/ECPPlayerBase.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/PlayerController.h"

namespace
{
// 抖动噪声的采样通道常量（和 Godot 的 _apply_rig 一致）。
// 用不同的通道坐标把各轴去相关，否则六路会同步抖成一条直线。
//
// 旋转用 FRotator 的三个分量，没有轴向歧义：
constexpr float ShakeChannelPitch = 11.f;
constexpr float ShakeChannelYaw = 37.f;
constexpr float ShakeChannelRoll = 73.f;
// 位移要注意轴向：Godot 的源顺序是 (x=左右, y=上下, z=前后)，
// 这里按 UE 轴向重新命名，免得再犯"Y 到底是上下还是左右"的错。
constexpr float ShakeChannelRight = 101.f;   // Godot 的 x
constexpr float ShakeChannelUp = 131.f;      // Godot 的 y
constexpr float ShakeChannelForward = 167.f; // Godot 的 z

FORCEINLINE float NoiseAt(float Channel, float T)
{
	// UE 的是 Perlin，Godot 那边是 Simplex。作为抖动源观感一致，不需要逐值对齐。
	return FMath::PerlinNoise2D(FVector2D(Channel, T));
}
}

float UECPCameraModifier::ExpInterp(float Current, float Target, float DeltaTime, float Rate)
{
	// 精确对应 Godot 的 lerpf(cur, tgt, 1 - exp(-k * delta))。
	// 不要换成 FMath::FInterpTo —— 它是 Dist * Clamp(dt*speed, 0, 1)，
	// 数值和曲线形状都不一样（会在 1/speed 秒时精确到达而非渐近）。
	return FMath::Lerp(Current, Target, 1.f - FMath::Exp(-Rate * DeltaTime));
}

void UECPCameraModifier::AddTrauma(float Amount) { Trauma = FMath::Clamp(FMath::Max(Trauma, Amount), 0.f, 1.f); }

AECPPlayerBase* UECPCameraModifier::GetViewPlayer() const
{
	const APlayerCameraManager* Manager = CameraOwner;
	if (!Manager)
	{
		return nullptr;
	}

	const APlayerController* PC = Manager->GetOwningPlayerController();
	return PC ? Cast<AECPPlayerBase>(PC->GetPawn()) : nullptr;
}

bool UECPCameraModifier::ModifyCamera(float DeltaTime, struct FMinimalViewInfo& InOutPOV)
{
	// 必须调 Super：它负责 UpdateAlpha（Enable/Disable 的淡入淡出）
	// 以及"Alpha 归零后真正把自己禁用掉"
	Super::ModifyCamera(DeltaTime, InOutPOV);

	AECPPlayerBase* Player = GetViewPlayer();
	if (!Player)
	{
		// 没角色（主菜单 / 未附身）：全部重置，下次附身时直接吸附到目标值
		SmoothedEyeHeight = -1.f;
		SmoothedFOV = -1.f;
		BobPhase = 0.f;
		BobAmp = FVector2D::ZeroVector;
		Kick = FVector::ZeroVector;
		KickPitch = 0.f;
		KickRoll = 0.f;
		Roll = 0.f;
		Trauma = 0.f;
		NoiseTime = 0.f;
		PrevMovementState = -1;
		return false;
	}

	DetectJumpAndLand(Player);
	TickLayers(DeltaTime, Player, InOutPOV);

	// 顺序有讲究：眼高的修正加在【世界 Z】上，而 bob/kick/shake 是【相机局部空间】的偏移。
	// 先把世界 Z 修正做完，局部偏移再按最终旋转转成世界，两者就不会互相污染。
	ApplyEyeHeight(DeltaTime, Player, InOutPOV);
	ApplyLayersToPOV(InOutPOV);

	// false = 让链上后面的修饰器继续执行
	return false;
}

// ============================================================
//  起跳 / 落地 的边沿检测
// ============================================================
void UECPCameraModifier::DetectJumpAndLand(AECPPlayerBase* Player)
{
	const UECPCharacterMovementComponent* Move = Player->GetECPMovement();
	if (!Move)
	{
		return;
	}

	const int32 NowState = static_cast<int32>(Move->CurrentMovementState);

	// 第一帧只记录，不做边沿判定
	if (PrevMovementState >= 0 && NowState != PrevMovementState)
	{
		const EECPMovementState Prev = static_cast<EECPMovementState>(PrevMovementState);
		const EECPMovementState Now = Move->CurrentMovementState;
		const bool bPrevInAir = (Prev == EECPMovementState::Air);
		const bool bNowInAir = (Now == EECPMovementState::Air);

		// ---- 起跳：从非空中进入空中，且垂直速度向上 ----
		// 加垂直速度条件是排除"走下平台"（那种情况垂直速度是负的）
		if (bNowInAir && !bPrevInAir && Player->GetVelocity().Z > 0.f)
		{
			Kick.Z += JumpKickHeight; // UE: Z = 上下
			KickPitch += JumpKickPitchDegrees;
		}

		// ---- 落地冲击：从空中回到非空中 ----
		// 注意：这和"落地锁定"是两套独立系统。Godot 的门槛是 1.2 m/s，
		// 而落地分级的门槛是 5.5 m/s，所以绝大多数落地只有冲击、没有锁定。
		// PeakFallSpeed 在落地后仍然保留（下次进入下落时才清零），这里可以安全读。
		if (bPrevInAir && !bNowInAir && Move->PeakFallSpeed >= LandKickMinImpact)
		{
			const float Scale =
			    FMath::Clamp(FMath::GetRangePct(LandKickImpactMin, LandKickImpactMax, Move->PeakFallSpeed),
			                 LandKickScaleMin, LandKickScaleMax);

			Kick.Z -= LandKickDepth * Scale;                         // UE: Z = 上下（下压）
			Kick.Y += FMath::FRandRange(-0.5f, 0.5f) * 0.6f * Scale; // UE: Y = 左右（Godot: (rand-0.5)*0.006 m）
			KickPitch -= LandKickPitchDegrees * Scale;
		}
	}

	PrevMovementState = NowState;
}

// ============================================================
//  推进所有层
// ============================================================
float UECPCameraModifier::VerticalFOVToHorizontalFOV(float VerticalDegrees, float Aspect)
{
	// 投影关系：tan(h/2) = tan(v/2) * aspect
	// 所以比例作用在 tan 域 —— 这是最容易算错的一步
	const float HalfTan = FMath::Tan(FMath::DegreesToRadians(VerticalDegrees * 0.5f)) * FMath::Max(Aspect, 0.01f);
	return FMath::RadiansToDegrees(2.f * FMath::Atan(HalfTan));
}

void UECPCameraModifier::TickLayers(float DeltaTime, AECPPlayerBase* Player, const FMinimalViewInfo& View)
{
	const UECPCharacterMovementComponent* Move = Player->GetECPMovement();
	if (!Move)
	{
		return;
	}

	const float Speed2D = Player->GetVelocity().Size2D();
	const float WalkSpeed = FMath::Max(Move->MaxWalkSpeed, 1.f);
	const float RunSpeed = WalkSpeed * FMath::Max(Move->SprintSpeedScale, 0.01f);

	// Godot: grounded = is_on_floor() and not land_locked
	// 也就是落地锁定期间 bob 会停 —— 这正是"落地要沉一下"的一部分
	const bool bGrounded = Move->IsMovingOnGround() && Move->LandLockRemaining <= 0.f;
	const bool bCrouching = Move->IsCrouching();

	// Godot 的 view 侧冲刺判定比状态机严一点：还要求速度超过走路速度的 0.7 倍，
	// 这样冲刺 FOV 只在真的跑起来之后才切，不会一按 Shift 就变焦。
	const bool bSprinting = Move->bSprinting && Speed2D > WalkSpeed * SprintFOVSpeedRatio;

	// ---------------- bob ----------------
	FVector2D TargetAmp = FVector2D::ZeroVector;
	float Interval = BobIntervalWalk;

	if (bGrounded && Speed2D > BobMinSpeed)
	{
		if (bCrouching)
		{
			TargetAmp = BobCrouch;
			Interval = BobIntervalCrouch;
		}
		else if (bSprinting)
		{
			TargetAmp = BobRun;
			Interval = BobIntervalSprint;
		}
		else
		{
			// 走~跑之间连续混合（Godot: inverse_lerp(walk*0.4, run, speed)）
			const float AmpAlpha =
			    FMath::Clamp((Speed2D - WalkSpeed * 0.4f) / FMath::Max(RunSpeed - WalkSpeed * 0.4f, 1.f), 0.f, 1.f);
			TargetAmp = FMath::Lerp(BobWalk, BobRun, AmpAlpha);

			const float IntervalAlpha =
			    FMath::Clamp((Speed2D - WalkSpeed) / FMath::Max(RunSpeed - WalkSpeed, 1.f), 0.f, 1.f);
			Interval = FMath::Lerp(BobIntervalWalk, BobIntervalSprint, IntervalAlpha);
		}

		BobPhase += DeltaTime * UE_TWO_PI / FMath::Max(Interval, 0.05f);
	}

	BobAmp = FMath::Lerp(BobAmp, TargetAmp, 1.f - FMath::Exp(-BobAmpInterpRate * DeltaTime));

	// ---------------- 冲击回正 ----------------
	const float KickAlpha = 1.f - FMath::Exp(-KickRecoverRate * DeltaTime);
	Kick = FMath::Lerp(Kick, FVector::ZeroVector, KickAlpha);
	KickPitch = FMath::Lerp(KickPitch, 0.f, KickAlpha);
	KickRoll = FMath::Lerp(KickRoll, 0.f, KickAlpha);

	// ---------------- 侧移倾斜 ----------------
	// Godot 取的是"角色本地空间前进方向"的横向分量；UE 里 MoveInput.Y 就是它。
	// 注意：Godot 在本地的 wish_dir 长度为 0 时把 wish_x 当 0，所以直接读输入即可。
	float TargetRoll = -Player->MoveInput.Y * StrafeRollDegrees * FMath::Clamp(Speed2D / WalkSpeed, 0.f, 1.f);
	if (!bGrounded)
	{
		TargetRoll *= StrafeRollAirScale;
	}
	Roll = ExpInterp(Roll, TargetRoll, DeltaTime, StrafeRollInterpRate);

	// ---------------- 抖动衰减 ----------------
	if (Trauma > 0.f)
	{
		NoiseTime += DeltaTime;
		Trauma = FMath::Max(Trauma - TraumaDecay * DeltaTime, 0.f);
	}

	// ---------------- FOV ----------------
	// 引擎每帧都重算 POV，所以 View.FOV 就是相机组件的基准值（腰射 FOV）。
	// 冲刺 FOV 是绝对角度，要按 tan 域换算成水平域 —— 不能直接乘。
	// 开镜那一档等接了武器（有 ads 进度和 ads_fov）再加，现在只有基础、冲刺两档。
	const float BaseFOV = View.FOV;
	const float Aspect = View.AspectRatio > 0.f ? View.AspectRatio : 1.777778f;
	const float SprintFOV = VerticalFOVToHorizontalFOV(SprintFOVDegrees, Aspect);
	const float TargetFOV = (bSprinting && bGrounded) ? SprintFOV : BaseFOV;

	if (SmoothedFOV < 0.f)
	{
		SmoothedFOV = TargetFOV;
	}
	else
	{
		SmoothedFOV = ExpInterp(SmoothedFOV, TargetFOV, DeltaTime, FOVInterpRate);
	}
}

// ============================================================
//  眼高（蹲伏平滑）
// ============================================================
void UECPCameraModifier::ApplyEyeHeight(float DeltaTime, AECPPlayerBase* Player, FMinimalViewInfo& InOutPOV)
{
	const UCapsuleComponent* Capsule = Player->GetCapsuleComponent();
	const UCharacterMovementComponent* Move = Player->GetCharacterMovement();
	if (!Capsule || !Move)
	{
		return;
	}

	const float TargetEyeHeight = Move->IsCrouching() ? CrouchEyeHeight : StandEyeHeight;

	if (SmoothedEyeHeight < 0.f)
	{
		// 首帧直接吸附，否则会看到相机从 0 自己往上飘
		SmoothedEyeHeight = TargetEyeHeight;
	}
	else
	{
		SmoothedEyeHeight = ExpInterp(SmoothedEyeHeight, TargetEyeHeight, DeltaTime, EyeHeightInterpRate);
	}

	// 相机现在实际在脚底上方多高，然后把它修正到插值后的眼高。
	// 用"加差值"而不是直接赋值，这样不会盖掉后面要叠的层。
	const float FeetZ = Player->GetActorLocation().Z - Capsule->GetScaledCapsuleHalfHeight();
	const float CurrentEyeHeight = InOutPOV.Location.Z - FeetZ;

	InOutPOV.Location.Z += (SmoothedEyeHeight - CurrentEyeHeight);
}

// ============================================================
//  把所有层叠加到 POV
// ============================================================
void UECPCameraModifier::ApplyLayersToPOV(FMinimalViewInfo& InOutPOV)
{
	// ---- bob：纯平移，垂直方向 2 倍频 ----
	// Godot 是 Vector3(sin(t)*amp.x, abs(sin(2t))*amp.y, ...)，其中 x=左右、y=上下。
	// UE 的 FVector 是 (前后, 左右, 上下)，所以横向进 Y、垂直进 Z，前后恒为 0。
	// 一个步幅（BobPhase 走完一个 2π）里：
	//   左右 1 次（sin 的周期是 2π）
	//   上下 2 次（abs(sin) 的周期是 π —— 一步一次，和脚步音对齐）
	// 注意别写成 abs(sin(BobPhase * 2))：那会变成 4 次/步幅，
	// 和"每步一声"的脚步音对不上（耳朵听 2 次、镜头颠 4 次）。
	const FVector Bob(0.f,                                          // 前后
	                  FMath::Sin(BobPhase) * BobAmp.X,              // 左右
	                  FMath::Abs(FMath::Sin(BobPhase)) * BobAmp.Y); // 上下

	// ---- 抖动：幅度按 trauma²（高 trauma 强烈、低 trauma 迅速平息）----
	FVector ShakePos = FVector::ZeroVector;
	FVector ShakeRot = FVector::ZeroVector; // 度：X=俯仰 Y=偏航 Z=横滚
	if (Trauma > 0.f)
	{
		const float S = Trauma * Trauma;
		const float T = NoiseTime * ShakeFrequency;

		ShakeRot.X = NoiseAt(ShakeChannelPitch, T) * ShakeRotMaxDegrees.X * S;
		ShakeRot.Y = NoiseAt(ShakeChannelYaw, T) * ShakeRotMaxDegrees.Y * S;
		ShakeRot.Z = NoiseAt(ShakeChannelRoll, T) * ShakeRotMaxDegrees.Z * S;

		ShakePos.X = NoiseAt(ShakeChannelForward, T) * ShakePosMax.X * S;
		ShakePos.Y = NoiseAt(ShakeChannelRight, T) * ShakePosMax.Y * S;
		ShakePos.Z = NoiseAt(ShakeChannelUp, T) * ShakePosMax.Z * S;
	}

	// ---- 先加旋转，再用最终旋转把局部位置偏移转到世界 ----
	InOutPOV.Rotation.Pitch += KickPitch + ShakeRot.X;
	InOutPOV.Rotation.Yaw += ShakeRot.Y;
	InOutPOV.Rotation.Roll += Roll + KickRoll + ShakeRot.Z;
	InOutPOV.Rotation.Normalize();

	InOutPOV.Location += InOutPOV.Rotation.RotateVector(Bob + Kick + ShakePos);

	// ---- FOV ----
	if (SmoothedFOV > 0.f)
	{
		InOutPOV.FOV = SmoothedFOV;
	}
}
