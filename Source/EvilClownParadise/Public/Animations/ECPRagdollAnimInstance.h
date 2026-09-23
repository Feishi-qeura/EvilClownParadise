#pragma once
#include "CoreMinimal.h"
#include "Animation/AnimInstance.h"
#include "Animation/PoseSnapshot.h"
#include "ECPRagdollAnimInstance.generated.h"

// 作为 ABP_EC_RagdollNet 的原生父类，链接图只保留姿态组合，采样计算移到 C++。
UCLASS(Transient, Blueprintable)
class EVILCLOWNPARADISE_API UECPRagdollAnimInstance : public UAnimInstance
{
	GENERATED_BODY()
public:
	virtual void NativeUpdateAnimation(float DeltaSeconds) override;
	UPROPERTY(Transient, BlueprintReadOnly, Category="ECP|Ragdoll") FPoseSnapshot ReplicatedRagdollPose;
	UPROPERTY(Transient, BlueprintReadOnly, Category="ECP|Ragdoll") bool bUseReplicatedRagdollPose = false;
};
