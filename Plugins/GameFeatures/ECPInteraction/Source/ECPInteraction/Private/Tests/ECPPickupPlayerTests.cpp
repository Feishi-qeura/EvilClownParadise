#if WITH_DEV_AUTOMATION_TESTS

#include "Characters/ECPPlayerBase.h"
#include "Data/ECPPlayerController.h"

#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FECPPickupPlayerEmptyActionsTest,
	"ECP.Pickup.Player.EmptyActionsAreSafe",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FECPPickupPlayerEmptyActionsTest::RunTest(const FString& Parameters)
{
	AECPPlayerBase* Player = NewObject<AECPPlayerBase>();
	TestNotNull(TEXT("测试玩家应能被创建"), Player);

	// 没有持有物时，所有输入入口必须保持空状态，避免网络延迟或重复按键触发空指针。
	TestFalse(TEXT("初始不处于检查态"), Player->IsInspectingPickup());
	TestNull(TEXT("初始没有检查物品"), Player->GetInspectedPickup());
	TestNull(TEXT("初始没有装备物品"), Player->GetEquippedPickup());
	TestEqual(TEXT("初始库存为空"), Player->GetStoredPickupCount(), 0);

	Player->RequestEquipPickup();
	Player->RequestStorePickup();
	Player->RequestPlacePickup();
	Player->RequestDropPickup();
	Player->RequestRotatePickup(nullptr, FECPInspectionRotationSample());
	Player->RequestFinalizePickupRotation(nullptr, FECPInspectionRotationSample());

	// 死亡、布娃娃边沿与 EndPlay 可能相邻触发；重复清空必须是幂等操作。
	Player->DropAllOwnedPickups();
	Player->DropAllOwnedPickups();
	TestFalse(TEXT("空动作后仍不处于检查态"), Player->IsInspectingPickup());
	TestEqual(TEXT("重复清空后库存仍为空"), Player->GetStoredPickupCount(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FECPPickupControllerEmptyInputModeTest,
	"ECP.Pickup.Controller.EmptyInputModeIsSafe",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FECPPickupControllerEmptyInputModeTest::RunTest(const FString& Parameters)
{
	AECPPlayerController* Controller = NewObject<AECPPlayerController>();
	TestNotNull(TEXT("测试控制器应能被创建"), Controller);

	// 尚未拥有 Pawn 时刷新输入模式必须安全，也不能错误显示检查态鼠标。
	Controller->RefreshPickupInputMode();
	TestFalse(TEXT("无 Pawn 时输入模式保持普通状态"), Controller->IsPickupInputModeActive());
	return true;
}

#endif
