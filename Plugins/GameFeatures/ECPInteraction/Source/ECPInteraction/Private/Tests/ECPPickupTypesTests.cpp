#if WITH_DEV_AUTOMATION_TESTS

#include "Actors/ECPPickupTypes.h"
#include "Data/ECPPlayerController.h"

#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FECPPickupTransitionTest, "ECP.Pickup.Rules.Transitions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FECPPickupTransitionTest::RunTest(const FString& Parameters)
{
	// 这些断言固定玩法允许的七条边，防止未来新增按钮时绕过既定状态机。
	TestTrue(TEXT("World can enter Pickup"), ECPPickupRules::CanTransition(EECPPickupState::World, EECPPickupState::Pickup));
	TestTrue(TEXT("Pickup can return to World"), ECPPickupRules::CanTransition(EECPPickupState::Pickup, EECPPickupState::World));
	TestTrue(TEXT("Pickup can enter Equipped"), ECPPickupRules::CanTransition(EECPPickupState::Pickup, EECPPickupState::Equipped));
	TestTrue(TEXT("Pickup can enter Stored"), ECPPickupRules::CanTransition(EECPPickupState::Pickup, EECPPickupState::Stored));
	TestTrue(TEXT("Equipped can return to World"), ECPPickupRules::CanTransition(EECPPickupState::Equipped, EECPPickupState::World));
	TestTrue(TEXT("Equipped can enter Stored"), ECPPickupRules::CanTransition(EECPPickupState::Equipped, EECPPickupState::Stored));
	TestTrue(TEXT("Stored can return to World"), ECPPickupRules::CanTransition(EECPPickupState::Stored, EECPPickupState::World));
	TestFalse(TEXT("World cannot skip directly to Equipped"), ECPPickupRules::CanTransition(EECPPickupState::World, EECPPickupState::Equipped));
	TestFalse(TEXT("Stored cannot equip without first returning to World"), ECPPickupRules::CanTransition(EECPPickupState::Stored, EECPPickupState::Equipped));
	TestFalse(TEXT("Same-state requests are not transitions"), ECPPickupRules::CanTransition(EECPPickupState::Pickup, EECPPickupState::Pickup));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FECPPickupPriorityTest, "ECP.Pickup.Rules.Priority",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FECPPickupPriorityTest::RunTest(const FString& Parameters)
{
	// 字面量期望独立于实现：主机最高，其次低延迟；同延迟才用加入与到达序号稳定排序。
	TestTrue(TEXT("Earlier server frame cannot be preempted by a later host request"),
		ECPPickupRules::IsHigherPriority({100, false, 999.f, 99, 99}, {101, true, 1.f, 0, 0}));
	TestTrue(TEXT("Listen host wins among requests from the same server frame"),
		ECPPickupRules::IsHigherPriority({100, true, 999.f, 99, 99}, {100, false, 1.f, 0, 0}));
	TestTrue(TEXT("Lower ping wins among remote players"),
		ECPPickupRules::IsHigherPriority({100, false, 20.f, 9, 99}, {100, false, 80.f, 1, 0}));
	TestTrue(TEXT("Lower join order stabilizes equal ping"),
		ECPPickupRules::IsHigherPriority({100, false, 20.f, 2, 99}, {100, false, 20.f, 3, 0}));
	TestTrue(TEXT("Earlier arrival stabilizes equal player priority"),
		ECPPickupRules::IsHigherPriority({100, false, 20.f, 2, 4}, {100, false, 20.f, 2, 5}));
	TestFalse(TEXT("Equal priorities do not replace the existing candidate"),
		ECPPickupRules::IsHigherPriority({100, false, 20.f, 2, 4}, {100, false, 20.f, 2, 4}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FECPPickupRotationClampTest, "ECP.Pickup.Rules.RotationClamp",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FECPPickupRotationClampTest::RunTest(const FString& Parameters)
{
	const FRotator Clamped = ECPPickupRules::ClampRotationDelta(FRotator(75.f, -42.f, 4.f), 15.f);
	// 单次网络旋转三个轴都必须受限，不能只限制鼠标常用的 Pitch/Yaw。
	TestEqual(TEXT("Pitch is clamped"), Clamped.Pitch, 15.0);
	TestEqual(TEXT("Yaw is clamped"), Clamped.Yaw, -15.0);
	TestEqual(TEXT("Roll remains inside the limit"), Clamped.Roll, 4.0);
	const FRotator Authoritative(0.f, 10.f, 0.f);
	const FRotator Predicted(0.f, 35.f, 0.f);
	TestEqual(TEXT("A pending local prediction remains the next drag start"),
		ECPPickupRules::ResolveInspectionStartRotation(Authoritative, Predicted, true), Predicted);
	TestEqual(TEXT("Without a prediction the authoritative rotation is used"),
		ECPPickupRules::ResolveInspectionStartRotation(Authoritative, Predicted, false), Authoritative);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FECPPickupMassResponseTest, "ECP.Pickup.Rules.MassResponse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FECPPickupMassResponseTest::RunTest(const FString& Parameters)
{
	const float LightDelta = ECPPickupRules::ExpectedVelocityDelta(1000.f, 1.f);
	const float HeavyDelta = ECPPickupRules::ExpectedVelocityDelta(1000.f, 10.f);
	const float WeakImpulse = ECPPickupRules::ExpectedThrowImpulse(1000.f, 1.f);
	const float StrongImpulse = ECPPickupRules::ExpectedThrowImpulse(1000.f, 2.f);
	TestTrue(TEXT("The same impulse accelerates a lighter object more"), LightDelta > HeavyDelta);
	TestEqual(TEXT("One kilogram receives the literal impulse delta"), LightDelta, 1000.f);
	TestEqual(TEXT("Ten kilograms receives one tenth of the velocity delta"), HeavyDelta, 100.f);
	TestEqual(TEXT("Double player strength produces double throw impulse"), StrongImpulse, WeakImpulse * 2.f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FECPPickupControllerPriorityTest, "ECP.Pickup.Rules.ControllerPriority",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FECPPickupControllerPriorityTest::RunTest(const FString& Parameters)
{
	AECPPlayerController* Controller = NewObject<AECPPlayerController>();
	TestNotNull(TEXT("Controller test object is created"), Controller);
	if (!Controller)
	{
		return false;
	}
	Controller->SetPickupJoinOrder(7, true);
	const FECPPickupPriority Priority = Controller->GetPickupPriority(123, 11);
	TestEqual(TEXT("Server frame is preserved"), Priority.ServerFrame, static_cast<uint64>(123));
	TestTrue(TEXT("Listen Server host flag is preserved"), Priority.bListenServerHost);
	TestEqual(TEXT("Missing PlayerState uses lowest latency priority"), Priority.PingMilliseconds, MAX_flt);
	TestEqual(TEXT("Server join order is preserved"), Priority.JoinOrder, 7);
	TestEqual(TEXT("Per-item arrival order is preserved"), Priority.ArrivalOrder, static_cast<uint32>(11));
	return true;
}

#endif
