#if WITH_DEV_AUTOMATION_TESTS

#include "Characters/ECPCharBase.h"

#include "Components/ActorComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "UObject/UnrealType.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FECPRagdollReplicationOrderingTest,
    "ECP.Ragdoll.RecoveryPreservesReplicatedMovement",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FECPRagdollReplicationOrderingTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    // 此用例只验证复制属性的先后顺序：真实调用角色 RepNotify，不需要蓝图、物理资产或 PIE。
    // 若客户端再次缓存进入 rag 时已收到的 false，并在恢复时覆盖服务器的 true，断言必须失败。
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("ECPRagdollReplicationOrderWorld"));
    if (!TestNotNull(TEXT("创建独立复制顺序测试世界"), World)) return false;
    ON_SCOPE_EXIT
    {
        World->DestroyWorld(false);
    };

    // 通过反射模拟引擎收到属性后的赋值和通知，不为测试向游戏角色暴露修改内部状态的接口。
    FStructProperty* MotionProperty = FindFProperty<FStructProperty>(AECPCharBase::StaticClass(), TEXT("MotionState"));
    FBoolProperty* MeshReplicationProperty = FindFProperty<FBoolProperty>(
        UActorComponent::StaticClass(), TEXT("bReplicates"));
    if (!TestNotNull(TEXT("查找布娃娃复制状态"), MotionProperty)
        || !TestNotNull(TEXT("查找组件复制开关"), MeshReplicationProperty)) return false;
    if (!TestTrue(TEXT("复制状态保留真实结构类型"), MotionProperty->Struct == FECPRagdollNetState::StaticStruct())) return false;

    bool bSuccess = true;
    for (const bool bFlagsArriveFirst : { true, false })
    {
        const FString Order = bFlagsArriveFirst ? TEXT("复制开关先到") : TEXT("布娃娃阶段先到");
        FActorSpawnParameters SpawnParameters;
        SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        AECPCharBase* Client = World->SpawnActor<AECPCharBase>(AECPCharBase::StaticClass(),
            FVector(bFlagsArriveFirst ? 0.f : 1000.f, 0.f, 200.f), FRotator::ZeroRotator, SpawnParameters);
        APlayerController* Controller = World->SpawnActor<APlayerController>();
        if (!TestNotNull(Order + TEXT("：创建角色"), Client)
            || !TestNotNull(Order + TEXT("：创建本地控制器"), Controller)) return false;

        // 无网络驱动的测试世界使用本地控制器及自主代理角色，仍执行生产代码中的客户端分支。
        Controller->SetAsLocalPlayerController();
        Controller->Possess(Client);
        Client->DispatchBeginPlay();
        Client->SetRole(ROLE_AutonomousProxy);
        if (!TestTrue(Order + TEXT("：自主客户端已完成初始化"),
            Client->HasActorBegunPlay() && Client->IsLocallyControlled() && !Client->HasAuthority())) return false;

        UFunction* Notify = Client->FindFunction(TEXT("OnRep_MotionState"));
        if (!TestNotNull(Order + TEXT("：查找真实复制回调"), Notify)) return false;
        FECPRagdollNetState* State = MotionProperty->ContainerPtrToValuePtr<FECPRagdollNetState>(Client);
        USkeletalMeshComponent* Mesh = Client->GetMesh();

        // 属性反序列化先写值，再触发 RepNotify；不用 SetReplicateMovement 额外触发服务器行为。
        const auto ReceiveReplicationFlags = [Client, Mesh, MeshReplicationProperty](bool bReplicated)
        {
            Client->SetReplicatingMovement(bReplicated);
            MeshReplicationProperty->SetPropertyValue_InContainer(Mesh, bReplicated);
        };
        const auto ReceiveMotionState = [Client, State, Notify](EECPMotionPhase Phase, uint32 Sequence, const FTransform& Frame)
        {
            *State = FECPRagdollNetState();
            State->Phase = Phase;
            State->Sequence = Sequence;
            State->ServerTime = static_cast<double>(Sequence);
            State->Frame = Frame;
            Client->ProcessEvent(Notify, nullptr);
        };

        ReceiveReplicationFlags(true);
        const FTransform EntryFrame = Mesh->GetComponentTransform();
        if (bFlagsArriveFirst) ReceiveReplicationFlags(false);
        ReceiveMotionState(EECPMotionPhase::Ragdoll, 1, EntryFrame);
        bSuccess &= TestTrue(Order + TEXT("：真实进入布娃娃表现"), Client->IsRagdollActive());
        // 阶段通知不能擅自改写已经收到的复制元数据，不论开关当前为 true 还是 false。
        bSuccess &= TestEqual(Order + TEXT("：进入时保留已收到的移动复制开关"),
            Client->IsReplicatingMovement(), !bFlagsArriveFirst);
        bSuccess &= TestEqual(Order + TEXT("：进入时保留已收到的组件复制开关"),
            Mesh->GetIsReplicated(), !bFlagsArriveFirst);
        if (!bFlagsArriveFirst) ReceiveReplicationFlags(false);

        // 服务器已恢复移动复制，再收到恢复阶段时，客户端不能用 rag 期间的旧 false 覆盖它。
        const FTransform RecoveryFrame(FRotator::ZeroRotator, FVector(1600.f, 200.f, 100.f));
        if (bFlagsArriveFirst) ReceiveReplicationFlags(true);
        ReceiveMotionState(EECPMotionPhase::RagdollLanding, 2, RecoveryFrame);
        bSuccess &= TestFalse(Order + TEXT("：真实退出布娃娃表现"), Client->IsRagdollActive());
        bSuccess &= TestEqual(Order + TEXT("：恢复通知保留服务器的移动复制值"),
            Client->IsReplicatingMovement(), bFlagsArriveFirst);
        bSuccess &= TestEqual(Order + TEXT("：恢复通知保留服务器的组件复制值"),
            Mesh->GetIsReplicated(), bFlagsArriveFirst);
        bSuccess &= TestTrue(Order + TEXT("：恢复仍将网格附回胶囊"), Mesh->GetAttachParent() == Client->GetCapsuleComponent());

        // 开关晚到时由引擎正常应用；两种合法到达顺序最终都必须允许客户端重新发送移动。
        if (!bFlagsArriveFirst) ReceiveReplicationFlags(true);
        bSuccess &= TestTrue(Order + TEXT("：恢复后移动复制保持启用"), Client->IsReplicatingMovement());
        bSuccess &= TestTrue(Order + TEXT("：恢复后组件复制保持启用"), Mesh->GetIsReplicated());
    }
    return bSuccess;
}

#endif

