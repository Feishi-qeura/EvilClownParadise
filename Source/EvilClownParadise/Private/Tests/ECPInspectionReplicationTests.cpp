#if WITH_DEV_AUTOMATION_TESTS

#include "Actors/ECPPickupItem.h"
#include "Characters/ECPPlayerBase.h"
#include "Components/SceneComponent.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Misc/AutomationTest.h"
#include "UObject/UnrealType.h"
#include "UObject/Script.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FECPInspectionPredictionTest,
    "ECP.Pickup.Item.DelayedReplicationPreservesOwnerPreview",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FECPInspectionPredictionTest::RunTest(const FString& Parameters)
{
    // 使用真实组件树验证表现入口，不用无父组件的 Transform 写入冒充网络附着。
    AddExpectedError(TEXT("FPoseSearchModule::Search, missing IPoseHistory"),
        EAutomationExpectedErrorFlags::Contains, 1);
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("ECPInspectionPredictionWorld"));
    if (!TestNotNull(TEXT("创建预测测试世界"), World)) return false;
    UClass* PlayerClass = LoadClass<AECPPlayerBase>(nullptr,
        TEXT("/Game/zxx/BluePrint/Player/BP_ECPlayertest.BP_ECPlayertest_C"));
    UClass* ItemClass = LoadClass<AECPPickupItem>(nullptr,
        TEXT("/Game/zxx/BluePrint/thing/BP_Smallitems.BP_Smallitems_C"));
    AECPPlayerBase* Player = PlayerClass ? World->SpawnActor<AECPPlayerBase>(PlayerClass) : nullptr;
    AECPPickupItem* Item = ItemClass ? World->SpawnActor<AECPPickupItem>(ItemClass) : nullptr;
    APlayerController* Controller = World->SpawnActor<APlayerController>();
    if (!TestNotNull(TEXT("真实角色"), Player) || !TestNotNull(TEXT("真实物品"), Item)
        || !TestNotNull(TEXT("本地控制器"), Controller))
    {
        World->DestroyWorld(false);
        return false;
    }
    Player->NetworkDebugWidgetClass = nullptr;
    Controller->SetAsLocalPlayerController();
    Controller->Possess(Player);
    Player->DispatchBeginPlay();
    const FVector OriginalScale(0.4f, 1.25f, 2.f);
    Item->SetActorScale3D(OriginalScale);
    Item->DispatchBeginPlay();
    TestTrue(TEXT("建立 Pickup 前置状态"), Item->TryEnterPickup(Player));

    FECPInspectionRotationSample Sample;
    Sample.Generation = Item->GetInspectionGeneration();
    Sample.Session = 1;
    Sample.Sequence = 1;
    Sample.Rotation = FRotator(0.f, 10.f, 0.f);
    const uint32 PhysicalRevision = Item->GetStateRevision();
    USceneComponent* AttachmentParent = Item->GetRootComponent()->GetAttachParent();
    TestTrue(TEXT("权威接受第一个绝对样本"), Item->ApplyInspectionRotation(Player, Sample));
    // 缺少第二包，第三包可大于旧的 25 度增量上限；完整目标必须一次追上。
    Sample.Sequence = 3;
    Sample.Rotation = FRotator(0.f, 90.f, 0.f);
    TestTrue(TEXT("缺包后接受最新目标"), Item->ApplyInspectionRotation(Player, Sample));
    TestEqual(TEXT("绝对目标不受旧增量限幅影响"), Item->GetInspectionRotation().Yaw, 90.0);
    TestEqual(TEXT("只旋转不改变物理状态版本"), Item->GetStateRevision(), PhysicalRevision);
    TestTrue(TEXT("只旋转保留原附着关系"), Item->GetRootComponent()->GetAttachParent() == AttachmentParent);

    Player->SetRole(ROLE_AutonomousProxy);
    Item->SetRole(ROLE_SimulatedProxy);
    Sample.Session = 2;
    Sample.Sequence = 8;
    Sample.Rotation = FRotator(15.f, 60.f, 0.f);
    Item->PreviewInspectionRotation(Sample);
    const FQuat ExpectedPreview = Item->GetRootComponent()->GetRelativeRotation().Quaternion();
    TestTrue(TEXT("客户端真实拖拽保留非均匀世界缩放"),
        Item->GetActorScale3D().Equals(OriginalScale, UE_KINDA_SMALL_NUMBER));
    FStructProperty* Property = FindFProperty<FStructProperty>(AECPPickupItem::StaticClass(), TEXT("AttachmentState"));
    UFunction* Notify = Item->FindFunction(TEXT("OnRep_AttachmentState"));
    if (TestNotNull(TEXT("复制状态"), Property) && TestNotNull(TEXT("复制回调"), Notify))
    {
        FEditorScriptExecutionGuard ScriptGuard;
        FECPPickupNetState* State = Property->ContainerPtrToValuePtr<FECPPickupNetState>(Item);
        State->Inspection = Sample;
        State->Inspection.Sequence = 7;
        State->Inspection.Rotation = FRotator(0.f, 10.f, 0.f);
        // 旋转不改物理 Revision：本测试也验证快路径能实际更新表现。
        Item->ProcessEvent(Notify, nullptr);
        TestTrue(TEXT("延迟确认不能把拥有者预览拉回旧角度"),
            Item->GetRootComponent()->GetRelativeRotation().Quaternion().Equals(ExpectedPreview, 0.001f));

        State->Inspection.Sequence = 8;
        State->Inspection.Rotation = FRotator(0.f, 25.f, 0.f);
        Item->ProcessEvent(Notify, nullptr);
        const FQuat ConfirmedRotation = State->Inspection.Rotation.Quaternion()
            * Item->InspectionRelativeTransform.GetRotation();
        TestTrue(TEXT("最新确认即使修正角度也会结束旧预测"),
            Item->GetRootComponent()->GetRelativeRotation().Quaternion().Equals(ConfirmedRotation, 0.001f));

        Sample.Session = 3;
        Sample.Sequence = 1;
        Sample.Rotation = FRotator(0.f, 120.f, 0.f);
        Item->PreviewInspectionRotation(Sample);
        State->Inspection = FECPInspectionRotationSample();
        State->Inspection.Generation = Sample.Generation + 1;
        ++State->Revision;
        Item->ProcessEvent(Notify, nullptr);
        TestTrue(TEXT("持有代际变化清掉上一轮预测"),
            Item->GetInspectionRotation().IsNearlyZero());

        // 主机不能走独立预览入口，再由另一个角度不同的权威路径覆盖。
        Item->SetRole(ROLE_Authority);
        Sample.Generation = State->Inspection.Generation;
        Item->PreviewInspectionRotation(Sample);
        TestTrue(TEXT("主机忽略非权威预览写入"), Item->GetInspectionRotation().IsNearlyZero());
    }
    World->DestroyWorld(false);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FECPInspectionTargetIdentityTest,
    "ECP.Pickup.Rotation.TargetIdentityAndReentry",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FECPInspectionTargetIdentityTest::RunTest(const FString& Parameters)
{
    AddExpectedError(TEXT("FPoseSearchModule::Search, missing IPoseHistory"),
        EAutomationExpectedErrorFlags::Contains, 1);
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("ECPInspectionIdentityWorld"));
    if (!TestNotNull(TEXT("创建旋转身份测试世界"), World)) return false;
    UClass* PlayerClass = LoadClass<AECPPlayerBase>(nullptr,
        TEXT("/Game/zxx/BluePrint/Player/BP_ECPlayertest.BP_ECPlayertest_C"));
    UClass* ItemClass = LoadClass<AECPPickupItem>(nullptr,
        TEXT("/Game/zxx/BluePrint/thing/BP_Smallitems.BP_Smallitems_C"));
    AECPPlayerBase* Player = PlayerClass ? World->SpawnActor<AECPPlayerBase>(PlayerClass) : nullptr;
    AECPPickupItem* First = ItemClass ? World->SpawnActor<AECPPickupItem>(ItemClass) : nullptr;
    AECPPickupItem* Second = ItemClass ? World->SpawnActor<AECPPickupItem>(ItemClass) : nullptr;
    APlayerController* Controller = World->SpawnActor<APlayerController>();
    if (!Player || !First || !Second || !Controller)
    {
        AddError(TEXT("真实角色、物品或控制器创建失败"));
        World->DestroyWorld(false);
        return false;
    }
    Player->NetworkDebugWidgetClass = nullptr;
    Controller->SetAsLocalPlayerController();
    Controller->Possess(Player);
    Player->DispatchBeginPlay();
    First->DispatchBeginPlay();
    Second->DispatchBeginPlay();
    TestTrue(TEXT("第一次拾取"), First->TryEnterPickup(Player));
    Player->CompleteQueuedPickup(First);
    FECPInspectionRotationSample OldSample;
    OldSample.Generation = First->GetInspectionGeneration();
    OldSample.Session = 1;
    OldSample.Sequence = 1;
    OldSample.Rotation = FRotator(0.f, 45.f, 0.f);
    Player->RequestRotatePickup(First, OldSample);
    TestEqual(TEXT("正常请求旋转指定物品"), First->GetInspectionRotation().Yaw, 45.0);

    First->ReleaseFrom(Player);
    TestTrue(TEXT("切换到另一件物品"), Second->TryEnterPickup(Player));
    Player->CompleteQueuedPickup(Second);
    ++OldSample.Sequence;
    OldSample.Rotation.Yaw = 150.f;
    // 新旧物品即使拥有相同代际，也不能仅凭“当前槽位”把旧物品包解释成新物品的请求。
    Player->RequestRotatePickup(First, OldSample);
    TestTrue(TEXT("旧目标请求不能旋转新物品"), Second->GetInspectionRotation().IsNearlyZero());

    Second->ReleaseFrom(Player);
    TestTrue(TEXT("重新拾取第一件物品"), First->TryEnterPickup(Player));
    Player->CompleteQueuedPickup(First);
    TestTrue(TEXT("重新拾取产生新的服务器代际"), First->GetInspectionGeneration() != OldSample.Generation);
    Player->RequestFinalizePickupRotation(First, OldSample);
    TestTrue(TEXT("上一轮持有的可靠尾包被拒绝"), First->GetInspectionRotation().IsNearlyZero());
    OldSample.Generation = First->GetInspectionGeneration();
    Player->RequestRotatePickup(First, OldSample);
    TestEqual(TEXT("新代际合法请求正常接受"), First->GetInspectionRotation().Yaw, 150.0);
    World->DestroyWorld(false);
    return true;
}
#endif
