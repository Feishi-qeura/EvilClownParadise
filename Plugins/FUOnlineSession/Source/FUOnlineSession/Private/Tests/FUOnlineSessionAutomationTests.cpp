#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Misc/App.h"
#include "HAL/FileManager.h"
#include "FU_OnlineSessionTypes.h"
#include "FU_OnlineDiagnosticTypes.h"
#include "Diagnostics/FU_OnlineSessionDiagnostics.h"
#include "FU_OnlineSessionRequestValidation.h"
#include "FU_OnlineOperationStateMachine.h"
#include "FU_OnlineProviderStatusEvaluator.h"
#include "FU_SteamAppIdBootstrap.h"
#include "NetDriver/FU_OnlineSessionNetDriverLease.h"
#include "FU_CheckSessionStatusAsync.h"
#include "ProviderTraits/FU_OnlineSessionProviderTraits.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFUOnlineSessionOperationStateMachineTest,
	"FUOnlineSession.OperationStateMachine",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFUOnlineSessionOperationStateMachineTest::RunTest(const FString& Parameters)
{
	// 【回归：Busy 不能使在途回调失效】第二次公开调用必须取得自己的尝试序号和诊断 ID，
	// 但不能覆盖第一条已提交请求的 generation、根操作、当前 OSS 操作或完成标记。
	FFU_OnlineOperationStateMachine BusyMachine;
	FFU_OperationTicket First = BusyMachine.BeginAttempt(EFU_OperationKind::Create);
	TestTrue(TEXT("首次 Create 可以在 preflight 后被接收"), BusyMachine.AcceptAttempt(First, EFU_OperationKind::Create));
	const FFU_OperationState ActiveBeforeBusy = BusyMachine.Get();
	const FFU_OperationTicket Rejected = BusyMachine.RecordRejectedAttempt(EFU_OperationKind::Join);
	TestFalse(TEXT("Busy 尝试没有被接收"), Rejected.bAccepted);
	TestTrue(TEXT("Busy 尝试仍分配严格递增的尝试序号"), Rejected.AttemptSequence > First.AttemptSequence);
	TestTrue(TEXT("Busy 尝试仍分配独立诊断 ID"), Rejected.OperationId.IsValid() && Rejected.OperationId != First.OperationId);
	TestEqual(TEXT("Busy reject preserves active generation"), BusyMachine.Get().ActiveGeneration, First.Generation);
	TestEqual(TEXT("Busy reject preserves active operation id"), BusyMachine.Get().ActiveOperationId, ActiveBeforeBusy.ActiveOperationId);
	TestEqual(TEXT("Busy reject preserves root kind"), BusyMachine.Get().RootKind, EFU_OperationKind::Create);
	TestEqual(TEXT("Busy reject preserves submitted kind"), BusyMachine.Get().ActiveKind, EFU_OperationKind::Create);
	TestEqual(TEXT("Busy reject preserves phase"), BusyMachine.Get().Phase, EFU_OperationPhase::Submitted);
	TestEqual(TEXT("Busy reject preserves completion flag"), BusyMachine.Get().bCompletionBroadcast, ActiveBeforeBusy.bCompletionBroadcast);

	// 【回归：NoSession 不是 Create 已终止的证据】超时只广播一次失败并保留原始 delegate；
	// 在原回调真正到达前，即使 GetNamedSession 暂时为空也不能回到 Idle。
	const EFU_OperationAction CreateTimeout = BusyMachine.HandleTimeout(First.Generation);
	TestTrue(TEXT("Create 超时请求一次失败广播"), EnumHasAnyFlags(CreateTimeout, EFU_OperationAction::BroadcastFailure));
	TestTrue(TEXT("Create 超时保留原始回调"), EnumHasAnyFlags(CreateTimeout, EFU_OperationAction::KeepOriginalDelegate));
	TestTrue(TEXT("Create 超时只清已触发 watchdog"), EnumHasAnyFlags(CreateTimeout, EFU_OperationAction::ClearWatchdog));
	TestFalse(TEXT("Create 超时不清原始 delegate"), EnumHasAnyFlags(CreateTimeout, EFU_OperationAction::ClearOriginalDelegate));
	TestFalse(TEXT("NoSession cannot end original Create"), BusyMachine.CanFinishRecovery(false, true));
	TestEqual(TEXT("Create 超时进入恢复态"), BusyMachine.Get().Phase, EFU_OperationPhase::Recovering);
	TestTrue(TEXT("Create 超时等待原回调"), BusyMachine.Get().bAwaitingOriginalCompletion);

	// 迟到成功不得再让上层旅行，而应只启动一次补偿 Destroy；重复回调必须成为 no-op。
	const EFU_OperationAction LateCreateSuccess = BusyMachine.HandleOriginalCompletion(First.Generation, true, true);
	TestTrue(TEXT("迟到 Create 成功启动补偿 Destroy"), EnumHasAnyFlags(LateCreateSuccess, EFU_OperationAction::StartRecoveryDestroy));
	TestFalse(TEXT("迟到 Create 成功不重复广播"), EnumHasAnyFlags(LateCreateSuccess, EFU_OperationAction::BroadcastFailure));
	TestEqual(TEXT("补偿 Destroy 只计一次"), BusyMachine.Get().RecoveryDestroyAttempts, static_cast<uint8>(1));
	TestEqual(TEXT("重复迟到回调被 generation gate 丢弃"), BusyMachine.HandleOriginalCompletion(First.Generation, true, true), EFU_OperationAction::None);

	// 【链式操作】公开 Create 的根身份在预销毁期间保持 Create，但当前 OSS kind 必须是 Destroy；
	// Destroy 正常完成后续步 Create 使用同一 OperationId、一个新 generation。
	FFU_OnlineOperationStateMachine ChainedMachine;
	FFU_OperationTicket ChainedCreate = ChainedMachine.BeginAttempt(EFU_OperationKind::Create);
	TestTrue(TEXT("预销毁提交被接收"), ChainedMachine.AcceptAttempt(ChainedCreate, EFU_OperationKind::Destroy));
	TestEqual(TEXT("预销毁保留根 Create"), ChainedMachine.Get().RootKind, EFU_OperationKind::Create);
	TestEqual(TEXT("预销毁记录当前 Destroy"), ChainedMachine.Get().ActiveKind, EFU_OperationKind::Destroy);
	const uint64 DestroyGeneration = ChainedCreate.Generation;
	const EFU_OperationAction PreDestroyComplete = ChainedMachine.HandleOriginalCompletion(
		DestroyGeneration,
		EFU_OperationKind::Destroy,
		true,
		false);
	TestTrue(TEXT("预销毁正常回调清 watchdog"), EnumHasAnyFlags(PreDestroyComplete, EFU_OperationAction::ClearWatchdog));
	TestTrue(TEXT("预销毁正常回调清当前 Destroy delegate"), EnumHasAnyFlags(PreDestroyComplete, EFU_OperationAction::ClearOriginalDelegate));
	TestTrue(TEXT("同一根尝试可以续步 Create"), ChainedMachine.ContinueAcceptedAttempt(EFU_OperationKind::Create));
	TestEqual(TEXT("续步保持 OperationId"), ChainedMachine.Get().ActiveOperationId, ChainedCreate.OperationId);
	TestTrue(TEXT("续步分配新 generation"), ChainedMachine.Get().ActiveGeneration > DestroyGeneration);
	TestEqual(TEXT("续步当前 kind 更新为 Create"), ChainedMachine.Get().ActiveKind, EFU_OperationKind::Create);

	// Find 超时只启动可取消协议且从不请求传输租约释放；取消前仍保留原 Find delegate。
	FFU_OnlineOperationStateMachine FindMachine;
	FFU_OperationTicket Find = FindMachine.BeginAcceptedAttempt(EFU_OperationKind::Find);
	const EFU_OperationAction FindTimeout = FindMachine.HandleTimeout(Find.Generation);
	TestTrue(TEXT("Find 超时启动取消"), EnumHasAnyFlags(FindTimeout, EFU_OperationAction::StartFindCancellation));
	TestTrue(TEXT("Find 超时保留原 Find 回调"), EnumHasAnyFlags(FindTimeout, EFU_OperationAction::KeepOriginalDelegate));
	TestFalse(TEXT("Find 超时不占用也不释放 NetDriver lease"), EnumHasAnyFlags(FindTimeout, EFU_OperationAction::RequestLeaseRelease));

	// 同 generation 的错误 delegate kind 也必须被拒绝，防止链式 Destroy 的迟到回调清掉后续 Join。
	TestFalse(TEXT("错误 submitted kind 不是当前回调"), FindMachine.IsExpectedCallback(Find.Generation, EFU_OperationKind::Join));
	TestEqual(
		TEXT("错误 submitted kind 不改变状态"),
		FindMachine.HandleOriginalCompletion(Find.Generation, EFU_OperationKind::Join, false, false),
		EFU_OperationAction::None);

	// OSS Join 报 Success 后，地址解析或 PlayerController 仍可能失败；NamedSession 已存在时必须先公开失败一次，
	// 再进入补偿 Destroy，不能直接 Idle/释放 lease。
	FFU_OnlineOperationStateMachine JoinPostProcessMachine;
	const FFU_OperationTicket Join = JoinPostProcessMachine.BeginAcceptedAttempt(EFU_OperationKind::Join);
	const EFU_OperationAction JoinPostProcessFailure = JoinPostProcessMachine.HandleOriginalCompletion(
		Join.Generation,
		EFU_OperationKind::Join,
		false,
		true);
	TestTrue(TEXT("Join 后处理失败广播一次公共失败"), EnumHasAnyFlags(JoinPostProcessFailure, EFU_OperationAction::BroadcastFailure));
	TestTrue(TEXT("Join 后处理失败启动补偿 Destroy"), EnumHasAnyFlags(JoinPostProcessFailure, EFU_OperationAction::StartRecoveryDestroy));
	TestFalse(TEXT("Join 后处理失败不直接释放 lease"), EnumHasAnyFlags(JoinPostProcessFailure, EFU_OperationAction::RequestLeaseRelease));
	TestEqual(TEXT("Join 后处理失败保持 Recovering"), JoinPostProcessMachine.Get().Phase, EFU_OperationPhase::Recovering);

	// 显式恢复必须同时看到原回调终态、NoSession、无活动/待定驱动及租约已释放；任一证据缺失都拒绝。
	TestFalse(TEXT("原回调未终止时拒绝显式恢复"), BusyMachine.CanStartExplicitRecovery(false, true, true, true));
	TestFalse(TEXT("仍有 Session 时拒绝显式恢复"), BusyMachine.CanStartExplicitRecovery(true, false, true, true));
	TestFalse(TEXT("仍有驱动时拒绝显式恢复"), BusyMachine.CanStartExplicitRecovery(true, true, false, true));
	TestFalse(TEXT("租约未释放时拒绝显式恢复"), BusyMachine.CanStartExplicitRecovery(true, true, true, false));
	TestTrue(TEXT("四项安全证据齐全时允许显式恢复"), BusyMachine.CanStartExplicitRecovery(true, true, true, true));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFUOnlineSessionDefaultResultTest,"FUOnlineSession.Types.DefaultResult",EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFUOnlineSessionNetDriverLeaseTest,
	"FUOnlineSession.NetDriverLease.Snapshots",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFUOnlineSessionNetDriverLeaseTest::RunTest(const FString& Parameters)
{
	// 【先写租约契约】真正的 GEngine 数组由协调器读取；这里用纯快照锁住所有危险分支，
	// 防止未来改动让正在旅行的 World 或第三方修改过的定义被 FU 覆盖。
	FFU_NetDriverLeasePreflight Snapshot;
	TestEqual(
		TEXT("非游戏线程绝不读取或修改进程级租约"),
		FFU_OnlineSessionNetDriverLease::EvaluateAcquire(Snapshot),
		EFU_NetDriverLeaseResult::NotGameThread);

	Snapshot.bIsGameThread = true;
	TestEqual(
		TEXT("缺少 GameNetDriver 定义时不自行插入"),
		FFU_OnlineSessionNetDriverLease::EvaluateAcquire(Snapshot),
		EFU_NetDriverLeaseResult::GameNetDriverMissing);

	Snapshot.GameNetDriverDefinitionCount = 2;
	TestEqual(
		TEXT("重复 GameNetDriver 定义时不猜测目标"),
		FFU_OnlineSessionNetDriverLease::EvaluateAcquire(Snapshot),
		EFU_NetDriverLeaseResult::GameNetDriverDuplicate);

	Snapshot.GameNetDriverDefinitionCount = 1;
	Snapshot.bHasLiveTargetNamedDriver = true;
	TestEqual(
		TEXT("任意活动 GameNetDriver 都阻止首租"),
		FFU_OnlineSessionNetDriverLease::EvaluateAcquire(Snapshot),
		EFU_NetDriverLeaseResult::ActiveOrPendingDriver);

	Snapshot.bHasLiveTargetNamedDriver = false;
	Snapshot.bHasPendingNetGame = true;
	TestEqual(
		TEXT("即使 PendingNetGame 尚未创建驱动也阻止首租"),
		FFU_OnlineSessionNetDriverLease::EvaluateAcquire(Snapshot),
		EFU_NetDriverLeaseResult::ActiveOrPendingDriver);

	Snapshot.bHasPendingNetGame = false;
	Snapshot.bHasPendingTravel = true;
	TestEqual(
		TEXT("TravelURL 已排队但尚无 PendingNetGame 时也阻止首租"),
		FFU_OnlineSessionNetDriverLease::EvaluateAcquire(Snapshot),
		EFU_NetDriverLeaseResult::ActiveOrPendingDriver);

	Snapshot.bHasPendingTravel = false;
	Snapshot.bLeaseExists = true;
	Snapshot.bSameOwner = true;
	Snapshot.bSameProvider = true;
	Snapshot.bInstalledValueStillMatches = true;
	TestEqual(
		TEXT("同一 GameInstance 和 Provider 的完整租约可幂等复用"),
		FFU_OnlineSessionNetDriverLease::EvaluateAcquire(Snapshot),
		EFU_NetDriverLeaseResult::AlreadyOwned);

	Snapshot.bInstalledValueStillMatches = false;
	TestEqual(
		TEXT("既有租约安装值变化优先判为外部修改"),
		FFU_OnlineSessionNetDriverLease::EvaluateAcquire(Snapshot),
		EFU_NetDriverLeaseResult::ExternallyModified);
	Snapshot.bInstalledValueStillMatches = true;

	Snapshot.bSameOwner = false;
	TestEqual(
		TEXT("另一 GameInstance 不能复用进程级租约"),
		FFU_OnlineSessionNetDriverLease::EvaluateAcquire(Snapshot),
		EFU_NetDriverLeaseResult::OwnedByAnotherGameInstance);

	Snapshot.bLeaseOwnerExpired = true;
	TestEqual(
		TEXT("失效旧 Owner 且所有 World 安全时允许先恢复基线再重租"),
		FFU_OnlineSessionNetDriverLease::EvaluateAcquire(Snapshot),
		EFU_NetDriverLeaseResult::StaleOwnerReclaimable);
	Snapshot.bHasPendingTravel = true;
	TestEqual(
		TEXT("失效旧 Owner 也不能越过已排队 Travel 强制回收"),
		FFU_OnlineSessionNetDriverLease::EvaluateAcquire(Snapshot),
		EFU_NetDriverLeaseResult::ActiveOrPendingDriver);
	Snapshot.bHasPendingTravel = false;
	Snapshot.bLeaseOwnerExpired = false;
	Snapshot.bSameOwner = true;

	Snapshot.bSameDesiredDriver = false;
	TestEqual(
		TEXT("同 Provider 重租也必须要求本次目标类名与已安装值一致"),
		FFU_OnlineSessionNetDriverLease::EvaluateAcquire(Snapshot),
		EFU_NetDriverLeaseResult::DesiredDriverMismatch);
	Snapshot.bSameDesiredDriver = true;

	Snapshot.bSameProvider = false;
	TestEqual(
		TEXT("同一实例不能在已租约期间切换 Provider"),
		FFU_OnlineSessionNetDriverLease::EvaluateAcquire(Snapshot),
		EFU_NetDriverLeaseResult::ProviderSwitchBlocked);

	FNetDriverDefinition Definition;
	Definition.DefName = NAME_GameNetDriver;
	Definition.DriverClassName = FName(TEXT("/Script/OnlineSubsystemUtils.IpNetDriver"));
	Definition.DriverClassNameFallback = Definition.DriverClassName;
	Definition.MaxChannelsOverride = 77;
	Definition.bRunParallelConnectionTick = true;
	const FName SteamDriverClass(TEXT("/Script/SteamSockets.SteamSocketsNetDriver"));
	const FFU_NetDriverDefinitionValue OriginalValue =
		FFU_NetDriverDefinitionValue::FromDefinition(Definition);
	const FFU_NetDriverDefinitionValue InstalledValue =
		FFU_OnlineSessionNetDriverLease::BuildInstalledValue(OriginalValue, SteamDriverClass);

	// 【最小写入契约】元编程 traits 只选择两个类名；租约不得顺手改写定义名、通道数或并行 Tick。
	TestEqual(TEXT("安装值使用目标主驱动"), InstalledValue.DriverClassName, SteamDriverClass);
	TestEqual(TEXT("安装值禁用静默异类回退"), InstalledValue.DriverClassNameFallback, SteamDriverClass);
	TestEqual(TEXT("安装保留 DefName"), InstalledValue.DefName, OriginalValue.DefName);
	TestEqual(TEXT("安装保留 MaxChannelsOverride"), InstalledValue.MaxChannelsOverride, 77);
	TestTrue(TEXT("安装保留 bRunParallelConnectionTick"), InstalledValue.bRunParallelConnectionTick);

	FFU_NetDriverLeaseFingerprint Expected;
	Expected.DefinitionPointer = &Definition;
	Expected.DefinitionIndex = 0;
	Expected.DefinitionArrayNum = 2;
	Expected.OrderedDefinitionNames = { NAME_GameNetDriver, FName(TEXT("BeaconNetDriver")) };
	Expected.OriginalValue = OriginalValue;
	Expected.InstalledValue = InstalledValue;

	FFU_NetDriverLeaseFingerprint ExternallyChanged = Expected;
	ExternallyChanged.OrderedDefinitionNames = { FName(TEXT("BeaconNetDriver")), NAME_GameNetDriver };
	TestEqual(
		TEXT("定义顺序指纹变化视为外部修改，绝不恢复覆盖"),
		FFU_OnlineSessionNetDriverLease::EvaluateRestore(Expected, ExternallyChanged, true),
		EFU_NetDriverLeaseResult::ExternallyModified);
	TestEqual(
		TEXT("即使 World 尚活动，外部指纹变化也必须立即毒化而非无限延迟"),
		FFU_OnlineSessionNetDriverLease::EvaluateRestore(Expected, ExternallyChanged, false),
		EFU_NetDriverLeaseResult::ExternallyModified);

	ExternallyChanged = Expected;
	++ExternallyChanged.InstalledValue.MaxChannelsOverride;
	TestEqual(
		TEXT("租约期间任一非驱动字段变化也视为外部修改"),
		FFU_OnlineSessionNetDriverLease::EvaluateRestore(Expected, ExternallyChanged, true),
		EFU_NetDriverLeaseResult::ExternallyModified);

	ExternallyChanged = Expected;
	ExternallyChanged.DefinitionPointer = nullptr;
	TestEqual(
		TEXT("元素指针变化时绝不按旧索引恢复"),
		FFU_OnlineSessionNetDriverLease::EvaluateRestore(Expected, ExternallyChanged, true),
		EFU_NetDriverLeaseResult::ExternallyModified);

	ExternallyChanged = Expected;
	++ExternallyChanged.DefinitionArrayNum;
	TestEqual(
		TEXT("数组长度变化时绝不覆盖第三方结构"),
		FFU_OnlineSessionNetDriverLease::EvaluateRestore(Expected, ExternallyChanged, true),
		EFU_NetDriverLeaseResult::ExternallyModified);
	TestEqual(
		TEXT("仍有 World 驱动时只能延迟恢复"),
		FFU_OnlineSessionNetDriverLease::EvaluateRestore(Expected, Expected, false),
		EFU_NetDriverLeaseResult::ReleaseDeferred);
	TestEqual(
		TEXT("所有 World 清空且完整指纹一致时才能恢复"),
		FFU_OnlineSessionNetDriverLease::EvaluateRestore(Expected, Expected, true),
		EFU_NetDriverLeaseResult::Restored);

	// 【Task 6 RED：释放完成不是“可以再次 Acquire”】显式恢复需要一个只读结论：
	// 只有游戏线程、进程未中毒且进程级租约已经不存在时才算完成；任何现存租约（含错误 owner/provider）
	// 都 fail-closed，避免 TryRecoverProvider 把另一实例刚取得的租约误判为自己的释放完成。
	FFU_NetDriverLeaseReleaseSnapshot ReleaseSnapshot;
	TestFalse(
		TEXT("非游戏线程的释放完成查询保守失败"),
		FFU_OnlineSessionNetDriverLease::EvaluateReleaseComplete(ReleaseSnapshot));
	ReleaseSnapshot.bIsGameThread = true;
	TestFalse(
		TEXT("无有效 exact owner 时不能宣称释放完成"),
		FFU_OnlineSessionNetDriverLease::EvaluateReleaseComplete(ReleaseSnapshot));
	ReleaseSnapshot.bOwnerValid = true;
	ReleaseSnapshot.bProcessLeasePoisoned = true;
	TestFalse(
		TEXT("进程租约中毒后不能宣称安全释放"),
		FFU_OnlineSessionNetDriverLease::EvaluateReleaseComplete(ReleaseSnapshot));
	ReleaseSnapshot.bProcessLeasePoisoned = false;
	ReleaseSnapshot.bLeaseExists = true;
	ReleaseSnapshot.bSameOwner = true;
	ReleaseSnapshot.bSameProvider = true;
	TestFalse(
		TEXT("匹配 owner 和 provider 的租约仍存在时尚未释放"),
		FFU_OnlineSessionNetDriverLease::EvaluateReleaseComplete(ReleaseSnapshot));
	ReleaseSnapshot.bSameOwner = false;
	TestFalse(
		TEXT("错误 owner 的现存租约不能被当成已安全释放"),
		FFU_OnlineSessionNetDriverLease::EvaluateReleaseComplete(ReleaseSnapshot));
	ReleaseSnapshot.bLeaseExists = false;
	ReleaseSnapshot.bSameProvider = false;
	TestTrue(
		TEXT("无租约且线程与进程状态安全时释放完成"),
		FFU_OnlineSessionNetDriverLease::EvaluateReleaseComplete(ReleaseSnapshot));
	return true;
}

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

	// 【能力分离】Find 不会创建/旅行 NetDriver；即使另一 Provider 正占租，也必须能搜索两套房间列表。
	Inputs.bRequiresNetDriver = false;
	Inputs.bHasNetDriverDefinition = false;
	Inputs.bHasNetDriverClass = false;
	Inputs.bNetDriverLeaseAvailable = false;
	TestEqual(
		TEXT("搜索能力忽略无关的 NetDriver 与租约互斥"),
		FFU_OnlineProviderStatusEvaluator::Evaluate(EFU_OnlineProvider::Steam, Inputs),
		EFU_OnlineProviderStatusCode::Ready);

	Inputs.bRequiresNetDriver = true;
	Inputs.bHasNetDriverDefinition = true;
	Inputs.bHasNetDriverClass = true;
	Inputs.bHasConflictingActiveNetDriver = false;
	Inputs.bNetDriverLeaseAvailable = false;
	TestEqual(
		TEXT("全进程租约冲突必须在实际改写 GEngine 前阻止 Provider"),
		FFU_OnlineProviderStatusEvaluator::Evaluate(EFU_OnlineProvider::Steam, Inputs),
		EFU_OnlineProviderStatusCode::NetDriverLeaseUnavailable);

	Inputs.bNetDriverLeaseAvailable = true;
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
	Inputs.bRequiresNetDriver = false;
	TestEqual(
		TEXT("Steam Lobby 搜索不依赖尚未使用的 SteamSockets 传输层"),
		FFU_OnlineProviderStatusEvaluator::Evaluate(EFU_OnlineProvider::Steam, Inputs),
		EFU_OnlineProviderStatusCode::Ready);
	Inputs.bRequiresNetDriver = true;
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
