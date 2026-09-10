#pragma once

#include "CoreMinimal.h"
#include "FU_OnlineDiagnosticTypes.h"

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
	explicit FFU_OnlineSessionDiagnostics(
		const FFU_OnlineDiagnosticDispatchConfig& InConfig,
		TFunction<void(const FFU_OnlineDiagnosticEvent&)> InBlueprintBroadcast);

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
	TSharedRef<FFU_OnlineDiagnosticOverlayModel> OverlayModel;
	TWeakObjectPtr<UGameViewportClient> OverlayViewport;
	TSharedPtr<SFU_OnlineDiagnosticOverlay> OverlayWidget;
};
