#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Misc/App.h"
#include "HAL/FileManager.h"
#include "FU_OnlineSessionTypes.h"
#include "FU_OnlineDiagnosticTypes.h"
#include "Diagnostics/FU_OnlineSessionDiagnostics.h"
#include "FU_OnlineSessionRequestValidation.h"
#include "FU_OnlineProviderStatusEvaluator.h"
#include "FU_SteamAppIdBootstrap.h"
#include "FU_CheckSessionStatusAsync.h"
#include "ProviderTraits/FU_OnlineSessionProviderTraits.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFUOnlineSessionDefaultResultTest,"FUOnlineSession.Types.DefaultResult",EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFUOnlineSessionDiagnosticHistoryTest,
	"FUOnlineSession.Diagnostics.History",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFUOnlineSessionDiagnosticHistoryTest::RunTest(const FString& Parameters)
{
	// 【先写契约】诊断历史必须有固定上限，而且无论调用方误传什么字段，都不能把房间密码保留到
	// 日志、屏幕浮层或 Blueprint 事件中。这里关闭可选输出，只验证所有输出共享的脱敏历史源。
	FFU_OnlineDiagnosticDispatchConfig Config;
	Config.HistoryLimit = 2;
	Config.bEmitToLog = false;
	Config.bEnableOverlay = false;

	int32 BlueprintBroadcastCount = 0;
	FFU_OnlineSessionDiagnostics Diagnostics(
		Config,
		[&BlueprintBroadcastCount](const FFU_OnlineDiagnosticEvent&)
		{
			++BlueprintBroadcastCount;
		});

	FFU_OnlineDiagnosticEvent FirstEvent;
	FirstEvent.Code = TEXT("FU.Test.First");
	FFU_OnlineDiagnosticField SensitiveField;
	SensitiveField.Key = FName(TEXT("RoomPassword"));
	SensitiveField.Value = TEXT("secret");
	FirstEvent.Fields.Add(SensitiveField);
	Diagnostics.Emit(FirstEvent);

	FFU_OnlineDiagnosticEvent SecondEvent;
	SecondEvent.Code = TEXT("FU.Test.Second");
	Diagnostics.Emit(SecondEvent);

	FFU_OnlineDiagnosticEvent ThirdEvent;
	ThirdEvent.Code = TEXT("FU.Test.Third");
	Diagnostics.Emit(ThirdEvent);

	const TArray<FFU_OnlineDiagnosticEvent> History = Diagnostics.GetHistory();
	TestEqual(TEXT("诊断历史保留配置上限"), History.Num(), 2);
	TestEqual(TEXT("最旧记录在超过上限时被移除"), History[0].Code, FString(TEXT("FU.Test.Second")));
	TestEqual(TEXT("最新记录保持在历史末尾"), History[1].Code, FString(TEXT("FU.Test.Third")));
	TestTrue(TEXT("诊断序号在同一 GameInstance 中严格递增"), History[0].Sequence < History[1].Sequence);
	TestEqual(TEXT("关闭日志与浮层也不应关闭 Blueprint 诊断广播"), BlueprintBroadcastCount, 3);

	const FFU_OnlineDiagnosticEvent SanitizedFirst = FFU_OnlineSessionDiagnostics::Sanitize(FirstEvent);
	TestEqual(TEXT("敏感字段值必须被统一脱敏"), SanitizedFirst.Fields[0].Value, FString(TEXT("<redacted>")));

	// 再写入一次含敏感字段的事件以验证最终报告，而不是只验证内存结构；该写入仍会受两条上限约束。
	Diagnostics.Emit(FirstEvent);
	const FString Report = Diagnostics.BuildReport();
	TestFalse(TEXT("报告中绝不能出现原始房间密码"), Report.Contains(TEXT("secret"), ESearchCase::CaseSensitive));
	TestTrue(TEXT("报告明确标出已脱敏字段"), Report.Contains(TEXT("RoomPassword=<redacted>"), ESearchCase::CaseSensitive));

	FString SavedPath;
	FString SaveError;
	TestTrue(TEXT("诊断报告能保存到受限 Saved/Logs 目录"), Diagnostics.SaveReport(SavedPath, SaveError));
	TestTrue(TEXT("保存成功时报告文件存在"), IFileManager::Get().FileExists(*SavedPath));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFUOnlineSessionDiagnosticOverlayModelTest,
	"FUOnlineSession.Diagnostics.OverlayModel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFUOnlineSessionDiagnosticOverlayModelTest::RunTest(const FString& Parameters)
{
	// 【模型测试】不创建真实 Viewport，证明无窗口/Commandlet 情况下浮层只是缺席的输出通道，
	// 而不是让诊断分发器失效；纯模型也使严重级别和过期逻辑可稳定地自动化验证。
	FFU_OnlineDiagnosticDispatchConfig Config;
	Config.bEnableOverlay = true;
	Config.MinimumOverlaySeverity = EFU_OnlineDiagnosticSeverity::Warning;
	Config.OverlayDurationSeconds = 1.0f;
	Config.OverlayRowLimit = 2;

	FFU_OnlineDiagnosticOverlayModel OverlayModel;
	const FDateTime NowUtc(2026, 9, 11, 12, 0, 0);

	FFU_OnlineDiagnosticEvent InfoEvent;
	InfoEvent.Severity = EFU_OnlineDiagnosticSeverity::Info;
	InfoEvent.Code = TEXT("FU.Test.Info");
	InfoEvent.Message = TEXT("Info must not be shown at Warning threshold");
	OverlayModel.Add(InfoEvent, Config, NowUtc);
	TestEqual(TEXT("Info 默认低于 Warning 浮层阈值"), OverlayModel.GetVisibleRows(NowUtc).Num(), 0);

	FFU_OnlineDiagnosticEvent WarningEvent;
	WarningEvent.Severity = EFU_OnlineDiagnosticSeverity::Warning;
	WarningEvent.Code = TEXT("FU.Test.Warning");
	WarningEvent.Message = TEXT("Warning is visible");
	OverlayModel.Add(WarningEvent, Config, NowUtc);
	TestEqual(TEXT("Warning 进入可见浮层行"), OverlayModel.GetVisibleRows(NowUtc).Num(), 1);

	TestEqual(
		TEXT("过期浮层行自动隐藏"),
		OverlayModel.GetVisibleRows(NowUtc + FTimespan::FromSeconds(2.0)).Num(),
		0);
	return true;
}

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
	// 【FU 状态顺序】World 是所有 OnlineSubsystem 查询的根前置条件，必须先于后续依赖失败。
	FFU_OnlineProviderStatusInputs Inputs;
	TestEqual(
		TEXT("缺少 World 时返回 WorldUnavailable"),
		FFU_OnlineProviderStatusEvaluator::Evaluate(EFU_OnlineProvider::Steam, Inputs),
		EFU_OnlineProviderStatusCode::WorldUnavailable);

	//测试少子系统时，误报成后续的接口或登录错误
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
	// 【Steam 运行环境快照】本测试要继续验证旧的 Identity 顺序，所以先让新增前置全部就绪。
	Inputs.bSteamAppIdBootstrapReady = true;
	Inputs.bSteamSocketsModuleAvailable = true;
	Inputs.bSteamSocketsEnabled = true;
	Inputs.bSteamSocketsSocketSubsystemAvailable = true;
	Inputs.bSteamAppIdMatchesExpectation = true;

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
	FFUOnlineSessionSteamLobbyProjectIsolationTest,
	"FUOnlineSession.ProviderTraits.SteamLobbyProjectIsolation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFUOnlineSessionSteamLobbyProjectIsolationTest::RunTest(const FString& Parameters)
{
	// 【FU 回归测试：Steam 公共测试 AppID 的 Lobby 隔离】
	// SteamDevAppId=480 是许多开发者共享的 Spacewar 测试环境。如果创建房间时没有发布
	// 项目标识、搜索时也没有把同一个标识作为后端条件，Steam 会先返回大量无关 Lobby，
	// 本项目的房间可能在结果数量上限之外，从而出现“搜索成功但 RawResults=0”的现象。
	FOnlineSessionSettings SteamCreateSettings;
	TFU_OnlineSessionProviderTraits<EFU_OnlineProvider::Steam>::ConfigureCreateSettings(SteamCreateSettings);

	FString AdvertisedKeyword;
	TestTrue(
		TEXT("Steam 创建设置必须发布 Lobby 项目标识"),
		SteamCreateSettings.Get(SEARCH_KEYWORDS, AdvertisedKeyword));

	FOnlineSessionSearch SteamSearch;
	TFU_OnlineSessionProviderTraits<EFU_OnlineProvider::Steam>::ConfigureSearch(SteamSearch);

	FString RequestedKeyword;
	TestTrue(
		TEXT("Steam 搜索必须向后端提交 Lobby 项目标识"),
		SteamSearch.QuerySettings.Get(SEARCH_KEYWORDS, RequestedKeyword));
	TestFalse(TEXT("Steam Lobby 项目标识不能为空"), RequestedKeyword.IsEmpty());
	TestTrue(
		TEXT("Steam Lobby 项目标识必须区分当前 Unreal 项目"),
		RequestedKeyword.Contains(FApp::GetProjectName(), ESearchCase::CaseSensitive));
	TestEqual(
		TEXT("Steam 创建与搜索必须使用完全相同的 Lobby 项目标识"),
		RequestedKeyword,
		AdvertisedKeyword);
	TestEqual(
		TEXT("Steam Lobby 项目标识必须执行精确匹配"),
		SteamSearch.QuerySettings.GetComparisonOp(SEARCH_KEYWORDS),
		EOnlineComparisonOp::Equals);

	// NULL/LAN 使用 UDP 广播发现，不经过 Steam Lobby 后端。
	// 如果把 Steam 专用过滤条件误加到 LAN，会把两种 Provider 再次耦合起来。
	FOnlineSessionSettings LanCreateSettings;
	TFU_OnlineSessionProviderTraits<EFU_OnlineProvider::Lan>::ConfigureCreateSettings(LanCreateSettings);
	FString UnexpectedLanKeyword;
	TestFalse(
		TEXT("LAN 创建设置不应发布 Steam Lobby 项目标识"),
		LanCreateSettings.Get(SEARCH_KEYWORDS, UnexpectedLanKeyword));

	FOnlineSessionSearch LanSearch;
	TFU_OnlineSessionProviderTraits<EFU_OnlineProvider::Lan>::ConfigureSearch(LanSearch);
	TestFalse(
		TEXT("LAN 搜索不应携带 Steam Lobby 项目标识"),
		LanSearch.QuerySettings.Get(SEARCH_KEYWORDS, UnexpectedLanKeyword));

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
	// 【测试隔离】这里专门覆盖 NetDriver 顺序，Steam 的新增环境前置必须全部满足。
	Inputs.bSteamAppIdBootstrapReady = true;
	Inputs.bSteamSocketsModuleAvailable = true;
	Inputs.bSteamSocketsEnabled = true;
	Inputs.bSteamSocketsSocketSubsystemAvailable = true;
	Inputs.bSteamAppIdMatchesExpectation = true;

	// Session/Identity 正常并不代表可以联网；GameNetDriver 定义缺失时必须阻止按钮继续执行。
	Inputs.bHasNetDriverDefinition = true;
	Inputs.bHasNetDriverClass = true;
	Inputs.bHasConflictingActiveNetDriver = true;
	TestEqual(
		TEXT("活动驱动属于另一个 Provider 时拒绝切换"),
		FFU_OnlineProviderStatusEvaluator::Evaluate(EFU_OnlineProvider::Steam, Inputs),
		EFU_OnlineProviderStatusCode::ActiveNetDriverConflict);

	Inputs.bHasConflictingActiveNetDriver = false;
	Inputs.bHasNetDriverDefinition = false;
	TestEqual(
		TEXT("缺少 GameNetDriver Definition 时返回明确状态"),
		FFU_OnlineProviderStatusEvaluator::Evaluate(EFU_OnlineProvider::Steam, Inputs),
		EFU_OnlineProviderStatusCode::NetDriverDefinitionUnavailable);

	// 这正是本次日志暴露的问题：Steam 子系统可用，但目标 Steam 驱动类不能加载。
	Inputs.bHasNetDriverDefinition = true;
	Inputs.bHasNetDriverClass = false;
	TestEqual(
		TEXT("Steam NetDriver 类不可加载时不能误报 Ready"),
		FFU_OnlineProviderStatusEvaluator::Evaluate(EFU_OnlineProvider::Steam, Inputs),
		EFU_OnlineProviderStatusCode::NetDriverClassUnavailable);

	Inputs.bHasNetDriverClass = true;
	TestEqual(
		TEXT("NetDriver 与 Provider 依赖全部存在时才返回 Ready"),
		FFU_OnlineProviderStatusEvaluator::Evaluate(EFU_OnlineProvider::Steam, Inputs),
		EFU_OnlineProviderStatusCode::Ready);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFUOnlineSessionAppIdBootstrapTest,
	"FUOnlineSession.AppIdBootstrap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFUOnlineSessionAppIdBootstrapTest::RunTest(const FString& Parameters)
{
	// 【纯快照测试】不读取真实 Steam 客户端或 ini；每个输入都对应启动期的一个可复现分支。
	TestEqual(
		TEXT("False Add is not bootstrap success"),
		FFU_SteamAppIdBootstrap::Evaluate({ false, false, true, false, true, 480, 480 }),
		EFU_SteamBootstrapStatus::LayerAddFailed);

	TestEqual(
		TEXT("Existing Steam fails closed"),
		FFU_SteamAppIdBootstrap::Evaluate({ false, true, true, false, false, 480, 0 }),
		EFU_SteamBootstrapStatus::SteamAlreadyInstantiated);

	TestEqual(
		TEXT("Shipping never injects a development layer"),
		FFU_SteamAppIdBootstrap::Evaluate({ true, false, false, false, false, 0, 0 }),
		EFU_SteamBootstrapStatus::ShippingNoInjection);

	TestEqual(
		TEXT("非 Shipping 的非法开发 AppID 必须失败"),
		FFU_SteamAppIdBootstrap::Evaluate({ false, false, true, true, true, 0, 0 }),
		EFU_SteamBootstrapStatus::InvalidDevelopmentAppId);

	TestEqual(
		TEXT("缺少 Engine 配置分支必须失败"),
		FFU_SteamAppIdBootstrap::Evaluate({ false, false, false, false, false, 480, 0 }),
		EFU_SteamBootstrapStatus::BranchUnavailable);

	TestEqual(
		TEXT("不属于当前 Engine 分支的同标签层必须失败"),
		FFU_SteamAppIdBootstrap::Evaluate({ false, false, true, true, false, 480, 480 }),
		EFU_SteamBootstrapStatus::LayerOwnershipConflict);

	TestEqual(
		TEXT("读回 AppID 不一致必须失败"),
		FFU_SteamAppIdBootstrap::Evaluate({ false, false, true, true, true, 480, 481 }),
		EFU_SteamBootstrapStatus::EffectiveValueConflict);

	TestEqual(
		TEXT("完整非 Shipping 注入快照才 Ready"),
		FFU_SteamAppIdBootstrap::Evaluate({ false, false, true, true, true, 480, 480 }),
		EFU_SteamBootstrapStatus::Ready);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFUOnlineSessionSteamSocketsStatusTest,
	"FUOnlineSession.ProviderStatus.SteamSockets",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFUOnlineSessionSteamSocketsStatusTest::RunTest(const FString& Parameters)
{
	// 【共享基线】先让旧依赖都满足，后续每个断言只切断一层新增 Steam 前置。
	FFU_OnlineProviderStatusInputs Inputs;
	Inputs.bHasWorld = true;
	Inputs.bHasSubsystem = true;
	Inputs.bHasSessionInterface = true;
	Inputs.bHasNetDriverDefinition = true;
	Inputs.bHasNetDriverClass = true;
	Inputs.bHasIdentityInterface = true;
	Inputs.bIsLoggedIn = true;
	Inputs.bSteamAppIdBootstrapReady = true;
	Inputs.bSteamSocketsModuleAvailable = false;
	Inputs.bSteamSocketsEnabled = false;
	Inputs.bSteamSocketsSocketSubsystemAvailable = false;
	Inputs.bSteamAppIdMatchesExpectation = true;

	TestEqual(
		TEXT("缺少 SteamSockets 模块必须阻止 Steam"),
		FFU_OnlineProviderStatusEvaluator::Evaluate(EFU_OnlineProvider::Steam, Inputs),
		EFU_OnlineProviderStatusCode::SteamSocketsModuleUnavailable);
	TestEqual(
		TEXT("SteamSockets outage does not block LAN"),
		FFU_OnlineProviderStatusEvaluator::Evaluate(EFU_OnlineProvider::Lan, Inputs),
		EFU_OnlineProviderStatusCode::Ready);

	Inputs.bSteamSocketsModuleAvailable = true;
	TestEqual(
		TEXT("Disabled SteamSockets blocks Steam"),
		FFU_OnlineProviderStatusEvaluator::Evaluate(EFU_OnlineProvider::Steam, Inputs),
		EFU_OnlineProviderStatusCode::SteamSocketsDisabled);

	Inputs.bSteamSocketsEnabled = true;
	TestEqual(
		TEXT("缺少命名 SteamSockets SocketSubsystem 必须阻止 Steam"),
		FFU_OnlineProviderStatusEvaluator::Evaluate(EFU_OnlineProvider::Steam, Inputs),
		EFU_OnlineProviderStatusCode::SteamSocketsSocketSubsystemUnavailable);

	Inputs.bSteamSocketsSocketSubsystemAvailable = true;
	Inputs.bSteamAppIdBootstrapReady = false;
	TestEqual(
		TEXT("开发 AppID Bootstrap 不可用必须优先报告"),
		FFU_OnlineProviderStatusEvaluator::Evaluate(EFU_OnlineProvider::Steam, Inputs),
		EFU_OnlineProviderStatusCode::SteamAppIdBootstrapInvalid);

	Inputs.bSteamAppIdBootstrapReady = true;
	Inputs.bShippingBuild = true;
	Inputs.bShippingSteamAppIdExpected = false;
	TestEqual(
		TEXT("Shipping 缺少预期 Steam AppID 必须阻止 Steam"),
		FFU_OnlineProviderStatusEvaluator::Evaluate(EFU_OnlineProvider::Steam, Inputs),
		EFU_OnlineProviderStatusCode::ShippingSteamAppIdMissing);

	Inputs.bShippingSteamAppIdExpected = true;
	Inputs.bSteamAppIdMatchesExpectation = false;
	TestEqual(
		TEXT("Steam AppID 不匹配必须阻止 Steam"),
		FFU_OnlineProviderStatusEvaluator::Evaluate(EFU_OnlineProvider::Steam, Inputs),
		EFU_OnlineProviderStatusCode::SteamAppIdMismatch);

	return true;
}

#endif
