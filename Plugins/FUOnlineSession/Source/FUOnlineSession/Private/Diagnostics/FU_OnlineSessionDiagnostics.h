#pragma once

#include "CoreMinimal.h"
#include "FU_OnlineDiagnosticTypes.h"
#include "FU_OnlineOperationStateMachine.h"

class UGameViewportClient;
class SFU_OnlineDiagnosticOverlay;

/** Runtime 分发器的不可持久化输出配置；来源是 UFU_OnlineSessionSettings。 */
struct FFU_OnlineDiagnosticDispatchConfig
{
	int32 HistoryLimit = 200;
	bool bEmitToLog = true;
	bool bEnableOverlay = true;
	EFU_OnlineDiagnosticSeverity MinimumOverlaySeverity = EFU_OnlineDiagnosticSeverity::Warning;
	float OverlayDurationSeconds = 8.0f;
	int32 OverlayRowLimit = 6;
};

/** Slate 浮层使用的非反射行模型；它只保留已经脱敏后的短文本。 */
struct FFU_OnlineDiagnosticOverlayRow
{
	FDateTime ExpiresAtUtc;
	EFU_OnlineDiagnosticSeverity Severity = EFU_OnlineDiagnosticSeverity::Info;
	FString Text;
};

/**
 * 将诊断事件转成有限条、自动过期的屏幕行。
 *
 * 模型与 Viewport 是否存在无关：没有可显示的 Viewport 时诊断历史照常工作，
 * 之后有 Viewport 才附着 Slate View，不会丢失已经记录的问题。
 */
class FFU_OnlineDiagnosticOverlayModel final
{
public:
	void Add(
		const FFU_OnlineDiagnosticEvent& Event,
		const FFU_OnlineDiagnosticDispatchConfig& Config,
		FDateTime NowUtc);

	TArray<FFU_OnlineDiagnosticOverlayRow> GetVisibleRows(FDateTime NowUtc) const;

private:
	TArray<FFU_OnlineDiagnosticOverlayRow> Rows;
};

/**
 * 一处入口分发所有联机诊断。
 *
 * 生命周期归 GameInstanceSubsystem 所有，且只在游戏线程调用：这既让 Slate/Blueprint
 * 回调保持线程安全，也确保日志、浮层和历史引用同一份脱敏事件。
 */
class FFU_OnlineSessionDiagnostics final
{
public:
	/**
	 * 报告写入边界；默认实现使用 FFileHelper。仅测试可替换它，以验证写入失败不会递归重试或泄露写入错误原文。
	 */
	using FReportWriter = TFunction<bool(const FString& Contents, const FString& Destination, FString& OutRawFailureDetail)>;
	/** 已脱敏事件的日志出口；默认 WriteToLog，测试可捕获完整格式行而不写 UE_LOG。 */
	using FLogSink = TFunction<void(const FFU_OnlineDiagnosticEvent& Event, const FString& FormattedLine)>;

	explicit FFU_OnlineSessionDiagnostics(
		const FFU_OnlineDiagnosticDispatchConfig& InConfig,
		TFunction<void(const FFU_OnlineDiagnosticEvent&)> InBlueprintBroadcast,
		FReportWriter InReportWriter = FReportWriter(),
		FLogSink InLogSink = FLogSink());

	/** 统一执行脱敏 -> 有界历史 -> UE_LOG -> Slate 浮层 -> Blueprint 广播。 */
	FFU_OnlineDiagnosticEvent Emit(const FFU_OnlineDiagnosticEvent& CandidateEvent);

	/** 返回副本，防止 Blueprint 或调用方修改内部历史。 */
	TArray<FFU_OnlineDiagnosticEvent> GetHistory() const;
	void ClearHistory();

	/** 生成仅含安全字段的纯文本报告；不执行磁盘写入。 */
	FString BuildReport() const;

	/**
	 * 仅允许导出到 Project/Saved/Logs/FUOnlineSession；调用方不能指定任意路径。
	 * 保存失败只通过返回值和 OutError 报告，绝不递归产生新的保存诊断事件。
	 */
	bool SaveReport(FString& OutSavedPath, FString& OutError);

	/** 可独立测试的统一脱敏入口，保证所有输出使用同一套规则。 */
	static FFU_OnlineDiagnosticEvent Sanitize(const FFU_OnlineDiagnosticEvent& CandidateEvent);

	/**
	 * 为 TryRecoverProvider 的非法枚举生成固定、无调用参数的安全诊断；合法 Provider 返回 unset。
	 * 纯构造函数让拒绝路径可测试，实际公开观察仍统一经过 Emit 的日志/历史/Blueprint 出口。
	 */
	static TOptional<FFU_OnlineDiagnosticEvent> BuildUnsupportedRecoveryProviderDiagnostic(
		EFU_OnlineProvider Provider);

	/**
	 * Provider 预检模板与自动化测试共用的纯事件构造缝。它不依赖 UObject/OSS，
	 * 使“诊断先于旧失败委托”能以真实的稳定状态码和 OperationId 验证。
	 */
	static FFU_OnlineDiagnosticEvent BuildProviderPreflightDiagnostic(
		EFU_OnlineProvider Provider,
		EFU_OnlineDiagnosticOperation Operation,
		const FGuid& OperationId,
		bool bIsReady,
		const FString& Code,
		const FString& Message);

	/** Viewport 存在时才创建资产无关的 Slate 浮层；传入 nullptr 等价于解绑。 */
	void AttachViewport(UGameViewportClient* InViewport);
	void DetachViewport();

private:
	static FString FormatEventForOutput(const FFU_OnlineDiagnosticEvent& Event);
	static void WriteToLog(const FFU_OnlineDiagnosticEvent& Event);

	FFU_OnlineDiagnosticDispatchConfig Config;
	TArray<FFU_OnlineDiagnosticEvent> History;
	// 分发器只在 GameInstance 游戏线程使用，故无需跨线程原子计数；序号仍能稳定关联同一实例内的事件顺序。
	int64 NextSequence = 0;
	TFunction<void(const FFU_OnlineDiagnosticEvent&)> BlueprintBroadcast;
	FReportWriter ReportWriter;
	FLogSink LogSink;
	TSharedRef<FFU_OnlineDiagnosticOverlayModel> OverlayModel;
	TWeakObjectPtr<UGameViewportClient> OverlayViewport;
	TSharedPtr<SFU_OnlineDiagnosticOverlay> OverlayWidget;
};

/** Find 取消边界的有限结果；只描述已由状态机验证过的路径，不携带 OSS 原始错误文本。 */
enum class EFU_FindCancellationDiagnosticOutcome : uint8
{
	InterfaceUnavailable,
	DelegateBound,
	RequestSubmitted,
	SynchronousRejected,
	CancelWonRace,
	FailedWaitingForOriginal
};

/** Recovery Destroy 的有限内部结果；所有分支都沿用根操作 ID，不接受 OSS 原始错误。 */
enum class EFU_RecoveryDestroyDiagnosticOutcome : uint8
{
	InterfaceUnavailable,
	NoSession,
	StateRejected,
	SubmitAccepted,
	SynchronousRejected,
	CallbackSucceeded,
	CallbackFailed,
	RepeatedTimeout
};

/**
 * A1 内部竞态的纯诊断 policy。Subsystem 的模板路径直接消费这些 builder，自动化测试也调用同一实现；
 * builder 只构造固定 code/status/message，不清 delegate、不改状态机，也不触发任何旧完成委托。
 */
class FFU_OnlineOperationPathDiagnostics final
{
public:
	static FFU_OnlineDiagnosticEvent BuildLateCallback(
		EFU_OnlineProvider Provider,
		EFU_OnlineDiagnosticOperation Operation,
		const FGuid& OperationId,
		bool bSucceeded,
		EFU_OperationAction Actions);

	static FFU_OnlineDiagnosticEvent BuildRecoveringFindOriginal(
		EFU_OnlineProvider Provider,
		const FGuid& OperationId,
		bool bSucceeded,
		EFU_OperationAction Actions);

	static FFU_OnlineDiagnosticEvent BuildFindCancellation(
		EFU_OnlineProvider Provider,
		const FGuid& OperationId,
		EFU_FindCancellationDiagnosticOutcome Outcome);

	static FFU_OnlineDiagnosticEvent BuildRecoveryDestroy(
		EFU_OnlineProvider Provider,
		const FGuid& OperationId,
		EFU_RecoveryDestroyDiagnosticOutcome Outcome,
		bool bSessionStillExists,
		EFU_OperationAction Actions);
};

/**
 * 无 UObject 的预检 gate：生产模板与自动化测试必须共用它，保证诊断 sink 一定早于旧失败续步。
 */
class FFU_OnlineProviderPreflightGate final
{
public:
	static bool Dispatch(
		EFU_OnlineProvider Provider,
		EFU_OnlineDiagnosticOperation Operation,
		const FGuid& OperationId,
		const FFU_OnlineProviderStatus& Status,
		TFunctionRef<void(FFU_OnlineDiagnosticEvent)> DiagnosticSink,
		TFunctionRef<void()> FailureContinuation);
};
