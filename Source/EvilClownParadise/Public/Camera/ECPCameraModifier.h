// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Camera/CameraModifier.h"
#include "ECPCameraModifier.generated.h"

class AECPPlayerBase;
struct FMinimalViewInfo;

/**
 * 第一人称相机叠加层，对应 Godot 项目的 Header/View 那层节点（player_view.gd）。
 *
 * 为什么所有叠加都必须在这里做：
 * 相机开了 bUsePawnControlRotation，世界旋转每帧会被引擎强制覆盖
 * （UCameraComponent::GetCameraView 里直接 SetWorldRotation），
 * 父节点上的旋转一律无效。这个钩子跑在视图目标算完之后，是唯一的落点。
 * 而且它每帧调用一次（渲染帧），比角色 Tick 的时机更准。
 *
 * 层叠顺序（和 Godot 的 _apply_rig 一致）：
 *   位置 = bob + kick + shake_pos          （相机局部空间）
 *   旋转 = (kick_pitch + shake.x, shake.y, roll + kick_roll + shake.z)
 *
 * 注意：Godot 的所有插值都是 1 - exp(-k*dt)，UE 没有等价的内建函数
 * （FMath::FInterpTo 是"钳位线性"，形状不同），所以本文件统一用
 * ExpInterp() 手算。
 */
UCLASS()
class EVILCLOWNPARADISE_API UECPCameraModifier : public UCameraModifier
{
	GENERATED_BODY()

public:
	// ==================== 眼高 ====================
	// Godot 的 PlayerView：站立 1.6 m、蹲伏 1.05 m，过渡 k=14

	/** 站立眼高（相对脚底，cm） */
	UPROPERTY(EditAnywhere, Category = "ECP|3C|Camera|EyeHeight")
	float StandEyeHeight = 160.f;

	/** 蹲伏眼高（相对脚底，cm）。它和蹲伏胶囊半高（60）是两回事 */
	UPROPERTY(EditAnywhere, Category = "ECP|3C|Camera|EyeHeight")
	float CrouchEyeHeight = 105.f;

	/** 眼高过渡速率 k（1/s）。Godot camera_lerp = 14 */
	UPROPERTY(EditAnywhere, Category = "ECP|3C|Camera|EyeHeight")
	float EyeHeightInterpRate = 14.f;

	// ==================== FOV ====================
	// 基准 FOV 由相机组件的 FieldOfView 提供（107.5 水平 = 75 垂直），这里只给冲刺档。
	//
	// ⚠ 换算陷阱：Godot 的 fov 是【垂直】、UE 的 FieldOfView 是【水平】，
	//   关系是 tan(h/2) = tan(v/2) * aspect —— 比例作用在 tan 域，不是角度本身。
	//   所以不能写 "107.5 * 1.133"，必须按下面的 VerticalFOVToHorizontalFOV 换算。
	//   （直接乘角度会把冲刺 FOV 算成 90.6° 垂直，变化幅度翻倍，体感"切得太猛"。）

	/** 冲刺时的【垂直】FOV（度）。Godot = 82（基础是 75） */
	UPROPERTY(EditAnywhere, Category = "ECP|3C|Camera|FOV")
	float SprintFOVDegrees = 82.f;

	/** 冲刺判定要求的最低速度比例（相对走路速度）。Godot 是 0.7 */
	UPROPERTY(EditAnywhere, Category = "ECP|3C|Camera|FOV")
	float SprintFOVSpeedRatio = 0.7f;

	/** FOV 过渡速率 k（1/s）。Godot = 8，这里手调降到 5（过渡时间 0.37s → 0.60s）。
	 *  参考（到 95% 的时间）：k=8 → 0.37s ｜ k=6 → 0.50s ｜ k=5 → 0.60s ｜ k=4 → 0.75s
	 *  注意：以后接开镜时会共用这个速率，但开镜通常需要更快（0.15~0.25s），
	 *        到时候要拆成两个参数（冲刺用慢的、开镜用快的）。 */
	UPROPERTY(EditAnywhere, Category = "ECP|3C|Camera|FOV")
	float FOVInterpRate = 5.f;

	// ==================== 步频晃动 bob ====================
	// 纯平移，垂直方向是 2 倍频。单位 cm（Godot 源值是米）。
	//
	// ⚠ 用 FVector2D 就是为了避免轴向事故：Godot 的 Node3D 是 (X=左右, Y=上下, Z=前后)，
	//   而 UE 的 FVector 是 (X=前后, Y=左右, Z=上下)。相机的 bob 没有前后分量，
	//   所以这里只留 (横向, 垂直) 两个分量，装不错。

	/** (横向, 垂直) cm。Godot 源值 (0.008, 0.011) 米 */
	UPROPERTY(EditAnywhere, Category = "ECP|3C|Camera|Bob")
	FVector2D BobWalk = FVector2D(0.8f, 1.1f);

	/** (横向, 垂直) cm。Godot 源值 (0.012, 0.016) 米。
	 *  这里上调到走路的 2 倍（Godot 原本是 1.5 倍），让冲刺的颠簸明显区别于走路。
	 *  连上频率一起看：冲刺是 3.17 Hz + 2 倍幅度，走路是 2.35 Hz + 1 倍幅度。 */
	UPROPERTY(EditAnywhere, Category = "ECP|3C|Camera|Bob")
	FVector2D BobRun = FVector2D(1.6f, 2.2f);

	/** (横向, 垂直) cm。Godot 源值 (0.005, 0.006) 米 */
	UPROPERTY(EditAnywhere, Category = "ECP|3C|Camera|Bob")
	FVector2D BobCrouch = FVector2D(0.5f, 0.6f);

	// 步频周期（秒）= 一个【步幅】（含两步）的时长。
	// Godot 原值 0.66 / 0.50 / 0.88，这里按真人步频重算、并整体加快约 15%：
	//   interval = 120 / 步频(步/分)
	//   走   0.85s → 141 步/分（偏快走）    → 垂直 bob 2.35 Hz
	//   冲刺 0.63s → 190 步/分（跑步）      → 垂直 bob 3.17 Hz
	//   蹲   1.20s → 100 步/分（正常走路）  → 垂直 bob 1.67 Hz
	//
	// ⚠ 一个步幅里应该发生两次"落脚"。所以三者必须一致：
	//     脚步音 2 次/interval（`step_gap = interval * 0.5`）
	//     垂直 bob 2 次/interval（`abs(sin(BobPhase))`）
	//   实际实现脚步音时请沿用这三个 interval，不要再单独取数。

	/** 走路步幅时长（秒） */
	UPROPERTY(EditAnywhere, Category = "ECP|3C|Camera|Bob")
	float BobIntervalWalk = 0.85f;

	/** 冲刺步幅时长（秒） */
	UPROPERTY(EditAnywhere, Category = "ECP|3C|Camera|Bob")
	float BobIntervalSprint = 0.63f;

	/** 蹲行步幅时长（秒） */
	UPROPERTY(EditAnywhere, Category = "ECP|3C|Camera|Bob")
	float BobIntervalCrouch = 1.20f;

	/** 幅度混合速率 k（1/s）。Godot = 12 */
	UPROPERTY(EditAnywhere, Category = "ECP|3C|Camera|Bob")
	float BobAmpInterpRate = 12.f;

	/** 低于这个水平速度不晃（cm/s）。Godot = 0.35 m/s */
	UPROPERTY(EditAnywhere, Category = "ECP|3C|Camera|Bob")
	float BobMinSpeed = 35.f;

	// ==================== 侧移倾斜 ====================
	// 注意：方向符号请对照 Godot 版本确认，左右相反就把数值改成负数

	/** 满速侧移时的最大倾斜角（度） */
	UPROPERTY(EditAnywhere, Category = "ECP|3C|Camera|StrafeRoll")
	float StrafeRollDegrees = 1.35f;

	/** 倾斜过渡速率 k（1/s）。Godot = 10 */
	UPROPERTY(EditAnywhere, Category = "ECP|3C|Camera|StrafeRoll")
	float StrafeRollInterpRate = 10.f;

	/** 滞空时倾斜的衰减比例。Godot = 0.35 */
	UPROPERTY(EditAnywhere, Category = "ECP|3C|Camera|StrafeRoll")
	float StrafeRollAirScale = 0.35f;

	// ==================== 跳 / 落冲击 ====================
	// 注意：落地冲击和落地锁定是【两套独立系统】，门槛不同。
	//   冲击门槛 = LandKickMinImpact（Godot 1.2 m/s），几乎每次落地都有；
	//   锁定门槛 = CMC 的 SoftLandSpeed（5.5 m/s），只有重落地才有。

	/** 起跳瞬间的上抬冲击（cm） */
	UPROPERTY(EditAnywhere, Category = "ECP|3C|Camera|Kick")
	float JumpKickHeight = 3.2f;

	/** 起跳瞬间的抬头角度（度） */
	UPROPERTY(EditAnywhere, Category = "ECP|3C|Camera|Kick")
	float JumpKickPitchDegrees = 0.7f;

	/** 落地冲击的下压幅度（cm），会按落速缩放 */
	UPROPERTY(EditAnywhere, Category = "ECP|3C|Camera|Kick")
	float LandKickDepth = 7.0f;

	/** 落地冲击的下压角度（度），会按落速缩放 */
	UPROPERTY(EditAnywhere, Category = "ECP|3C|Camera|Kick")
	float LandKickPitchDegrees = 1.8f;

	/** 冲击回正速率 k（1/s）。Godot recover = 16 */
	UPROPERTY(EditAnywhere, Category = "ECP|3C|Camera|Kick")
	float KickRecoverRate = 16.f;

	/** 低于这个峰值下落速度不产生落地冲击（cm/s）。Godot = 1.2 m/s */
	UPROPERTY(EditAnywhere, Category = "ECP|3C|Camera|Kick")
	float LandKickMinImpact = 120.f;

	/** 落速→冲击强度的映射区间（cm/s）。Godot: inverse_lerp(2.5, 16.0) */
	UPROPERTY(EditAnywhere, Category = "ECP|3C|Camera|Kick")
	float LandKickImpactMin = 250.f;

	UPROPERTY(EditAnywhere, Category = "ECP|3C|Camera|Kick")
	float LandKickImpactMax = 1600.f;

	/** 冲击强度的上下限。Godot = [0.15, 1.0] */
	UPROPERTY(EditAnywhere, Category = "ECP|3C|Camera|Kick")
	float LandKickScaleMin = 0.15f;

	UPROPERTY(EditAnywhere, Category = "ECP|3C|Camera|Kick")
	float LandKickScaleMax = 1.0f;

	// ==================== 抖动（trauma 模型）====================
	// 目前没有注入源（受击 / 开火都还没有），等武器和伤害接上后调用 AddTrauma()。
	// 现在可以用 AECPPlayerBase::AddCameraTrauma() 手动触发来验收。

	/** 每秒衰减量。Godot = 0.45（满档约 2.2 秒归零） */
	UPROPERTY(EditAnywhere, Category = "ECP|3C|Camera|Shake")
	float TraumaDecay = 0.45f;

	/** 最大抖动角度（度），(俯仰, 偏航, 横滚)。
	 *  Godot 源值是 0.020 / 0.026 / 0.030 弧度 = 1.146 / 1.490 / 1.719 度，
	 *  这里是手调值：幅度 +22%（让抖动更有存在感） */
	UPROPERTY(EditAnywhere, Category = "ECP|3C|Camera|Shake")
	FVector ShakeRotMaxDegrees = FVector(1.4f, 1.8f, 2.1f);

	/** 最大抖动位移（cm）。UE 轴向：X=前后 Y=左右 Z=上下。
	 *  Godot 源值是 (0.016, 0.016, 0.010) 米（左右, 上下, 前后），转过轴后是 1.0/1.6/1.6，
	 *  这里是手调值：幅度约 +25% */
	UPROPERTY(EditAnywhere, Category = "ECP|3C|Camera|Shake")
	FVector ShakePosMax = FVector(1.2f, 2.0f, 2.0f);

	/** 抖动噪声频率（Hz）。这个值直接决定抖动的"性格"：
	 *    24  Godot 原值，偏"嗡嗡"的震颤
	 *    16  一般晃动
	 *     8  颠簸感 —— 能数得出"一下一下"的冲击，像走路踩到坑
	 *     5  以下开始变成"镜头在慢慢飘"，不再像抖动了
	 *  当前 8 是为了"走路颠簸"的手感手调的。 */
	UPROPERTY(EditAnywhere, Category = "ECP|3C|Camera|Shake")
	float ShakeFrequency = 8.f;

	/** 叠加一档抖动（0~1）。多次事件取最大值，避免连环受击叠成白屏 */
	UFUNCTION(BlueprintCallable, Category = "ECP|3C|Camera")
	void AddTrauma(float Amount);

	virtual bool ModifyCamera(float DeltaTime, struct FMinimalViewInfo& InOutPOV) override;

private:
	// ---- 运行时状态 ----
	float SmoothedEyeHeight = -1.f;      // 负值 = 未初始化
	float SmoothedFOV = -1.f;
	float BobPhase = 0.f;
	FVector2D BobAmp = FVector2D::ZeroVector;
	/** 冲击位移，cm，相机局部空间。UE 轴向：X=前后 Y=左右 Z=上下 */
	FVector Kick = FVector::ZeroVector;
	float KickPitch = 0.f;               // 度
	float KickRoll = 0.f;                // 度
	float Roll = 0.f;                    // 度
	float Trauma = 0.f;
	float NoiseTime = 0.f;
	int32 PrevMovementState = -1;        // 缓存的 EECPMovementState，用于边沿检测

	/** 取当前被观察的本项目角色；取不到返回 null */
	AECPPlayerBase* GetViewPlayer() const;

	/** 推进所有层（bob / 侧移 / 冲击回正 / 抖动衰减 / FOV 目标） */
	void TickLayers(float DeltaTime, AECPPlayerBase* Player, const struct FMinimalViewInfo& View);

	/** 垂直 FOV（Godot 的约定）→ 水平 FOV（UE 的约定）：tan(h/2) = tan(v/2) * aspect */
	static float VerticalFOVToHorizontalFOV(float VerticalDegrees, float Aspect);

	/** 检测"起跳"和"落地"两个边沿，注入冲击 */
	void DetectJumpAndLand(AECPPlayerBase* Player);

	/** 蹲伏眼高平滑 */
	void ApplyEyeHeight(float DeltaTime, AECPPlayerBase* Player, struct FMinimalViewInfo& InOutPOV);

	/** 把本帧累积的层叠加到 POV 上 */
	void ApplyLayersToPOV(struct FMinimalViewInfo& InOutPOV);

	/** Godot 的指数插值：cur + (tgt - cur) * (1 - exp(-k*dt)) */
	static float ExpInterp(float Current, float Target, float DeltaTime, float Rate);
};
