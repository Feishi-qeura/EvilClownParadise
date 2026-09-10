#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "FUOnlineSessionConfigManager.h"

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
		FFUOnlineSessionConfigManager::AnalyzeExternalGameNetDriver(NoExternalDriver, true),
		EFU_ExternalGameNetDriverState::None);

	const FString CompatibleLegacySteamDriver = TEXT(
		"[/Script/Engine.GameEngine]\n"
		"+NetDriverDefinitions=(DefName=\"GameNetDriver\",DriverClassName=\"OnlineSubsystemSteam.SteamNetDriver\",DriverClassNameFallback=\"OnlineSubsystemUtils.IpNetDriver\")\n");

	TestEqual(
		TEXT("旧写法但语义相同的 Steam 驱动只视为兼容重复项"),
		FFUOnlineSessionConfigManager::AnalyzeExternalGameNetDriver(CompatibleLegacySteamDriver, true),
		EFU_ExternalGameNetDriverState::Compatible);

	const FString ConflictingCustomDriver = TEXT(
		"[/Script/Engine.GameEngine]\n"
		"+NetDriverDefinitions=(DefName=\"GameNetDriver\",DriverClassName=\"/Script/MyNetwork.MyNetDriver\",DriverClassNameFallback=\"/Script/OnlineSubsystemUtils.IpNetDriver\")\n");

	TestEqual(
		TEXT("自定义 GameNetDriver 不允许被插件静默覆盖"),
		FFUOnlineSessionConfigManager::AnalyzeExternalGameNetDriver(ConflictingCustomDriver, true),
		EFU_ExternalGameNetDriverState::Conflict);

	return true;
}

#endif
