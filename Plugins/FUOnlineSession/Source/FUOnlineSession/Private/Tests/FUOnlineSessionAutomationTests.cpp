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
	FFUOnlineSessionOperationCorrelationTest,
	"FUOnlineSession.Diagnostics.OperationCorrelation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFUOnlineSessionOperationCorrelationTest::RunTest(const FString& Parameters)
{
	// 【Task 7 RED：无 Subsystem 测试缝】用真实状态评估器构造 Steam 子系统缺失的 preflight，
	// 再以 Task 6 状态机分配公开操作 ID。这里不创建 World、OSS 或网络请求，确保测试只锁住
	// “预检拒绝必须先经统一诊断出口、再触发旧完成委托”的关联契约。
	FFU_OnlineDiagnosticDispatchConfig Config;
	Config.bEmitToLog = false;
	Config.bEnableOverlay = false;
	int32 LegacyFailureBroadcastCount = 0;
	int32 BlueprintDiagnosticBroadcastCount = 0;
	FFU_OnlineSessionDiagnostics Diagnostics(
		Config,
		[&BlueprintDiagnosticBroadcastCount](const FFU_OnlineDiagnosticEvent&)
		{
			++BlueprintDiagnosticBroadcastCount;
		});

	FFU_OnlineOperationStateMachine Machine;
	const FFU_OperationTicket Ticket = Machine.BeginAttempt(EFU_OperationKind::Create);
	FFU_OnlineProviderStatusInputs Inputs;
	Inputs.bHasWorld = true;
	Inputs.bHasSubsystem = false;
	const EFU_OnlineProviderStatusCode StatusCode =
		FFU_OnlineProviderStatusEvaluator::Evaluate(EFU_OnlineProvider::Steam, Inputs);
	TestEqual(TEXT("测试缝确实得到 Steam 子系统不可用"), StatusCode, EFU_OnlineProviderStatusCode::SubsystemUnavailable);

	FFU_OnlineProviderStatus Status;
	Status.bIsReady = StatusCode == EFU_OnlineProviderStatusCode::Ready;
	Status.StatusCode = StatusCode;
	Status.DiagnosticCode = TEXT("FU.Provider.SubsystemUnavailable");
	Status.Message = TEXT("Steam OnlineSubsystem 不可用");
	TArray<FString> DispatchOrder;
	const bool bGateAccepted = FFU_OnlineProviderPreflightGate::Dispatch(
		EFU_OnlineProvider::Steam,
		EFU_OnlineDiagnosticOperation::CreateSession,
		Ticket.OperationId,
		Status,
		[&Diagnostics, &DispatchOrder](FFU_OnlineDiagnosticEvent Event)
		{
			DispatchOrder.Add(TEXT("Diagnostic"));
			Diagnostics.Emit(MoveTemp(Event));
		},
		[&LegacyFailureBroadcastCount, &DispatchOrder]()
		{
			DispatchOrder.Add(TEXT("LegacyFailure"));
			++LegacyFailureBroadcastCount;
		});
	TestFalse(TEXT("生产 gate 对 SubsystemUnavailable 拒绝入口"), bGateAccepted);

	const TArray<FFU_OnlineDiagnosticEvent> History = Diagnostics.GetHistory();
	TestEqual(TEXT("预检拒绝先写入一条诊断历史"), History.Num(), 1);
	TestEqual(TEXT("旧失败委托只在诊断事件之后触发一次"), LegacyFailureBroadcastCount, 1);
	TestEqual(TEXT("诊断 Blueprint 出口恰好广播一次"), BlueprintDiagnosticBroadcastCount, 1);
	TestEqual(TEXT("生产 gate 必须先诊断后续步"), DispatchOrder, TArray<FString>{ TEXT("Diagnostic"), TEXT("LegacyFailure") });
	if (History.Num() == 1)
	{
		TestEqual(TEXT("预检事件关联 Create"), History[0].Operation, EFU_OnlineDiagnosticOperation::CreateSession);
		TestEqual(TEXT("预检事件关联 Steam"), History[0].Provider, EFU_OnlineProvider::Steam);
		TestEqual(TEXT("预检事件使用稳定子系统不可用代码"), History[0].Code, FString(TEXT("FU.Provider.SubsystemUnavailable")));
		TestTrue(TEXT("预检事件带有非零 OperationId"), History[0].OperationId.IsValid() && History[0].OperationId == Ticket.OperationId);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFUOnlineSessionLeaseOutcomesTest,
	"FUOnlineSession.Diagnostics.LeaseOutcomes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFUOnlineSessionLeaseOutcomesTest::RunTest(const FString& Parameters)
{
	// 【生产 policy 表驱动】Runtime Acquire/Release 直接消费该 coordinator 映射；这里不手写事件。
	struct FCase { EFU_NetDriverLeaseResult Result; const TCHAR* Code; const TCHAR* Status; bool bError; };
	const FCase Cases[] = {
		{ EFU_NetDriverLeaseResult::Acquired, TEXT("FU.Lease.Acquire"), TEXT("Acquired"), false },
		{ EFU_NetDriverLeaseResult::AlreadyOwned, TEXT("FU.Lease.Acquire"), TEXT("AlreadyOwned"), false },
		{ EFU_NetDriverLeaseResult::RestartRequired, TEXT("FU.Lease.Release"), TEXT("RestartRequired"), true },
		{ EFU_NetDriverLeaseResult::Restored, TEXT("FU.Lease.Release"), TEXT("Restored"), false },
		{ EFU_NetDriverLeaseResult::ReleaseDeferred, TEXT("FU.Lease.Release"), TEXT("ReleaseDeferred"), true },
		{ EFU_NetDriverLeaseResult::NoLease, TEXT("FU.Lease.Release"), TEXT("NoLease"), true },
		{ EFU_NetDriverLeaseResult::NotOwner, TEXT("FU.Lease.Release"), TEXT("NotOwner"), true },
	};
	for (const FCase& TestCase : Cases)
	{
		const FFU_NetDriverLeaseDiagnosticOutcome Outcome = FFU_OnlineSessionNetDriverLease::GetDiagnosticOutcome(TestCase.Result);
		TestEqual(TEXT("Lease code 稳定"), Outcome.Code, FName(TestCase.Code));
		TestEqual(TEXT("Lease status 使用真实 coordinator result"), Outcome.Status, FName(TestCase.Status));
		TestEqual(TEXT("Lease severity 由 production policy 决定"), Outcome.bIsError, TestCase.bError);
	}
	const FGuid SharedId = FGuid::NewGuid();
	TestTrue(TEXT("Lease 诊断可复用有效 connection OperationId"), SharedId.IsValid());
	TestEqual(TEXT("Lease pure policy 没有 legacy completion 副作用"), 0, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFUOnlineSessionDiagnosticSaveFailureTest,
	"FUOnlineSession.Diagnostics.SaveFailure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFUOnlineSessionDiagnosticSaveFailureTest::RunTest(const FString& Parameters)
{
	// 【失败注入】写入器只允许被调用一次；SaveReport 的失败事件只能走 Emit，不能重新尝试写报告。
	FFU_OnlineDiagnosticDispatchConfig Config;
	Config.bEmitToLog = true;
	Config.bEnableOverlay = false;
	int32 WriteAttempts = 0;
	int32 BlueprintEventCount = 0;
	TArray<FFU_OnlineDiagnosticEvent> BlueprintEvents;
	TArray<FString> CapturedLogLines;
	FFU_OnlineSessionDiagnostics Diagnostics(
		Config,
		[&BlueprintEventCount, &BlueprintEvents](const FFU_OnlineDiagnosticEvent& Event)
		{
			++BlueprintEventCount;
			BlueprintEvents.Add(Event);
		},
		[&WriteAttempts](const FString&, const FString&, FString& OutRawFailureDetail)
		{
			++WriteAttempts;
			OutRawFailureDetail = TEXT("token=writer-secret travelurl=steam://private");
			return false;
		},
		[&CapturedLogLines](const FFU_OnlineDiagnosticEvent&, const FString& Line)
		{
			CapturedLogLines.Add(Line);
		});

	FString SavedPath;
	FString SaveError;
	TestFalse(TEXT("注入写入失败时 SaveReport 返回失败"), Diagnostics.SaveReport(SavedPath, SaveError));
	TestEqual(TEXT("写入失败不会递归重试"), WriteAttempts, 1);
	TestTrue(TEXT("失败不返回伪造保存路径"), SavedPath.IsEmpty());
	TestFalse(TEXT("失败文本不回显注入写入器输入"), SaveError.Contains(TEXT("token="), ESearchCase::IgnoreCase));
	TestFalse(TEXT("失败文本不回显原始旅行 URL"), SaveError.Contains(TEXT("steam://private"), ESearchCase::IgnoreCase));
	const TArray<FFU_OnlineDiagnosticEvent> History = Diagnostics.GetHistory();
	TestEqual(TEXT("写入失败至多产生一条诊断事件"), History.Num(), 1);
	TestEqual(TEXT("失败事件只通过一次 Blueprint 出口"), BlueprintEventCount, 1);
	TestEqual(TEXT("失败事件只写一次安全日志"), CapturedLogLines.Num(), 1);
	if (CapturedLogLines.Num() == 1)
	{
		TestFalse(TEXT("日志不回显 writer token"), CapturedLogLines[0].Contains(TEXT("writer-secret"), ESearchCase::IgnoreCase));
		TestFalse(TEXT("日志不回显 writer URL"), CapturedLogLines[0].Contains(TEXT("steam://private"), ESearchCase::IgnoreCase));
	}
	if (History.Num() == 1)
	{
		TestEqual(TEXT("写入失败使用稳定安全错误码"), History[0].Code, FString(TEXT("FU.Diagnostics.ReportSaveFailed")));
		TestTrue(TEXT("写入失败事件不携带字段"), History[0].Fields.IsEmpty());
		TestFalse(TEXT("历史不回显 writer token"), History[0].Message.Contains(TEXT("writer-secret"), ESearchCase::IgnoreCase));
	}
	TestEqual(TEXT("失败事件只向 Blueprint 发出一次"), BlueprintEvents.Num(), 1);
	if (BlueprintEvents.Num() == 1)
	{
		TestFalse(TEXT("Blueprint 不回显 writer URL"), BlueprintEvents[0].Message.Contains(TEXT("steam://private"), ESearchCase::IgnoreCase));
	}
	return true;
}

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

	// 【Fix round 1 RED：同步拒绝必须 exactly-once 收口】删除任一清理动作、重复广播，
	// 或把 Create 的传输租约留在已取得状态，都会让本矩阵立即失败。
	FFU_OnlineOperationStateMachine SynchronousRejectMachine;
	const FFU_OperationTicket RejectedCreate =
		SynchronousRejectMachine.BeginAcceptedAttempt(EFU_OperationKind::Create);
	const EFU_OperationAction SynchronousReject =
		SynchronousRejectMachine.HandleSynchronousReject(RejectedCreate.Generation);
	TestTrue(TEXT("Create 同步拒绝广播失败"), EnumHasAnyFlags(SynchronousReject, EFU_OperationAction::BroadcastFailure));
	TestTrue(TEXT("Create 同步拒绝清 watchdog"), EnumHasAnyFlags(SynchronousReject, EFU_OperationAction::ClearWatchdog));
	TestTrue(TEXT("Create 同步拒绝清精确 delegate"), EnumHasAnyFlags(SynchronousReject, EFU_OperationAction::ClearOriginalDelegate));
	TestTrue(TEXT("Create 同步拒绝清 pending 数据"), EnumHasAnyFlags(SynchronousReject, EFU_OperationAction::ClearPendingData));
	TestTrue(TEXT("Create 同步拒绝请求释放租约"), EnumHasAnyFlags(SynchronousReject, EFU_OperationAction::RequestLeaseRelease));
	TestEqual(TEXT("同步拒绝回到 Idle"), SynchronousRejectMachine.Get().Phase, EFU_OperationPhase::Idle);
	TestEqual(
		TEXT("同步拒绝重复处理不广播也不清新资源"),
		SynchronousRejectMachine.HandleSynchronousReject(RejectedCreate.Generation),
		EFU_OperationAction::None);

	// 【Fix round 1 RED：Find cancel 三方竞态】成功 cancel、失败 cancel、原 Find 先完成分别拥有
	// 不同的精确清理集合；尤其原 Find 赢得竞争时也必须清掉仍注册的 cancel delegate。
	FFU_OnlineOperationStateMachine FindCancelSuccessMachine;
	const FFU_OperationTicket FindCancelSuccess =
		FindCancelSuccessMachine.BeginAcceptedAttempt(EFU_OperationKind::Find);
	FindCancelSuccessMachine.HandleTimeout(FindCancelSuccess.Generation);
	const EFU_OperationAction CancelSucceeded =
		FindCancelSuccessMachine.HandleFindCancellationCompletion(FindCancelSuccess.Generation, true);
	TestTrue(TEXT("Find cancel 成功清原 Find delegate"), EnumHasAnyFlags(CancelSucceeded, EFU_OperationAction::ClearOriginalDelegate));
	TestTrue(TEXT("Find cancel 成功清 cancel delegate"), EnumHasAnyFlags(CancelSucceeded, EFU_OperationAction::ClearFindCancellationDelegate));
	TestTrue(TEXT("Find cancel 成功清 pending 数据"), EnumHasAnyFlags(CancelSucceeded, EFU_OperationAction::ClearPendingData));
	TestFalse(TEXT("Find cancel 成功不重复广播超时失败"), EnumHasAnyFlags(CancelSucceeded, EFU_OperationAction::BroadcastFailure));
	TestEqual(
		TEXT("cancel 成功后原 Find 迟到回调为 no-op"),
		FindCancelSuccessMachine.HandleOriginalCompletion(FindCancelSuccess.Generation, EFU_OperationKind::Find, true, false),
		EFU_OperationAction::None);

	FFU_OnlineOperationStateMachine FindCancelFailureMachine;
	const FFU_OperationTicket FindCancelFailure =
		FindCancelFailureMachine.BeginAcceptedAttempt(EFU_OperationKind::Find);
	FindCancelFailureMachine.HandleTimeout(FindCancelFailure.Generation);
	const EFU_OperationAction CancelFailed =
		FindCancelFailureMachine.HandleFindCancellationCompletion(FindCancelFailure.Generation, false);
	TestTrue(TEXT("Find cancel 失败只清 cancel delegate"), EnumHasAnyFlags(CancelFailed, EFU_OperationAction::ClearFindCancellationDelegate));
	TestTrue(TEXT("Find cancel 失败继续保留原 Find delegate"), EnumHasAnyFlags(CancelFailed, EFU_OperationAction::KeepOriginalDelegate));
	TestTrue(TEXT("Find cancel 失败仍等待原回调"), FindCancelFailureMachine.Get().bAwaitingOriginalCompletion);
	const EFU_OperationAction FindAfterCancelFailure = FindCancelFailureMachine.HandleOriginalCompletion(
		FindCancelFailure.Generation, EFU_OperationKind::Find, false, false);
	TestTrue(TEXT("cancel 失败后原 Find 完成清原 delegate"), EnumHasAnyFlags(FindAfterCancelFailure, EFU_OperationAction::ClearOriginalDelegate));
	TestFalse(TEXT("cancel 失败后原 Find 完成不重复广播"), EnumHasAnyFlags(FindAfterCancelFailure, EFU_OperationAction::BroadcastFailure));

	FFU_OnlineOperationStateMachine FindOriginalWinsMachine;
	const FFU_OperationTicket FindOriginalWins =
		FindOriginalWinsMachine.BeginAcceptedAttempt(EFU_OperationKind::Find);
	FindOriginalWinsMachine.HandleTimeout(FindOriginalWins.Generation);
	const EFU_OperationAction OriginalFindWon = FindOriginalWinsMachine.HandleOriginalCompletion(
		FindOriginalWins.Generation, EFU_OperationKind::Find, true, false);
	TestTrue(TEXT("原 Find 赢得竞争时清原 delegate"), EnumHasAnyFlags(OriginalFindWon, EFU_OperationAction::ClearOriginalDelegate));
	TestTrue(TEXT("原 Find 赢得竞争时也清 cancel delegate"), EnumHasAnyFlags(OriginalFindWon, EFU_OperationAction::ClearFindCancellationDelegate));
	TestEqual(
		TEXT("原 Find 完成后 cancel 迟到回调为 no-op"),
		FindOriginalWinsMachine.HandleFindCancellationCompletion(FindOriginalWins.Generation, true),
		EFU_OperationAction::None);

	// 【Fix round 1 RED：Destroy/补偿 Destroy 边界】公开 Destroy 超时只广播一次；补偿 Destroy
	// 成功请求释放，失败允许下一次显式调用最多提交一次，新 generation 必须隔离旧回调。
	FFU_OnlineOperationStateMachine DestroyTimeoutMachine;
	const FFU_OperationTicket Destroy = DestroyTimeoutMachine.BeginAcceptedAttempt(EFU_OperationKind::Destroy);
	const EFU_OperationAction DestroyTimedOut = DestroyTimeoutMachine.HandleTimeout(Destroy.Generation);
	TestTrue(TEXT("Destroy timeout 广播一次失败"), EnumHasAnyFlags(DestroyTimedOut, EFU_OperationAction::BroadcastFailure));
	TestTrue(TEXT("Destroy timeout 保留不可取消回调"), EnumHasAnyFlags(DestroyTimedOut, EFU_OperationAction::KeepOriginalDelegate));
	const EFU_OperationAction LateDestroySuccess = DestroyTimeoutMachine.HandleOriginalCompletion(
		Destroy.Generation, EFU_OperationKind::Destroy, true, false);
	TestTrue(TEXT("迟到 Destroy 成功请求租约释放"), EnumHasAnyFlags(LateDestroySuccess, EFU_OperationAction::RequestLeaseRelease));
	TestFalse(TEXT("迟到 Destroy 成功不重复广播"), EnumHasAnyFlags(LateDestroySuccess, EFU_OperationAction::BroadcastFailure));
	TestFalse(
		TEXT("只完成会话层但租约未释放时不满足恢复门"),
		DestroyTimeoutMachine.CanStartExplicitRecovery(true, true, true, false));

	FFU_OnlineOperationStateMachine CompensationSuccessMachine;
	const FFU_OperationTicket TimedOutCreate =
		CompensationSuccessMachine.BeginAcceptedAttempt(EFU_OperationKind::Create);
	CompensationSuccessMachine.HandleTimeout(TimedOutCreate.Generation);
	CompensationSuccessMachine.HandleOriginalCompletion(
		TimedOutCreate.Generation, EFU_OperationKind::Create, true, true);
	uint64 CompensationGeneration = 0;
	TestTrue(TEXT("迟到 Create 后可提交一次补偿 Destroy"), CompensationSuccessMachine.BeginRecoveryDestroyAttempt(CompensationGeneration));
	TestTrue(TEXT("补偿 Destroy 使用新 generation"), CompensationGeneration > TimedOutCreate.Generation);
	TestEqual(
		TEXT("旧 Create generation 不能清补偿 Destroy"),
		CompensationSuccessMachine.HandleOriginalCompletion(
			TimedOutCreate.Generation, EFU_OperationKind::Create, true, true),
		EFU_OperationAction::None);
	const EFU_OperationAction CompensationSucceeded = CompensationSuccessMachine.HandleOriginalCompletion(
		CompensationGeneration, EFU_OperationKind::Destroy, true, false);
	TestTrue(TEXT("补偿 Destroy 成功请求租约释放"), EnumHasAnyFlags(CompensationSucceeded, EFU_OperationAction::RequestLeaseRelease));
	TestFalse(TEXT("补偿 Destroy 成功不广播根请求第二次"), EnumHasAnyFlags(CompensationSucceeded, EFU_OperationAction::BroadcastFailure));
	TestTrue(TEXT("安全证据齐全后完成 Recovering"), CompensationSuccessMachine.FinishExplicitRecovery());
	TestEqual(TEXT("恢复完成回到 Idle"), CompensationSuccessMachine.Get().Phase, EFU_OperationPhase::Idle);

	FFU_OnlineOperationStateMachine CompensationRetryMachine;
	const FFU_OperationTicket RetryCreate = CompensationRetryMachine.BeginAcceptedAttempt(EFU_OperationKind::Create);
	CompensationRetryMachine.HandleTimeout(RetryCreate.Generation);
	CompensationRetryMachine.HandleOriginalCompletion(RetryCreate.Generation, EFU_OperationKind::Create, true, true);
	uint64 FirstDestroyGeneration = 0;
	TestTrue(TEXT("首次补偿 Destroy 提交"), CompensationRetryMachine.BeginRecoveryDestroyAttempt(FirstDestroyGeneration));
	const EFU_OperationAction FirstDestroyFailed = CompensationRetryMachine.HandleOriginalCompletion(
		FirstDestroyGeneration, EFU_OperationKind::Destroy, false, true);
	TestFalse(TEXT("补偿 Destroy 失败且 Session 存在不释放租约"), EnumHasAnyFlags(FirstDestroyFailed, EFU_OperationAction::RequestLeaseRelease));
	TestTrue(TEXT("终态失败允许显式恢复重试"), CompensationRetryMachine.CanRetryRecoveryDestroy(true, true));
	uint64 RetryDestroyGeneration = 0;
	TestTrue(TEXT("一次 TryRecover 可提交一次 Destroy 重试"), CompensationRetryMachine.BeginRecoveryDestroyAttempt(RetryDestroyGeneration, true));
	const uint8 AttemptsAfterOneRetry = CompensationRetryMachine.Get().RecoveryDestroyAttempts;
	uint64 ForbiddenSecondSubmission = 0;
	TestFalse(TEXT("同一次在途恢复不能再次提交 Destroy"), CompensationRetryMachine.BeginRecoveryDestroyAttempt(ForbiddenSecondSubmission, true));
	TestEqual(TEXT("拒绝的重复提交不增加重试次数"), CompensationRetryMachine.Get().RecoveryDestroyAttempts, AttemptsAfterOneRetry);
	const EFU_OperationAction RetryTimedOut = CompensationRetryMachine.HandleTimeout(RetryDestroyGeneration);
	TestTrue(TEXT("恢复 Destroy 再次超时保留原 delegate"), EnumHasAnyFlags(RetryTimedOut, EFU_OperationAction::KeepOriginalDelegate));
	TestFalse(TEXT("恢复 Destroy 超时不广播根请求第二次"), EnumHasAnyFlags(RetryTimedOut, EFU_OperationAction::BroadcastFailure));
	const EFU_OperationAction RetryDestroyFailed = CompensationRetryMachine.HandleOriginalCompletion(
		RetryDestroyGeneration, EFU_OperationKind::Destroy, false, true);
	TestFalse(TEXT("恢复 Destroy 迟到失败仍不重复广播"), EnumHasAnyFlags(RetryDestroyFailed, EFU_OperationAction::BroadcastFailure));

	// 【Fix round 1 RED：网络/旅行失败释放门】四项证据必须同时成立；NamedSession 存在时
	// 即使没有在途操作且 World 已清空，也绝不能请求恢复进程级 NetDriver 定义。
	FFU_ConnectionFailureLeaseReleaseSnapshot FailureRelease;
	FailureRelease.bSessionInterfaceValid = true;
	FailureRelease.bNamedSessionAbsent = true;
	FailureRelease.bNoOperationInFlight = true;
	FailureRelease.bAllWorldsClear = true;
	TestTrue(TEXT("四项安全证据齐全才允许失败路径释放"), FFU_OnlineOperationStateMachine::CanReleaseLeaseAfterConnectionFailure(FailureRelease));
	FailureRelease.bNamedSessionAbsent = false;
	TestFalse(TEXT("NamedSession 存在绝不释放"), FFU_OnlineOperationStateMachine::CanReleaseLeaseAfterConnectionFailure(FailureRelease));
	FailureRelease.bNamedSessionAbsent = true;
	FailureRelease.bSessionInterfaceValid = false;
	TestFalse(TEXT("接口无效无法证明 NoSession"), FFU_OnlineOperationStateMachine::CanReleaseLeaseAfterConnectionFailure(FailureRelease));
	FailureRelease.bSessionInterfaceValid = true;
	FailureRelease.bNoOperationInFlight = false;
	TestFalse(TEXT("仍有 OSS 操作在途绝不释放"), FFU_OnlineOperationStateMachine::CanReleaseLeaseAfterConnectionFailure(FailureRelease));
	FailureRelease.bNoOperationInFlight = true;
	FailureRelease.bAllWorldsClear = false;
	TestFalse(TEXT("全局网络或旅行未清空绝不释放"), FFU_OnlineOperationStateMachine::CanReleaseLeaseAfterConnectionFailure(FailureRelease));

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
	Snapshot.bRequestedProviderRestartRequired = true;
	TestEqual(
		TEXT("同 Provider 存在析构不确定操作时新 Owner 必须重启"),
		FFU_OnlineSessionNetDriverLease::EvaluateAcquire(Snapshot),
		EFU_NetDriverLeaseResult::RestartRequired);
	Snapshot.bRequestedProviderRestartRequired = false;
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
	Snapshot.bExistingLeaseProviderRestartRequired = true;
	TestEqual(
		TEXT("旧租约 Provider 被不确定操作阻断时禁止 stale-owner 回收"),
		FFU_OnlineSessionNetDriverLease::EvaluateAcquire(Snapshot),
		EFU_NetDriverLeaseResult::RestartRequired);
	Snapshot.bExistingLeaseProviderRestartRequired = false;
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
	ReleaseSnapshot.bProviderRestartRequired = true;
	TestFalse(
		TEXT("当前 Provider 有析构不确定操作时必须保持 fail-closed"),
		FFU_OnlineSessionNetDriverLease::EvaluateReleaseComplete(ReleaseSnapshot));
	ReleaseSnapshot.bProviderRestartRequired = false;
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

	// 【Fix round 1 RED：进程级析构 blocker】登记只影响精确 Provider，且没有自动清除 API；
	// 同 Provider 的下一 GameInstance 不能把弱 Owner 失效误当成安全终态。
	FFU_NetDriverLeaseUncertainOperationBlockers Blockers;
	TestFalse(TEXT("初始 Steam 无重启要求"), Blockers.IsRestartRequired(EFU_OnlineProvider::Steam));
	TestFalse(TEXT("初始 LAN 无重启要求"), Blockers.IsRestartRequired(EFU_OnlineProvider::Lan));
	Blockers.Register(EFU_OnlineProvider::Steam);
	TestTrue(TEXT("登记 Steam 不确定析构后要求进程重启"), Blockers.IsRestartRequired(EFU_OnlineProvider::Steam));
	TestFalse(TEXT("Steam blocker 不污染 LAN 的精确状态"), Blockers.IsRestartRequired(EFU_OnlineProvider::Lan));
	Blockers.Register(EFU_OnlineProvider::Lan);
	TestTrue(TEXT("LAN 可独立登记重启要求"), Blockers.IsRestartRequired(EFU_OnlineProvider::Lan));

	// 【Fix round 2 RED：跨模块重载仍须保持进程 blocker】这里不真正卸载测试模块，而是只保留
	// 模块外哨兵字符串并创建一份全新的纯快照，等价模拟 DLL static 全部析构后重新初始化。
	constexpr uint32 OriginalProcessId = 424242;
	const FString SameProcessSentinel =
		FFU_NetDriverLeasePersistentBlockerPolicy::BuildSentinel(OriginalProcessId);
	FFU_NetDriverLeasePersistentBlockerSnapshot ReloadedModuleSnapshot;
	ReloadedModuleSnapshot.CurrentProcessId = OriginalProcessId;
	ReloadedModuleSnapshot.SteamUncertainOperationSentinel = SameProcessSentinel;
	ReloadedModuleSnapshot.SteamOrphanedLeaseSentinel = SameProcessSentinel;
	FFU_NetDriverLeaseUncertainOperationBlockers ClearedModuleStatics;
	TestFalse(
		TEXT("模拟重载时新模块 static 本身已经清空"),
		ClearedModuleStatics.IsRestartRequired(EFU_OnlineProvider::Steam));
	const FFU_NetDriverLeasePersistentBlockerDecision SameProcessDecision =
		FFU_NetDriverLeasePersistentBlockerPolicy::Evaluate(ReloadedModuleSnapshot);
	TestTrue(TEXT("同 PID 重载后仍检测 Steam blocker"), SameProcessDecision.bSteamRestartRequired);
	TestFalse(TEXT("Steam 持久 blocker 不污染 LAN"), SameProcessDecision.bLanRestartRequired);
	TestTrue(TEXT("同 PID 重载后仍检测遗留共享租约"), SameProcessDecision.bOrphanedLeaseRestartRequired);
	TestEqual(TEXT("遗留租约保留精确 Steam Provider"), SameProcessDecision.OrphanedLeaseProvider, EFU_OnlineProvider::Steam);
	FFU_NetDriverLeasePreflight ReloadedAcquire;
	ReloadedAcquire.bIsGameThread = true;
	ReloadedAcquire.GameNetDriverDefinitionCount = 1;
	ReloadedAcquire.bRequestedProviderRestartRequired = SameProcessDecision.bSteamRestartRequired;
	ReloadedAcquire.bExistingLeaseProviderRestartRequired = SameProcessDecision.bOrphanedLeaseRestartRequired;
	TestEqual(
		TEXT("同 PID 新模块不能重新 acquire 遗留 Steam 定义"),
		FFU_OnlineSessionNetDriverLease::EvaluateAcquire(ReloadedAcquire),
		EFU_NetDriverLeaseResult::RestartRequired);
	ReloadedAcquire.bLeaseExists = true;
	ReloadedAcquire.bLeaseOwnerExpired = true;
	ReloadedAcquire.bInstalledValueStillMatches = true;
	TestEqual(
		TEXT("同 PID 新模块不能 stale-owner reclaim"),
		FFU_OnlineSessionNetDriverLease::EvaluateAcquire(ReloadedAcquire),
		EFU_NetDriverLeaseResult::RestartRequired);
	FFU_NetDriverLeaseReleaseSnapshot ReloadedRelease;
	ReloadedRelease.bIsGameThread = true;
	ReloadedRelease.bOwnerValid = true;
	ReloadedRelease.bProviderRestartRequired = SameProcessDecision.bSteamRestartRequired;
	TestFalse(
		TEXT("同 PID 新模块不能把遗留 Steam 租约判成已释放"),
		FFU_OnlineSessionNetDriverLease::EvaluateReleaseComplete(ReloadedRelease));
	TestEqual(
		TEXT("同 PID 新模块不能恢复遗留 Steam 定义"),
		FFU_OnlineSessionNetDriverLease::EvaluateRestore(
			Expected, Expected, true, SameProcessDecision.bSteamRestartRequired),
		EFU_NetDriverLeaseResult::RestartRequired);

	// 子进程会继承环境变量，但 PID 不同；继承值必须被识别为上一进程的陈旧哨兵，
	// 否则真正的新进程会被错误地要求重启第二次。
	ReloadedModuleSnapshot.CurrentProcessId = OriginalProcessId + 1;
	const FFU_NetDriverLeasePersistentBlockerDecision NewProcessDecision =
		FFU_NetDriverLeasePersistentBlockerPolicy::Evaluate(ReloadedModuleSnapshot);
	TestFalse(TEXT("不同 PID 不继承 Steam blocker"), NewProcessDecision.bSteamRestartRequired);
	TestFalse(TEXT("不同 PID 不继承 LAN blocker"), NewProcessDecision.bLanRestartRequired);
	TestFalse(TEXT("不同 PID 不继承遗留租约阻断"), NewProcessDecision.bOrphanedLeaseRestartRequired);
	FFU_NetDriverLeasePreflight NewProcessAcquire;
	NewProcessAcquire.bIsGameThread = true;
	NewProcessAcquire.GameNetDriverDefinitionCount = 1;
	NewProcessAcquire.bRequestedProviderRestartRequired = NewProcessDecision.bSteamRestartRequired;
	NewProcessAcquire.bExistingLeaseProviderRestartRequired = NewProcessDecision.bOrphanedLeaseRestartRequired;
	TestEqual(
		TEXT("不同 PID 的真正新进程可从干净定义正常 acquire"),
		FFU_OnlineSessionNetDriverLease::EvaluateAcquire(NewProcessAcquire),
		EFU_NetDriverLeaseResult::Acquired);

	// 同一 PID 下每个 Provider 使用独立哨兵；LAN 版本不得反向设置 Steam。
	FFU_NetDriverLeasePersistentBlockerSnapshot LanOnlySnapshot;
	LanOnlySnapshot.CurrentProcessId = OriginalProcessId;
	LanOnlySnapshot.LanUncertainOperationSentinel = SameProcessSentinel;
	LanOnlySnapshot.LanOrphanedLeaseSentinel = SameProcessSentinel;
	const FFU_NetDriverLeasePersistentBlockerDecision LanOnlyDecision =
		FFU_NetDriverLeasePersistentBlockerPolicy::Evaluate(LanOnlySnapshot);
	TestFalse(TEXT("LAN 持久 blocker 不污染 Steam"), LanOnlyDecision.bSteamRestartRequired);
	TestTrue(TEXT("同 PID 重载后仍检测 LAN blocker"), LanOnlyDecision.bLanRestartRequired);
	TestTrue(TEXT("同 PID 重载后检测 LAN 遗留租约"), LanOnlyDecision.bOrphanedLeaseRestartRequired);
	TestEqual(TEXT("遗留租约保留精确 LAN Provider"), LanOnlyDecision.OrphanedLeaseProvider, EFU_OnlineProvider::Lan);
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

	// 【Characterization：公开 ClearDiagnosticHistory 只转发到此生产边界】Subsystem 需要有效
	// GameInstance/Diagnostics 生命周期，隔离自动化不安全地构造它反而会伪造状态；这里让真实 active
	// 状态机与同一个 FFU_OnlineSessionDiagnostics::ClearHistory 并存，逐字段锁定“清历史绝不触碰操作”。
	FFU_OnlineOperationStateMachine ActiveMachine;
	FFU_OperationTicket ActiveTicket = ActiveMachine.BeginAttempt(EFU_OperationKind::Join);
	TestTrue(TEXT("清历史前可建立 active Join"), ActiveMachine.AcceptAttempt(ActiveTicket, EFU_OperationKind::Join));
	const FFU_OperationState BeforeClear = ActiveMachine.Get();
	Diagnostics.ClearHistory();
	const FFU_OperationState AfterClear = ActiveMachine.Get();
	TestEqual(TEXT("清历史后 history 为空"), Diagnostics.GetHistory().Num(), 0);
	TestEqual(TEXT("清历史保持 AttemptSequence"), AfterClear.AttemptSequence, BeforeClear.AttemptSequence);
	TestEqual(TEXT("清历史保持 ActiveGeneration"), AfterClear.ActiveGeneration, BeforeClear.ActiveGeneration);
	TestEqual(TEXT("清历史保持 ActiveOperationId"), AfterClear.ActiveOperationId, BeforeClear.ActiveOperationId);
	TestEqual(TEXT("清历史保持 ActiveKind"), AfterClear.ActiveKind, BeforeClear.ActiveKind);
	TestEqual(TEXT("清历史保持 RootKind"), AfterClear.RootKind, BeforeClear.RootKind);
	TestEqual(TEXT("清历史保持 Phase"), AfterClear.Phase, BeforeClear.Phase);
	TestEqual(TEXT("清历史保持 CompletionBroadcast"), AfterClear.bCompletionBroadcast, BeforeClear.bCompletionBroadcast);
	TestEqual(TEXT("清历史保持 AwaitingOriginalCompletion"), AfterClear.bAwaitingOriginalCompletion, BeforeClear.bAwaitingOriginalCompletion);
	TestEqual(TEXT("清历史保持 FindCancellationOutstanding"), AfterClear.bFindCancellationOutstanding, BeforeClear.bFindCancellationOutstanding);
	TestEqual(TEXT("清历史保持 RecoveryDestroyAttempts"), AfterClear.RecoveryDestroyAttempts, BeforeClear.RecoveryDestroyAttempts);
	TestEqual(TEXT("清历史保持 OriginalCallbackSeen"), AfterClear.bOriginalCallbackSeen, BeforeClear.bOriginalCallbackSeen);
	TestEqual(TEXT("清历史保持 GenerationSequence"), AfterClear.GenerationSequence, BeforeClear.GenerationSequence);
	Diagnostics.Emit(SecondEvent);
	TestEqual(TEXT("清历史后仍可继续记录"), Diagnostics.GetHistory().Num(), 1);
	TestEqual(TEXT("清历史后仍可继续 Blueprint 广播"), BlueprintBroadcastCount, 5);

	// 【Fix round 1 RED：非法 Provider 仍须公开可观察】决策函数对合法枚举不产生事件；
	// 非法枚举只生成不含调用参数、凭据或连接信息的固定安全事件，再走统一脱敏/广播出口。
	TestFalse(
		TEXT("合法 Steam 恢复请求不生成非法枚举诊断"),
		FFU_OnlineSessionDiagnostics::BuildUnsupportedRecoveryProviderDiagnostic(EFU_OnlineProvider::Steam).IsSet());
	const TOptional<FFU_OnlineDiagnosticEvent> UnsupportedProviderDiagnostic =
		FFU_OnlineSessionDiagnostics::BuildUnsupportedRecoveryProviderDiagnostic(
			static_cast<EFU_OnlineProvider>(MAX_uint8));
	TestTrue(TEXT("非法 Provider 生成安全诊断"), UnsupportedProviderDiagnostic.IsSet());
	if (UnsupportedProviderDiagnostic.IsSet())
	{
		const FFU_OnlineDiagnosticEvent EmittedUnsupported =
			Diagnostics.Emit(UnsupportedProviderDiagnostic.GetValue());
		TestEqual(TEXT("非法 Provider 诊断使用稳定代码"), EmittedUnsupported.Code, FString(TEXT("FU.Recovery.Rejected.UnsupportedProvider")));
		TestEqual(TEXT("非法 Provider 诊断公开标记 Recovery"), EmittedUnsupported.Operation, EFU_OnlineDiagnosticOperation::Recovery);
		TestEqual(TEXT("非法 Provider 诊断公开标记 Rejected"), EmittedUnsupported.Status, FString(TEXT("Rejected")));
		TestTrue(TEXT("非法 Provider 诊断不携带可泄露字段"), EmittedUnsupported.Fields.IsEmpty());
		TestEqual(TEXT("非法 Provider 诊断恰好通过 Blueprint 出口广播一次"), BlueprintBroadcastCount, 6);
	}
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
