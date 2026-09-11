#include "FU_OnlineOperationStateMachine.h"

FFU_OperationTicket FFU_OnlineOperationStateMachine::BeginAttempt(const EFU_OperationKind RootKind)
{
	// 每次公共调用都必须可诊断，即使它随后因 Busy 或 preflight 失败被拒绝；
	// 这里只推进 AttemptSequence，不得写 ActiveGeneration 或任何在途完成状态。
	FFU_OperationTicket Ticket;
	Ticket.AttemptSequence = ++State.AttemptSequence;
	Ticket.OperationId = FGuid::NewGuid();
	Ticket.Kind = RootKind;
	return Ticket;
}

bool FFU_OnlineOperationStateMachine::AcceptAttempt(
	FFU_OperationTicket& Ticket,
	const EFU_OperationKind SubmittedKind)
{
	// Ticket 必须属于本机最新公开尝试且槽位为空；拒绝路径严格只返回 false，
	// 防止错误调用在这里重置上一请求的 generation、timer 对应状态或 exactly-once 标记。
	if (State.Phase != EFU_OperationPhase::Idle
		|| Ticket.bAccepted
		|| Ticket.AttemptSequence == 0
		|| Ticket.AttemptSequence != State.AttemptSequence
		|| !Ticket.OperationId.IsValid())
	{
		return false;
	}

	Ticket.Generation = ++State.GenerationSequence;
	Ticket.bAccepted = true;
	State.ActiveGeneration = Ticket.Generation;
	State.ActiveOperationId = Ticket.OperationId;
	State.ActiveKind = SubmittedKind;
	State.RootKind = Ticket.Kind;
	State.Phase = EFU_OperationPhase::Submitted;
	State.bCompletionBroadcast = false;
	State.bAwaitingOriginalCompletion = true;
	State.bFindCancellationOutstanding = false;
	State.RecoveryDestroyAttempts = 0;
	State.bOriginalCallbackSeen = false;
	return true;
}

bool FFU_OnlineOperationStateMachine::ContinueAcceptedAttempt(const EFU_OperationKind SubmittedKind)
{
	// 只有链式预销毁“成功且尚未向蓝图完成根请求”时可以续步；OperationId/RootKind 原样保留，
	// 新 generation 保证迟到的 Destroy 回调无法命中新 Create/Join delegate。
	if (State.Phase != EFU_OperationPhase::Idle
		|| State.bCompletionBroadcast
		|| !State.ActiveOperationId.IsValid())
	{
		return false;
	}

	State.ActiveGeneration = ++State.GenerationSequence;
	State.ActiveKind = SubmittedKind;
	State.Phase = EFU_OperationPhase::Submitted;
	State.bAwaitingOriginalCompletion = true;
	State.bFindCancellationOutstanding = false;
	State.bOriginalCallbackSeen = false;
	return true;
}

FFU_OperationTicket FFU_OnlineOperationStateMachine::BeginAcceptedAttempt(const EFU_OperationKind Kind)
{
	FFU_OperationTicket Ticket = BeginAttempt(Kind);
	AcceptAttempt(Ticket, Kind);
	return Ticket;
}

FFU_OperationTicket FFU_OnlineOperationStateMachine::RecordRejectedAttempt(const EFU_OperationKind Kind)
{
	// 复用 BeginAttempt 确保每次拒绝也有唯一身份；绝不能通过 BeginAcceptedAttempt 短暂占用槽位。
	return BeginAttempt(Kind);
}

EFU_OperationAction FFU_OnlineOperationStateMachine::HandleSynchronousReject(const uint64 Generation)
{
	if (!IsCurrent(Generation) || State.Phase != EFU_OperationPhase::Submitted)
	{
		return EFU_OperationAction::None;
	}

	State.Phase = EFU_OperationPhase::Idle;
	State.bCompletionBroadcast = true;
	State.bAwaitingOriginalCompletion = false;
	State.bFindCancellationOutstanding = false;
	State.bOriginalCallbackSeen = true;

	EFU_OperationAction Actions = EFU_OperationAction::BroadcastFailure
		| EFU_OperationAction::ClearWatchdog
		| EFU_OperationAction::ClearOriginalDelegate
		| EFU_OperationAction::ClearPendingData;
	if (State.ActiveKind == EFU_OperationKind::Create || State.ActiveKind == EFU_OperationKind::Join)
	{
		Actions |= EFU_OperationAction::RequestLeaseRelease;
	}
	return Actions;
}

EFU_OperationAction FFU_OnlineOperationStateMachine::HandleTimeout(const uint64 Generation)
{
	if (!IsCurrent(Generation))
	{
		return EFU_OperationAction::None;
	}
	if (State.Phase == EFU_OperationPhase::Recovering
		&& State.ActiveKind == EFU_OperationKind::Destroy
		&& State.bAwaitingOriginalCompletion)
	{
		// 补偿 Destroy 的 watchdog 只记录超时并继续等待原 callback；根请求早已失败，不能二次广播。
		return EFU_OperationAction::ClearWatchdog | EFU_OperationAction::KeepOriginalDelegate;
	}
	if (State.bCompletionBroadcast)
	{
		return EFU_OperationAction::None;
	}
	if (State.Phase != EFU_OperationPhase::Submitted)
	{
		return EFU_OperationAction::None;
	}

	// 不可取消的 OSS 操作超时后仍可能成功；状态进入 Recovering 并保留原 delegate，
	// 只把面向蓝图的失败完成一次，绝不因当前 NoSession 猜测后台已经终止。
	State.Phase = EFU_OperationPhase::Recovering;
	State.bCompletionBroadcast = true;
	State.bAwaitingOriginalCompletion = true;
	EFU_OperationAction Actions = EFU_OperationAction::BroadcastFailure
		| EFU_OperationAction::ClearWatchdog
		| EFU_OperationAction::KeepOriginalDelegate;

	if (State.ActiveKind == EFU_OperationKind::Find)
	{
		State.bFindCancellationOutstanding = true;
		Actions |= EFU_OperationAction::StartFindCancellation;
	}
	return Actions;
}

EFU_OperationAction FFU_OnlineOperationStateMachine::HandleOriginalCompletion(
	const uint64 Generation,
	const bool bSucceeded,
	const bool bSessionStillExists)
{
	return HandleOriginalCompletion(Generation, State.ActiveKind, bSucceeded, bSessionStillExists);
}

EFU_OperationAction FFU_OnlineOperationStateMachine::HandleOriginalCompletion(
	const uint64 Generation,
	const EFU_OperationKind SubmittedKind,
	const bool bSucceeded,
	const bool bSessionStillExists)
{
	if (!IsExpectedCallback(Generation, SubmittedKind))
	{
		return EFU_OperationAction::None;
	}

	const EFU_OperationPhase PreviousPhase = State.Phase;
	const EFU_OperationKind CompletedKind = State.ActiveKind;
	const bool bFindCancellationWasOutstanding = State.bFindCancellationOutstanding;
	State.bAwaitingOriginalCompletion = false;
	State.bOriginalCallbackSeen = true;
	State.bFindCancellationOutstanding = false;

	EFU_OperationAction Actions = EFU_OperationAction::ClearWatchdog
		| EFU_OperationAction::ClearOriginalDelegate
		| EFU_OperationAction::ClearPendingData;
	if (PreviousPhase == EFU_OperationPhase::Submitted)
	{
		State.Phase = EFU_OperationPhase::Idle;
		// 成功的预销毁只是根 Create/Join 的中间步骤；其余结果都终结根请求。
		const bool bSuccessfulChainStep = bSucceeded
			&& CompletedKind == EFU_OperationKind::Destroy
			&& State.RootKind != EFU_OperationKind::Destroy;
		if (!bSuccessfulChainStep)
		{
			State.bCompletionBroadcast = true;
		}
		// OSS 成功后的地址/控制器处理也属于 Join 的总结果；若 NamedSession 已经存在，
		// 公开失败之后必须进入补偿 Destroy，不能假装 Idle 后释放仍会被 Session 使用的租约。
		if (!bSucceeded
			&& bSessionStillExists
			&& (CompletedKind == EFU_OperationKind::Create || CompletedKind == EFU_OperationKind::Join))
		{
			State.Phase = EFU_OperationPhase::Recovering;
			State.RecoveryDestroyAttempts = 1;
			return Actions
				| EFU_OperationAction::BroadcastFailure
				| EFU_OperationAction::StartRecoveryDestroy;
		}
		if (!bSucceeded)
		{
			Actions |= EFU_OperationAction::BroadcastFailure;
			if ((CompletedKind == EFU_OperationKind::Create || CompletedKind == EFU_OperationKind::Join)
				|| (CompletedKind == EFU_OperationKind::Destroy && !bSessionStillExists))
			{
				Actions |= EFU_OperationAction::RequestLeaseRelease;
			}
		}
		return Actions;
	}

	// Timeout 已经向蓝图完成失败，迟到回调只执行资源收敛，绝不再次广播或旅行。
	if (CompletedKind == EFU_OperationKind::Find)
	{
		// 原 Find 与 CancelFindSessions 完成是竞争关系；若原 Find 先到，Cancel 的全局委托
		// 仍然注册在接口上，必须由同一 generation 的动作显式清掉，避免随后误收另一轮取消结果。
		if (bFindCancellationWasOutstanding)
		{
			Actions |= EFU_OperationAction::ClearFindCancellationDelegate;
		}
		State.Phase = EFU_OperationPhase::Idle;
		return Actions;
	}

	if (CompletedKind == EFU_OperationKind::Create || CompletedKind == EFU_OperationKind::Join)
	{
		if (bSucceeded || bSessionStillExists)
		{
			// 这里只授权一次自动补偿；真正提交前 Subsystem 还会重新确认接口和 NamedSession。
			if (State.RecoveryDestroyAttempts == 0)
			{
				State.RecoveryDestroyAttempts = 1;
				return Actions | EFU_OperationAction::StartRecoveryDestroy;
			}
			return Actions;
		}

		// timeout 根请求已失败；即使 NoSession，也要保持 Recovering，直到租约释放和全 World 安全被显式确认。
		return Actions | EFU_OperationAction::RequestLeaseRelease;
	}

	// Destroy 成功（或 Provider 已明确 NoSession）可以结束恢复；失败且 Session 仍存在时保持 Recovering，
	// 以后只能由显式 TryRecoverProvider 再提交一次 Destroy，绝不自动循环。
	if (bSucceeded || !bSessionStillExists)
	{
		// 补偿 Destroy 只证明会话层终止；TryRecoverProvider 仍需确认驱动与 exact lease release 后才能 Idle。
		return Actions | EFU_OperationAction::RequestLeaseRelease;
	}

	return Actions;
}

bool FFU_OnlineOperationStateMachine::CanFinishRecovery(
	const bool bOriginalCallbackSeen,
	const bool bNoSession) const
{
	// NoSession 只是当前本地快照；只有原 OSS 回调已终止才证明不可取消操作不会在下一帧创建 Session。
	return State.Phase == EFU_OperationPhase::Recovering
		&& bOriginalCallbackSeen
		&& State.bOriginalCallbackSeen
		&& !State.bAwaitingOriginalCompletion
		&& bNoSession;
}

bool FFU_OnlineOperationStateMachine::CanStartExplicitRecovery(
	const bool bOriginalCallbackSeen,
	const bool bNoSession,
	const bool bNoLiveOrPendingDriver,
	const bool bLeaseReleased) const
{
	return CanFinishRecovery(bOriginalCallbackSeen, bNoSession)
		&& bNoLiveOrPendingDriver
		&& bLeaseReleased;
}

bool FFU_OnlineOperationStateMachine::CanReleaseLeaseAfterConnectionFailure(
	const FFU_ConnectionFailureLeaseReleaseSnapshot& Snapshot)
{
	// Network/TravelFailure 只能说明一次连接或旅行失败，不能证明 OnlineSubsystem 已删除 NamedSession。
	// 四项证据采用全 AND：任何接口未知、会话仍在、回调仍在途或全局 World 尚可能创建驱动，
	// 都保留租约，交由显式 Destroy/受控恢复路径在取得可靠终态后处理。
	return Snapshot.bSessionInterfaceValid
		&& Snapshot.bNamedSessionAbsent
		&& Snapshot.bNoOperationInFlight
		&& Snapshot.bAllWorldsClear;
}

bool FFU_OnlineOperationStateMachine::BeginRecoveryDestroyAttempt(
	uint64& OutGeneration,
	const bool bExplicitRetry)
{
	OutGeneration = 0;
	if (State.Phase != EFU_OperationPhase::Recovering
		|| State.bAwaitingOriginalCompletion
		|| (!bExplicitRetry && State.RecoveryDestroyAttempts != 1))
	{
		return false;
	}

	if (bExplicitRetry)
	{
		if (State.RecoveryDestroyAttempts == MAX_uint8)
		{
			return false;
		}
		++State.RecoveryDestroyAttempts;
	}

	State.ActiveGeneration = ++State.GenerationSequence;
	State.ActiveKind = EFU_OperationKind::Destroy;
	State.bAwaitingOriginalCompletion = true;
	State.bOriginalCallbackSeen = false;
	OutGeneration = State.ActiveGeneration;
	return true;
}

bool FFU_OnlineOperationStateMachine::CanRetryRecoveryDestroy(
	const bool bOriginalCallbackSeen,
	const bool bSessionStillExists) const
{
	return State.Phase == EFU_OperationPhase::Recovering
		&& bOriginalCallbackSeen
		&& State.bOriginalCallbackSeen
		&& !State.bAwaitingOriginalCompletion
		&& bSessionStillExists;
}

EFU_OperationAction FFU_OnlineOperationStateMachine::HandleFindCancellationCompletion(
	const uint64 Generation,
	const bool bSucceeded)
{
	if (!IsCurrent(Generation)
		|| State.Phase != EFU_OperationPhase::Recovering
		|| State.ActiveKind != EFU_OperationKind::Find
		|| !State.bFindCancellationOutstanding)
	{
		return EFU_OperationAction::None;
	}

	State.bFindCancellationOutstanding = false;
	if (!bSucceeded)
	{
		// CancelFindSessions 失败不代表原 Find 已终止；继续保留原 delegate 等待真实完成。
		return EFU_OperationAction::ClearFindCancellationDelegate
			| EFU_OperationAction::KeepOriginalDelegate;
	}

	State.bAwaitingOriginalCompletion = false;
	State.bOriginalCallbackSeen = true;
	State.Phase = EFU_OperationPhase::Idle;
	return EFU_OperationAction::ClearWatchdog
		| EFU_OperationAction::ClearOriginalDelegate
		| EFU_OperationAction::ClearFindCancellationDelegate
		| EFU_OperationAction::ClearPendingData;
}

bool FFU_OnlineOperationStateMachine::FinishExplicitRecovery()
{
	if (State.Phase != EFU_OperationPhase::Recovering || State.bAwaitingOriginalCompletion)
	{
		return false;
	}

	State.Phase = EFU_OperationPhase::Idle;
	return true;
}

bool FFU_OnlineOperationStateMachine::IsExpectedCallback(
	const uint64 Generation,
	const EFU_OperationKind SubmittedKind) const
{
	// kind 与 generation 必须同时匹配；链式预销毁随后会换 generation，任何旧 Destroy 回调均不能清新 delegate。
	return IsCurrent(Generation)
		&& State.ActiveKind == SubmittedKind
		&& State.Phase != EFU_OperationPhase::Idle
		&& State.bAwaitingOriginalCompletion;
}

const FFU_OperationState& FFU_OnlineOperationStateMachine::Get() const
{
	return State;
}

bool FFU_OnlineOperationStateMachine::IsCurrent(const uint64 Generation) const
{
	return Generation != 0 && Generation == State.ActiveGeneration;
}
