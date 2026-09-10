#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
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

	// Steam 与 LAN 已由两个模板入口分开；静态配置必须始终打开 Steam 传输，
	// LAN 按钮会在运行时切换到 IpNetDriver，不再依赖一个互相冲突的全局开关。
	TestTrue(
		TEXT("管理区块始终启用 Steam Networking"),
		ManagedBlock.Contains(TEXT("bUseSteamNetworking=true")));

	// 旧路径会加载失败并静默回退到 IpNetDriver，这个回归必须被测试阻止。
	TestFalse(
		TEXT("管理区块不再生成旧 OnlineSubsystemSteam NetDriver 路径"),
		ManagedBlock.Contains(TEXT("/Script/OnlineSubsystemSteam.SteamNetDriver")));

	return true;
}

#endif
