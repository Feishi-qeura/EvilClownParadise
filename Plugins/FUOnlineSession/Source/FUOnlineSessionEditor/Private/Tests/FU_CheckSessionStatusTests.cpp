#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "FU_CheckSessionStatusAsync.h"
#include "FU_CheckSessionStatusTestReceiver.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "IpConnection.h"
#include "IpNetDriver.h"
#include "UObject/StrongObjectPtr.h"
#include "HAL/PlatformProcess.h"
#include <limits>

namespace
{
	// 使用真实 World、TimerManager、节点及动态委托；只手动提供网络状态，不建立外部 Socket。
	struct FSessionStatusFixture
	{
		TGuardValue<uint64> FrameGuard{GFrameCounter, GFrameCounter};
		UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
		TStrongObjectPtr<UGameInstance> GameInstance{NewObject<UGameInstance>()};
		TStrongObjectPtr<UFU_CheckSessionStatusTestReceiver> Receiver{NewObject<UFU_CheckSessionStatusTestReceiver>()};
		TStrongObjectPtr<UFU_CheckSessionStatusAsync> Action;
		UIpNetDriver* Driver = nullptr;
		UIpConnection* Connection = nullptr;

		FSessionStatusFixture()
		{
			World->SetGameInstance(GameInstance.Get());
		}

		~FSessionStatusFixture()
		{
			// 模拟关卡结束，给生产清理回调执行机会，再解除测试构造的网络引用。
			FWorldDelegates::OnWorldBeginTearDown.Broadcast(World);
			if (Action.IsValid())
			{
				World->GetTimerManager().ClearAllTimersForObject(Action.Get());
				Action->SetReadyToDestroy();
			}
			World->SetNetDriver(nullptr);
			if (Driver)
			{
				Driver->ServerConnection = nullptr;
				Driver->SetWorld(nullptr);
			}
			if (Connection)
			{
				Connection->Driver = nullptr;
			}
			World->DestroyWorld(false);
		}

		void MakeClient()
		{
			Driver = NewObject<UIpNetDriver>(World);
			Connection = NewObject<UIpConnection>(Driver);
			Driver->ServerConnection = Connection;
			Driver->NetDriverName = NAME_GameNetDriver;
			Driver->bNoTimeouts = false;
			Driver->InitialConnectTimeout = 5.0f;
			Driver->ConnectionTimeout = 5.0f;
			Connection->Driver = Driver;
			Connection->SetConnectionState(USOCK_Open);
			Driver->SetWorld(World);
			World->SetNetDriver(Driver);
		}

		APlayerController* AddPlayer(bool bWithState)
		{
			APlayerController* Player = World->SpawnActor<APlayerController>();
			Player->SetRole(ROLE_AutonomousProxy);
			World->AddController(Player);
			if (bWithState)
			{
				Player->PlayerState = World->SpawnActor<APlayerState>();
				Player->PlayerState->ExactPing = 42.0f;
			}
			return Player;
		}

		void Start()
		{
			Action.Reset(UFU_CheckSessionStatusAsync::FU_CheckSessionStatus(World, 0.5f));
			Action->OnServer.AddDynamic(Receiver.Get(), &UFU_CheckSessionStatusTestReceiver::ReceiveServer);
			Action->OnClient.AddDynamic(Receiver.Get(), &UFU_CheckSessionStatusTestReceiver::ReceiveClient);
			Action->ClientConnectionOvertime.AddDynamic(Receiver.Get(), &UFU_CheckSessionStatusTestReceiver::ReceiveTimeout);
			Action->Activate();
			Tick(0.01f);
		}

		void Tick(float Seconds = 0.51f)
		{
			// TimerManager 每帧只执行一次；推进帧编号，让测试不依赖编辑器实际帧率。
			++GFrameCounter;
			World->GetTimerManager().Tick(Seconds);
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFUSessionStatusStartupTest, "FUOnlineSession.Ping.StartupAndRecovery", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FFUSessionStatusStartupTest::RunTest(const FString& Parameters)
{
	// 捕获“初始化尚未复制出 PC/PlayerState，就永久停止节点”的回归。
	FSessionStatusFixture Fixture;
	Fixture.MakeClient();
	Fixture.Start();
	TestEqual(TEXT("等待 PlayerController 不误报超时"), Fixture.Receiver->TimeoutCount, 0);
	APlayerController* Player = Fixture.AddPlayer(false);
	Fixture.Tick();
	TestEqual(TEXT("等待 PlayerState 不误报超时"), Fixture.Receiver->TimeoutCount, 0);
	Player->PlayerState = Fixture.World->SpawnActor<APlayerState>();
	Player->PlayerState->ExactPing = 42.0f;
	Fixture.Tick();
	Fixture.Tick();
	TestEqual(TEXT("就绪后持续输出客户端 Ping"), Fixture.Receiver->ClientCount, 2);
	TestEqual(TEXT("返回实际缓存 Ping"), Fixture.Receiver->LastPing, 42.0f);
	APlayerState* SavedState = Player->PlayerState;
	Player->PlayerState = nullptr;
	Fixture.Tick();
	TestEqual(TEXT("短暂缺少状态不结束监测"), Fixture.Receiver->TimeoutCount, 0);
	Player->PlayerState = SavedState;
	Fixture.Tick();
	TestEqual(TEXT("状态恢复后继续回调"), Fixture.Receiver->ClientCount, 3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFUSessionStatusServerTest, "FUOnlineSession.Ping.ServerOnce", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FFUSessionStatusServerTest::RunTest(const FString& Parameters)
{
	// 服务端分支不能依赖先存在 PlayerController，重复 Activate 也不能重复广播。
	FSessionStatusFixture Fixture;
	Fixture.Start();
	Fixture.Action->Activate();
	Fixture.Tick();
	TestEqual(TEXT("Standalone 仅一次 OnServer"), Fixture.Receiver->ServerCount, 1);
	TestEqual(TEXT("服务端不广播客户端输出"), Fixture.Receiver->ClientCount, 0);
	TestEqual(TEXT("无 PC 不代表服务端超时"), Fixture.Receiver->TimeoutCount, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFUSessionStatusSilenceTest, "FUOnlineSession.Ping.ReceiveTimeout", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FFUSessionStatusSilenceTest::RunTest(const FString& Parameters)
{
	// 对象仍存在时也必须识别真正的收包超时，不能一直输出旧 Ping。
	FSessionStatusFixture Fixture;
	Fixture.MakeClient();
	Fixture.AddPlayer(true);
	Fixture.Start();
	Fixture.Connection->LastReceiveTime = Fixture.Driver->GetElapsedTime() - 1.0;
	Fixture.Tick();
	TestEqual(TEXT("短暂抖动仍持续采样"), Fixture.Receiver->ClientCount, 2);
	Fixture.Connection->LastReceiveTime = Fixture.Driver->GetElapsedTime();
	Fixture.Tick();
	TestEqual(TEXT("收包恢复不误报超时"), Fixture.Receiver->TimeoutCount, 0);
	Fixture.Connection->LastReceiveTime = Fixture.Driver->GetElapsedTime() - 10000.0;
	Fixture.Tick();
	Fixture.Tick();
	TestEqual(TEXT("持续无收包仅报告一次超时"), Fixture.Receiver->TimeoutCount, 1);
	TestEqual(TEXT("超时后不再广播旧 Ping"), Fixture.Receiver->ClientCount, 3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFUSessionStatusCleanupTest, "FUOnlineSession.Ping.WorldCleanup", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FFUSessionStatusCleanupTest::RunTest(const FString& Parameters)
{
	// 使用真实生命周期委托，捕获切图后旧 World 的 timer 仍广播的回归。
	FSessionStatusFixture Fixture;
	Fixture.MakeClient();
	Fixture.AddPlayer(true);
	Fixture.Start();
	FWorldDelegates::OnWorldBeginTearDown.Broadcast(Fixture.World);
	Fixture.Tick();
	TestEqual(TEXT("切图停止采样"), Fixture.Receiver->ClientCount, 1);
	TestEqual(TEXT("正常切图不误报断线"), Fixture.Receiver->TimeoutCount, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFUSessionStatusIntervalTest, "FUOnlineSession.Ping.InvalidInterval", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FFUSessionStatusIntervalTest::RunTest(const FString& Parameters)
{
	// 非有限刷新间隔不能注册永远不执行的节点。
	FSessionStatusFixture Fixture;
	TestNull(TEXT("拒绝零刷新间隔"), UFU_CheckSessionStatusAsync::FU_CheckSessionStatus(Fixture.World, 0.0f));
	TestNull(TEXT("拒绝 NaN 刷新间隔"), UFU_CheckSessionStatusAsync::FU_CheckSessionStatus(Fixture.World, std::numeric_limits<float>::quiet_NaN()));
	TestNull(TEXT("拒绝无穷刷新间隔"), UFU_CheckSessionStatusAsync::FU_CheckSessionStatus(Fixture.World, std::numeric_limits<float>::infinity()));
	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFUSessionStatusNetworkFailureTest, "FUOnlineSession.Ping.NetworkFailureIsolation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FFUSessionStatusNetworkFailureTest::RunTest(const FString& Parameters)
{
	// 引擎会把主动注入的四次故障写为 Error；仅接受这些带测试标记的预期日志。
	AddExpectedError(TEXT("ErrorString = FU test:"), EAutomationExpectedErrorFlags::Contains, 4);
	// PIE 的其他 World 和同一 World 的 Beacon 失败不能终止游戏连接；重复故障也只输出一次。
	FSessionStatusFixture Fixture;
	Fixture.MakeClient();
	Fixture.AddPlayer(true);
	Fixture.Start();
	FSessionStatusFixture OtherWorld;
	UIpNetDriver* OtherDriver = NewObject<UIpNetDriver>(Fixture.World);
	GEngine->BroadcastNetworkFailure(OtherWorld.World, Fixture.Driver, ENetworkFailure::ConnectionLost, TEXT("FU test: other world"));
	GEngine->BroadcastNetworkFailure(Fixture.World, OtherDriver, ENetworkFailure::ConnectionLost, TEXT("FU test: other driver"));
	TestEqual(TEXT("无关网络事件不触发超时"), Fixture.Receiver->TimeoutCount, 0);
	GEngine->BroadcastNetworkFailure(Fixture.World, Fixture.Driver, ENetworkFailure::ConnectionLost, TEXT("FU test: game connection"));
	GEngine->BroadcastNetworkFailure(Fixture.World, Fixture.Driver, ENetworkFailure::ConnectionTimeout, TEXT("FU test: duplicate"));
	Fixture.Tick();
	TestEqual(TEXT("自身断线仅触发一次"), Fixture.Receiver->TimeoutCount, 1);
	TestEqual(TEXT("明确断线后停止采样"), Fixture.Receiver->ClientCount, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFUSessionStatusDeadlineTest, "FUOnlineSession.Ping.ReadinessDeadline", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FFUSessionStatusDeadlineTest::RunTest(const FString& Parameters)
{
	// 用短配置验证真正的宽限到期，防止把提前退出改成无限等待。
	FSessionStatusFixture Fixture;
	Fixture.MakeClient();
	Fixture.Driver->InitialConnectTimeout = 0.02f;
	Fixture.Start();
	TestEqual(TEXT("初始化宽限期间无超时"), Fixture.Receiver->TimeoutCount, 0);
	FPlatformProcess::Sleep(0.03f);
	Fixture.Tick();
	Fixture.Tick();
	TestEqual(TEXT("初始化超时有且仅有一次输出"), Fixture.Receiver->TimeoutCount, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFUSessionStatusSchedulingTest, "FUOnlineSession.Ping.SchedulingAndStop", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FFUSessionStatusSchedulingTest::RunTest(const FString& Parameters)
{
	// 重入不能额外广播；长帧也不能补发多个相同 Ping；显式结束必须清除 timer。
	FSessionStatusFixture Fixture;
	Fixture.MakeClient();
	Fixture.AddPlayer(true);
	Fixture.Start();
	Fixture.Action->Activate();
	TestEqual(TEXT("重复 Activate 不重复首帧输出"), Fixture.Receiver->ClientCount, 1);
	Fixture.Tick(5.01f);
	TestEqual(TEXT("长帧最多追加一次输出"), Fixture.Receiver->ClientCount, 2);
	Fixture.Action->SetReadyToDestroy();
	Fixture.Tick();
	TestEqual(TEXT("显式结束后不再输出"), Fixture.Receiver->ClientCount, 2);
	TestEqual(TEXT("显式结束不误报超时"), Fixture.Receiver->TimeoutCount, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFUSessionStatusNoTimeoutTest, "FUOnlineSession.Ping.RespectNoTimeouts", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FFUSessionStatusNoTimeoutTest::RunTest(const FString& Parameters)
{
	// 调试器暂停等开发场景允许引擎禁用收包超时，监测器不能绕过该配置误报。
	FSessionStatusFixture Fixture;
	Fixture.MakeClient();
	Fixture.Driver->bNoTimeouts = true;
	Fixture.Connection->LastReceiveTime = Fixture.Driver->GetElapsedTime() - 10000.0;
	Fixture.AddPlayer(true);
	Fixture.Start();
	Fixture.Tick();
	TestEqual(TEXT("禁用超时仍持续读取 Ping"), Fixture.Receiver->ClientCount, 2);
	TestEqual(TEXT("遵循驱动禁用收包超时配置"), Fixture.Receiver->TimeoutCount, 0);
	return true;
}

#endif

