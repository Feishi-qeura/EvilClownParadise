#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "FU_OnlineSessionTypes.h"
#include "FU_OnlineSessionRequestValidation.h"
#include "FU_OnlineProviderStatusEvaluator.h"
#include "FU_CheckSessionStatusAsync.h"
#include "ProviderTraits/FU_OnlineSessionProviderTraits.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFUOnlineSessionDefaultResultTest,"FUOnlineSession.Types.DefaultResult",EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFUOnlineSessionDefaultResultTest::RunTest(const FString& Parameters)
{
	FFU_SessionResult Result;
	TestEqual(TEXT("Default session id"), Result.SessionId, FString());
	TestEqual(TEXT("Default max players"), Result.MaxPlayers, 0);
	TestEqual(TEXT("Default current players"), Result.CurrentPlayers, 0);
	TestEqual(TEXT("Default ping"), Result.PingInMs, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFUOnlineSessionRequestValidationTest,"FUOnlineSession.Session.Validation",EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFUOnlineSessionRequestValidationTest::RunTest(const FString& Parameters)
{
	TestFalse(TEXT("Reject zero players"), FFU_SessionRequestValidation::CanCreate(0, TEXT("Room")));
	TestFalse(TEXT("Reject empty room name"), FFU_SessionRequestValidation::CanCreate(2, FString()));
	TestTrue(TEXT("Accept a valid request"), FFU_SessionRequestValidation::CanCreate(2, TEXT("Room")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFUOnlineSessionPingNullWorldTest,"FUOnlineSession.Ping.NullWorld",EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFUOnlineSessionPingNullWorldTest::RunTest(const FString& Parameters)
{
	TestNull(TEXT("空世界上下文被拒绝"), UFU_CheckSessionStatusAsync::FU_CheckSessionStatus(nullptr, 1.0f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFUOnlineSessionProviderStatusEvaluationTest,"FUOnlineSession.ProviderStatus.Evaluation",EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFUOnlineSessionProviderStatusEvaluationTest::RunTest(const FString& Parameters)
{
	//测试少子系统时，误报成后续的接口或登录错误
	FFU_OnlineProviderStatusInputs Inputs;
	Inputs.bHasWorld = true;
	Inputs.bHasSubsystem = false;

	TestEqual(
		TEXT("缺少子系统时返回 SubsystemUnavailable"),
		FFU_OnlineProviderStatusEvaluator::Evaluate(EFU_OnlineProvider::Steam, Inputs),
		EFU_OnlineProviderStatusCode::SubsystemUnavailable);

	//LAN/NULL不依赖平台账号，因此即使没有Identity，Interface也应该可以创建局域网Session
	Inputs.bHasSubsystem = true;
	Inputs.bHasSessionInterface = false;

	TestEqual(
		TEXT("缺少 Session Interface 时返回 SessionInterfaceUnavailable"),
		FFU_OnlineProviderStatusEvaluator::Evaluate(EFU_OnlineProvider::Lan, Inputs),
		EFU_OnlineProviderStatusCode::SessionInterfaceUnavailable);

	Inputs.bHasSessionInterface = true;
	Inputs.bHasNetDriverDefinition = true;
	Inputs.bHasNetDriverClass = true;
	Inputs.bHasIdentityInterface = false;
	Inputs.bIsLoggedIn = false;

	TestEqual(
		TEXT("LAN 不要求 Identity 登录"),
		FFU_OnlineProviderStatusEvaluator::Evaluate(EFU_OnlineProvider::Lan, Inputs),
		EFU_OnlineProviderStatusCode::Ready);

	//Steam Lobby依赖Steam用户身份；接口存在但未登录时必须给蓝图明确原因
	TestEqual(
		TEXT("Steam 缺少 Identity Interface 时返回 IdentityInterfaceUnavailable"),
		FFU_OnlineProviderStatusEvaluator::Evaluate(EFU_OnlineProvider::Steam, Inputs),
		EFU_OnlineProviderStatusCode::IdentityInterfaceUnavailable);

	Inputs.bHasIdentityInterface = true;

	TestEqual(
		TEXT("Steam 未登录时返回 NotLoggedIn"),
		FFU_OnlineProviderStatusEvaluator::Evaluate(EFU_OnlineProvider::Steam, Inputs),
		EFU_OnlineProviderStatusCode::NotLoggedIn);

	Inputs.bIsLoggedIn = true;

	TestEqual(
		TEXT("Steam 所有依赖可用时返回 Ready"),
		FFU_OnlineProviderStatusEvaluator::Evaluate(EFU_OnlineProvider::Steam, Inputs),
		EFU_OnlineProviderStatusCode::Ready);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFUOnlineSessionProviderNetDriverTraitsTest,
	"FUOnlineSession.ProviderTraits.NetDriverSelection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFUOnlineSessionProviderNetDriverTraitsTest::RunTest(const FString& Parameters)
{
	// 这个测试防止 Steam Lobby 再次被交给 IpNetDriver。
	// 一旦映射错误，steam.<SteamId> 会被当作普通 DNS 主机名并产生 AddressResolutionFailed。
	TestEqual(
		TEXT("Steam Provider 必须选择 SteamSocketsNetDriver"),
		TFU_OnlineSessionProviderTraits<EFU_OnlineProvider::Steam>::GetNetDriverClassName(),
		FName(TEXT("/Script/SteamSockets.SteamSocketsNetDriver")));

	// NULL/LAN 的解析结果是 IPv4 地址，因此它必须继续使用标准 IpNetDriver。
	TestEqual(
		TEXT("LAN Provider 必须选择 IpNetDriver"),
		TFU_OnlineSessionProviderTraits<EFU_OnlineProvider::Lan>::GetNetDriverClassName(),
		FName(TEXT("/Script/OnlineSubsystemUtils.IpNetDriver")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFUOnlineSessionProviderNetDriverStatusTest,
	"FUOnlineSession.ProviderStatus.NetDriverRequirements",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFUOnlineSessionProviderNetDriverStatusTest::RunTest(const FString& Parameters)
{
	FFU_OnlineProviderStatusInputs Inputs;
	Inputs.bHasWorld = true;
	Inputs.bHasSubsystem = true;
	Inputs.bHasSessionInterface = true;
	Inputs.bHasIdentityInterface = true;
	Inputs.bIsLoggedIn = true;

	// Session/Identity 正常并不代表可以联网；GameNetDriver 定义缺失时必须阻止按钮继续执行。
	Inputs.bHasNetDriverDefinition = true;
	Inputs.bHasNetDriverClass = true;
	Inputs.bHasConflictingActiveNetDriver = true;

	Inputs.bHasNetDriverClass = true;
	TestEqual(
		TEXT("NetDriver 与 Provider 依赖全部存在时才返回 Ready"),
		FFU_OnlineProviderStatusEvaluator::Evaluate(EFU_OnlineProvider::Steam, Inputs),
		EFU_OnlineProviderStatusCode::Ready);

	return true;
}

#endif
