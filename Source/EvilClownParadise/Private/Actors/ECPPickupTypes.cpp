#include "Actors/ECPPickupTypes.h"

bool ECPPickupRules::CanTransition(EECPPickupState From, EECPPickupState To)
{
	// 显式列出允许边，避免新增枚举值时被默认放行并绕过服务器玩法校验。
	switch (From)
	{
	case EECPPickupState::World:
		return To == EECPPickupState::Pickup;
	case EECPPickupState::Pickup:
		return To == EECPPickupState::World || To == EECPPickupState::Equipped || To == EECPPickupState::Stored;
	case EECPPickupState::Equipped:
		return To == EECPPickupState::World || To == EECPPickupState::Stored;
	case EECPPickupState::Stored:
		return To == EECPPickupState::World;
	default:
		return false;
	}
}

bool ECPPickupRules::IsHigherPriority(const FECPPickupPriority& Left, const FECPPickupPriority& Right)
{
	// 只有同一服务器帧才比较主机和 Ping；更晚一帧的主机也不能抢走已先到的合法请求。
	if (Left.ServerFrame != Right.ServerFrame)
	{
		return Left.ServerFrame < Right.ServerFrame;
	}
	// Listen Server 主机是独立规则，不能依赖 PlayerId 恰好为零。
	if (Left.bListenServerHost != Right.bListenServerHost)
	{
		return Left.bListenServerHost;
	}
	// 远端玩家按服务器记录的 Ping 排序；不要信任客户端随请求上报的任意延迟值。
	if (Left.PingMilliseconds != Right.PingMilliseconds)
	{
		return Left.PingMilliseconds < Right.PingMilliseconds;
	}
	if (Left.JoinOrder != Right.JoinOrder)
	{
		return Left.JoinOrder < Right.JoinOrder;
	}
	// 相同玩家的重复请求保持第一次到达者，排序结果因此在所有构建中稳定。
	return Left.ArrivalOrder < Right.ArrivalOrder;
}

float ECPPickupRules::ExpectedThrowImpulse(float BaseImpulse, float PlayerStrength)
{
	// 力量作为无量纲倍率显式进入服务器冲量，负数配置不得产生反向投掷。
	return FMath::Max(0.f, BaseImpulse) * FMath::Max(0.f, PlayerStrength);
}

FRotator ECPPickupRules::ClampRotationDelta(const FRotator& Delta, float MaxDegrees)
{
	// 所有轴独立限幅，恶意客户端不能借未使用的 Roll 绕过单次旋转上限。
	const float Limit = FMath::Max(0.f, MaxDegrees);
	return FRotator(
		FMath::Clamp(Delta.Pitch, -Limit, Limit),
		FMath::Clamp(Delta.Yaw, -Limit, Limit),
		FMath::Clamp(Delta.Roll, -Limit, Limit));
}

FRotator ECPPickupRules::ResolveInspectionStartRotation(
	const FRotator& Authoritative, const FRotator& Predicted, bool bHasPrediction)
{
	// 可靠定稿尚未往返时，下一次快速拖动必须延续本地绝对预测，不能跳回旧复制角度。
	return bHasPrediction ? Predicted : Authoritative;
}

float ECPPickupRules::ExpectedVelocityDelta(float ImpulseStrength, float MassKg)
{
	// Chaos 的 AddImpulse 遵循 Δv=Impulse/Mass；这里只暴露同一公式用于规则测试和调参预览。
	return FMath::Max(0.f, ImpulseStrength) / FMath::Max(0.1f, MassKg);
}


bool ECPPickupRules::IsNewerInspectionSequence(uint32 Candidate, uint32 Previous)
{
	// 半区间比较同时处理 uint32 回绕；零留给尚未开始的会话，不依赖有符号溢出。
	const uint32 Distance = Candidate - Previous;
	return Distance != 0 && Distance < 0x80000000u;
}

bool ECPPickupRules::CanAcceptInspectionSample(
	const FECPInspectionRotationSample& Accepted, const FECPInspectionRotationSample& Incoming)
{
	// 网络入口独立验证有限角度、服务器代际和非零令牌，不能只信本地输入已校验。
	if (Incoming.Generation == 0 || Incoming.Generation != Accepted.Generation
		|| Incoming.Session == 0 || Incoming.Sequence == 0 || Incoming.Rotation.ContainsNaN())
	{
		return false;
	}
	if (Incoming.Session != Accepted.Session)
	{
		return Accepted.Session == 0 || IsNewerInspectionSequence(Incoming.Session, Accepted.Session);
	}
	// Final 是关闭会话的屏障；即使后到流包声称序号更大，也不能重开已结束的拖动。
	return !Accepted.bFinal && IsNewerInspectionSequence(Incoming.Sequence, Accepted.Sequence);
}

bool ECPPickupRules::ShouldRetainInspectionPrediction(
	const FECPInspectionRotationSample& Accepted, const FECPInspectionRotationSample& Predicted)
{
	// 退出后重新拾取时，本地旧物品/旧代际的预测必须失效，不能越过新的权威状态。
	if (Predicted.Generation == 0 || Predicted.Generation != Accepted.Generation
		|| Predicted.Session == 0 || Predicted.Sequence == 0)
	{
		return false;
	}
	if (Predicted.Session != Accepted.Session)
	{
		return Accepted.Session == 0 || IsNewerInspectionSequence(Predicted.Session, Accepted.Session);
	}
	return IsNewerInspectionSequence(Predicted.Sequence, Accepted.Sequence);
}
