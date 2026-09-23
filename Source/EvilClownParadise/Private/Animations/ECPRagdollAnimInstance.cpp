#include "Animations/ECPRagdollAnimInstance.h"
#include "Characters/ECPCharBase.h"

void UECPRagdollAnimInstance::NativeUpdateAnimation(float DeltaSeconds)
{
	Super::NativeUpdateAnimation(DeltaSeconds);
	// 此回调在游戏线程执行，先取服务器时间轴姿态，再由 AnimGraph 评估输出。
	AECPCharBase* Character = Cast<AECPCharBase>(TryGetPawnOwner());
	bUseReplicatedRagdollPose = Character && Character->BuildRagdollSnapshot(ReplicatedRagdollPose);
}
