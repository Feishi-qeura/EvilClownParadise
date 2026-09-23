#pragma once

#include "CoreMinimal.h"
#include "ECPPickupTypes.generated.h"

/** 物品的四个联网生命周期；World 是唯一允许新玩家申请占用的状态。 */
UENUM(BlueprintType)
enum class EECPPickupState : uint8
{
	World,
	Pickup,
	Equipped,
	Stored
};

/**
 * 服务器同帧竞争使用的纯排序键。
 * Ping 使用服务器观测值；加入与到达序号只负责同延迟时的确定性，不能越过主机和延迟。
 */
struct FECPPickupPriority
{
	uint64 ServerFrame = MAX_uint64;
	bool bListenServerHost = false;
	float PingMilliseconds = MAX_flt;
	int32 JoinOrder = MAX_int32;
	uint32 ArrivalOrder = MAX_uint32;
};

/**
 * 一次拖动的绝对目标及其确认标记；Generation 由服务器每次进入检查态生成。
 * 不把物理 Revision 当代际，否则每个旋转回包都会让尚在路上的合法输入失效。
 */
USTRUCT(BlueprintType)
struct FECPInspectionRotationSample
{
	GENERATED_BODY()
	UPROPERTY() uint32 Generation = 0;
	UPROPERTY() uint32 Session = 0;
	UPROPERTY() uint32 Sequence = 0;
	UPROPERTY() FRotator Rotation = FRotator::ZeroRotator;
	UPROPERTY() bool bFinal = false;
};

/** 纯规则集中在无 Actor 依赖的命名空间中，便于自动化测试覆盖网络裁决边界。 */
namespace ECPPickupRules
{
	EVILCLOWNPARADISE_API bool IsNewerInspectionSequence(uint32 Candidate, uint32 Previous);
	EVILCLOWNPARADISE_API bool CanAcceptInspectionSample(
		const FECPInspectionRotationSample& Accepted, const FECPInspectionRotationSample& Incoming);
	EVILCLOWNPARADISE_API bool ShouldRetainInspectionPrediction(
		const FECPInspectionRotationSample& Accepted, const FECPInspectionRotationSample& Predicted);
	EVILCLOWNPARADISE_API bool CanTransition(EECPPickupState From, EECPPickupState To);
	EVILCLOWNPARADISE_API bool IsHigherPriority(const FECPPickupPriority& Left, const FECPPickupPriority& Right);
	EVILCLOWNPARADISE_API FRotator ClampRotationDelta(const FRotator& Delta, float MaxDegrees);
	EVILCLOWNPARADISE_API FRotator ResolveInspectionStartRotation(
		const FRotator& Authoritative, const FRotator& Predicted, bool bHasPrediction);
	EVILCLOWNPARADISE_API float ExpectedThrowImpulse(float BaseImpulse, float PlayerStrength);
	EVILCLOWNPARADISE_API float ExpectedVelocityDelta(float ImpulseStrength, float MassKg);
}
