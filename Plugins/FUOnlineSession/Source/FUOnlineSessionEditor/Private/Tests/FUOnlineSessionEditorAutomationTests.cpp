#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "FUOnlineSessionConfigManager.h"
#include "FU_OnlineSessionSettings.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFUOnlineSessionExternalDriverAnalysisTest,
	"FUOnlineSession.EditorConfig.ExternalDriverAnalysis",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFUOnlineSessionExternalDriverAnalysisTest::RunTest(const FString& Parameters)
{
	const FString NoExternalDriver = TEXT(
		"[OnlineSubsystem]\n"
		"DefaultPlatformService=Steam\n");

	TestEqual(
		TEXT("没有外部 GameNetDriver 时可以安全写入"),
		FFUOnlineSessionConfigManager::AnalyzeExternalGameNetDriver(NoExternalDriver),
		EFU_ExternalGameNetDriverState::None);

	const FString CompatibleLegacySteamDriver = TEXT(
		"[/Script/Engine.GameEngine]\n"
		"+NetDriverDefinitions=(DefName=\"GameNetDriver\",DriverClassName=\"OnlineSubsystemSteam.SteamNetDriver\",DriverClassNameFallback=\"OnlineSubsystemUtils.IpNetDriver\")\n");

	TestEqual(
		TEXT("旧写法但语义相同的 Steam 驱动只视为兼容重复项"),
		FFUOnlineSessionConfigManager::AnalyzeExternalGameNetDriver(CompatibleLegacySteamDriver),
		EFU_ExternalGameNetDriverState::Compatible);

	const FString ConflictingCustomDriver = TEXT(
		"[/Script/Engine.GameEngine]\n"
		"+NetDriverDefinitions=(DefName=\"GameNetDriver\",DriverClassName=\"/Script/MyNetwork.MyNetDriver\",DriverClassNameFallback=\"/Script/OnlineSubsystemUtils.IpNetDriver\")\n");

	TestEqual(
		TEXT("自定义 GameNetDriver 不允许被插件静默覆盖"),
		FFUOnlineSessionConfigManager::AnalyzeExternalGameNetDriver(ConflictingCustomDriver),
		EFU_ExternalGameNetDriverState::Conflict);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFUOnlineSessionManagedConfigBlockTest,
	"FUOnlineSession.EditorConfig.ManagedBlockUsesSteamSockets",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFUOnlineSessionManagedConfigBlockTest::RunTest(const FString& Parameters)
{
	UFU_OnlineSessionSettings* Settings = NewObject<UFU_OnlineSessionSettings>();
	Settings->SteamDevAppId = 480;

	const FString ManagedBlock =
		FFUOnlineSessionConfigManager::BuildManagedConfigBlock(*Settings);

	// 自动配置必须生成 UE 5.8 SteamSockets 模块中的真实驱动类。
	TestTrue(
		TEXT("管理区块包含 SteamSocketsNetDriver"),
		ManagedBlock.Contains(TEXT("/Script/SteamSockets.SteamSocketsNetDriver")));

	TestTrue(
		TEXT("管理区块包含 SteamSocketsNetConnection"),
		ManagedBlock.Contains(TEXT("/Script/SteamSockets.SteamSocketsNetConnection")));

	// 【FU 回归测试：双 Provider 的 SocketSubsystem 隔离】
	// bUseSteamNetworking 控制 SteamSockets 是否成为“全局默认”SocketSubsystem。
	// 如果这里为 true，运行时即使把 NetDriver 类切换成 IpNetDriver，IpNetDriver 仍可能
	// 拿到 SteamSockets，并在创建 LAN 广播 Socket 时因 SO_BROADCAST 不受支持而监听失败。
	// 因此全局默认必须保留为平台原生 Socket；Steam 模板仍会通过显式
	// SteamSocketsNetDriver 使用 SteamSockets，这不会关闭 Steam Lobby 联机。
	TestTrue(
		TEXT("管理区块禁止 SteamSockets 接管全局默认 SocketSubsystem"),
		ManagedBlock.Contains(TEXT("bUseSteamNetworking=false")));

	TestFalse(
		TEXT("管理区块不能重新生成会破坏 LAN 广播的旧配置"),
		ManagedBlock.Contains(TEXT("bUseSteamNetworking=true")));

	// 旧路径会加载失败并静默回退到 IpNetDriver，这个回归必须被测试阻止。
	TestFalse(
		TEXT("管理区块不再生成旧 OnlineSubsystemSteam NetDriver 路径"),
		ManagedBlock.Contains(TEXT("/Script/OnlineSubsystemSteam.SteamNetDriver")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFUOnlineSessionProviderOperationPreflightWiringTest,
	"FUOnlineSession.Runtime.ProviderOperationsRequireReadyStatus",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFUOnlineSessionProviderOperationPreflightWiringTest::RunTest(const FString& Parameters)
{
	// 【FU 回归测试：运行时仍需自我保护】
	// CheckSteamProviderStatus/CheckLanProviderStatus 是给蓝图展示 UI 的便利接口，
	// 但插件不能假设每个使用者都会先调用它。这里直接检查 Runtime 模板入口的源码接线，
	// 防止以后重构时漏掉某一个入口，使未登录 Steam 的请求再次进入异步 OSS 流程。
	const FString RuntimeSourcePath = FPaths::Combine(
		FPaths::ProjectPluginsDir(),
		TEXT("FUOnlineSession/Source/FUOnlineSession/Private/FU_OnlineSessionSubsystem.cpp"));

	FString RuntimeSource;
	TestTrue(
		TEXT("能够读取 FU Online Session Runtime 实现"),
		FFileHelper::LoadFileToString(RuntimeSource, *RuntimeSourcePath));

	TestTrue(
		TEXT("创建模板入口必须检查 Provider 是否 Ready"),
		RuntimeSource.Contains(TEXT("FU_ValidateProviderReady<Provider>(TEXT(\"CreateSession\"))")));

	TestTrue(
		TEXT("搜索模板入口必须检查 Provider 是否 Ready"),
		RuntimeSource.Contains(TEXT("FU_ValidateProviderReady<Provider>(TEXT(\"FindSessions\"))")));

	TestTrue(
		TEXT("加入模板入口必须检查 Provider 是否 Ready"),
		RuntimeSource.Contains(TEXT("FU_ValidateProviderReady<Provider>(TEXT(\"JoinSession\"))")));

	return true;
}

#endif
