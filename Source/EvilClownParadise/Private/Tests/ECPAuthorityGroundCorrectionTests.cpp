#if WITH_DEV_AUTOMATION_TESTS

#include "Characters/ECPCharBase.h"

#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "UObject/UnrealType.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FECPAuthorityGroundCorrectionTest,
    "ECP.Animation.AuthorityGroundCorrectionLeavesDefaultLoop",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FECPAuthorityGroundCorrectionTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    // 无关卡数据库的真实 ABP 会产生这条已知历史提示，其他错误仍由自动化框架正常报告。
    AddExpectedError(TEXT("FPoseSearchModule::Search, missing IPoseHistory"),
        EAutomationExpectedErrorFlags::Contains, 1);
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("ECPAuthorityGroundCorrectionWorld"));
    if (!TestNotNull(TEXT("创建独立权威端落地测试世界"), World)) return false;
    ON_SCOPE_EXIT
    {
        World->DestroyWorld(false);
    };

    UClass* PlayerClass = LoadClass<AECPCharBase>(nullptr,
        TEXT("/Game/zxx/BluePrint/Player/BP_ECPlayertest.BP_ECPlayertest_C"));
    AECPCharBase* Player = PlayerClass ? World->SpawnActor<AECPCharBase>(PlayerClass) : nullptr;
    UAnimInstance* Anim = Player && Player->GetMesh() ? Player->GetMesh()->GetAnimInstance() : nullptr;
    UAnimMontage* Montage = Player ? Player->JumpMontage.Get() : nullptr;
    if (!TestNotNull(TEXT("加载真实角色"), Player) || !TestNotNull(TEXT("创建真实动画实例"), Anim)
        || !TestNotNull(TEXT("加载真实 AMT_Jump"), Montage)) return false;
    if (!TestTrue(TEXT("角色执行权威端分支"), Player->HasAuthority())) return false;

    FStructProperty* MotionProperty = FindFProperty<FStructProperty>(AECPCharBase::StaticClass(), TEXT("MotionState"));
    if (!TestNotNull(TEXT("读取权威复制阶段"), MotionProperty)) return false;
    const FECPRagdollNetState* State = MotionProperty->ContainerPtrToValuePtr<FECPRagdollNetState>(Player);
    const uint32 PreviousSequence = State->Sequence;

    // 只改移动模式，不手动调用 Landed，重现移动平台或玩法直接切换地面状态的路径。
    UCharacterMovementComponent* Movement = Player->GetCharacterMovement();
    Movement->SetMovementMode(MOVE_Falling);
    bool bSuccess = TestEqual(TEXT("下降先播放空中循环"),
        Anim->Montage_GetCurrentSection(Montage), FName(TEXT("DefaultLoop")));
    Movement->SetMovementMode(MOVE_Walking);
    bSuccess &= TestEqual(TEXT("权威端跳过碰撞回调时仍进入 Landing"),
        Anim->Montage_GetCurrentSection(Montage), FName(TEXT("Landing")));
    bSuccess &= TestTrue(TEXT("权威端发布 JumpLanding 而不只是本机播放"), State->Phase == EECPMotionPhase::JumpLanding);
    bSuccess &= TestEqual(TEXT("一次地面转换只发布一个新的落地轮次"), State->Sequence, PreviousSequence + 1);
    bSuccess &= TestTrue(TEXT("权威端落地动画期间继续锁定移动"), Player->IsMovementLocked());

    // 正常碰撞 Landed 紧随其后也必须被本轮播放标志去重，不能重播或再递增网络轮次。
    Player->Landed(FHitResult());
    bSuccess &= TestEqual(TEXT("重复落地回调不重复发布阶段"), State->Sequence, PreviousSequence + 1);
    return bSuccess;
}

#endif
