#if WITH_DEV_AUTOMATION_TESTS

#include "Actors/ECPPickupItem.h"
#include "Characters/ECPPlayerBase.h"

#include "Components/SceneComponent.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Misc/AutomationTest.h"
#include "UObject/UnrealType.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FECPPickupItemRejectedTransitionTest, "ECP.Pickup.Item.RejectedTransitions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FECPPickupItemRejectedTransitionTest::RunTest(const FString& Parameters)
{
	AECPPickupItem* Item = NewObject<AECPPickupItem>();
	AECPPlayerBase* FirstPlayer = NewObject<AECPPlayerBase>();
	AECPPlayerBase* OtherPlayer = NewObject<AECPPlayerBase>();
	TestNotNull(TEXT("Item test object is created"), Item);
	TestNotNull(TEXT("First player test object is created"), FirstPlayer);
	TestNotNull(TEXT("Other player test object is created"), OtherPlayer);
	if (!Item || !FirstPlayer || !OtherPlayer)
	{
		return false;
	}

	TestEqual(TEXT("Initial item state is World"), Item->GetPickupState(), EECPPickupState::World);
	const uint32 InitialRevision = Item->GetStateRevision();
	// 没有所有权时，装备、入库、旋转和丢弃都必须拒绝且不能产生一个新版本。
	TestFalse(TEXT("Non-owner cannot equip"), Item->TryEquip(OtherPlayer));
	TestFalse(TEXT("Non-owner cannot store"), Item->TryStore(OtherPlayer));
	TestFalse(TEXT("Non-owner cannot rotate"), Item->ApplyInspectionRotation(OtherPlayer, FECPInspectionRotationSample()));
	FECPInspectionRotationSample FinalSample;
	FinalSample.Generation = 1;
	FinalSample.Session = 1;
	FinalSample.Sequence = 1;
	FinalSample.bFinal = true;
	TestFalse(TEXT("Non-owner cannot finalize an absolute rotation"),
		Item->ApplyInspectionRotation(OtherPlayer, FinalSample));
	TestFalse(TEXT("Non-owner cannot drop"), Item->TryDrop(OtherPlayer, FTransform::Identity, FVector::ZeroVector));
	TestEqual(TEXT("Rejected transitions preserve revision"), Item->GetStateRevision(), InitialRevision);
	TestNull(TEXT("Rejected transitions preserve empty owner"), Item->GetPickupOwner());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FECPPickupItemUninitializedQueueTest, "ECP.Pickup.Item.UninitializedQueue",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FECPPickupItemUninitializedQueueTest::RunTest(const FString& Parameters)
{
	AECPPickupItem* Item = NewObject<AECPPickupItem>();
	AECPPlayerBase* Player = NewObject<AECPPlayerBase>();
	// 没有 Sphere1 根组件和 BeginPlay 初始化的原生测试对象不得进入队列或产生半成品状态。
	TestFalse(TEXT("Uninitialized item rejects a pickup request"), Item->QueuePickupRequest(Player));
	TestEqual(TEXT("Rejected queue keeps World"), Item->GetPickupState(), EECPPickupState::World);
	TestEqual(TEXT("Rejected queue keeps revision zero"), Item->GetStateRevision(), static_cast<uint32>(0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FECPPickupItemRotationSpeedMultiplierTest,
	"ECP.Pickup.Item.RotationSpeedMultiplier",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FECPPickupItemRotationSpeedMultiplierTest::RunTest(const FString& Parameters)
{
	AECPPickupItem* Item = NewObject<AECPPickupItem>();
	FFloatProperty* SpeedProperty =
		FindFProperty<FFloatProperty>(AECPPickupItem::StaticClass(), TEXT("InspectionRotationSpeedMultiplier"));
	if (!TestNotNull(TEXT("物品提供蓝图可覆盖的检查旋转速度倍率"), SpeedProperty))
	{
		return false;
	}

	// 锁定蓝图类默认值可覆盖的声明契约，防止以后只保留数值却误删编辑器暴露标记或安全范围。
	TestTrue(TEXT("旋转速度倍率可在物品蓝图类默认值中编辑"),
		SpeedProperty->HasAllPropertyFlags(CPF_Edit | CPF_DisableEditOnInstance));
	TestTrue(TEXT("旋转速度倍率对蓝图可见"),
		SpeedProperty->HasAnyPropertyFlags(CPF_BlueprintVisible));
	TestEqual(TEXT("编辑器最小倍率保持为 0.1"),
		SpeedProperty->GetMetaData(TEXT("ClampMin")), FString(TEXT("0.1")));
	TestEqual(TEXT("编辑器最大倍率保持为 10.0"),
		SpeedProperty->GetMetaData(TEXT("ClampMax")), FString(TEXT("10.0")));

	const FRotator RotationDelta(1.f, -2.f, 0.5f);
	// 直接调用公开入口验证真实 C++ 行为，避免 ProcessEvent 参数布局影响数值断言。
	TestTrue(TEXT("默认倍率把当前拖拽旋转速度提高到两倍"),
		Item->ScaleInspectionRotationDelta(RotationDelta).Equals(
			FRotator(2.f, -4.f, 1.f), UE_KINDA_SMALL_NUMBER));

	// 模拟子物品蓝图覆盖默认值，验证不同物品可以拥有独立拖拽速度。
	SpeedProperty->SetPropertyValue_InContainer(Item, 3.f);
	TestTrue(TEXT("物品覆盖倍率后按自身配置缩放旋转增量"),
		Item->ScaleInspectionRotationDelta(RotationDelta).Equals(
			FRotator(3.f, -6.f, 1.5f), UE_KINDA_SMALL_NUMBER));

	// 即使倍率由运行时代码绕过编辑器元数据写入，缩放入口也必须执行相同边界保护。
	SpeedProperty->SetPropertyValue_InContainer(Item, 0.f);
	TestTrue(TEXT("运行时倍率不会低于 0.1"),
		Item->ScaleInspectionRotationDelta(RotationDelta).Equals(
			FRotator(0.1f, -0.2f, 0.05f), UE_KINDA_SMALL_NUMBER));
	SpeedProperty->SetPropertyValue_InContainer(Item, 20.f);
	TestTrue(TEXT("运行时倍率不会高于 10.0"),
		Item->ScaleInspectionRotationDelta(RotationDelta).Equals(
			FRotator(10.f, -20.f, 5.f), UE_KINDA_SMALL_NUMBER));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FECPPickupItemScaleLifecycleTest,
	"ECP.Pickup.Item.ScaleLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FECPPickupItemScaleLifecycleTest::RunTest(const FString& Parameters)
{
	// 无关卡移动数据库时 ABP_Player 的 PoseSearch 会记录一次预期错误，本测试只验证拾取物品生命周期。
	AddExpectedError(TEXT("FPoseSearchModule::Search, missing IPoseHistory"),
		EAutomationExpectedErrorFlags::Contains, 1);
	// 使用项目真实蓝图组件树，完整覆盖服务器采集、相机附着、入库和释放后的缩放恢复。
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("ECPPickupScaleLifecycleWorld"));
	if (!TestNotNull(TEXT("创建拾取生命周期测试世界"), World))
	{
		return false;
	}

	UClass* PlayerClass = LoadClass<AECPPlayerBase>(
		nullptr, TEXT("/Game/zxx/BluePrint/Player/BP_ECPlayertest.BP_ECPlayertest_C"));
	UClass* ItemClass = LoadClass<AECPPickupItem>(
		nullptr, TEXT("/Game/zxx/BluePrint/thing/BP_Smallitems.BP_Smallitems_C"));
	AECPPlayerBase* Player = PlayerClass ? World->SpawnActor<AECPPlayerBase>(PlayerClass) : nullptr;
	AECPPickupItem* Item = ItemClass ? World->SpawnActor<AECPPickupItem>(ItemClass) : nullptr;
	APlayerController* Controller = World->SpawnActor<APlayerController>();
	const bool bReady = TestNotNull(TEXT("加载 BP_ECPlayertest"), Player)
		&& TestNotNull(TEXT("加载 BP_Smallitems"), Item)
		&& TestNotNull(TEXT("创建权威玩家控制器"), Controller);

	if (bReady)
	{
		Controller->Possess(Player);
		Player->DispatchBeginPlay();
		const FVector OriginalScale(0.4f, 1.25f, 2.0f);
		Item->SetActorScale3D(OriginalScale);
		Item->DispatchBeginPlay();

		TestTrue(TEXT("真实物品可以进入 Pickup"), Item->TryEnterPickup(Player));
		TestTrue(TEXT("Pickup 相机附着保留原世界缩放"),
			Item->GetActorScale3D().Equals(OriginalScale, UE_KINDA_SMALL_NUMBER));
		FECPInspectionRotationSample Rotation;
		Rotation.Generation = Item->GetInspectionGeneration();
		Rotation.Session = 1;
		Rotation.Sequence = 1;
		Rotation.Rotation = FRotator(12.f, 24.f, 0.f);
		TestTrue(TEXT("主机拖拽通过真实权威入口应用绝对旋转"), Item->ApplyInspectionRotation(Player, Rotation));
		TestTrue(TEXT("Pickup 拖拽旋转保留原世界缩放"),
			Item->GetActorScale3D().Equals(OriginalScale, UE_KINDA_SMALL_NUMBER));
		TestTrue(TEXT("Pickup 可以转入 Stored"), Item->TryStore(Player));
		TestTrue(TEXT("Stored 状态保留原世界缩放"),
			Item->GetActorScale3D().Equals(OriginalScale, UE_KINDA_SMALL_NUMBER));

		// 释放请求故意携带默认单位缩放，验证服务器只接受位置/旋转并恢复已保存的物品缩放。
		const FTransform ReleaseTransform(FRotator::ZeroRotator, FVector(300.f, 0.f, 120.f));
		TestTrue(TEXT("Stored 物品可以释放回 World"),
			Item->DropFromInventory(Player, ReleaseTransform, FVector::ZeroVector));
		TestEqual(TEXT("释放后状态回到 World"), Item->GetPickupState(), EECPPickupState::World);
		TestTrue(TEXT("释放回 World 后仍保留原世界缩放"),
			Item->GetActorScale3D().Equals(OriginalScale, UE_KINDA_SMALL_NUMBER));
	}

	World->DestroyWorld(false);
	return bReady;
}

#endif
