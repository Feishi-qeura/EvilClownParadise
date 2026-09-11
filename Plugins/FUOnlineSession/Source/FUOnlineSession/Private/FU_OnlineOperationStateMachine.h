#pragma once

#include "CoreMinimal.h"

/** 当前根请求或实际提交给 OnlineSubsystem 的操作种类。 */
enum class EFU_OperationKind : uint8
{
	Create,
	Find,
	Join,
	Destroy
};

/** 一个 Provider 的异步槽位只允许处于以下三个互斥阶段之一。 */
enum class EFU_OperationPhase : uint8
{
	Idle,
	Submitted,
	Recovering
};

/**
 * 状态机把副作用表达成位标志，由 Subsystem 在 UObject/OSS 边界执行。
 * 纯状态机绝不访问 World、timer、delegate 或 NetDriver，因此自动化测试可以覆盖所有危险竞态。
 */
enum class EFU_OperationAction : uint16
{
	None = 0,
	BroadcastFailure = 1 << 0,
	ClearWatchdog = 1 << 1,
	ClearOriginalDelegate = 1 << 2,
	KeepOriginalDelegate = 1 << 3,
	StartFindCancellation = 1 << 4,
	ClearFindCancellationDelegate = 1 << 5,
	StartRecoveryDestroy = 1 << 6,
	RequestLeaseRelease = 1 << 7,
	ClearPendingData = 1 << 8
};
ENUM_CLASS_FLAGS(EFU_OperationAction);

/** 每次公共入口都分配一张票；只有 preflight 通过后才填写 Generation 并标记 Accepted。 */
struct FFU_OperationTicket
{
	uint64 AttemptSequence = 0;
	uint64 Generation = 0;
	FGuid OperationId;
	EFU_OperationKind Kind = EFU_OperationKind::Create;
	bool bAccepted = false;
};

/**
 * 单 Provider 的纯操作快照。前九个字段保持 Task 6 约定顺序；追加字段记录链式根操作和回调证据。
 */
struct FFU_OperationState
{
	uint64 AttemptSequence = 0;
	uint64 ActiveGeneration = 0;
	FGuid ActiveOperationId;
	EFU_OperationKind ActiveKind = EFU_OperationKind::Create;
	EFU_OperationPhase Phase = EFU_OperationPhase::Idle;
	bool bCompletionBroadcast = false;
	bool bAwaitingOriginalCompletion = false;
	bool bFindCancellationOutstanding = false;
	uint8 RecoveryDestroyAttempts = 0;

	// Create/Join 可能先提交 Destroy；RootKind 决定最终广播类型，ActiveKind 只描述当前 OSS delegate。
	EFU_OperationKind RootKind = EFU_OperationKind::Create;
	bool bOriginalCallbackSeen = false;
	uint64 GenerationSequence = 0;
};

/**
 * Network/TravelFailure 发生时请求恢复全局 NetDriver 租约所需的四项只读证据。
 * 该值对象不持有 UObject/OSS 指针，便于用纯测试锁定“未知即不释放”的保守规则。
 */
struct FFU_ConnectionFailureLeaseReleaseSnapshot
{
	bool bSessionInterfaceValid = false;
	bool bNamedSessionAbsent = false;
	bool bNoOperationInFlight = false;
	bool bAllWorldsClear = false;
};

/**
 * OnlineSubsystem 操作的无 UObject 状态机。
 * 所有 generation 检查都先于状态写入，迟到或重复回调只能得到 None，绝不会污染新请求。
 */
class FFU_OnlineOperationStateMachine final
{
public:
	/** 在任何 preflight 之前分配尝试序号和 OperationId，但不占用异步槽位。 */
	FFU_OperationTicket BeginAttempt(EFU_OperationKind RootKind);

	/** preflight 全部通过、即将绑定 delegate 时，才为票据分配活动 generation。 */
	bool AcceptAttempt(FFU_OperationTicket& Ticket, EFU_OperationKind SubmittedKind);

	/** 链式预销毁成功后，保持根 OperationId 并为 Create/Join 续步分配新 generation。 */
	bool ContinueAcceptedAttempt(EFU_OperationKind SubmittedKind);

	/** 兼容直接提交的纯测试/内部调用；等价于 BeginAttempt 后立即 AcceptAttempt。 */
	FFU_OperationTicket BeginAcceptedAttempt(EFU_OperationKind Kind);
	FFU_OperationTicket RecordRejectedAttempt(EFU_OperationKind Kind);
	EFU_OperationAction HandleSynchronousReject(uint64 Generation);
	EFU_OperationAction HandleTimeout(uint64 Generation);
	EFU_OperationAction HandleOriginalCompletion(uint64 Generation, bool bSucceeded, bool bSessionStillExists);
	EFU_OperationAction HandleOriginalCompletion(uint64 Generation, EFU_OperationKind SubmittedKind, bool bSucceeded, bool bSessionStillExists);
	bool IsExpectedCallback(uint64 Generation, EFU_OperationKind SubmittedKind) const;
	bool CanFinishRecovery(bool bOriginalCallbackSeen, bool bNoSession) const;
	bool CanStartExplicitRecovery(bool bOriginalCallbackSeen, bool bNoSession, bool bNoLiveOrPendingDriver, bool bLeaseReleased) const;

	/** 只有接口、NamedSession、操作终态和全 World 网络状态四项证据同时安全时才允许失败路径释放。 */
	static bool CanReleaseLeaseAfterConnectionFailure(
		const FFU_ConnectionFailureLeaseReleaseSnapshot& Snapshot);

	/** 自动补偿或一次显式重试真正绑定 Destroy 前，为其分配独立 generation。 */
	bool BeginRecoveryDestroyAttempt(uint64& OutGeneration, bool bExplicitRetry = false);

	/** 只有上一条恢复 Destroy 的原回调已经终止且 Session 仍存在，显式入口才可再提交一次。 */
	bool CanRetryRecoveryDestroy(bool bOriginalCallbackSeen, bool bSessionStillExists) const;

	/** Find 的 Cancel 回调成功时可与原 Find 回调二选一结束等待；失败仍等待原回调。 */
	EFU_OperationAction HandleFindCancellationCompletion(uint64 Generation, bool bSucceeded);

	/** 已证明安全恢复完成后清空逻辑槽位；只允许 Recovering -> Idle。 */
	bool FinishExplicitRecovery();

	const FFU_OperationState& Get() const;

private:
	bool IsCurrent(uint64 Generation) const;
	FFU_OperationState State;
};
