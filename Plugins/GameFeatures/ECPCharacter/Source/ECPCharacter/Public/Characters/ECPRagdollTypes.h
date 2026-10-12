#pragma once

#include "CoreMinimal.h"
#include "Networking/ECPNetworkPolicy.h"
#include "ECPRagdollTypes.generated.h"

class UPackageMap;

/** 阶段和姿态放在同一份复制数据中，避免先恢复移动、后收到恢复位置。 */
UENUM(BlueprintType)
enum class EECPMotionPhase : uint8
{
	Normal,
	Ragdoll,
	RagdollLanding,
	JumpLanding
};

/**
 * 服务器保留的最新完整状态；新加入或重新进入相关范围的客户端也能直接还原。
 * 网络包不反复发送骨骼名字，双方用同一骨架顺序，并由 SkeletonSignature 校验。
 */
USTRUCT(BlueprintType)
struct ECPCHARACTER_API FECPRagdollNetState
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "ECP|Animation")
	EECPMotionPhase Phase = EECPMotionPhase::Normal;

	/** 一次进入/恢复的轮次标识，用于拒绝上一次布娃娃残留的数据。 */
	UPROPERTY()
	uint32 Sequence = 0;

	/** 使用服务器采样时间插值，收到新包时不重新开始一段追赶动画。 */
	UPROPERTY()
	double ServerTime = 0.0;

	/** 布娃娃阶段为固定 Mesh 世界参考系；落地阶段为服务器确定的 Actor 恢复变换。 */
	UPROPERTY(BlueprintReadOnly, Category = "ECP|Animation")
	FTransform Frame = FTransform::Identity;

	/** 世界空间的骨盆速度，仅在短时间缺包时用于整体平移，不在客户端重新计算物理。 */
	UPROPERTY(BlueprintReadOnly, Category = "ECP|Animation")
	FVector LinearVelocity = FVector::ZeroVector;

	UPROPERTY()
	uint32 SkeletonSignature = 0;

	/** Root 相对 Frame，其余骨骼相对父骨；缩放也显式保留。 */
	UPROPERTY(BlueprintReadOnly, Category = "ECP|Animation")
	TArray<FTransform> LocalTransforms;

	bool NetSerialize(FArchive& Ar, UPackageMap* Map, bool& bOutSuccess);
};

template<>
struct TStructOpsTypeTraits<FECPRagdollNetState> : public TStructOpsTypeTraitsBase2<FECPRagdollNetState>
{
	enum
	{
		WithNetSerializer = true,
		// 包中没有连接专属对象引用，同一姿态可复用序列化结果，减少多客户端服务器开销。
		WithNetSharedSerialization = true
	};
};

namespace ECPRagdoll
{
	/** 给网络数组明确上限，异常包不能触发无限分配；当前角色只有 50 根骨骼。 */
	inline constexpr int32 MaxPoseBones = 256;
	/** 保留布娃娃调用名，所有物理表现从 Foundation 读取相同网络采样策略。 */
	inline constexpr float DefaultPoseInterval = ECPNetworkPolicy::DefaultPoseInterval;
	inline constexpr float DefaultInterpolationDelay = .04f;
	/** 缺包时只允许公共策略限定的短暂外推，避免角色与推动器时间轴分离。 */
	inline constexpr float MaxExtrapolation = ECPNetworkPolicy::MaxExtrapolation;

	/**
	 * 从服务器时间轴取姿态。Frames 应按收到顺序追加，最后一个有效样本决定当前轮次。
	 * 自动过滤其他轮次、骨架和无效样本；超前时只对 Root 做有界世界速度外推。
	 */
	ECPCHARACTER_API bool SamplePose(const TArray<FECPRagdollNetState>& Frames,
		double DisplayTime, TArray<FTransform>& OutLocalTransforms);
}
