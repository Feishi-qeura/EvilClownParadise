#if WITH_EDITOR

#include "Characters/ECPCharBase.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimTypes.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "UObject/UnrealType.h"

namespace
{
    // 仅在编辑器按需观察用户操作，不发 RPC、不跳跃、不切段，也不修改角色状态。
    // ECP.Animation.WatchLanding 1 开始，0 停止；90 秒自动停止，避免遗留诊断开销。
    TAutoConsoleVariable<int32> WatchLanding(TEXT("ECP.Animation.WatchLanding"), 0,
        TEXT("Observe PIE landing state for up to 90 seconds; never drives gameplay."), ECVF_Default);

    struct FAnimationObservation
    {
        FString State;
        double LastLogTime = -1.0;
    };

    struct FAnimationStateObserver
    {
        FDelegateHandle TickHandle;
        TMap<TWeakObjectPtr<UAnimInstance>, FAnimationObservation> Observations;
        double StartedAt = 0.0;

        FAnimationStateObserver()
        {
            // 世界完成本帧角色更新后读取主实例和链接实例，定位是否有两套蒙太奇状态。
            TickHandle = FWorldDelegates::OnWorldPostActorTick.AddRaw(this, &FAnimationStateObserver::Observe);
        }

        ~FAnimationStateObserver()
        {
            FWorldDelegates::OnWorldPostActorTick.Remove(TickHandle);
        }

        void Observe(UWorld* World, ELevelTick, float)
        {
            // 默认完全关闭；只接触 PIE 世界，不读取编辑关卡或在打包游戏中注册。
            if (WatchLanding.GetValueOnGameThread() == 0)
            {
                StartedAt = 0.0;
                Observations.Reset();
                return;
            }
            if (!World || World->WorldType != EWorldType::PIE) return;
            const double Now = FPlatformTime::Seconds();
            if (StartedAt == 0.0) StartedAt = Now;
            if (Now - StartedAt > 90.0)
            {
                // 使用与用户控制台相同的优先级，确保超时关闭不会被较高的控制台值拒绝。
                WatchLanding.AsVariable()->Set(0, ECVF_SetByConsole);
                UE_LOG(LogTemp, Display, TEXT("ECP_ANIM_WATCH stopped after 90 seconds"));
                return;
            }
            const FStructProperty* Property = FindFProperty<FStructProperty>(AECPCharBase::StaticClass(), TEXT("MotionState"));
            for (TActorIterator<AECPCharBase> It(World); It; ++It)
            {
                // 未完成初始化的实例无法代表运行状态；反射只读阶段字段，不暴露或写入私有状态。
                AECPCharBase* Character = *It;
                const USkeletalMeshComponent* Mesh = Character->GetMesh();
                const UCharacterMovementComponent* Movement = Character->GetCharacterMovement();
                if (!Mesh || !Movement || !Property) continue;
                const FECPRagdollNetState* Motion = Property->ContainerPtrToValuePtr<FECPRagdollNetState>(Character);
                const UAnimMontage* Montage = Character->JumpMontage;
                TArray<UAnimInstance*> Instances = Mesh->GetLinkedAnimInstances();
                Instances.Insert(Mesh->GetAnimInstance(), 0);
                for (UAnimInstance* Anim : Instances)
                {
                    if (!Anim || !Montage) continue;
                    const int32 Loop = Montage->GetSectionIndex(TEXT("DefaultLoop"));
                    const int32 Next = Loop != INDEX_NONE ? Anim->Montage_GetNextSectionID(Montage, Loop) : INDEX_NONE;
                    const bool bActive = Anim->Montage_IsActive(Montage);
                    const FString State = FString::Printf(TEXT("Mode=%d Phase=%d Seq=%u Lock=%d Rag=%d Active=%d Section=%s LoopNext=%s"),
                        int32(Movement->MovementMode), int32(Motion->Phase), Motion->Sequence, Character->IsMovementLocked(),
                        Character->IsRagdollActive(), bActive, *Anim->Montage_GetCurrentSection(Montage).ToString(), *Montage->GetSectionName(Next).ToString());
                    FAnimationObservation& Previous = Observations.FindOrAdd(Anim);
                    // 状态变化立即记录；每半秒记录实际骨骼和 Slot，区分时钟正常但姿势不再评估。
                    if (Previous.State != State || Now - Previous.LastLogTime >= 0.5)
                    {
                        UE_LOG(LogTemp, Display, TEXT("ECP_ANIM_WATCH Time=%.3f Actor=%s Net=%d Role=%d Local=%d Anim=%s %s Pos=%.3f Z=%.2f VZ=%.2f RootMotion=%d"),
                            World->GetTimeSeconds(), *Character->GetPathName(), int32(World->GetNetMode()), int32(Character->GetLocalRole()),
                            Character->IsLocallyControlled(), *Anim->GetName(), *State, Anim->Montage_GetPosition(Montage),
                            Character->GetActorLocation().Z, Character->GetVelocity().Z, Character->IsPlayingRootMotion());
                        // 使用组件空间排除角色整体位移；读取实际更新计数，不能把蒙太奇时钟当成姿势刷新。
                        const FTransform Hips = Mesh->GetSocketTransform(Character->RagdollPelvisBone, RTS_Component);
                        const FBoolProperty* MovingProperty = FindFProperty<FBoolProperty>(Anim->GetClass(), TEXT("EC_Moving"));
                        const FBoolProperty* AirProperty = FindFProperty<FBoolProperty>(Anim->GetClass(), TEXT("EC_InAir"));
                        const FBoolProperty* PoseProperty = FindFProperty<FBoolProperty>(Anim->GetClass(), TEXT("bUseReplicatedRagdollPose"));
                        UE_LOG(LogTemp, Display, TEXT("ECP_ANIM_POSE Actor=%s Anim=%s Update=%d Tick=%d VisTick=%d Pause=%d NoSkeleton=%d AutoOnly=%d Rate=%.2f Speed=%.2f Moving=%d InAir=%d RagPose=%d SlotNode=%.3f SlotMontage=%.3f Hips=%s HipsRot=%s"),
                            *Character->GetPathName(), *Anim->GetName(), int32(Anim->GetUpdateCounter().Get()), Mesh->IsComponentTickEnabled(),
                            int32(Mesh->VisibilityBasedAnimTickOption), Mesh->bPauseAnims, Mesh->bNoSkeletonUpdate, Mesh->bOnlyAllowAutonomousTickPose,
                            Mesh->GlobalAnimRateScale, Character->GetVelocity().Size2D(),
                            MovingProperty ? int32(MovingProperty->GetPropertyValue_InContainer(Anim)) : -1,
                            AirProperty ? int32(AirProperty->GetPropertyValue_InContainer(Anim)) : -1,
                            PoseProperty ? int32(PoseProperty->GetPropertyValue_InContainer(Anim)) : -1,
                            Anim->GetSlotNodeGlobalWeight(TEXT("DefaultSlot")), Anim->GetSlotMontageGlobalWeight(TEXT("DefaultSlot")),
                            *Hips.GetLocation().ToCompactString(), *Hips.Rotator().ToCompactString());
                        // Active 查询只返回当前登记实例；枚举仍在混合的旧实例，识别残留空中循环。
                        for (const FAnimMontageInstance* Instance : Anim->MontageInstances)
                        {
                            if (!Instance || !Instance->Montage) continue;
                            UE_LOG(LogTemp, Display, TEXT("ECP_ANIM_INSTANCE Actor=%s Anim=%s Montage=%s ID=%d Active=%d Playing=%d Stopped=%d Weight=%.3f Target=%.3f Section=%s Next=%s Pos=%.3f"),
                                *Character->GetPathName(), *Anim->GetName(), *Instance->Montage->GetName(), Instance->GetInstanceID(),
                                Instance->IsActive(), Instance->IsPlaying(), Instance->IsStopped(), Instance->GetWeight(), Instance->GetDesiredWeight(),
                                *Instance->GetCurrentSection().ToString(), *Instance->GetNextSection().ToString(), Instance->GetPosition());
                        }
                        Previous.State = State;
                        Previous.LastLogTime = Now;
                    }
                }
            }
        }
    } AnimationStateObserver;
}

#endif

