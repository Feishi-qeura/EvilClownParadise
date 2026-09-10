#pragma once

#include "CoreMinimal.h"
#include "FU_OnlineSessionTypes.h"
#include "FU_OnlineDiagnosticTypes.generated.h"

/**
 * 诊断事件的严重级别。
 *
 * Verbose 仅用于需要逐帧或逐步骤追踪的开发场景；运行时默认浮层从 Warning 开始，
 * 防止正常联机流程遮挡玩家画面。枚举值独立于旧 ini byte，旧值会在 Subsystem 中显式映射。
 */
UENUM(BlueprintType)
enum class EFU_OnlineDiagnosticSeverity : uint8
{
	Verbose UMETA(DisplayName = "Verbose"),
	Info UMETA(DisplayName = "Info"),
	Warning UMETA(DisplayName = "Warning"),
	Error UMETA(DisplayName = "Error")
};

/** 每条诊断所属的联机操作；便于 Blueprint 和导出报告按操作筛选问题。 */
UENUM(BlueprintType)
enum class EFU_OnlineDiagnosticOperation : uint8
{
	Environment UMETA(DisplayName = "Environment"),
	CreateSession UMETA(DisplayName = "Create Session"),
	FindSessions UMETA(DisplayName = "Find Sessions"),
	JoinSession UMETA(DisplayName = "Join Session"),
	DestroySession UMETA(DisplayName = "Destroy Session"),
	NetDriverLease UMETA(DisplayName = "NetDriver Lease"),
	AppIdBootstrap UMETA(DisplayName = "AppID Bootstrap"),
	LegacyConfigMigration UMETA(DisplayName = "Legacy Config Migration"),
	Recovery UMETA(DisplayName = "Recovery")
};

/** 联机操作从请求到恢复的阶段。阶段与 OperationId 组合后可还原异步调用链。 */
UENUM(BlueprintType)
enum class EFU_OnlineDiagnosticPhase : uint8
{
	Requested UMETA(DisplayName = "Requested"),
	Preflight UMETA(DisplayName = "Preflight"),
	Submitted UMETA(DisplayName = "Submitted"),
	Callback UMETA(DisplayName = "Callback"),
	Timeout UMETA(DisplayName = "Timeout"),
	Recovery UMETA(DisplayName = "Recovery"),
	Completed UMETA(DisplayName = "Completed")
};

/**
 * 一项可扩展的、Blueprint 可读的诊断上下文字段。
 *
 * 所有值都会在分发器入口统一脱敏；调用方仍应只放入定位问题必需的非敏感数据。
 */
USTRUCT(BlueprintType)
struct FUONLINESESSION_API FFU_OnlineDiagnosticField
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "FUOnlineSession|Diagnostics")
	FName Key = NAME_None;

	UPROPERTY(BlueprintReadOnly, Category = "FUOnlineSession|Diagnostics")
	FString Value;
};

/**
 * 统一的联机诊断事件。
 *
 * 该结构故意不携带密码、授权票据或连接 Token；即使错误调用方把敏感字段传入，
 * FFU_OnlineSessionDiagnostics 也会在写入任一输出通道之前替换为 <redacted>。
 */
USTRUCT(BlueprintType)
struct FUONLINESESSION_API FFU_OnlineDiagnosticEvent
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "FUOnlineSession|Diagnostics")
	FDateTime TimestampUtc;

	/** 同一 GameInstance 内严格递增的事件序号，便于日志时间相同或跨线程转发时恢复原始顺序。 */
	UPROPERTY(BlueprintReadOnly, Category = "FUOnlineSession|Diagnostics")
	int64 Sequence = 0;

	UPROPERTY(BlueprintReadOnly, Category = "FUOnlineSession|Diagnostics")
	FGuid OperationId;

	UPROPERTY(BlueprintReadOnly, Category = "FUOnlineSession|Diagnostics")
	EFU_OnlineProvider Provider = EFU_OnlineProvider::Lan;

	UPROPERTY(BlueprintReadOnly, Category = "FUOnlineSession|Diagnostics")
	EFU_OnlineDiagnosticOperation Operation = EFU_OnlineDiagnosticOperation::Environment;

	UPROPERTY(BlueprintReadOnly, Category = "FUOnlineSession|Diagnostics")
	EFU_OnlineDiagnosticPhase Phase = EFU_OnlineDiagnosticPhase::Requested;

	UPROPERTY(BlueprintReadOnly, Category = "FUOnlineSession|Diagnostics")
	EFU_OnlineDiagnosticSeverity Severity = EFU_OnlineDiagnosticSeverity::Info;

	UPROPERTY(BlueprintReadOnly, Category = "FUOnlineSession|Diagnostics")
	FString Code;

	UPROPERTY(BlueprintReadOnly, Category = "FUOnlineSession|Diagnostics")
	FString Status;

	UPROPERTY(BlueprintReadOnly, Category = "FUOnlineSession|Diagnostics")
	FString WorldName;

	/** 没有 PIE 上下文时保持 INDEX_NONE，便于独立打包程序与 PIE 共用同一接口。 */
	UPROPERTY(BlueprintReadOnly, Category = "FUOnlineSession|Diagnostics")
	int32 PIEInstanceId = INDEX_NONE;

	UPROPERTY(BlueprintReadOnly, Category = "FUOnlineSession|Diagnostics")
	FString Message;

	UPROPERTY(BlueprintReadOnly, Category = "FUOnlineSession|Diagnostics")
	FString Cause;

	UPROPERTY(BlueprintReadOnly, Category = "FUOnlineSession|Diagnostics")
	FString RecommendedAction;

	UPROPERTY(BlueprintReadOnly, Category = "FUOnlineSession|Diagnostics")
	TArray<FFU_OnlineDiagnosticField> Fields;
};

/** 每一条经过统一脱敏的诊断事件都会广播给 Blueprint；输出开关不会关闭该通知。 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(
	FFU_OnOnlineDiagnosticEvent,
	const FFU_OnlineDiagnosticEvent&, Event);
