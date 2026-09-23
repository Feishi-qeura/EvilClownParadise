#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "Characters/ECPCharBase.h"
#include "Animation/AnimInstance.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerState.h"
#include "HAL/IConsoleManager.h"
#include "TimerManager.h"
#include "UObject/UnrealType.h"

// 仅诊断双人 PIE；动作在真正游戏定时器中执行，避免编辑器 Python 保护上下文把 RPC 当本地调用。
struct FECPMotionPIEProbe
{
    FTimerHandle Timer;
    int32 Stage = 0;
    float StageTime = 0.f;
    float LastRequestTime = -1.f;
    float LastReportTime = -1.f;
    FVector FirstLocation;
    FVector SecondLocation;
};

static void ECPStartMotionPIEProbe(bool bHost)
{
    if (!GEngine) return;
    AECPCharBase* Driver = nullptr;
    for (const FWorldContext& Context : GEngine->GetWorldContexts())
    {
        UWorld* World = Context.World();
        // 严格限制在 PIE，不改编辑关卡；一次测试只驱动一个真正本地拥有的 Pawn。
        if (!World || World->WorldType != EWorldType::PIE
            || World->GetNetMode() != (bHost ? NM_ListenServer : NM_Client)) continue;
        for (TActorIterator<AECPCharBase> It(World); It; ++It)
        {
            if (It->IsLocallyControlled()) { Driver = *It; break; }
        }
    }
    if (!Driver || Driver->IsRagdollActive() || Driver->IsMovementLocked())
    {
        UE_LOG(LogTemp, Warning, TEXT("ECP_MOTION_PROBE_NOT_READY"));
        return;
    }
    const TSharedRef<FECPMotionPIEProbe> Probe = MakeShared<FECPMotionPIEProbe>();
    Probe->StageTime = Driver->GetWorld()->GetTimeSeconds();
    const int32 PlayerId = Driver->GetPlayerState() ? Driver->GetPlayerState()->GetPlayerId() : INDEX_NONE;
    UE_LOG(LogTemp, Display, TEXT("ECP_MOTION_PROBE_START Host=%d Player=%d"), bHost, PlayerId);
    Driver->Jump();
    Driver->GetWorldTimerManager().SetTimer(Probe->Timer,
        FTimerDelegate::CreateWeakLambda(Driver, [Driver, PlayerId, Probe]()
        {
            const float Now = Driver->GetWorld()->GetTimeSeconds();
            const float Elapsed = Now - Probe->StageTime;
            auto Advance = [&]() { ++Probe->Stage; Probe->StageTime = Now; Probe->LastRequestTime = -1.f; };
            // 持续记录对应两端的同一 PlayerId，不能用 Actors 的枚举顺序猜服务器副本。
            if (Now - Probe->LastReportTime >= .1f)
            {
                Probe->LastReportTime = Now;
                for (const FWorldContext& Context : GEngine->GetWorldContexts())
                {
                    UWorld* World = Context.World();
                    if (!World || World->WorldType != EWorldType::PIE) continue;
                    for (TActorIterator<AECPCharBase> It(World); It; ++It)
                    {
                        if (!It->GetPlayerState() || It->GetPlayerState()->GetPlayerId() != PlayerId) continue;
                        USkeletalMeshComponent* Mesh = It->GetMesh();
                        UAnimInstance* Anim = Mesh->GetAnimInstance();
                        FStructProperty* Property = FindFProperty<FStructProperty>(AECPCharBase::StaticClass(), TEXT("MotionState"));
                        const FECPRagdollNetState* State = Property ? Property->ContainerPtrToValuePtr<FECPRagdollNetState>(*It) : nullptr;
                        UE_LOG(LogTemp, Display, TEXT("ECP_MOTION_ROW Stage=%d Time=%.3f Net=%d Role=%d Local=%d Rag=%d Lock=%d Mode=%d Phase=%d Seq=%u Actor=%s Mesh=%s Relative=%s Parent=%s Hips=%s COM=%s Frame=%s Section=%s Pos=%.3f"),
                            Probe->Stage, Now, int32(World->GetNetMode()), int32(It->GetLocalRole()), It->IsLocallyControlled(), It->IsRagdollActive(), It->IsMovementLocked(), int32(It->GetCharacterMovement()->MovementMode),
                            State ? int32(State->Phase) : -1, State ? State->Sequence : 0,
                            *It->GetActorLocation().ToCompactString(), *Mesh->GetComponentLocation().ToCompactString(), *Mesh->GetRelativeLocation().ToCompactString(), *GetNameSafe(Mesh->GetAttachParent()),
                            *Mesh->GetSocketLocation(It->RagdollPelvisBone).ToCompactString(), *Mesh->GetCenterOfMass(It->RagdollPelvisBone).ToCompactString(), State ? *State->Frame.GetLocation().ToCompactString() : TEXT("None"),
                            Anim ? *Anim->Montage_GetCurrentSection(It->JumpMontage).ToString() : TEXT("None"), Anim ? Anim->Montage_GetPosition(It->JumpMontage) : 0.f);
                    }
                }
            }
            if (Elapsed > 15.f)
            {
                UE_LOG(LogTemp, Warning, TEXT("ECP_MOTION_PROBE_TIMEOUT Stage=%d"), Probe->Stage);
                Driver->StopJumping();
                Driver->GetWorldTimerManager().ClearTimer(Probe->Timer);
                return;
            }
            switch (Probe->Stage)
            {
            case 0:
                Driver->StopJumping();
                if (Elapsed > 4.f) Advance();
                break;
            case 1:
                if (Driver->IsRagdollActive()) { Probe->FirstLocation = Driver->GetActorLocation(); Advance(); }
                else if (Now - Probe->LastRequestTime > .5f) { Driver->RequestRagdollToggle(); Probe->LastRequestTime = Now; }
                break;
            case 2:
                if (Elapsed > 3.f) Advance();
                break;
            case 3:
                if (!Driver->IsRagdollActive()) Advance();
                else if (Now - Probe->LastRequestTime > .5f) { Driver->RequestRagdollToggle(); Probe->LastRequestTime = Now; }
                break;
            case 4:
                if (!Driver->IsMovementLocked() && Elapsed > .5f) Advance();
                break;
            case 5:
                if (Elapsed < 2.f) Driver->AddMovementInput(FVector::ForwardVector, 1.f);
                else if (Elapsed > 2.5f) { Probe->SecondLocation = Driver->GetActorLocation(); Advance(); }
                break;
            case 6:
                if (Driver->IsRagdollActive()) Advance();
                else if (Now - Probe->LastRequestTime > .5f) { Driver->RequestRagdollToggle(); Probe->LastRequestTime = Now; }
                break;
            case 7:
                if (Elapsed > 2.f)
                {
                    UE_LOG(LogTemp, Display, TEXT("ECP_MOTION_PROBE_SECOND_DIST First=%.1f Second=%.1f"), FVector::Dist2D(Driver->GetActorLocation(), Probe->FirstLocation), FVector::Dist2D(Driver->GetActorLocation(), Probe->SecondLocation));
                    Advance();
                }
                break;
            case 8:
                if (!Driver->IsRagdollActive()) Advance();
                else if (Now - Probe->LastRequestTime > .5f) { Driver->RequestRagdollToggle(); Probe->LastRequestTime = Now; }
                break;
            case 9:
                if (!Driver->IsMovementLocked())
                {
                    UE_LOG(LogTemp, Display, TEXT("ECP_MOTION_PROBE_DONE"));
                    Driver->GetWorldTimerManager().ClearTimer(Probe->Timer);
                }
                break;
            }
        }), 1.f / 60.f, true);
}

static FAutoConsoleCommand ECPProbeClientMotion(TEXT("ECP.Test.ClientMotionRoundTrip"),
    TEXT("PIE only: observe a remote player's jump and repeated ragdoll recovery."),
    FConsoleCommandDelegate::CreateLambda([]() { ECPStartMotionPIEProbe(false); }));
static FAutoConsoleCommand ECPProbeHostMotion(TEXT("ECP.Test.HostMotionRoundTrip"),
    TEXT("PIE only: observe the host player's jump and repeated ragdoll recovery."),
    FConsoleCommandDelegate::CreateLambda([]() { ECPStartMotionPIEProbe(true); }));

#endif
