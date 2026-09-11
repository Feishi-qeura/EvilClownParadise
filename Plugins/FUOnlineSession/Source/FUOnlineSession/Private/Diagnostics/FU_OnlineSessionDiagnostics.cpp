#include "Diagnostics/FU_OnlineSessionDiagnostics.h"

#include "Diagnostics/SFU_OnlineDiagnosticOverlay.h"
#include "FUOnlineSessionModule.h"
#include "Engine/GameViewportClient.h"
#include "GenericPlatform/GenericPlatformProperties.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/App.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Widgets/SWidget.h"

namespace FUOnlineSessionDiagnosticsPrivate
{
	/**
	 * 输出限制不是用户可配项：它是诊断面向日志和 Blueprint 的安全边界。
	 * 即使某个外部调用传来极大的错误文本或字段数组，也不能无限放大内存或报告文件。
	 */
	constexpr int32 MaxTextLength = 1024;
	constexpr int32 MaxFieldValueLength = 256;
	constexpr int32 MaxFieldCount = 32;

	FString TruncateText(const FString& Value, const int32 Limit)
	{
		if (Value.Len() <= Limit)
		{
			return Value;
		}

		return Value.Left(Limit) + TEXT("…<truncated>");
	}

	bool IsSensitiveFieldKey(const FName Key)
	{
		const FString NormalizedKey = Key.ToString().ToLower();
		// 【安全优先】使用包含匹配而不是白名单精确匹配，宁可多遮挡一个可疑字段，
		// 也不能因为调用方使用 AuthTicket / RoomPassword 等变体而把凭据写进日志。
		return NormalizedKey.Contains(TEXT("password"))
			|| NormalizedKey.Contains(TEXT("passphrase"))
			|| NormalizedKey.Contains(TEXT("token"))
			|| NormalizedKey.Contains(TEXT("ticket"))
			|| NormalizedKey.Contains(TEXT("secret"))
			|| NormalizedKey.Contains(TEXT("auth"))
			|| NormalizedKey.Contains(TEXT("credential"))
			|| NormalizedKey.Contains(TEXT("steamid"))
			|| NormalizedKey.Contains(TEXT("steam_id"))
			|| NormalizedKey.Contains(TEXT("connectstring"));
	}

	bool IsInlineValueDelimiter(const TCHAR Character)
	{
		return FChar::IsWhitespace(Character)
			|| Character == TEXT(',')
			|| Character == TEXT(';')
			|| Character == TEXT('&')
			|| Character == TEXT(']')
			|| Character == TEXT(')');
	}

	/**
	 * 现有引擎错误文本有时会带 Key=Value 上下文。字段结构会被强制脱敏，
	 * 这里再防御性处理 Message/Cause 等自由文本，覆盖 RoomPassword=xxx、Token:xxx 这类常见形式。
	 */
	FString RedactInlineSensitiveAssignments(const FString& Value, const int32 Limit)
	{
		FString Result = TruncateText(Value, Limit);
		static const TCHAR* SensitiveMarkers[] =
		{
			TEXT("password"),
			TEXT("passphrase"),
			TEXT("token"),
			TEXT("ticket"),
			TEXT("secret"),
			TEXT("auth"),
			TEXT("credential"),
			TEXT("steamid"),
			TEXT("connectstring")
		};

		for (const TCHAR* Marker : SensitiveMarkers)
		{
			const FString MarkerString(Marker);
			int32 SearchOffset = 0;
			while (SearchOffset < Result.Len())
			{
				const int32 MarkerIndex = Result.Find(
					MarkerString,
					ESearchCase::IgnoreCase,
					ESearchDir::FromStart,
					SearchOffset);
				if (MarkerIndex == INDEX_NONE)
				{
					break;
				}

				int32 ValueStart = MarkerIndex + MarkerString.Len();
				while (ValueStart < Result.Len() && FChar::IsWhitespace(Result[ValueStart]))
				{
					++ValueStart;
				}

				if (ValueStart >= Result.Len() || (Result[ValueStart] != TEXT('=') && Result[ValueStart] != TEXT(':')))
				{
					SearchOffset = MarkerIndex + MarkerString.Len();
					continue;
				}

				++ValueStart;
				while (ValueStart < Result.Len() && FChar::IsWhitespace(Result[ValueStart]))
				{
					++ValueStart;
				}

				int32 ValueEnd = ValueStart;
				if (ValueStart < Result.Len() && (Result[ValueStart] == TEXT('\"') || Result[ValueStart] == TEXT('\'')))
				{
					const TCHAR Quote = Result[ValueStart++];
					ValueEnd = ValueStart;
					while (ValueEnd < Result.Len() && Result[ValueEnd] != Quote)
					{
						++ValueEnd;
					}
					if (ValueEnd < Result.Len())
					{
						++ValueEnd;
					}
					// 将开引号也覆盖，避免日志留下不完整且容易误导的凭据片段。
					--ValueStart;
				}
				else
				{
					while (ValueEnd < Result.Len() && !IsInlineValueDelimiter(Result[ValueEnd]))
					{
						++ValueEnd;
					}
				}

				Result = Result.Left(ValueStart) + TEXT("<redacted>") + Result.Mid(ValueEnd);
				SearchOffset = ValueStart + FCString::Strlen(TEXT("<redacted>"));
			}
		}

		return Result;
	}

	const TCHAR* ToText(const EFU_OnlineDiagnosticSeverity Severity)
	{
		switch (Severity)
		{
		case EFU_OnlineDiagnosticSeverity::Verbose: return TEXT("Verbose");
		case EFU_OnlineDiagnosticSeverity::Info: return TEXT("Info");
		case EFU_OnlineDiagnosticSeverity::Warning: return TEXT("Warning");
		case EFU_OnlineDiagnosticSeverity::Error: return TEXT("Error");
		default: return TEXT("Unknown");
		}
	}

	const TCHAR* ToText(const EFU_OnlineProvider Provider)
	{
		return Provider == EFU_OnlineProvider::Steam ? TEXT("Steam") : TEXT("Lan/NULL");
	}

	const TCHAR* ToText(const EFU_OnlineDiagnosticOperation Operation)
	{
		switch (Operation)
		{
		case EFU_OnlineDiagnosticOperation::Environment: return TEXT("Environment");
		case EFU_OnlineDiagnosticOperation::CreateSession: return TEXT("CreateSession");
		case EFU_OnlineDiagnosticOperation::FindSessions: return TEXT("FindSessions");
		case EFU_OnlineDiagnosticOperation::JoinSession: return TEXT("JoinSession");
		case EFU_OnlineDiagnosticOperation::DestroySession: return TEXT("DestroySession");
		case EFU_OnlineDiagnosticOperation::NetDriverLease: return TEXT("NetDriverLease");
		case EFU_OnlineDiagnosticOperation::AppIdBootstrap: return TEXT("AppIdBootstrap");
		case EFU_OnlineDiagnosticOperation::LegacyConfigMigration: return TEXT("LegacyConfigMigration");
		case EFU_OnlineDiagnosticOperation::Recovery: return TEXT("Recovery");
		default: return TEXT("Unknown");
		}
	}

	const TCHAR* ToText(const EFU_OnlineDiagnosticPhase Phase)
	{
		switch (Phase)
		{
		case EFU_OnlineDiagnosticPhase::Requested: return TEXT("Requested");
		case EFU_OnlineDiagnosticPhase::Preflight: return TEXT("Preflight");
		case EFU_OnlineDiagnosticPhase::Submitted: return TEXT("Submitted");
		case EFU_OnlineDiagnosticPhase::Callback: return TEXT("Callback");
		case EFU_OnlineDiagnosticPhase::Timeout: return TEXT("Timeout");
		case EFU_OnlineDiagnosticPhase::Recovery: return TEXT("Recovery");
		case EFU_OnlineDiagnosticPhase::Completed: return TEXT("Completed");
		default: return TEXT("Unknown");
		}
	}

	FString FormatFields(const TArray<FFU_OnlineDiagnosticField>& Fields)
	{
		FString Result;
		for (const FFU_OnlineDiagnosticField& Field : Fields)
		{
			if (!Result.IsEmpty())
			{
				Result += TEXT(", ");
			}

			Result += Field.Key.ToString();
			Result += TEXT("=");
			Result += Field.Value;
		}
		return Result;
	}
}

void FFU_OnlineDiagnosticOverlayModel::Add(
	const FFU_OnlineDiagnosticEvent& Event,
	const FFU_OnlineDiagnosticDispatchConfig& Config,
	const FDateTime NowUtc)
{
	// 【显示与记录分离】浮层是可选输出，历史和 Blueprint 不因低严重级别/无 Viewport 而被抑制。
	if (!Config.bEnableOverlay || static_cast<uint8>(Event.Severity) < static_cast<uint8>(Config.MinimumOverlaySeverity))
	{
		return;
	}

	FFU_OnlineDiagnosticOverlayRow& Row = Rows.AddDefaulted_GetRef();
	Row.ExpiresAtUtc = NowUtc + FTimespan::FromSeconds(FMath::Max(1.0f, Config.OverlayDurationSeconds));
	Row.Severity = Event.Severity;
	Row.Text = FString::Printf(
		TEXT("[FU][%s][%s] %s"),
		FUOnlineSessionDiagnosticsPrivate::ToText(Event.Severity),
		*Event.Code,
		*Event.Message);

	// 【有界浮层】即使发生连续网络故障，Slate 每帧也只需渲染有限数量的行。
	const int32 RowLimit = FMath::Max(1, Config.OverlayRowLimit);
	if (Rows.Num() > RowLimit)
	{
		Rows.RemoveAt(0, Rows.Num() - RowLimit, EAllowShrinking::No);
	}
}

TArray<FFU_OnlineDiagnosticOverlayRow> FFU_OnlineDiagnosticOverlayModel::GetVisibleRows(const FDateTime NowUtc) const
{
	TArray<FFU_OnlineDiagnosticOverlayRow> VisibleRows;
	for (const FFU_OnlineDiagnosticOverlayRow& Row : Rows)
	{
		if (Row.ExpiresAtUtc > NowUtc)
		{
			VisibleRows.Add(Row);
		}
	}

	return VisibleRows;
}

FFU_OnlineSessionDiagnostics::FFU_OnlineSessionDiagnostics(
	const FFU_OnlineDiagnosticDispatchConfig& InConfig,
	TFunction<void(const FFU_OnlineDiagnosticEvent&)> InBlueprintBroadcast,
	FReportWriter InReportWriter,
	FLogSink InLogSink)
	: Config(InConfig)
	, BlueprintBroadcast(MoveTemp(InBlueprintBroadcast))
	, ReportWriter(MoveTemp(InReportWriter))
	, LogSink(MoveTemp(InLogSink))
	, OverlayModel(MakeShared<FFU_OnlineDiagnosticOverlayModel>())
{
	// 【运行时防御】Subsystem 已消毒配置；这里仍做最小边界保护，让私有测试或未来调用者不会创建无界容器。
	Config.HistoryLimit = FMath::Clamp(Config.HistoryLimit, 1, 1000);
	Config.OverlayDurationSeconds = FMath::Clamp(Config.OverlayDurationSeconds, 1.0f, 60.0f);
	Config.OverlayRowLimit = FMath::Clamp(Config.OverlayRowLimit, 1, 20);
	if (!ReportWriter)
	{
		// 【默认生产写入器】测试可注入失败 writer；运行时仍保持既有受限路径与 UTF-8 报告格式。
		ReportWriter = [](const FString& Contents, const FString& Destination, FString& OutRawFailureDetail)
		{
			return FFileHelper::SaveStringToFile(
				Contents,
				*Destination,
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		};
	}
	if (!LogSink)
	{
		LogSink = [](const FFU_OnlineDiagnosticEvent& Event, const FString&)
		{
			WriteToLog(Event);
		};
	}
}

FFU_OnlineDiagnosticEvent FFU_OnlineSessionDiagnostics::Emit(const FFU_OnlineDiagnosticEvent& CandidateEvent)
{
	FFU_OnlineDiagnosticEvent Event = Sanitize(CandidateEvent);
	// 【顺序不变量】Sequence 只由实际分发器分配，外部调用者不能伪造或倒退本 GameInstance 的诊断时间线。
	Event.Sequence = ++NextSequence;

	History.Add(Event);
	if (History.Num() > Config.HistoryLimit)
	{
		History.RemoveAt(0, History.Num() - Config.HistoryLimit, EAllowShrinking::No);
	}

	if (Config.bEmitToLog)
	{
		LogSink(Event, FormatEventForOutput(Event));
	}

	// 浮层模型独立于实际 Viewport；没有 Viewport 时它只是暂不显示，绝不影响后续 Blueprint/历史。
	OverlayModel->Add(Event, Config, FDateTime::UtcNow());

	// 【不可关闭的可观测性】输出开关只控制日志/屏幕，不控制 Blueprint；项目 UI 始终能自行收集完整诊断。
	if (BlueprintBroadcast)
	{
		BlueprintBroadcast(Event);
	}

	return Event;
}

TArray<FFU_OnlineDiagnosticEvent> FFU_OnlineSessionDiagnostics::GetHistory() const
{
	return History;
}

void FFU_OnlineSessionDiagnostics::ClearHistory()
{
	History.Reset();
}

FString FFU_OnlineSessionDiagnostics::BuildReport() const
{
	FString PluginVersion = TEXT("Unknown");
	if (const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("FUOnlineSession")))
	{
		PluginVersion = Plugin->GetDescriptor().VersionName;
	}

	FString Report;
	Report += TEXT("FU Online Session Diagnostic Report\n");
	Report += FString::Printf(TEXT("GeneratedUtc: %s\n"), *FDateTime::UtcNow().ToIso8601());
	Report += FString::Printf(TEXT("PluginVersion: %s\n"), *PluginVersion);
	Report += FString::Printf(TEXT("Project: %s\n"), FApp::GetProjectName());
	Report += FString::Printf(TEXT("Engine: %s\n"), *FEngineVersion::Current().ToString());
	Report += FString::Printf(TEXT("Platform: %s\n"), ANSI_TO_TCHAR(FPlatformProperties::PlatformName()));
	Report += FString::Printf(TEXT("BuildConfiguration: %s\n"), LexToString(FApp::GetBuildConfiguration()));
	Report += FString::Printf(TEXT("HistoryCount: %d\n\n"), History.Num());

	for (const FFU_OnlineDiagnosticEvent& Event : History)
	{
		Report += FString::Printf(
			TEXT("[%s] [Seq=%lld] [%s] Op=%s/%s Provider=%s Id=%s World=%s PIE=%d Code=%s Status=%s\n"),
			*Event.TimestampUtc.ToIso8601(),
			Event.Sequence,
			FUOnlineSessionDiagnosticsPrivate::ToText(Event.Severity),
			FUOnlineSessionDiagnosticsPrivate::ToText(Event.Operation),
			FUOnlineSessionDiagnosticsPrivate::ToText(Event.Phase),
			FUOnlineSessionDiagnosticsPrivate::ToText(Event.Provider),
			*Event.OperationId.ToString(),
			*Event.WorldName,
			Event.PIEInstanceId,
			*Event.Code,
			*Event.Status);
		Report += FString::Printf(TEXT("  Message: %s\n"), *Event.Message);
		Report += FString::Printf(TEXT("  Cause: %s\n"), *Event.Cause);
		Report += FString::Printf(TEXT("  RecommendedAction: %s\n"), *Event.RecommendedAction);
		if (!Event.Fields.IsEmpty())
		{
			Report += FString::Printf(
				TEXT("  Fields: %s\n"),
				*FUOnlineSessionDiagnosticsPrivate::FormatFields(Event.Fields));
		}
		Report += TEXT("\n");
	}

	return Report;
}

bool FFU_OnlineSessionDiagnostics::SaveReport(FString& OutSavedPath, FString& OutError)
{
	OutSavedPath.Reset();
	OutError.Reset();

	// 【路径收束】报告只能写到项目 Saved/Logs，不能由 Blueprint 或错误文本影响写入目标。
	const FString ReportDirectory = FPaths::Combine(FPaths::ProjectLogDir(), TEXT("FUOnlineSession"));
	if (!IFileManager::Get().MakeDirectory(*ReportDirectory, true))
	{
		OutError = TEXT("无法创建 FUOnlineSession 诊断报告目录");
		// 【非递归失败反馈】Emit 只进入历史/日志/浮层/Blueprint，不会再次调用 SaveReport，
		// 因而能向使用者说明保存失败，又不会在磁盘不可写时形成递归写入循环。
		FFU_OnlineDiagnosticEvent FailureEvent;
		FailureEvent.Operation = EFU_OnlineDiagnosticOperation::Environment;
		FailureEvent.Phase = EFU_OnlineDiagnosticPhase::Completed;
		FailureEvent.Severity = EFU_OnlineDiagnosticSeverity::Error;
		FailureEvent.Code = TEXT("FU.Diagnostics.ReportSaveFailed");
		FailureEvent.Status = TEXT("CreateDirectoryFailed");
		FailureEvent.Message = OutError;
		FailureEvent.RecommendedAction = TEXT("检查项目 Saved/Logs 目录是否可写，然后重新保存诊断报告");
		Emit(FailureEvent);
		return false;
	}

	const FString Filename = FString::Printf(
		TEXT("FUOnlineSession-Diagnostics-%s.txt"),
		*FDateTime::UtcNow().ToString(TEXT("yyyyMMdd-HHmmss-fff")));
	const FString SavedPath = FPaths::Combine(ReportDirectory, Filename);
	FString RawWriterFailureDetail;
	if (!ReportWriter(BuildReport(), SavedPath, RawWriterFailureDetail))
	{
		OutError = TEXT("无法写入 FUOnlineSession 诊断报告");
		FFU_OnlineDiagnosticEvent FailureEvent;
		FailureEvent.Operation = EFU_OnlineDiagnosticOperation::Environment;
		FailureEvent.Phase = EFU_OnlineDiagnosticPhase::Completed;
		FailureEvent.Severity = EFU_OnlineDiagnosticSeverity::Error;
		FailureEvent.Code = TEXT("FU.Diagnostics.ReportSaveFailed");
		FailureEvent.Status = TEXT("WriteFailed");
		FailureEvent.Message = OutError;
		FailureEvent.RecommendedAction = TEXT("检查磁盘空间、文件锁定和项目 Saved/Logs 写入权限后重试");
		Emit(FailureEvent);
		return false;
	}

	OutSavedPath = SavedPath;
	return true;
}

FFU_OnlineDiagnosticEvent FFU_OnlineSessionDiagnostics::Sanitize(const FFU_OnlineDiagnosticEvent& CandidateEvent)
{
	FFU_OnlineDiagnosticEvent Result = CandidateEvent;
	if (Result.TimestampUtc.GetTicks() == 0)
	{
		Result.TimestampUtc = FDateTime::UtcNow();
	}
	if (!Result.OperationId.IsValid())
	{
		// 每一个独立事件至少有唯一 ID；Task 7 的异步操作会复用同一个 ID 串起多个阶段。
		Result.OperationId = FGuid::NewGuid();
	}

	Result.Code = FUOnlineSessionDiagnosticsPrivate::RedactInlineSensitiveAssignments(Result.Code, FUOnlineSessionDiagnosticsPrivate::MaxTextLength);
	Result.Status = FUOnlineSessionDiagnosticsPrivate::RedactInlineSensitiveAssignments(Result.Status, FUOnlineSessionDiagnosticsPrivate::MaxTextLength);
	Result.WorldName = FUOnlineSessionDiagnosticsPrivate::RedactInlineSensitiveAssignments(Result.WorldName, FUOnlineSessionDiagnosticsPrivate::MaxTextLength);
	Result.Message = FUOnlineSessionDiagnosticsPrivate::RedactInlineSensitiveAssignments(Result.Message, FUOnlineSessionDiagnosticsPrivate::MaxTextLength);
	Result.Cause = FUOnlineSessionDiagnosticsPrivate::RedactInlineSensitiveAssignments(Result.Cause, FUOnlineSessionDiagnosticsPrivate::MaxTextLength);
	Result.RecommendedAction = FUOnlineSessionDiagnosticsPrivate::RedactInlineSensitiveAssignments(Result.RecommendedAction, FUOnlineSessionDiagnosticsPrivate::MaxTextLength);

	Result.Fields.Reset();
	for (const FFU_OnlineDiagnosticField& CandidateField : CandidateEvent.Fields)
	{
		if (Result.Fields.Num() >= FUOnlineSessionDiagnosticsPrivate::MaxFieldCount)
		{
			break;
		}

		FFU_OnlineDiagnosticField& SanitizedField = Result.Fields.AddDefaulted_GetRef();
		SanitizedField.Key = CandidateField.Key;
		SanitizedField.Value = FUOnlineSessionDiagnosticsPrivate::IsSensitiveFieldKey(CandidateField.Key)
			? TEXT("<redacted>")
			: FUOnlineSessionDiagnosticsPrivate::RedactInlineSensitiveAssignments(
				CandidateField.Value,
				FUOnlineSessionDiagnosticsPrivate::MaxFieldValueLength);
	}

	return Result;
}

TOptional<FFU_OnlineDiagnosticEvent> FFU_OnlineSessionDiagnostics::BuildUnsupportedRecoveryProviderDiagnostic(
	const EFU_OnlineProvider Provider)
{
	if (Provider == EFU_OnlineProvider::Steam || Provider == EFU_OnlineProvider::Lan)
	{
		return TOptional<FFU_OnlineDiagnosticEvent>();
	}

	// 非法枚举可能来自损坏存档、反射调用或未来版本错配；事件故意不回显原始数值、
	// 房间参数或连接上下文，只公开稳定错误码和修复方向，避免诊断路径扩大敏感输入面。
	FFU_OnlineDiagnosticEvent Event;
	Event.Provider = Provider;
	Event.Operation = EFU_OnlineDiagnosticOperation::Recovery;
	Event.Phase = EFU_OnlineDiagnosticPhase::Preflight;
	Event.Severity = EFU_OnlineDiagnosticSeverity::Warning;
	Event.Code = TEXT("FU.Recovery.Rejected.UnsupportedProvider");
	Event.Status = TEXT("Rejected");
	Event.Message = TEXT("TryRecoverProvider 收到不支持的 Provider，未读取或修改任何 Provider 状态");
	Event.RecommendedAction = TEXT("检查 Blueprint 枚举接线或版本兼容性后重试");
	return Event;
}

FFU_OnlineDiagnosticEvent FFU_OnlineSessionDiagnostics::BuildProviderPreflightDiagnostic(
	const EFU_OnlineProvider Provider,
	const EFU_OnlineDiagnosticOperation Operation,
	const FGuid& OperationId,
	const bool bIsReady,
	const FString& Code,
	const FString& Message)
{
	FFU_OnlineDiagnosticEvent Event;
	Event.OperationId = OperationId;
	Event.Provider = Provider;
	Event.Operation = Operation;
	Event.Phase = EFU_OnlineDiagnosticPhase::Preflight;
	Event.Severity = bIsReady ? EFU_OnlineDiagnosticSeverity::Info : EFU_OnlineDiagnosticSeverity::Warning;
	Event.Code = Code;
	Event.Status = bIsReady ? TEXT("Ready") : TEXT("Rejected");
	Event.Message = Message;
	Event.RecommendedAction = bIsReady
		? TEXT("环境已就绪；可继续调用对应的 Create、Find、Join 或 Destroy 蓝图入口")
		: TEXT("根据 StatusCode 修复环境后重新运行 RunProviderDiagnostics");
	return Event;
}

bool FFU_OnlineProviderPreflightGate::Dispatch(
	const EFU_OnlineProvider Provider,
	const EFU_OnlineDiagnosticOperation Operation,
	const FGuid& OperationId,
	const FFU_OnlineProviderStatus& Status,
	TFunctionRef<void(FFU_OnlineDiagnosticEvent)> DiagnosticSink,
	TFunctionRef<void()> FailureContinuation)
{
	// 【顺序契约】无论 Ready 或 Rejected，先交给唯一诊断 sink；拒绝时才允许旧失败续步执行。
	DiagnosticSink(FFU_OnlineSessionDiagnostics::BuildProviderPreflightDiagnostic(
		Provider,
		Operation,
		OperationId,
		Status.bIsReady,
		Status.DiagnosticCode.ToString(),
		Status.Message));
	if (!Status.bIsReady)
	{
		FailureContinuation();
		return false;
	}
	return true;
}

void FFU_OnlineSessionDiagnostics::AttachViewport(UGameViewportClient* InViewport)
{
	if (OverlayViewport.Get() == InViewport && OverlayWidget.IsValid())
	{
		return;
	}

	DetachViewport();
	if (!Config.bEnableOverlay || InViewport == nullptr)
	{
		return;
	}

	OverlayViewport = InViewport;
	OverlayWidget = SNew(SFU_OnlineDiagnosticOverlay).Model(OverlayModel);
	// 【精确所有权】只移除本类此前 Add 的 Widget，不接管或清空项目原有的 Viewport 内容。
	InViewport->AddViewportWidgetContent(OverlayWidget.ToSharedRef(), 10000);
}

void FFU_OnlineSessionDiagnostics::DetachViewport()
{
	if (OverlayViewport.IsValid() && OverlayWidget.IsValid())
	{
		OverlayViewport->RemoveViewportWidgetContent(OverlayWidget.ToSharedRef());
	}

	OverlayWidget.Reset();
	OverlayViewport.Reset();
}

FString FFU_OnlineSessionDiagnostics::FormatEventForOutput(const FFU_OnlineDiagnosticEvent& Event)
{
	FString Output = FString::Printf(
		TEXT("FU Diagnostic [Seq=%lld] [%s] Code=%s Operation=%s/%s Provider=%s Id=%s World=%s PIE=%d Status=%s Message=%s Cause=%s Action=%s"),
		Event.Sequence,
		FUOnlineSessionDiagnosticsPrivate::ToText(Event.Severity),
		*Event.Code,
		FUOnlineSessionDiagnosticsPrivate::ToText(Event.Operation),
		FUOnlineSessionDiagnosticsPrivate::ToText(Event.Phase),
		FUOnlineSessionDiagnosticsPrivate::ToText(Event.Provider),
		*Event.OperationId.ToString(),
		*Event.WorldName,
		Event.PIEInstanceId,
		*Event.Status,
		*Event.Message,
		*Event.Cause,
		*Event.RecommendedAction);

	if (!Event.Fields.IsEmpty())
	{
		Output += FString::Printf(
			TEXT(" Fields={%s}"),
			*FUOnlineSessionDiagnosticsPrivate::FormatFields(Event.Fields));
	}

	return Output;
}

void FFU_OnlineSessionDiagnostics::WriteToLog(const FFU_OnlineDiagnosticEvent& Event)
{
	const FString Message = FormatEventForOutput(Event);
	switch (Event.Severity)
	{
	case EFU_OnlineDiagnosticSeverity::Verbose:
		UE_LOG(LogFUOnlineSession, Verbose, TEXT("%s"), *Message);
		break;
	case EFU_OnlineDiagnosticSeverity::Info:
		UE_LOG(LogFUOnlineSession, Log, TEXT("%s"), *Message);
		break;
	case EFU_OnlineDiagnosticSeverity::Warning:
		UE_LOG(LogFUOnlineSession, Warning, TEXT("%s"), *Message);
		break;
	case EFU_OnlineDiagnosticSeverity::Error:
		UE_LOG(LogFUOnlineSession, Error, TEXT("%s"), *Message);
		break;
	default:
		UE_LOG(LogFUOnlineSession, Warning, TEXT("%s"), *Message);
		break;
	}
}
