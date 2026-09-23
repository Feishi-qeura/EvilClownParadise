#if WITH_DEV_AUTOMATION_TESTS

#include "Characters/ECPCharBase.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "Misc/AutomationTest.h"
#include "TimerManager.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FECPGroundCorrectionAnimationTest,
    "ECP.Animation.AutonomousGroundCorrectionLeavesDefaultLoop",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FECPGroundCorrectionAnimationTest::RunTest(const FString& Parameters)
{
    // 真实角色的 PoseSearch 在无关卡数据库的测试世界中会报告一次缺少历史，与蒙太奇跳段无关。
    AddExpectedError(TEXT("FPoseSearchModule::Search, missing IPoseHistory"),
        EAutomationExpectedErrorFlags::Contains, 1);
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("ECPGroundCorrectionWorld"));
    if (!TestNotNull(TEXT("创建纠正测试世界"), World)) return false;
    UClass* PlayerClass = LoadClass<AECPCharBase>(nullptr,
        TEXT("/Game/zxx/BluePrint/Player/BP_ECPlayertest.BP_ECPlayertest_C"));
    AECPCharBase* Player = PlayerClass ? World->SpawnActor<AECPCharBase>(PlayerClass) : nullptr;
    APlayerController* Controller = World->SpawnActor<APlayerController>();
    UAnimInstance* Anim = Player && Player->GetMesh() ? Player->GetMesh()->GetAnimInstance() : nullptr;
    UAnimMontage* Montage = Player ? Player->JumpMontage.Get() : nullptr;
    const bool bReady = TestNotNull(TEXT("真实角色"), Player)
        && TestNotNull(TEXT("本地控制器"), Controller)
        && TestNotNull(TEXT("动画实例"), Anim) && TestNotNull(TEXT("跳跃蒙太奇"), Montage);
    if (bReady)
    {
        // 模拟拥有端收到服务器地面纠正：只改变移动模式，不调用碰撞落地才会触发的 Landed。
        Controller->SetAsLocalPlayerController();
        Controller->Possess(Player);
        Player->SetRole(ROLE_AutonomousProxy);
        Player->GetCharacterMovement()->SetMovementMode(MOVE_Falling);
        TestEqual(TEXT("下降播放 DefaultLoop"), Anim->Montage_GetCurrentSection(Montage), FName(TEXT("DefaultLoop")));
        Player->GetCharacterMovement()->SetMovementMode(MOVE_Walking);
        TestEqual(TEXT("服务器地面纠正必须进入 Landing"), Anim->Montage_GetCurrentSection(Montage), FName(TEXT("Landing")));
        TestTrue(TEXT("Landing 期间保持移动锁"), Player->IsMovementLocked());

        // 权威端可能早已处于 Normal，不会再发新落地轮次；补播结束后必须能独立解锁。
        // 首帧先激活 PendingTimer；随后推进到新引擎帧再模拟到期，避免同帧 Tick 被忽略。
        World->GetTimerManager().Tick(0.f);
        {
            TGuardValue<uint64> TestFrame(GFrameCounter, GFrameCounter + 1);
            World->GetTimerManager().Tick(Montage->GetPlayLength() + 1.f);
        }
        TestFalse(TEXT("纠正补播结束不等待不存在的新轮次"), Player->IsMovementLocked());
    }
    World->DestroyWorld(false);
    return bReady;
}

#endif
