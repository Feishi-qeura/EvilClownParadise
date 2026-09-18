#include "FU_OnlineSessionSubsystem.h"
#include "FU_SessionPingQuery.h"
//模板通过ProviderTraits取得Steam/NULL的固定特化版本
#include "ProviderTraits/FU_OnlineSessionProviderTraits.h"
#include "FU_OnlineSessionMetadata.h"
#include "FU_OnlineProviderStatusEvaluator.h"
#include "FU_OnlineOperationStateMachine.h"
#include "FU_OnlineSessionRequestValidation.h"
#include "FU_OnlineSessionSettings.h"
#include "FU_SteamSocketsReadiness.h"
#include "FUOnlineSessionModule.h"
#include "Diagnostics/FU_OnlineSessionDiagnostics.h"
#include "NetDriver/FU_OnlineSessionNetDriverLease.h"
#include "Engine/Engine.h"
#include "Engine/LocalPlayer.h"
#include "Engine/NetDriver.h"
#include "Engine/World.h"
#include "Engine/GameInstance.h"
#include "GameFramework/PlayerController.h"
#include "Online/OnlineSessionNames.h"
#include "Interfaces/OnlineIdentityInterface.h"
#include "OnlineSessionSettings.h"
#include "OnlineSubsystem.h"
#include "OnlineSubsystemNames.h"
#include "OnlineSubsystemUtils.h"
#include "Modules/ModuleManager.h"
#include "String/LexFromString.h"
#include "UObject/SoftObjectPath.h"

//https://dev.epicgames.com/documentation/unreal-engine/online-subsystem-steam-interface-in-unreal-engine?lang=zh-CN
//https://partner.steamgames.com/doc/api/ISteamMatchmaking#LobbyCreated_t
//https://partner.steamgames.com/
//https://dev.epicgames.com/documentation/unreal-engine/online-subsystem-session-interface-in-unreal-engine
namespace FUOnlineSession
{
	/**
	 * 已发布配置使用 byte：0=Info、1=Warning、2=Error；公开枚举还包含 Verbose=0。
	 * 这里必须显式转换，不能让旧 default=1 错映射为 Info。
	 */
	EFU_OnlineDiagnosticSeverity GetOverlayMinimumSeverity(const uint8 PersistedValue)
	{
		switch (PersistedValue)
		{
		case 0: return EFU_OnlineDiagnosticSeverity::Info;
		case 1: return EFU_OnlineDiagnosticSeverity::Warning;
		default: return EFU_OnlineDiagnosticSeverity::Error;
		}
	}

	/**
	 * 将机器可读状态转成适合调试和蓝图提示的文字。
	 * 蓝图仍应优先根据 StatusCode 决定 UI；Message 只是默认说明，方便插件开箱使用。
	 */
	FString GetProviderStatusMessage(
		const EFU_OnlineProvider Provider,
		const EFU_OnlineProviderStatusCode StatusCode)
	{
		const TCHAR* ProviderName =
			Provider == EFU_OnlineProvider::Steam
				? TEXT("Steam")
				: TEXT("LAN/NULL");

		switch (StatusCode)
		{
		case EFU_OnlineProviderStatusCode::Ready:
			return FString::Printf(TEXT("%s 在线提供方已经可以使用"), ProviderName);

		case EFU_OnlineProviderStatusCode::WorldUnavailable:
			return TEXT("当前 GameInstance 尚未取得有效 World，请在游戏实例初始化完成后再检查");

		case EFU_OnlineProviderStatusCode::SubsystemUnavailable:
			return FString::Printf(
				TEXT("%s OnlineSubsystem 未加载，请检查插件依赖、插件 Engine.ini 与启动日志"),
				ProviderName);

		case EFU_OnlineProviderStatusCode::SessionInterfaceUnavailable:
			return FString::Printf(TEXT("%s OnlineSubsystem 没有提供 Session Interface"), ProviderName);

		case EFU_OnlineProviderStatusCode::IdentityInterfaceUnavailable:
			return TEXT("Steam Identity Interface 不可用，请确认 Steam 客户端和 OnlineSubsystemSteam 已启动");

		case EFU_OnlineProviderStatusCode::NotLoggedIn:
			return TEXT("本地用户尚未登录 Steam，请先启动并登录 Steam 客户端");

		case EFU_OnlineProviderStatusCode::NetDriverDefinitionUnavailable:
			return TEXT("引擎没有 GameNetDriver 定义，FU Online Session 无法准备地图连接驱动");

		case EFU_OnlineProviderStatusCode::NetDriverClassUnavailable:
			return FString::Printf(
				TEXT("%s 所需的 NetDriver 类不可用，请检查 FUOnlineSession 的传输插件依赖"),
				ProviderName);

		case EFU_OnlineProviderStatusCode::ActiveNetDriverConflict:
			return FString::Printf(
				TEXT("当前 World 正在使用另一种 NetDriver；请先退出联网关卡，再切换到 %s"),
				ProviderName);

		case EFU_OnlineProviderStatusCode::NetDriverLeaseUnavailable:
			return FString::Printf(
				TEXT("%s 无法取得进程级 GameNetDriver 租约；请等待其他 PIE/旅行结束，若检测到外部修改则重启进程"),
				ProviderName);

		case EFU_OnlineProviderStatusCode::SteamAppIdBootstrapInvalid:
			return TEXT("Steam 开发 AppID 运行时注入未通过验证；请重启并检查 FU Online Session 插件设置");

		case EFU_OnlineProviderStatusCode::SteamSocketsModuleUnavailable:
			return TEXT("SteamSockets 模块不可用；请确认 SteamSockets 插件已启用且 Win64 二进制可用");

		case EFU_OnlineProviderStatusCode::SteamSocketsDisabled:
			return TEXT("SteamSockets 已加载但当前禁用；请先确认 Steam OnlineSubsystem 已正常运行");

		case EFU_OnlineProviderStatusCode::SteamSocketsSocketSubsystemUnavailable:
			return TEXT("SteamSockets SocketSubsystem 尚未注册；请检查 SteamSockets 初始化日志");

		case EFU_OnlineProviderStatusCode::ShippingSteamAppIdMissing:
			return TEXT("Shipping 尚未配置 ExpectedShippingSteamAppId；插件不会回退到开发 AppID");

		case EFU_OnlineProviderStatusCode::SteamAppIdMismatch:
			return TEXT("当前 Steam AppID 与 FU Online Session 配置期望不一致；请检查打包与 Steam 环境");

		default:
			return TEXT("未知的在线提供方状态");
		}
	}

	/**
	 * 预检代码必须由公开 StatusCode 派生，不能用另一份 Provider/环境真相表拼接字符串。
	 * 这些稳定代码同时供 Blueprint、诊断报告与自动化断言使用，Message 仍交给统一 sanitizer 处理。
	 */
	const TCHAR* GetProviderStatusDiagnosticCode(const EFU_OnlineProviderStatusCode StatusCode)
	{
		switch (StatusCode)
		{
		case EFU_OnlineProviderStatusCode::Ready: return TEXT("FU.Provider.Ready");
		case EFU_OnlineProviderStatusCode::WorldUnavailable: return TEXT("FU.Provider.WorldUnavailable");
		case EFU_OnlineProviderStatusCode::SubsystemUnavailable: return TEXT("FU.Provider.SubsystemUnavailable");
		case EFU_OnlineProviderStatusCode::SessionInterfaceUnavailable: return TEXT("FU.Provider.SessionInterfaceUnavailable");
		case EFU_OnlineProviderStatusCode::IdentityInterfaceUnavailable: return TEXT("FU.Provider.IdentityInterfaceUnavailable");
		case EFU_OnlineProviderStatusCode::NotLoggedIn: return TEXT("FU.Provider.NotLoggedIn");
		case EFU_OnlineProviderStatusCode::NetDriverDefinitionUnavailable: return TEXT("FU.Provider.NetDriverDefinitionUnavailable");
		case EFU_OnlineProviderStatusCode::NetDriverClassUnavailable: return TEXT("FU.Provider.NetDriverClassUnavailable");
		case EFU_OnlineProviderStatusCode::ActiveNetDriverConflict: return TEXT("FU.Provider.ActiveNetDriverConflict");
		case EFU_OnlineProviderStatusCode::NetDriverLeaseUnavailable: return TEXT("FU.Provider.NetDriverLeaseUnavailable");
		case EFU_OnlineProviderStatusCode::SteamAppIdBootstrapInvalid: return TEXT("FU.Provider.SteamAppIdBootstrapInvalid");
		case EFU_OnlineProviderStatusCode::SteamSocketsModuleUnavailable: return TEXT("FU.Provider.SteamSocketsModuleUnavailable");
		case EFU_OnlineProviderStatusCode::SteamSocketsDisabled: return TEXT("FU.Provider.SteamSocketsDisabled");
		case EFU_OnlineProviderStatusCode::SteamSocketsSocketSubsystemUnavailable: return TEXT("FU.Provider.SteamSocketsSocketSubsystemUnavailable");
		case EFU_OnlineProviderStatusCode::ShippingSteamAppIdMissing: return TEXT("FU.Provider.ShippingSteamAppIdMissing");
		case EFU_OnlineProviderStatusCode::SteamAppIdMismatch: return TEXT("FU.Provider.SteamAppIdMismatch");
		default: return TEXT("FU.Provider.Unknown");
		}
	}
}

/**
 * 【FU 修复：集中查找 GameNetDriver】
 * Host 的 OpenLevel(?listen) 与 Client 的 ClientTravel 都按 DefName 查找驱动，
 * 因此模板只需要更新这一条定义，不需要复制两套 Session 实现。
 */
static FNetDriverDefinition* FU_FindGameNetDriverDefinition()
{
	if (!GEngine)
	{
		return nullptr;
	}

	return GEngine->NetDriverDefinitions.FindByPredicate(
		[](const FNetDriverDefinition& Definition)
		{
			return Definition.DefName == NAME_GameNetDriver;
		});
}

//模板定义
//模板被实例化进行编译检查，修改Provider是lobby还是NULL
template<EFU_OnlineProvider Provider>
UFU_OnlineSessionSubsystem::FFU_OnlineProviderState& UFU_OnlineSessionSubsystem::FU_GetProviderState()
{
	static_assert(Provider == EFU_OnlineProvider::Steam || Provider == EFU_OnlineProvider::Lan, "不受支持的FU在线提供商");

	//Provider是编译期常量，因此未选中的分支不会进入最终生成的函数
	if constexpr (Provider == EFU_OnlineProvider::Steam) {return SteamState;}
	else {return LanState;}
}

//const重载为只读成员函数提供状态访问，返回const引用以禁止调用者修改Provider数据
template<EFU_OnlineProvider Provider>
const UFU_OnlineSessionSubsystem::FFU_OnlineProviderState& UFU_OnlineSessionSubsystem::FU_GetProviderState() const
{
	//与上述书签一样
	static_assert(Provider == EFU_OnlineProvider::Steam || Provider == EFU_OnlineProvider::Lan, "不受支持的FU在线提供商");

	if constexpr (Provider == EFU_OnlineProvider::Steam) {return SteamState;}
	else {return LanState;}
}

template<EFU_OnlineProvider Provider>
FFU_OnlineOperationStateMachine& UFU_OnlineSessionSubsystem::FU_GetOperationMachine()
{
	FFU_OnlineProviderState& State = FU_GetProviderState<Provider>();
	if (!State.OperationMachine.IsValid())
	{
		// Private 纯状态机按 Provider 独立创建；Steam 与 LAN 的 Busy/generation 绝不共享。
		State.OperationMachine = TUniquePtr<FFU_OnlineOperationStateMachine, FFU_OnlineOperationStateMachineDeleter>(
			new FFU_OnlineOperationStateMachine());
	}
	return *State.OperationMachine;
}

template<EFU_OnlineProvider Provider>
FFU_OperationTicket UFU_OnlineSessionSubsystem::FU_BeginOperationAttempt(const EFU_OperationKind RootKind)
{
	FFU_OperationTicket Ticket = FU_GetOperationMachine<Provider>().BeginAttempt(RootKind);
	FU_EmitDiagnostic<Provider>(
		RootKind,
		Ticket.OperationId,
		EFU_OnlineDiagnosticPhase::Requested,
		EFU_OnlineDiagnosticSeverity::Info,
		TEXT("FU.Operation.Requested"),
		FString::Printf(TEXT("已分配公开尝试 AttemptSequence=%llu"), Ticket.AttemptSequence));
	return Ticket;
}

template<EFU_OnlineProvider Provider>
bool UFU_OnlineSessionSubsystem::FU_ReserveOperation(
	FFU_OperationTicket& Ticket,
	const EFU_OperationKind SubmittedKind,
	uint64& OutGeneration)
{
	FFU_OnlineOperationStateMachine& Machine = FU_GetOperationMachine<Provider>();
	const bool bAccepted = Ticket.bAccepted
		? Machine.ContinueAcceptedAttempt(SubmittedKind)
		: Machine.AcceptAttempt(Ticket, SubmittedKind);
	if (!bAccepted)
	{
		OutGeneration = 0;
		return false;
	}

	Ticket.Generation = Machine.Get().ActiveGeneration;
	Ticket.bAccepted = true;
	OutGeneration = Ticket.Generation;
	return true;
}

template<EFU_OnlineProvider Provider>
void UFU_OnlineSessionSubsystem::FU_EmitOperationSubmitted(
	const EFU_OperationKind SubmittedKind,
	const uint64 Generation)
{
	const FFU_OnlineOperationStateMachine& Machine = FU_GetOperationMachine<Provider>();
	// 【可观测语义】只有 delegate 已绑定、watchdog 已安装并且即将调用 OSS 时才发 Submitted。
	// Reserve 与本事件刻意拆开：NetDriver Acquire 的诊断仍发生在 Busy 槽位内，但不会谎称 OSS 已绑定。
	FU_EmitDiagnostic<Provider>(
		SubmittedKind,
		Machine.Get().ActiveOperationId,
		EFU_OnlineDiagnosticPhase::Submitted,
		EFU_OnlineDiagnosticSeverity::Info,
		TEXT("FU.Operation.Submitted"),
		FString::Printf(TEXT("已绑定 delegate/watchdog，准备调用 OSS Generation=%llu"), Generation));
}

template<EFU_OnlineProvider Provider>
void UFU_OnlineSessionSubsystem::FU_ArmOperationWatchdog(const uint64 Generation)
{
	FFU_OnlineProviderState& State = FU_GetProviderState<Provider>();
	UGameInstance* const GameInstance = GetGameInstance();
	if (!GameInstance)
	{
		return;
	}

	// timer 必须在 OSS submit 之前 armed；若 submit 同步回调，generation gate 会先完成并清掉该 timer。
	const float TimeoutSeconds = UFU_OnlineSessionSettings::GetRuntimeSettings().OperationTimeoutSeconds;
	FTimerDelegate TimerDelegate = FTimerDelegate::CreateUObject(
		this,
		&ThisClass::FU_OnOperationTimeout<Provider>,
		Generation);
	GameInstance->GetTimerManager().SetTimer(
		State.OperationWatchdogHandle,
		TimerDelegate,
		TimeoutSeconds,
		false);
}

template<EFU_OnlineProvider Provider>
void UFU_OnlineSessionSubsystem::FU_ClearOperationWatchdog()
{
	FFU_OnlineProviderState& State = FU_GetProviderState<Provider>();
	if (UGameInstance* const GameInstance = GetGameInstance())
	{
		GameInstance->GetTimerManager().ClearTimer(State.OperationWatchdogHandle);
	}
	State.OperationWatchdogHandle.Invalidate();
}

template<EFU_OnlineProvider Provider>
void UFU_OnlineSessionSubsystem::FU_ClearOperationDelegate(const EFU_OperationKind SubmittedKind)
{
	// 调用者已经通过 generation+kind+phase；这里只清本次真正注册的精确 Handle。
	switch (SubmittedKind)
	{
	case EFU_OperationKind::Create: FU_ClearCreateDelegate<Provider>(); break;
	case EFU_OperationKind::Find: FU_ClearFindDelegate<Provider>(); break;
	case EFU_OperationKind::Join: FU_ClearJoinDelegate<Provider>(); break;
	case EFU_OperationKind::Destroy: FU_ClearDestroyDelegate<Provider>(); break;
	default: break;
	}
}

template<EFU_OnlineProvider Provider>
void UFU_OnlineSessionSubsystem::FU_ClearFindCancellationDelegate()
{
	FFU_OnlineProviderState& State = FU_GetProviderState<Provider>();
	if (State.SessionInterface.IsValid() && State.CancelFindDelegateHandle.IsValid())
	{
		State.SessionInterface->ClearOnCancelFindSessionsCompleteDelegate_Handle(State.CancelFindDelegateHandle);
	}
	State.CancelFindDelegateHandle.Reset();
}

template<EFU_OnlineProvider Provider>
UFU_OnlineSessionSubsystem::FFU_OnlineCallbackResourceSnapshot
UFU_OnlineSessionSubsystem::FU_CaptureCallbackResources() const
{
	const FFU_OnlineProviderState& State = FU_GetProviderState<Provider>();
	FFU_OnlineCallbackResourceSnapshot Snapshot;
	Snapshot.SessionInterface = State.SessionInterface;
	Snapshot.SessionSearch = State.SessionSearch;
	Snapshot.CreateDelegateHandle = State.CreateDelegateHandle;
	Snapshot.FindDelegateHandle = State.FindDelegateHandle;
	Snapshot.JoinDelegateHandle = State.JoinDelegateHandle;
	Snapshot.DestroyDelegateHandle = State.DestroyDelegateHandle;
	Snapshot.CancelFindDelegateHandle = State.CancelFindDelegateHandle;
	Snapshot.OperationWatchdogHandle = State.OperationWatchdogHandle;
	return Snapshot;
}

void UFU_OnlineSessionSubsystem::FU_ClearCapturedCallbackResources(
	const FFU_OnlineCallbackResourceSnapshot& Snapshot,
	const EFU_OperationKind SubmittedKind,
	const EFU_OperationAction Actions)
{
	// TimerManager 以 Handle 的值定位 timer；传入局部副本可清旧 timer，又不会使新共享 Handle 失效。
	if (EnumHasAnyFlags(Actions, EFU_OperationAction::ClearWatchdog)
		&& Snapshot.OperationWatchdogHandle.IsValid())
	{
		if (UGameInstance* const GameInstance = GetGameInstance())
		{
			FTimerHandle CapturedWatchdog = Snapshot.OperationWatchdogHandle;
			GameInstance->GetTimerManager().ClearTimer(CapturedWatchdog);
		}
	}

	if (Snapshot.SessionInterface.IsValid()
		&& EnumHasAnyFlags(Actions, EFU_OperationAction::ClearOriginalDelegate))
	{
		// 每个 Handle 只能交回注册它的旧接口，不能借用诊断重入后替换的新接口。
		switch (SubmittedKind)
		{
		case EFU_OperationKind::Create:
			if (Snapshot.CreateDelegateHandle.IsValid())
			{
				FDelegateHandle CapturedHandle = Snapshot.CreateDelegateHandle;
				Snapshot.SessionInterface->ClearOnCreateSessionCompleteDelegate_Handle(CapturedHandle);
			}
			break;
		case EFU_OperationKind::Find:
			if (Snapshot.FindDelegateHandle.IsValid())
			{
				FDelegateHandle CapturedHandle = Snapshot.FindDelegateHandle;
				Snapshot.SessionInterface->ClearOnFindSessionsCompleteDelegate_Handle(CapturedHandle);
			}
			break;
		case EFU_OperationKind::Join:
			if (Snapshot.JoinDelegateHandle.IsValid())
			{
				FDelegateHandle CapturedHandle = Snapshot.JoinDelegateHandle;
				Snapshot.SessionInterface->ClearOnJoinSessionCompleteDelegate_Handle(CapturedHandle);
			}
			break;
		case EFU_OperationKind::Destroy:
			if (Snapshot.DestroyDelegateHandle.IsValid())
			{
				FDelegateHandle CapturedHandle = Snapshot.DestroyDelegateHandle;
				Snapshot.SessionInterface->ClearOnDestroySessionCompleteDelegate_Handle(CapturedHandle);
			}
			break;
		default:
			break;
		}
	}

	if (Snapshot.SessionInterface.IsValid()
		&& Snapshot.CancelFindDelegateHandle.IsValid()
		&& EnumHasAnyFlags(Actions, EFU_OperationAction::ClearFindCancellationDelegate))
	{
		FDelegateHandle CapturedHandle = Snapshot.CancelFindDelegateHandle;
		Snapshot.SessionInterface->ClearOnCancelFindSessionsCompleteDelegate_Handle(
			CapturedHandle);
	}
}

template<EFU_OnlineProvider Provider>
bool UFU_OnlineSessionSubsystem::FU_AreCapturedCallbackResourcesCurrent(
	const FFU_OnlineCallbackResourceSnapshot& Snapshot,
	const EFU_OperationKind SubmittedKind,
	const EFU_OperationAction Actions) const
{
	const FFU_OnlineProviderState& State = FU_GetProviderState<Provider>();
	if (State.SessionInterface.Get() != Snapshot.SessionInterface.Get())
	{
		return false;
	}
	if (EnumHasAnyFlags(Actions, EFU_OperationAction::ClearWatchdog)
		&& State.OperationWatchdogHandle != Snapshot.OperationWatchdogHandle)
	{
		return false;
	}
	if (EnumHasAnyFlags(Actions, EFU_OperationAction::ClearFindCancellationDelegate)
		&& State.CancelFindDelegateHandle != Snapshot.CancelFindDelegateHandle)
	{
		return false;
	}
	if (EnumHasAnyFlags(Actions, EFU_OperationAction::ClearOriginalDelegate))
	{
		const bool bOriginalHandleMatches =
			(SubmittedKind == EFU_OperationKind::Create && State.CreateDelegateHandle == Snapshot.CreateDelegateHandle)
			|| (SubmittedKind == EFU_OperationKind::Find && State.FindDelegateHandle == Snapshot.FindDelegateHandle)
			|| (SubmittedKind == EFU_OperationKind::Join && State.JoinDelegateHandle == Snapshot.JoinDelegateHandle)
			|| (SubmittedKind == EFU_OperationKind::Destroy && State.DestroyDelegateHandle == Snapshot.DestroyDelegateHandle);
		if (!bOriginalHandleMatches)
		{
			return false;
		}
	}
	return !EnumHasAnyFlags(Actions, EFU_OperationAction::ClearPendingData)
		|| SubmittedKind != EFU_OperationKind::Find
		|| State.SessionSearch == Snapshot.SessionSearch;
}

template<EFU_OnlineProvider Provider>
void UFU_OnlineSessionSubsystem::FU_ResetMatchedCallbackResources(
	const FFU_OnlineCallbackResourceSnapshot& Snapshot,
	const EFU_OperationKind SubmittedKind,
	const EFU_OperationAction Actions)
{
	FFU_OnlineProviderState& State = FU_GetProviderState<Provider>();
	// generation 相等仍不足以证明资源相同；每个字段都要与进入回调时的快照做身份匹配。
	if (EnumHasAnyFlags(Actions, EFU_OperationAction::ClearWatchdog)
		&& State.OperationWatchdogHandle == Snapshot.OperationWatchdogHandle)
	{
		State.OperationWatchdogHandle.Invalidate();
	}
	if (EnumHasAnyFlags(Actions, EFU_OperationAction::ClearOriginalDelegate))
	{
		switch (SubmittedKind)
		{
		case EFU_OperationKind::Create:
			if (State.CreateDelegateHandle == Snapshot.CreateDelegateHandle) { State.CreateDelegateHandle.Reset(); }
			break;
		case EFU_OperationKind::Find:
			if (State.FindDelegateHandle == Snapshot.FindDelegateHandle) { State.FindDelegateHandle.Reset(); }
			break;
		case EFU_OperationKind::Join:
			if (State.JoinDelegateHandle == Snapshot.JoinDelegateHandle) { State.JoinDelegateHandle.Reset(); }
			break;
		case EFU_OperationKind::Destroy:
			if (State.DestroyDelegateHandle == Snapshot.DestroyDelegateHandle) { State.DestroyDelegateHandle.Reset(); }
			break;
		default:
			break;
		}
	}
	if (EnumHasAnyFlags(Actions, EFU_OperationAction::ClearFindCancellationDelegate)
		&& State.CancelFindDelegateHandle == Snapshot.CancelFindDelegateHandle)
	{
		State.CancelFindDelegateHandle.Reset();
	}

	if (!EnumHasAnyFlags(Actions, EFU_OperationAction::ClearPendingData))
	{
		return;
	}
	// pending 标量没有独立 identity，由 dispatcher 的 generation gate 保护；搜索对象还要额外比较指针身份。
	switch (SubmittedKind)
	{
	case EFU_OperationKind::Create:
		State.PendingCreateRoomName.Reset();
		State.PendingCreateRoomPassword.Reset();
		State.PendingMaxPlayers = 0;
		break;
	case EFU_OperationKind::Find:
		if (State.SessionSearch == Snapshot.SessionSearch)
		{
			State.SessionSearch.Reset();
			State.PendingFindRoomName.Reset();
			State.bSteamFallbackFindInProgress = false;
		}
		break;
	case EFU_OperationKind::Join:
		State.PendingJoinResult.Reset();
		break;
	case EFU_OperationKind::Destroy:
		State.PendingOperation = EFU_PendingOperation::None;
		break;
	default:
		break;
	}
}

template<EFU_OnlineProvider Provider>
void UFU_OnlineSessionSubsystem::FU_BroadcastOperationFailure(const EFU_OperationKind RootKind)
{
	// 根操作决定 Blueprint 完成事件；预销毁的 SubmittedKind=Destroy 不能误播 Destroy 完成。
	switch (RootKind)
	{
	case EFU_OperationKind::Create:
		OnCreateSessionCompleteV2.Broadcast(Provider, false);
		break;
	case EFU_OperationKind::Find:
		OnFindSessionCompleteV2.Broadcast(Provider, TArray<FFU_SessionResult>{}, false);
		break;
	case EFU_OperationKind::Join:
		OnJoinSessionCompleteV2.Broadcast(Provider, EFU_JoinSessionResult::UnknownError);
		break;
	case EFU_OperationKind::Destroy:
		OnDestroySessionComplete.Broadcast(Provider, false);
		break;
	default:
		break;
	}
}

//子系统实例
//IOnlineSessionPtr智能指针
template<EFU_OnlineProvider Provider>
IOnlineSessionPtr UFU_OnlineSessionSubsystem::FU_GetSessionInterface() const
{
	using FProviderTraits = TFU_OnlineSessionProviderTraits<Provider>;
	//显式传入当前World和提供方名称，避免PIE多世界或默认子系统配置影响选中的Provider
	IOnlineSubsystem* OnlineSubsystem = Online::GetSubsystem(GetWorld(), FProviderTraits::GetSubsystemName());
	if (!OnlineSubsystem)
	{
		// 公开入口会以其原始 OperationId 发出 FU.Provider.SubsystemUnavailable；这里不能另记一条
		// 无关联 LogTemp，否则同一拒绝会出现无法由 Blueprint/报告串联的第二份 Provider 结论。
		return nullptr;
	}
	
	return OnlineSubsystem->GetSessionInterface();
}

//【FU 修复：Provider 同时选择会话层和传输层】
template<EFU_OnlineProvider Provider>
bool UFU_OnlineSessionSubsystem::FU_PrepareGameNetDriver(const EFU_OperationKind RootKind, const FGuid& OperationId)
{
	using FProviderTraits = TFU_OnlineSessionProviderTraits<Provider>;

	static_assert(
		Provider == EFU_OnlineProvider::Steam || Provider == EFU_OnlineProvider::Lan,
		"不受支持的 FU 在线提供商");

	// GEngine->NetDriverDefinitions 是引擎全局状态，只允许从游戏线程修改。
	if (!IsInGameThread() || !GEngine)
	{
		UE_LOG(
			LogFUOnlineSession,
			Error,
			TEXT("[%s] 无法准备 GameNetDriver：当前不在游戏线程或 GEngine 不可用"),
			FProviderTraits::GetDebugName());
		return false;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		UE_LOG(
			LogFUOnlineSession,
			Error,
			TEXT("[%s] 无法准备 GameNetDriver：GameInstance 尚未关联 World"),
			FProviderTraits::GetDebugName());
		return false;
	}

	const FName DesiredDriverClassName = FProviderTraits::GetNetDriverClassName();
	const FString DesiredDriverClassPath = DesiredDriverClassName.ToString();

	// 主动加载并验证目标类型，防止主驱动拼写错误后悄悄回退到 IpNetDriver。
	UClass* DesiredDriverClass =
		FSoftClassPath(DesiredDriverClassPath).TryLoadClass<UNetDriver>();
	if (!DesiredDriverClass)
	{
		UE_LOG(
			LogFUOnlineSession,
			Error,
			TEXT("[%s] 无法加载 NetDriver 类：%s"),
			FProviderTraits::GetDebugName(),
			*DesiredDriverClassPath);
		return false;
	}

	// 已经开始 Listen 或已经连入服务器时，当前 World 的驱动不能热替换。
	// 即使类型恰好一致也不能在这里直接成功：只有协调器确认“同 GameInstance/Provider 的既有租约”
	// 才能幂等重入，否则全局定义可能已被第三方改写，下一次 Travel 仍会创建错误驱动。
	if (UNetDriver* ActiveNetDriver = World->GetNetDriver())
	{
		if (!ActiveNetDriver->IsA(DesiredDriverClass))
		{
			UE_LOG(
				LogFUOnlineSession,
				Error,
				TEXT("[%s] 拒绝切换 NetDriver：当前=%s，目标=%s。请先退出联网关卡"),
				FProviderTraits::GetDebugName(),
				*ActiveNetDriver->GetClass()->GetPathName(),
				*DesiredDriverClassPath);
			return false;
		}
	}

	UGameInstance* const LeaseOwner = GetGameInstance();
	if (!LeaseOwner)
	{
		UE_LOG(
			LogFUOnlineSession,
			Error,
			TEXT("[%s] 无法准备 GameNetDriver：GameInstance 所有者不可用"),
			FProviderTraits::GetDebugName());
		return false;
	}

	// 【进程级协调】真正的 GEngine 写入全部收口到租约；它会检查所有 PIE World、PendingNetGame、
	// 唯一定义、所有权和五字段指纹，模板在这里仍只负责把 traits 选择的类名交给协调器。
	const EFU_NetDriverLeaseResult LeaseResult = FFU_OnlineSessionNetDriverLease::Acquire(
		*LeaseOwner,
		Provider,
		DesiredDriverClassName);
	const FFU_NetDriverLeaseDiagnosticOutcome LeaseOutcome =
		FFU_OnlineSessionNetDriverLease::GetDiagnosticOutcome(LeaseResult);
	const bool bLeaseAcquired = FFU_OnlineSessionNetDriverLease::IsAcquireSuccess(LeaseResult);
	if (bLeaseAcquired)
	{
		// Acquire 结果必须在公开诊断前反映到本地镜像。若 Blueprint 监听器在 Lease 事件中
		// 显式 Destroy 并释放租约，其 Reset 才会成为最终状态；事件返回后不能把已释放租约重新标记为 Prepared。
		PreparedNetDriverProvider = Provider;
	}
	FU_EmitDiagnostic<Provider>(
		RootKind,
		OperationId,
		EFU_OnlineDiagnosticPhase::Preflight,
		LeaseOutcome.bIsError ? EFU_OnlineDiagnosticSeverity::Warning : EFU_OnlineDiagnosticSeverity::Info,
		*LeaseOutcome.Code.ToString(),
		TEXT("GameNetDriver 租约协调器已完成 Acquire 决策"),
		FString(),
		*LeaseOutcome.Status.ToString());
	if (!bLeaseAcquired)
	{
		UE_LOG(
			LogFUOnlineSession,
			Error,
			TEXT("[%s] 无法取得 GameNetDriver 租约：Result=%s Target=%s"),
			FProviderTraits::GetDebugName(),
			FFU_OnlineSessionNetDriverLease::GetResultName(LeaseResult),
			*DesiredDriverClassPath);
		return false;
	}

	UE_LOG(
		LogFUOnlineSession,
		Display,
		TEXT("[%s] 已通过进程租约准备 GameNetDriver：%s（%s）"),
		FProviderTraits::GetDebugName(),
		*DesiredDriverClassPath,
		FFU_OnlineSessionNetDriverLease::GetResultName(LeaseResult));

	return true;
}

//运行时状态检测模板：只读取接口状态，不缓存接口，也不改变 Provider 的异步操作状态。
template<EFU_OnlineProvider Provider>
FFU_OnlineProviderStatus UFU_OnlineSessionSubsystem::FU_CheckProviderStatus(
	const bool bRequiresNetDriver) const
{
	using FProviderTraits = TFU_OnlineSessionProviderTraits<Provider>;

	FFU_OnlineProviderStatus Result;
	Result.Provider = Provider;
	Result.SubsystemName = FProviderTraits::GetSubsystemName();
	Result.RequiredNetDriverClass = FProviderTraits::GetNetDriverClassName();

	FFU_OnlineProviderStatusInputs Inputs;
	Inputs.bRequiresNetDriver = bRequiresNetDriver;
	const UWorld* World = GetWorld();
	Inputs.bHasWorld = World != nullptr;

#if UE_BUILD_SHIPPING
	Inputs.bShippingBuild = true;
#endif

	if (bRequiresNetDriver)
	{
		// Session 与 NetDriver 是两条不同依赖链；Create/Join 的完整状态必须同时检查两者。
		Inputs.bHasNetDriverDefinition = FU_FindGameNetDriverDefinition() != nullptr;
		Result.bNetDriverDefinitionAvailable = Inputs.bHasNetDriverDefinition;

		// ResolveClass 只读取当前注册状态，不启动异步联网操作或触发软类加载。
		Inputs.bHasNetDriverClass =
			FSoftClassPath(Result.RequiredNetDriverClass.ToString()).ResolveClass() != nullptr;
		Result.bNetDriverClassAvailable = Inputs.bHasNetDriverClass;
	}

	if constexpr (Provider == EFU_OnlineProvider::Steam)
	{
		// 【模块所有权】Bootstrap ticket 只由 Runtime 模块持有；状态查询不重新注入、不触碰配置缓存。
		const FFUOnlineSessionModule* const RuntimeModule =
			FModuleManager::GetModulePtr<FFUOnlineSessionModule>(TEXT("FUOnlineSession"));
		Inputs.bSteamAppIdBootstrapReady =
			RuntimeModule != nullptr && RuntimeModule->IsSteamAppIdBootstrapReady();
		Result.bSteamAppIdBootstrapReady = Inputs.bSteamAppIdBootstrapReady;

		if (bRequiresNetDriver)
		{
			// 【Steam 限域】Find 不需要也不加载传输模块；Create/Join 的 Steam 模板才执行真实探针。
			const FFU_SteamSocketsReadiness SteamSockets = FFU_SteamSocketsReadinessProbe::Probe();
			Inputs.bSteamSocketsModuleAvailable = SteamSockets.bModuleAvailable;
			Inputs.bSteamSocketsEnabled = SteamSockets.bModuleEnabled;
			Inputs.bSteamSocketsSocketSubsystemAvailable = SteamSockets.bSocketSubsystemAvailable;
			Result.bSteamSocketsModuleAvailable = Inputs.bSteamSocketsModuleAvailable;
			Result.bSteamSocketsEnabled = Inputs.bSteamSocketsEnabled;
			Result.bSteamSocketsSocketSubsystemAvailable = Inputs.bSteamSocketsSocketSubsystemAvailable;
		}
	}

	if (bRequiresNetDriver && World)
	{
		if (const UNetDriver* ActiveNetDriver = World->GetNetDriver())
		{
			Result.ActiveNetDriverClass = FName(*ActiveNetDriver->GetClass()->GetPathName());
			Inputs.bHasConflictingActiveNetDriver =
				ActiveNetDriver->GetClass()->GetPathName() != Result.RequiredNetDriverClass.ToString();
		}
	}

	if (bRequiresNetDriver)
	{
		// 一律由只读 Probe 判断：同 owner/provider 的既有租约可在活动驱动期间幂等复用，
		// 仅仅“活动类名刚好相同”却没有租约时仍会拒绝，防止下一次 Travel 使用未知的全局定义。
		const EFU_NetDriverLeaseResult LeaseProbeResult = FFU_OnlineSessionNetDriverLease::ProbeAcquire(
			GetGameInstance(),
			Provider,
			Result.RequiredNetDriverClass);
		Inputs.bNetDriverLeaseAvailable =
			FFU_OnlineSessionNetDriverLease::IsProbeSuccess(LeaseProbeResult);
		Result.bNetDriverLeaseAvailable = Inputs.bNetDriverLeaseAvailable;
		Result.NetDriverLeaseStatus = FFU_OnlineSessionNetDriverLease::GetResultName(LeaseProbeResult);
	}
	else
	{
		// Find 不触碰传输层，明确标为 NotRequired；这样失败日志不会把“未采样”误读成租约故障。
		Inputs.bNetDriverLeaseAvailable = true;
		Result.bNetDriverLeaseAvailable = true;
		Result.NetDriverLeaseStatus = TEXT("NotRequired");
	}

	IOnlineSubsystem* OnlineSubsystem = nullptr;
	if (World)
	{
		// 必须显式提供当前 World，PIE 多世界运行时才能检查属于当前 GameInstance 的子系统实例。
		OnlineSubsystem = Online::GetSubsystem(World, FProviderTraits::GetSubsystemName());
	}

	Inputs.bHasSubsystem = OnlineSubsystem != nullptr;
	Result.bSubsystemAvailable = Inputs.bHasSubsystem;

	if (OnlineSubsystem)
	{
		const IOnlineSessionPtr SessionInterface = OnlineSubsystem->GetSessionInterface();
		Inputs.bHasSessionInterface = SessionInterface.IsValid();
		Result.bSessionInterfaceAvailable = Inputs.bHasSessionInterface;

		if constexpr (Provider == EFU_OnlineProvider::Steam)
		{
			// Steam Lobby 依赖本地 Steam 用户；NULL/LAN 刻意不执行这段编译期分支。
			const IOnlineIdentityPtr IdentityInterface = OnlineSubsystem->GetIdentityInterface();
			Inputs.bHasIdentityInterface = IdentityInterface.IsValid();
			Result.bIdentityInterfaceAvailable = Inputs.bHasIdentityInterface;

			if (IdentityInterface.IsValid())
			{
				// 本插件现阶段按单本地玩家设计，因此检查 LocalUserNum 0。
				Inputs.bIsLoggedIn =
					IdentityInterface->GetLoginStatus(0) == ELoginStatus::LoggedIn;
				Result.bLoggedIn = Inputs.bIsLoggedIn;
			}

			// 【AppID 比对】仅在已取得真实 Steam 子系统后读取 GetAppId；不从 TargetRules 或临时宏伪造正式环境。
			const UFU_OnlineSessionSettings& Settings = UFU_OnlineSessionSettings::GetRuntimeSettings();
			const int32 ExpectedAppId = Inputs.bShippingBuild
				? Settings.ExpectedShippingSteamAppId
				: Settings.SteamDevAppId;
			Inputs.bShippingSteamAppIdExpected =
				!Inputs.bShippingBuild || ExpectedAppId > 0;

			int32 ActualAppId = 0;
			const FString ActualAppIdText = OnlineSubsystem->GetAppId();
			Inputs.bSteamAppIdMatchesExpectation =
				ExpectedAppId > 0
				&& LexTryParseString(ActualAppId, *ActualAppIdText)
				&& ActualAppId == ExpectedAppId;
			Result.bSteamAppIdMatchesExpectation = Inputs.bSteamAppIdMatchesExpectation;
		}
	}

	Result.StatusCode = FFU_OnlineProviderStatusEvaluator::Evaluate(Provider, Inputs);
	Result.bIsReady = Result.StatusCode == EFU_OnlineProviderStatusCode::Ready;
	// 蓝图状态节点与运行时诊断必须使用同一稳定代码，避免 UI 因 Message 本地化或文本变化失去分支依据。
	Result.DiagnosticCode = FName(FUOnlineSession::GetProviderStatusDiagnosticCode(Result.StatusCode));
	Result.Message = FUOnlineSession::GetProviderStatusMessage(Provider, Result.StatusCode);
	if (Result.StatusCode == EFU_OnlineProviderStatusCode::NetDriverLeaseUnavailable)
	{
		Result.Message += FString::Printf(TEXT("；LeaseResult=%s"), *Result.NetDriverLeaseStatus);
	}

	return Result;
}

template<EFU_OnlineProvider Provider>
bool UFU_OnlineSessionSubsystem::FU_ValidateProviderReady(
	const EFU_OperationKind RootKind,
	const FGuid& OperationId,
	const TCHAR* OperationName,
	const bool bRequiresNetDriver,
	TFunctionRef<void()> FailureContinuation)
{
	using FProviderTraits = TFU_OnlineSessionProviderTraits<Provider>;

	static_assert(
		Provider == EFU_OnlineProvider::Steam || Provider == EFU_OnlineProvider::Lan,
		"不受支持的 FU 在线提供商");

	// 【FU 修复：不能只依赖蓝图状态节点】
	// 状态检查本身是同步、只读的：它不会登录 Steam、不会修改 ProviderState，
	// 也不会注册异步委托。因此可以安全地放在每一个公开模板操作的入口处。
	const FFU_OnlineProviderStatus Status = FU_CheckProviderStatus<Provider>(bRequiresNetDriver);
	const auto GetDiagnosticOperation = [RootKind]()
	{
		switch (RootKind)
		{
		case EFU_OperationKind::Create: return EFU_OnlineDiagnosticOperation::CreateSession;
		case EFU_OperationKind::Find: return EFU_OnlineDiagnosticOperation::FindSessions;
		case EFU_OperationKind::Join: return EFU_OnlineDiagnosticOperation::JoinSession;
		case EFU_OperationKind::Destroy: return EFU_OnlineDiagnosticOperation::DestroySession;
		default: return EFU_OnlineDiagnosticOperation::Recovery;
		}
	};
	const bool bReady = FFU_OnlineProviderPreflightGate::Dispatch(
		Provider,
		GetDiagnosticOperation(),
		OperationId,
		Status,
		[this](FFU_OnlineDiagnosticEvent Event)
	{
		// 【可测生产缝】预检模板与纯自动化测试共用同一事件构造器；这里仍由 Subsystem
		// 负责补齐 World/PIE 并经唯一的 Emit 出口分发，避免测试手写一份相似事件。
		FFU_OnlineDiagnosticField SubsystemField;
		SubsystemField.Key = FName(TEXT("Subsystem"));
		SubsystemField.Value = FProviderTraits::GetSubsystemName().ToString();
		Event.Fields.Add(MoveTemp(SubsystemField));
		FU_EmitDiagnostic(MoveTemp(Event));
	},
		FailureContinuation);
	if (Status.bIsReady)
	{
		// 预检通过同样需要可观察，Blueprint 可用同一 OperationId 串起 Requested -> Preflight -> Submitted。
		check(bReady);
		return true;
	}

	// 一条日志同时记录状态码和各依赖层，便于区分“Steam 未登录”、
	// “OnlineSubsystem 缺失”以及“NetDriver 配置错误”，无需再从大量 OSS 日志中猜测。
	UE_LOG(
		LogFUOnlineSession,
		Warning,
		TEXT("[%s] 拒绝启动 %s：RequiresNetDriver=%s StatusCode=%d Subsystem=%s SessionInterface=%s "
			 "IdentityInterface=%s LoggedIn=%s NetDriverDefinition=%s NetDriverClass=%s NetDriverLease=%s LeaseResult=%s "
			 "SteamBootstrap=%s SteamSocketsModule=%s SteamSocketsEnabled=%s SteamSocketsSubsystem=%s AppIdMatches=%s "
			 "RequiredNetDriver=%s ActiveNetDriver=%s Message=\"%s\""),
		FProviderTraits::GetDebugName(),
		OperationName ? OperationName : TEXT("OnlineOperation"),
		bRequiresNetDriver ? TEXT("true") : TEXT("false"),
		static_cast<int32>(Status.StatusCode),
		Status.bSubsystemAvailable ? TEXT("true") : TEXT("false"),
		Status.bSessionInterfaceAvailable ? TEXT("true") : TEXT("false"),
		Status.bIdentityInterfaceAvailable ? TEXT("true") : TEXT("false"),
		Status.bLoggedIn ? TEXT("true") : TEXT("false"),
		Status.bNetDriverDefinitionAvailable ? TEXT("true") : TEXT("false"),
		Status.bNetDriverClassAvailable ? TEXT("true") : TEXT("false"),
		Status.bNetDriverLeaseAvailable ? TEXT("true") : TEXT("false"),
		Status.NetDriverLeaseStatus.IsEmpty() ? TEXT("Unknown") : *Status.NetDriverLeaseStatus,
		Status.bSteamAppIdBootstrapReady ? TEXT("true") : TEXT("false"),
		Status.bSteamSocketsModuleAvailable ? TEXT("true") : TEXT("false"),
		Status.bSteamSocketsEnabled ? TEXT("true") : TEXT("false"),
		Status.bSteamSocketsSocketSubsystemAvailable ? TEXT("true") : TEXT("false"),
		Status.bSteamAppIdMatchesExpectation ? TEXT("true") : TEXT("false"),
		*Status.RequiredNetDriverClass.ToString(),
		Status.ActiveNetDriverClass.IsNone() ? TEXT("None") : *Status.ActiveNetDriverClass.ToString(),
		*Status.Message);

	// 【Task 7 时序契约】先把拒绝原因与入口分配的 ID 写入历史/Blueprint，再由调用者广播旧失败委托。
	// 不能在这里新建 Ticket，否则预销毁续步、Busy 拒绝与后续诊断会断开成不同操作。
	check(!bReady);

	return false;
}

//清理创建委托
template<EFU_OnlineProvider Provider>
void UFU_OnlineSessionSubsystem::FU_ClearCreateDelegate()
{
	FFU_OnlineProviderState& State = FU_GetProviderState<Provider>();
	//移除委托来源于注册它的SessionInterface
	if (State.SessionInterface.IsValid() && State.CreateDelegateHandle.IsValid())
	{
		State.SessionInterface->ClearOnCreateSessionCompleteDelegate_Handle(State.CreateDelegateHandle);
	}
	
	State.CreateDelegateHandle.Reset();
}
//清理查找委托
template<EFU_OnlineProvider Provider>
void UFU_OnlineSessionSubsystem::FU_ClearFindDelegate()
{
	FFU_OnlineProviderState& State = FU_GetProviderState<Provider>();

	//查找委托属于当前Provider，必须使用对应接口移除
	if (State.SessionInterface.IsValid() && State.FindDelegateHandle.IsValid())
	{
		State.SessionInterface->ClearOnFindSessionsCompleteDelegate_Handle(State.FindDelegateHandle);
	}

	State.FindDelegateHandle.Reset();
}
//清理加入委托
template<EFU_OnlineProvider Provider>
void UFU_OnlineSessionSubsystem::FU_ClearJoinDelegate()
{
	FFU_OnlineProviderState& State = FU_GetProviderState<Provider>();

	//加入完成回调只能从发起JoinSession的接口移除。
	if (State.SessionInterface.IsValid() && State.JoinDelegateHandle.IsValid())
	{
		State.SessionInterface->ClearOnJoinSessionCompleteDelegate_Handle(State.JoinDelegateHandle);
	}

	State.JoinDelegateHandle.Reset();
}

//清理销毁委托
template<EFU_OnlineProvider Provider>
void UFU_OnlineSessionSubsystem::FU_ClearDestroyDelegate()
{
	FFU_OnlineProviderState& State = FU_GetProviderState<Provider>();

	//销毁委托也必须由原Provider的接口负责移除
	if (State.SessionInterface.IsValid() && State.DestroyDelegateHandle.IsValid())
	{
		State.SessionInterface->ClearOnDestroySessionCompleteDelegate_Handle(State.DestroyDelegateHandle);
	}

	State.DestroyDelegateHandle.Reset();
}

//清理Provider状态
template<EFU_OnlineProvider Provider>
void UFU_OnlineSessionSubsystem::FU_ClearProviderState()
{
	FFU_OnlineProviderState& State = FU_GetProviderState<Provider>();

	// 先保存接口和状态机快照；Deinitialize 会记录不确定操作后停表并精确解绑，最后才请求租约恢复。
	const IOnlineSessionPtr SessionInterfaceToRelease = State.SessionInterface;
	const bool bShouldCancelFind = State.OperationMachine.IsValid()
		&& State.OperationMachine->Get().Phase != EFU_OperationPhase::Idle
		&& State.OperationMachine->Get().ActiveKind == EFU_OperationKind::Find
		&& SessionInterfaceToRelease.IsValid()
		&& State.SessionSearch.IsValid();

	FU_ClearOperationWatchdog<Provider>();
	FU_ClearFindCancellationDelegate<Provider>();
	// 先解除所有原始委托，防止取消搜索时再次回调当前对象。
	FU_ClearCreateDelegate<Provider>();
	FU_ClearFindDelegate<Provider>();
	FU_ClearJoinDelegate<Provider>();
	FU_ClearDestroyDelegate<Provider>();

	if (bShouldCancelFind)
	{
		SessionInterfaceToRelease->CancelFindSessions();
	}

	//清空只属于该Provider的异步运行数据
	State.SessionSearch.Reset();
	State.CachedSearchResults.Reset();
	State.PendingJoinResult.Reset();
	State.PendingCreateRoomName.Reset();
	State.PendingFindRoomName.Reset();
	State.bSteamFallbackFindInProgress = false;
	State.PendingCreateRoomPassword.Reset();
	State.PendingMaxPlayers = 0;
	State.PendingOperation = EFU_PendingOperation::None;
	State.OperationMachine.Reset();
	State.SessionInterface.Reset();
}

//检查并销毁已有的Session
template<EFU_OnlineProvider Provider>
bool UFU_OnlineSessionSubsystem::FU_DestroyExistingSessionForPendingOperation(
	const EFU_PendingOperation InPendingOperation,
	FFU_OperationTicket& RootTicket)
{
	FFU_OnlineProviderState& State = FU_GetProviderState<Provider>();

	//没有有效接口，自然也无法检查已有Session
	if (!State.SessionInterface.IsValid())
	{
		return false;
	}

	//每个OnlineSubsystem都拥有自己的NAME_GameSession
	//Steam的同名Session不等于NULL的同名Session
	if (!State.SessionInterface->GetNamedSession(NAME_GameSession))
	{
		return false;
	}

	//返回true表示调用者必须等待销毁结果，不能立即继续创建或加入
	FU_DestroySession<Provider>(InPendingOperation, &RootTicket);
	return true;
}

//销毁入口
template<EFU_OnlineProvider Provider>
void UFU_OnlineSessionSubsystem::FU_DestroySession(
	const EFU_PendingOperation InPendingOperation,
	FFU_OperationTicket* RootTicket)
{
    FFU_OnlineProviderState& State = FU_GetProviderState<Provider>();
	FFU_OperationTicket LocalTicket;
	if (RootTicket == nullptr)
	{
		// 显式 Destroy 是公共入口；所有 Busy/接口检查之前先分配独立尝试身份。
		LocalTicket = FU_BeginOperationAttempt<Provider>(EFU_OperationKind::Destroy);
		RootTicket = &LocalTicket;
	}
	FFU_OnlineOperationStateMachine& Machine = FU_GetOperationMachine<Provider>();

    //同一个Provider同一时间只能执行一个Session异步操作
    if (Machine.Get().Phase != EFU_OperationPhase::Idle)
    {
		FU_EmitDiagnostic<Provider>(RootTicket->Kind, RootTicket->OperationId, EFU_OnlineDiagnosticPhase::Preflight,
			EFU_OnlineDiagnosticSeverity::Warning, TEXT("FU.Operation.Rejected.Busy"), TEXT("Destroy 请求被在途 OSS 操作拒绝"));
        if (InPendingOperation == EFU_PendingOperation::None)
        {
            OnDestroySessionComplete.Broadcast(Provider, false);
        }
        else if (InPendingOperation == EFU_PendingOperation::Create)
        {
            OnCreateSessionCompleteV2.Broadcast(Provider,false);
        }
        else if (InPendingOperation == EFU_PendingOperation::Join)
        {
            OnJoinSessionCompleteV2.Broadcast(Provider,EFU_JoinSessionResult::UnknownError);
        }

        return;
    }

	// 显式 Destroy 的 Requested 诊断与 Create/Find/Join 一样可能同步重入；链式 Destroy
	// 也仍持有尚未接收的根票据。只有最新票据可以读取/修改命名 Session 和 PendingOperation。
	if (!Machine.CanAcceptAttempt(*RootTicket))
	{
		FU_EmitDiagnostic<Provider>(RootTicket->Kind, RootTicket->OperationId, EFU_OnlineDiagnosticPhase::Preflight,
			EFU_OnlineDiagnosticSeverity::Warning, TEXT("FU.Operation.Rejected.StateChanged"),
			TEXT("Destroy Requested 诊断返回后票据已被重入请求取代；未修改共享销毁状态"));
		FU_BroadcastOperationFailure<Provider>(RootTicket->Kind);
		return;
	}

    //公开销毁入口可能尚未缓存接口，因此在这里主动取得
    if (!State.SessionInterface.IsValid())
    {
        State.SessionInterface = FU_GetSessionInterface<Provider>();
    }

    if (!State.SessionInterface.IsValid())
    {
		FU_EmitDiagnostic<Provider>(RootTicket->Kind, RootTicket->OperationId, EFU_OnlineDiagnosticPhase::Preflight,
			EFU_OnlineDiagnosticSeverity::Error, TEXT("FU.Provider.SessionInterfaceUnavailable"), TEXT("Destroy 无法取得 Session Interface"));
        if (InPendingOperation == EFU_PendingOperation::None)
        {
            OnDestroySessionComplete.Broadcast(Provider, false);
        }
        else if (InPendingOperation == EFU_PendingOperation::Create)
        {
            OnCreateSessionCompleteV2.Broadcast(Provider,false);
        }
        else
        {
            OnJoinSessionCompleteV2.Broadcast(Provider,EFU_JoinSessionResult::UnknownError);
        }

        return;
    }

    //Session可能在前一次检查后已经消失；此时不能吞掉等待中的创建或加入操作
    if (!State.SessionInterface->GetNamedSession(NAME_GameSession))
    {
        if (InPendingOperation == EFU_PendingOperation::Create)
        {
            FU_CreateSessionInternal<Provider>(RootTicket);
        }
        else if (InPendingOperation == EFU_PendingOperation::Join)
        {
            FU_JoinSessionInternal<Provider>(RootTicket);
        }
        else
        {
			// 状态和租约先达到 Destroy 的幂等终态；从第一个诊断事件起不再写共享会话状态。
			// 因而 Blueprint 监听器可以安全地立即创建下一房间，不会被旧 Destroy 栈帧回滚。
            if (ActiveGameplayProvider.IsSet() && ActiveGameplayProvider.GetValue() == Provider)
            {
                ActiveGameplayProvider.Reset();
            }

			// 没有命名 Session 等价于显式销毁已经达到目标；租约也应随之归还。
			FU_RequestNetDriverLeaseRelease(
				Provider,
				TEXT("Explicit DestroySession found no named session"),
				RootTicket->Kind,
				RootTicket->OperationId);
			FU_EmitDiagnostic<Provider>(RootTicket->Kind, RootTicket->OperationId, EFU_OnlineDiagnosticPhase::Completed,
				EFU_OnlineDiagnosticSeverity::Info, TEXT("FU.DestroySession.NoNamedSession"), TEXT("Destroy 未发现命名会话，按幂等成功完成"));
            OnDestroySessionComplete.Broadcast(Provider, true);
        }

        return;
    }

	uint64 Generation = 0;
	if (!FU_ReserveOperation<Provider>(*RootTicket, EFU_OperationKind::Destroy, Generation))
	{
		FU_EmitDiagnostic<Provider>(RootTicket->Kind, RootTicket->OperationId, EFU_OnlineDiagnosticPhase::Preflight,
			EFU_OnlineDiagnosticSeverity::Warning, TEXT("FU.Operation.Rejected.StateChanged"), TEXT("Destroy 提交前状态已变化"));
		FU_BroadcastOperationFailure<Provider>(RootTicket->Kind);
		return;
	}
    State.PendingOperation = InPendingOperation;

    //将Provider编译进回调地址，回调触发时不会丢失来源
	State.DestroyDelegateHandle = State.SessionInterface->AddOnDestroySessionCompleteDelegate_Handle(
		FOnDestroySessionCompleteDelegate::CreateUObject(
			this,
			&ThisClass::FU_OnDestroySessionComplete<Provider>,
			Generation));
	FU_ArmOperationWatchdog<Provider>(Generation);
	FU_EmitOperationSubmitted<Provider>(EFU_OperationKind::Destroy, Generation);

    //返回false代表请求没有成功启动，不会再收到完成回调
    if (!State.SessionInterface->DestroySession(NAME_GameSession)
		&& Machine.IsExpectedCallback(Generation, EFU_OperationKind::Destroy))
	{
        const EFU_PendingOperation FailedOperation = State.PendingOperation;
		const FGuid OperationId = Machine.Get().ActiveOperationId;
		const EFU_OperationAction Actions = Machine.HandleSynchronousReject(Generation);

		FU_ClearOperationWatchdog<Provider>();
        FU_ClearDestroyDelegate<Provider>();
        State.PendingOperation = EFU_PendingOperation::None;

		// HandleSynchronousReject 已回到 Idle。先清除链式参数；否则下面任一 Lease/完成诊断
		// 都能让 Blueprint 启动新操作，而旧栈帧随后会误删新请求的数据。
		if (FailedOperation == EFU_PendingOperation::Create)
		{
			State.PendingCreateRoomName.Reset();
			State.PendingCreateRoomPassword.Reset();
			State.PendingMaxPlayers = 0;
		}
		else if (FailedOperation == EFU_PendingOperation::Join)
		{
			State.PendingJoinResult.Reset();
		}

		// 某些 Provider 可能同步移除本地命名 Session 却仍返回 false。只有接口有效且复查明确 NoSession
		// 才允许清理活动来源并归还租约；接口失效或 Session 仍在时继续保守持租。
		if (State.SessionInterface.IsValid()
			&& State.SessionInterface->GetNamedSession(NAME_GameSession) == nullptr)
		{
			if (ActiveGameplayProvider.IsSet() && ActiveGameplayProvider.GetValue() == Provider)
			{
				ActiveGameplayProvider.Reset();
			}
			FU_RequestNetDriverLeaseRelease(Provider, TEXT("DestroySession rejected but named session is gone"));
		}
		FU_EmitDiagnostic<Provider>(
			EFU_OperationKind::Destroy, OperationId,
			EFU_OnlineDiagnosticPhase::Completed, EFU_OnlineDiagnosticSeverity::Warning,
			TEXT("FU.Operation.SynchronousReject"), TEXT("DestroySession 同步返回 false；已按 generation 精确清理"));

		if (FailedOperation == EFU_PendingOperation::None)
        {
			if (EnumHasAnyFlags(Actions, EFU_OperationAction::BroadcastFailure))
			{
				OnDestroySessionComplete.Broadcast(Provider, false);
			}
        }
        else if (FailedOperation == EFU_PendingOperation::Create)
        {
			if (EnumHasAnyFlags(Actions, EFU_OperationAction::BroadcastFailure))
			{
				OnCreateSessionCompleteV2.Broadcast(Provider, false);
			}
        }
        else
        {
			if (EnumHasAnyFlags(Actions, EFU_OperationAction::BroadcastFailure))
			{
				OnJoinSessionCompleteV2.Broadcast(Provider, EFU_JoinSessionResult::UnknownError);
			}
        }
    }
}

template<EFU_OnlineProvider Provider>
void UFU_OnlineSessionSubsystem::FU_OnRecoveryDestroyComplete(
	const FName SessionName,
	const bool bWasSuccessful,
	const uint64 Generation)
{
	FFU_OnlineProviderState& State = FU_GetProviderState<Provider>();
	FFU_OnlineOperationStateMachine& Machine = FU_GetOperationMachine<Provider>();
	const bool bSessionStillExists = State.SessionInterface.IsValid()
		&& State.SessionInterface->GetNamedSession(NAME_GameSession) != nullptr;
	const bool bDestroyReachedNoSession = bWasSuccessful && !bSessionStillExists;
	const FFU_OnlineCallbackResourceSnapshot ResourceSnapshot = FU_CaptureCallbackResources<Provider>();
	// 【A2 stale gate】必须让状态机同时验证 SessionName、generation、kind、phase 后再取 ID；
	// 真正 stale 的回调得到 None 并保持无事件、无清理，不能借用当前新操作身份。
	const EFU_OperationAction Actions = SessionName == NAME_GameSession
		? Machine.HandleOriginalCompletion(
			Generation,
			EFU_OperationKind::Destroy,
			bDestroyReachedNoSession,
			bSessionStillExists)
		: EFU_OperationAction::None;
	if (Actions == EFU_OperationAction::None)
	{
		return;
	}

	const FGuid OperationId = Machine.Get().ActiveOperationId;
	const FFU_OnlineDiagnosticEvent Event = FFU_OnlineOperationPathDiagnostics::BuildRecoveryDestroy(
		Provider,
		OperationId,
		bDestroyReachedNoSession
			? EFU_RecoveryDestroyDiagnosticOutcome::CallbackSucceeded
			: EFU_RecoveryDestroyDiagnosticOutcome::CallbackFailed,
		bSessionStillExists,
		Actions);
	FFU_OnlineOperationPathDispatchSinks Sinks;
	Sinks.Diagnostic = [this](const FFU_OnlineDiagnosticEvent& InEvent)
	{
		FU_EmitDiagnostic<Provider>(InEvent);
	};
	Sinks.CurrentGeneration = [&Machine]() { return Machine.Get().ActiveGeneration; };
	Sinks.SharedResourcesStillMatch = [this, ResourceSnapshot, Actions]()
	{
		return FU_AreCapturedCallbackResourcesCurrent<Provider>(ResourceSnapshot, EFU_OperationKind::Destroy, Actions);
	};
	Sinks.ExactOldResourceCleanup = [this, ResourceSnapshot, Actions]()
	{
		FU_ClearCapturedCallbackResources(ResourceSnapshot, EFU_OperationKind::Destroy, Actions);
	};
	Sinks.GenerationMatchedCleanup = [this, ResourceSnapshot, Actions]()
	{
		FU_ResetMatchedCallbackResources<Provider>(ResourceSnapshot, EFU_OperationKind::Destroy, Actions);
		if (EnumHasAnyFlags(Actions, EFU_OperationAction::RequestLeaseRelease))
		{
			if (ActiveGameplayProvider.IsSet() && ActiveGameplayProvider.GetValue() == Provider)
			{
				ActiveGameplayProvider.Reset();
			}
			FU_RequestNetDriverLeaseRelease(Provider, TEXT("Recovery Destroy reached terminal NoSession"));
		}
	};
	Sinks.LegacyCompletion = [this, &Machine]()
	{
		FU_BroadcastOperationFailure<Provider>(Machine.Get().RootKind);
	};
	FFU_OnlineOperationPathDispatcher::DispatchInternal(Event, Generation, false, Sinks);
}

//销毁完成后回调
template<EFU_OnlineProvider Provider>
void UFU_OnlineSessionSubsystem::FU_OnDestroySessionComplete(
	const FName SessionName,
	const bool bWasSuccessful,
	const uint64 Generation)
{
	FFU_OnlineProviderState& State = FU_GetProviderState<Provider>();
	FFU_OnlineOperationStateMachine& Machine = FU_GetOperationMachine<Provider>();
	// 非 NAME_GameSession 或错误 generation/kind/phase 的回调保持 watchdog 和当前 Handle 原样。
	if (SessionName != NAME_GameSession
		|| !Machine.IsExpectedCallback(Generation, EFU_OperationKind::Destroy))
	{
		return;
	}
	if (Machine.Get().Phase == EFU_OperationPhase::Recovering)
	{
		// 原始 Destroy 自身超时后仍绑定本回调；统一转入专用恢复处理，确保 phase/code/清理顺序一致。
		FU_OnRecoveryDestroyComplete<Provider>(SessionName, bWasSuccessful, Generation);
		return;
	}

	const bool bSessionStillExists = State.SessionInterface.IsValid()
		&& State.SessionInterface->GetNamedSession(NAME_GameSession) != nullptr;
	const bool bDestroyReachedNoSession = bWasSuccessful && !bSessionStillExists;
	// 状态机决策会让普通 Destroy 回到 Idle；所有后续事件必须使用回调入口快照，
	// 不能在 Blueprint 重入后从 Machine 读取另一个 generation 的根类型或 OperationId。
	const EFU_OperationKind RootKind = Machine.Get().RootKind;
	const FGuid OperationId = Machine.Get().ActiveOperationId;
	const EFU_PendingOperation CompletedOperation = State.PendingOperation;
	Machine.HandleOriginalCompletion(
		Generation,
		EFU_OperationKind::Destroy,
		bDestroyReachedNoSession,
		bSessionStillExists);

	FU_ClearOperationWatchdog<Provider>();
	FU_ClearDestroyDelegate<Provider>();
	State.PendingOperation = EFU_PendingOperation::None;

	if (!bDestroyReachedNoSession)
	{
		// 链式参数必须先于诊断清理；诊断监听器此时可合法启动下一个 Idle 请求。
		if (CompletedOperation == EFU_PendingOperation::Create)
		{
			//销毁失败后创建链终止
			State.PendingCreateRoomName.Reset();
			State.PendingCreateRoomPassword.Reset();
			State.PendingMaxPlayers = 0;
		}
		else if (CompletedOperation == EFU_PendingOperation::Join)
		{
			//销毁失败后加入链终止，原搜索结果不能继续使用
			State.PendingJoinResult.Reset();
		}

		// Lease release 自身也会同步公开诊断，所以它必须位于旧链式参数清理之后。
		// 失败回调仅在接口有效且明确 NoSession 时回收；未知/仍存在时继续保守持租。
		if (State.SessionInterface.IsValid() && !bSessionStillExists)
		{
			if (ActiveGameplayProvider.IsSet() && ActiveGameplayProvider.GetValue() == Provider)
			{
				ActiveGameplayProvider.Reset();
			}
			FU_RequestNetDriverLeaseRelease(Provider, TEXT("DestroySession callback failed but named session is gone"));
		}

		if (CompletedOperation == EFU_PendingOperation::None)
		{
			OnDestroySessionComplete.Broadcast(Provider, false);
		}
		else if (CompletedOperation == EFU_PendingOperation::Create)
		{
			OnCreateSessionCompleteV2.Broadcast(Provider, false);
		}
		else
		{
			OnJoinSessionCompleteV2.Broadcast(Provider, EFU_JoinSessionResult::UnknownError);
		}

		// 完成委托之后只发布带快照 ID 的只读诊断，不再改变本 Provider 状态。
		FU_EmitDiagnostic<Provider>(
			RootKind,
			OperationId,
			EFU_OnlineDiagnosticPhase::Callback,
			EFU_OnlineDiagnosticSeverity::Warning,
			TEXT("FU.DestroySession.Failed"),
			TEXT("Destroy 回调未能确认命名会话已移除"));

		return;
	}

	//销毁成功后，旧游戏会话已经不再有效
	if (ActiveGameplayProvider.IsSet() && ActiveGameplayProvider.GetValue() == Provider)
	{
		ActiveGameplayProvider.Reset();
	}

	if (CompletedOperation == EFU_PendingOperation::Create)
	{
		// 先让续步 Reserve 新 generation；随后再公开旧 Destroy 诊断，监听器只能看到 Busy，
		// 因而不能插入新请求并覆盖 Create 的 pending 或 NetDriver 租约。
		FU_CreateSessionInternal<Provider>();
	}
	else if (CompletedOperation == EFU_PendingOperation::Join)
	{
		// Join 续步采用与 Create 相同的 reserve-before-diagnostic 原子边界。
		FU_JoinSessionInternal<Provider>();
	}
	else
	{
		// 显式 Destroy 没有续步；租约归还必须发生在首个 Blueprint 可见事件之前。
		FU_RequestNetDriverLeaseRelease(Provider, TEXT("Explicit DestroySession completed"));
	}

	if (CompletedOperation == EFU_PendingOperation::None)
	{
		//只有玩家显式销毁时才广播销毁完成；之后本栈帧只发布旧 OperationId 的只读诊断。
		OnDestroySessionComplete.Broadcast(Provider, true);
	}

	FU_EmitDiagnostic<Provider>(
		RootKind,
		OperationId,
		EFU_OnlineDiagnosticPhase::Callback,
		EFU_OnlineDiagnosticSeverity::Info,
		TEXT("FU.DestroySession.Completed"),
		TEXT("Destroy 回调确认命名会话已移除"));
}

//实现创建房间入口
template<EFU_OnlineProvider Provider>
void UFU_OnlineSessionSubsystem::FU_CreateSession(const int32 MaxPlayers, const FString& RoomName, const FString& RoomPassword)
{
	// 公共入口第一件事就是分配尝试身份；后续 Busy/参数/环境拒绝都拥有独立 OperationId。
	FFU_OperationTicket RootTicket = FU_BeginOperationAttempt<Provider>(EFU_OperationKind::Create);
	FFU_OnlineProviderState& State = FU_GetProviderState<Provider>();
	FFU_OnlineOperationStateMachine& Machine = FU_GetOperationMachine<Provider>();

	// 【诊断 Blueprint 重入】Requested 会同步广播。若监听器在广播内启动了同 Provider
	// 操作，外层请求必须先按 Busy 拒绝，且绝不能继续覆盖新操作的 pending/SessionInterface。
	if (Machine.Get().Phase != EFU_OperationPhase::Idle)
	{
		FU_EmitDiagnostic<Provider>(RootTicket.Kind, RootTicket.OperationId, EFU_OnlineDiagnosticPhase::Preflight,
			EFU_OnlineDiagnosticSeverity::Warning, TEXT("FU.Operation.Rejected.Busy"),
			TEXT("当前 Provider 已有 OSS 操作在途；拒绝新的 Create 请求"));
		OnCreateSessionCompleteV2.Broadcast(Provider, false);
		return;
	}

	// Requested 监听器也可能只发起一个随后被参数检查拒绝的请求；此时槽位仍为 Idle，
	// 但 AttemptSequence 已前进。只读票据门可区分这种情况，避免旧调用继续取得异步槽位。
	if (!Machine.CanAcceptAttempt(RootTicket))
	{
		FU_EmitDiagnostic<Provider>(RootTicket.Kind, RootTicket.OperationId, EFU_OnlineDiagnosticPhase::Preflight,
			EFU_OnlineDiagnosticSeverity::Warning, TEXT("FU.Operation.Rejected.StateChanged"),
			TEXT("Create Requested 诊断返回后票据已被重入请求取代"));
		OnCreateSessionCompleteV2.Broadcast(Provider, false);
		return;
	}

	//一个游戏实例只能有一种活动游戏会话
	//如果另一种Provider已经处于游戏会话中，要求先显式销毁
	if (ActiveGameplayProvider.IsSet() && ActiveGameplayProvider.GetValue() != Provider)
	{
		FU_EmitDiagnostic<Provider>(RootTicket.Kind, RootTicket.OperationId, EFU_OnlineDiagnosticPhase::Preflight,
			EFU_OnlineDiagnosticSeverity::Warning, TEXT("FU.Operation.Rejected.ProviderConflict"),
			TEXT("另一在线提供方仍有活动游戏会话；拒绝覆盖当前会话"));
		OnCreateSessionCompleteV2.Broadcast(Provider, false);
		return;
	}

	if (!FFU_SessionRequestValidation::CanCreate(MaxPlayers, RoomName))
	{
		FU_EmitDiagnostic<Provider>(RootTicket.Kind, RootTicket.OperationId, EFU_OnlineDiagnosticPhase::Preflight,
			EFU_OnlineDiagnosticSeverity::Warning, TEXT("FU.Operation.Rejected.InvalidRequest"),
			TEXT("创建请求的玩家数或房间名无效"));
		OnCreateSessionCompleteV2.Broadcast(Provider, false);
		return;
	}

	// 【FU 修复：Runtime 自我保护】蓝图即使没有先检查状态，
	// Steam 未登录、子系统缺失或驱动不可用时也不会进入 CreateSession 异步流程。
	if (!FU_ValidateProviderReady<Provider>(RootTicket.Kind, RootTicket.OperationId, TEXT("CreateSession"), true, [this]()
	{
		OnCreateSessionCompleteV2.Broadcast(Provider, false);
	}))
	{
		return;
	}

	// Ready 事件与 Requested 一样公开给 Blueprint。通过预检并不等于外层票据仍然最新；
	// 必须在缓存接口和请求参数前再次验证，确保重入请求拥有的共享状态不会被旧调用覆盖。
	if (!Machine.CanAcceptAttempt(RootTicket))
	{
		FU_EmitDiagnostic<Provider>(RootTicket.Kind, RootTicket.OperationId, EFU_OnlineDiagnosticPhase::Preflight,
			EFU_OnlineDiagnosticSeverity::Warning, TEXT("FU.Operation.Rejected.StateChanged"),
			TEXT("Create Ready 诊断返回后票据已被重入请求取代；未修改共享创建状态"));
		OnCreateSessionCompleteV2.Broadcast(Provider, false);
		return;
	}

	State.SessionInterface = FU_GetSessionInterface<Provider>();

	if (!State.SessionInterface.IsValid())
	{
		FU_EmitDiagnostic<Provider>(RootTicket.Kind, RootTicket.OperationId, EFU_OnlineDiagnosticPhase::Preflight,
			EFU_OnlineDiagnosticSeverity::Error, TEXT("FU.Provider.SessionInterfaceUnavailable"),
			TEXT("状态预检后无法取得 Session Interface"));
		OnCreateSessionCompleteV2.Broadcast(Provider, false);
		return;
	}

	//密码不应Trim；
	State.PendingCreateRoomName = RoomName.TrimStartAndEnd();
	State.PendingCreateRoomPassword = RoomPassword;
	State.PendingMaxPlayers = MaxPlayers;

	//待销毁回调继续创建
	if (!FU_DestroyExistingSessionForPendingOperation<Provider>(EFU_PendingOperation::Create, RootTicket))
	{
		FU_CreateSessionInternal<Provider>(&RootTicket);
	}
}

//Creating应该由FU_CreateSessionInternal()设置
template<EFU_OnlineProvider Provider>
void UFU_OnlineSessionSubsystem::FU_CreateSessionInternal(FFU_OperationTicket* RootTicket)
{
    using FProviderTraits = TFU_OnlineSessionProviderTraits<Provider>;

    FFU_OnlineProviderState& State = FU_GetProviderState<Provider>();

	FFU_OnlineOperationStateMachine& Machine = FU_GetOperationMachine<Provider>();
	// 链式预销毁回调不再持有栈上 Ticket；状态机内的 RootKind/OperationId 是唯一续步身份。
	FFU_OperationTicket ContinuationTicket;
	if (RootTicket == nullptr)
	{
		ContinuationTicket.Kind = Machine.Get().RootKind;
		ContinuationTicket.OperationId = Machine.Get().ActiveOperationId;
		ContinuationTicket.AttemptSequence = Machine.Get().AttemptSequence;
		ContinuationTicket.bAccepted = true;
		RootTicket = &ContinuationTicket;
	}

	// 销毁完成回调必须先让纯状态机回到 Idle；Busy 防御分支绝不清参数或释放在途租约。
    if (Machine.Get().Phase != EFU_OperationPhase::Idle)
    {
		// 防御性 Busy 拒绝绝不能释放租约：该租约可能属于正在进行中的同 Provider 异步操作。
		FU_EmitDiagnostic<Provider>(RootTicket->Kind, RootTicket->OperationId, EFU_OnlineDiagnosticPhase::Preflight,
			EFU_OnlineDiagnosticSeverity::Warning, TEXT("FU.Operation.Rejected.Busy"), TEXT("Create 续步时状态机仍处于 Busy"));
        OnCreateSessionCompleteV2.Broadcast(Provider, false);
        return;
    }

	// 续步在上一个 Destroy 回调中已经拥有根 OperationId，但状态机暂时回到 Idle。
	// 在读取任何可变前置资源前先无广播 Reserve：后续所有失败都能用 generation 精确终结根请求，
	// 同时 PrepareGameNetDriver 的 Lease 诊断发生时槽位始终保持 Submitted。
	uint64 Generation = 0;
	if (!FU_ReserveOperation<Provider>(*RootTicket, EFU_OperationKind::Create, Generation))
	{
		FU_EmitDiagnostic<Provider>(RootTicket->Kind, RootTicket->OperationId, EFU_OnlineDiagnosticPhase::Preflight,
			EFU_OnlineDiagnosticSeverity::Warning, TEXT("FU.Operation.Rejected.StateChanged"), TEXT("Create 保留操作槽位前状态已变化"));
		OnCreateSessionCompleteV2.Broadcast(Provider, false);
		return;
	}

    APlayerController* PlayerController = FU_GetLocalPlayerController();

    if (!State.SessionInterface.IsValid() || !PlayerController || !PlayerController->GetLocalPlayer())
    {
		const EFU_OperationAction Actions = Machine.HandleSynchronousReject(Generation);
		FU_ClearOperationWatchdog<Provider>();
		FU_ClearCreateDelegate<Provider>();
        //创建无法开始时，等待参数已经没有继续保留的意义
        State.PendingCreateRoomName.Reset();
        State.PendingCreateRoomPassword.Reset();
        State.PendingMaxPlayers = 0;
		// 链式预销毁可能沿用旧会话租约；前置条件失败后不会再创建 Session，必须请求安全归还。
		if (EnumHasAnyFlags(Actions, EFU_OperationAction::RequestLeaseRelease))
		{
			FU_RequestNetDriverLeaseRelease(Provider, TEXT("CreateSession prerequisites unavailable"));
		}
		FU_EmitDiagnostic<Provider>(RootTicket->Kind, RootTicket->OperationId, EFU_OnlineDiagnosticPhase::Preflight,
			EFU_OnlineDiagnosticSeverity::Error, TEXT("FU.Operation.PrerequisitesUnavailable"), TEXT("Create 所需本地玩家或 Session Interface 不可用"));
		if (EnumHasAnyFlags(Actions, EFU_OperationAction::BroadcastFailure))
		{
			OnCreateSessionCompleteV2.Broadcast(Provider,false);
		}
        return;
    }
	// 后续 Lease/Submitted 诊断会同步进入 Blueprint；只保存纯值 LocalUserNum，绝不能在
	// 诊断返回后再次解引用可能已被 RemovePlayer/地图切换销毁的 PlayerController 或 LocalPlayer。
	const int32 LocalUserNum = PlayerController->GetLocalPlayer()->GetControllerId();

    FOnlineSessionSettings Settings;

    //Traits在编译期为Steam与NULL写入不同配置
    FProviderTraits::ConfigureCreateSettings(Settings);

    Settings.NumPublicConnections = State.PendingMaxPlayers;

	// 统一元数据辅助会保留必要房间名，并省略 Steam 不会发布的空密码值，避免无意义启动警告。
	FUOnlineSessionProviderTraitsPrivate::ConfigureRoomMetadata(
		Settings,
		State.PendingCreateRoomName,
		State.PendingCreateRoomPassword);

	// 【诊断重入边界】先由状态机保留 generation，再执行会发出 Lease 诊断的 Acquire。
	// 这样 Blueprint 在 Acquire 事件中再次调用 Create/Join/Destroy 时只能得到 Busy，不能推进一个
	// 可接收的新操作并让旧调用遗留租约或覆盖 pending。实际 OSS delegate 仍在 Prepare 成功后绑定。
	if (!FU_PrepareGameNetDriver<Provider>(RootTicket->Kind, RootTicket->OperationId))
	{
		if (!Machine.IsExpectedCallback(Generation, EFU_OperationKind::Create))
		{
			return;
		}

		const EFU_OperationAction Actions = Machine.HandleSynchronousReject(Generation);
		FU_ClearOperationWatchdog<Provider>();
		FU_ClearCreateDelegate<Provider>();
		State.PendingCreateRoomName.Reset();
		State.PendingCreateRoomPassword.Reset();
		State.PendingMaxPlayers = 0;
		if (EnumHasAnyFlags(Actions, EFU_OperationAction::RequestLeaseRelease))
		{
			FU_RequestNetDriverLeaseRelease(Provider, TEXT("CreateSession NetDriver preparation failed"));
		}
		FU_EmitDiagnostic<Provider>(RootTicket->Kind, RootTicket->OperationId, EFU_OnlineDiagnosticPhase::Preflight,
			EFU_OnlineDiagnosticSeverity::Error, TEXT("FU.NetDriver.PreparationFailed"), TEXT("Create 前无法准备目标 NetDriver"));
		if (EnumHasAnyFlags(Actions, EFU_OperationAction::BroadcastFailure))
		{
			OnCreateSessionCompleteV2.Broadcast(Provider, false);
		}
		return;
	}

    //Provider被编译进回调函数地址，回调时不会丢失来源
	State.CreateDelegateHandle = State.SessionInterface->AddOnCreateSessionCompleteDelegate_Handle(
		FOnCreateSessionCompleteDelegate::CreateUObject(
			this,
			&ThisClass::FU_OnCreateSessionComplete<Provider>,
			Generation));
	FU_ArmOperationWatchdog<Provider>(Generation);
	FU_EmitOperationSubmitted<Provider>(EFU_OperationKind::Create, Generation);

    //返回false表示创建请求没有启动，不会收到异步完成回调
	if (!State.SessionInterface->CreateSession(LocalUserNum, NAME_GameSession, Settings)
		&& Machine.IsExpectedCallback(Generation, EFU_OperationKind::Create))
    {
		const FGuid OperationId = Machine.Get().ActiveOperationId;
		const EFU_OperationAction Actions = Machine.HandleSynchronousReject(Generation);
		FU_ClearOperationWatchdog<Provider>();
        FU_ClearCreateDelegate<Provider>();

		// 状态机已回到 Idle；本 generation 的共享数据和租约必须在第一个 Blueprint 可见事件前收敛。
		// 否则监听器发起的新 Create 会被旧栈帧随后 Reset，形成难以复现的“成功提交但参数消失”。
        State.PendingCreateRoomName.Reset();
        State.PendingCreateRoomPassword.Reset();
        State.PendingMaxPlayers = 0;
		// OSS 返回 false 表示不会再回调；保留租约会永久污染后续 Provider 选择。
		if (EnumHasAnyFlags(Actions, EFU_OperationAction::RequestLeaseRelease))
		{
			FU_RequestNetDriverLeaseRelease(Provider, TEXT("CreateSession synchronous rejection"));
		}
		FU_EmitDiagnostic<Provider>(
			EFU_OperationKind::Create, OperationId,
			EFU_OnlineDiagnosticPhase::Completed, EFU_OnlineDiagnosticSeverity::Warning,
			TEXT("FU.Operation.SynchronousReject"), TEXT("CreateSession 同步返回 false；已按 generation 精确清理"));

		if (EnumHasAnyFlags(Actions, EFU_OperationAction::BroadcastFailure))
		{
			OnCreateSessionCompleteV2.Broadcast(Provider, false);
		}
    }
}

//实现创建完成回调
template<EFU_OnlineProvider Provider>
void UFU_OnlineSessionSubsystem::FU_OnCreateSessionComplete(
	const FName SessionName,
	const bool bWasSuccessful,
	const uint64 Generation)
{
	FFU_OnlineProviderState& State = FU_GetProviderState<Provider>();
	FFU_OnlineOperationStateMachine& Machine = FU_GetOperationMachine<Provider>();
	if (SessionName != NAME_GameSession
		|| !Machine.IsExpectedCallback(Generation, EFU_OperationKind::Create))
	{
		return;
	}

	const bool bWasRecovering = Machine.Get().Phase == EFU_OperationPhase::Recovering;
	const bool bSessionStillExists = State.SessionInterface.IsValid()
		&& State.SessionInterface->GetNamedSession(NAME_GameSession) != nullptr;
	const bool bOverallSucceeded = bWasSuccessful && bSessionStillExists;
	const EFU_OperationKind RootKind = Machine.Get().RootKind;
	const FGuid OperationId = Machine.Get().ActiveOperationId;
	// 必须在状态 decision 与同步诊断之前捕获，否则 Blueprint 重入安装的新 Handle 会被误认为旧资源。
	const FFU_OnlineCallbackResourceSnapshot ResourceSnapshot = FU_CaptureCallbackResources<Provider>();
	const EFU_OperationAction Actions = Machine.HandleOriginalCompletion(
		Generation,
		EFU_OperationKind::Create,
		bOverallSucceeded,
		bSessionStillExists);
	if (bWasRecovering)
	{
		const FFU_OnlineDiagnosticEvent Event = FFU_OnlineOperationPathDiagnostics::BuildLateCallback(
			Provider,
			EFU_OnlineDiagnosticOperation::CreateSession,
			OperationId,
			bOverallSucceeded,
			Actions);
		FFU_OnlineOperationPathDispatchSinks Sinks;
		Sinks.Diagnostic = [this](const FFU_OnlineDiagnosticEvent& InEvent)
		{
			FU_EmitDiagnostic<Provider>(InEvent);
		};
		Sinks.CurrentGeneration = [&Machine]() { return Machine.Get().ActiveGeneration; };
		Sinks.SharedResourcesStillMatch = [this, ResourceSnapshot, Actions]()
		{
			return FU_AreCapturedCallbackResourcesCurrent<Provider>(ResourceSnapshot, EFU_OperationKind::Create, Actions);
		};
		Sinks.ExactOldResourceCleanup = [this, ResourceSnapshot, Actions]()
		{
			FU_ClearCapturedCallbackResources(ResourceSnapshot, EFU_OperationKind::Create, Actions);
		};
		Sinks.GenerationMatchedCleanup = [this, ResourceSnapshot, Actions]()
		{
			FU_ResetMatchedCallbackResources<Provider>(ResourceSnapshot, EFU_OperationKind::Create, Actions);
			if (EnumHasAnyFlags(Actions, EFU_OperationAction::RequestLeaseRelease))
			{
				FU_RequestNetDriverLeaseRelease(Provider, TEXT("CreateSession late callback reached NoSession"));
			}
		};
		Sinks.RecoveryDestroySubmission = [this]() { FU_BeginRecoveryDestroy<Provider>(false); };
		// 真实旧完成 sink 被显式交给策略，但内部 dispatcher 永不调用；测试会观测其计数始终为 0。
		Sinks.LegacyCompletion = [this, bOverallSucceeded]()
		{
			OnCreateSessionCompleteV2.Broadcast(Provider, bOverallSucceeded);
		};
		FFU_OnlineOperationPathDispatcher::DispatchInternal(
			Event,
			Generation,
			EnumHasAnyFlags(Actions, EFU_OperationAction::StartRecoveryDestroy),
			Sinks);
		return;
	}

	FU_ClearOperationWatchdog<Provider>();
	FU_ClearCreateDelegate<Provider>();

	// 本次参数必须在任何 Lease 诊断、完成诊断或 Blueprint 委托以前清除。状态机此时可能已
	// 回到 Idle，后续任一可观察事件都允许合法重入并写入下一次 Create 的 pending。
	State.PendingCreateRoomName.Reset();
	State.PendingCreateRoomPassword.Reset();
	State.PendingMaxPlayers = 0;

	if (!bWasRecovering && bOverallSucceeded)
	{
		//只有创建真正成功后，才能记录活动游戏会话来源
		ActiveGameplayProvider = Provider;
	}
	else if (EnumHasAnyFlags(Actions, EFU_OperationAction::RequestLeaseRelease))
	{
		// 创建失败时不会产生 Listen NetDriver，立即/延迟恢复都由全进程安全扫描决定。
		FU_RequestNetDriverLeaseRelease(Provider, TEXT("CreateSession callback failure"));
	}

	if (!bWasRecovering)
	{
		// 与 Find/Join 一致，先交付旧 API 完成，再发只读诊断；诊断监听器即使启动下一请求，
		// 旧 Create 也不会随后再向 UI 投递一个无 OperationId 的完成结果。
		OnCreateSessionCompleteV2.Broadcast(Provider, bOverallSucceeded);
		FU_EmitDiagnostic<Provider>(
			RootKind,
			OperationId,
			EFU_OnlineDiagnosticPhase::Callback,
			bOverallSucceeded ? EFU_OnlineDiagnosticSeverity::Info : EFU_OnlineDiagnosticSeverity::Warning,
			bOverallSucceeded ? TEXT("FU.CreateSession.Completed") : TEXT("FU.CreateSession.Failed"),
			bOverallSucceeded ? TEXT("Create 回调确认命名会话存在") : TEXT("Create 回调未能确认命名会话存在"));
	}

	if (EnumHasAnyFlags(Actions, EFU_OperationAction::StartRecoveryDestroy))
	{
		FU_BeginRecoveryDestroy<Provider>(false);
	}
}

//实现搜索入口
template<EFU_OnlineProvider Provider>
void UFU_OnlineSessionSubsystem::FU_FindSessions(const FString& RoomName,const int32 MaxResults)
{
    using FProviderTraits = TFU_OnlineSessionProviderTraits<Provider>;

	FFU_OperationTicket RootTicket = FU_BeginOperationAttempt<Provider>(EFU_OperationKind::Find);
    FFU_OnlineProviderState& State = FU_GetProviderState<Provider>();
	FFU_OnlineOperationStateMachine& Machine = FU_GetOperationMachine<Provider>();

    //同一个Provider不能同时创建、搜索、加入或销毁
    if (Machine.Get().Phase != EFU_OperationPhase::Idle)
    {
		FU_EmitDiagnostic<Provider>(RootTicket.Kind, RootTicket.OperationId, EFU_OnlineDiagnosticPhase::Preflight,
			EFU_OnlineDiagnosticSeverity::Warning, TEXT("FU.Operation.Rejected.Busy"), TEXT("Find 请求被在途 OSS 操作拒绝"));
        OnFindSessionCompleteV2.Broadcast(Provider, TArray<FFU_SessionResult>{}, false);
        return;
    }

	// 【诊断 Blueprint 重入】只有槽位确认 Idle 后才检查票据新旧。若先调用 CanAcceptAttempt，
	// 正常 Busy 请求会被误归类为无原因失败，丢失联机排障最重要的 Busy 诊断。
	if (!Machine.CanAcceptAttempt(RootTicket))
	{
		FU_EmitDiagnostic<Provider>(RootTicket.Kind, RootTicket.OperationId, EFU_OnlineDiagnosticPhase::Preflight,
			EFU_OnlineDiagnosticSeverity::Warning, TEXT("FU.Operation.Rejected.StateChanged"),
			TEXT("Find Requested 诊断返回后票据已被重入请求取代"));
		OnFindSessionCompleteV2.Broadcast(Provider, TArray<FFU_SessionResult>{}, false);
		return;
	}

	// 【FU 修复：搜索前检查 Steam Identity】日志中的 FindSessions Started=true
	// 只表示调用被接口接收，不代表 Steam 用户已经具备在线搜索资格。
	// 在这里提前拒绝 NotLoggedIn，可避免随后只得到含义模糊的异步 false 和空 Results。
	// 搜索不会 Listen 或 ClientTravel，因此不能被另一 Provider 的进程级 NetDriver 租约阻止；
	// Steam 身份、AppID 与 OSS Session Interface 等搜索环境仍由同一 Evaluator 检查。
	if (!FU_ValidateProviderReady<Provider>(RootTicket.Kind, RootTicket.OperationId, TEXT("FindSessions"), false, [this]()
	{
		OnFindSessionCompleteV2.Broadcast(Provider, TArray<FFU_SessionResult>{}, false);
	}))
	{
		return;
	}

	// Ready 诊断同样会同步进入 Blueprint。只有票据仍是最新且槽位仍为空时，外层 Find
	// 才能清缓存并安装 SessionSearch；否则会覆盖重入请求已经绑定的 delegate/search。
	if (!Machine.CanAcceptAttempt(RootTicket))
	{
		FU_EmitDiagnostic<Provider>(
			RootTicket.Kind,
			RootTicket.OperationId,
			EFU_OnlineDiagnosticPhase::Preflight,
			EFU_OnlineDiagnosticSeverity::Warning,
			TEXT("FU.Operation.Rejected.StateChanged"),
			TEXT("Find 预检诊断返回后票据已被重入请求取代；外层调用未修改共享搜索状态"));
		OnFindSessionCompleteV2.Broadcast(Provider, TArray<FFU_SessionResult>{}, false);
		return;
	}

    APlayerController* PlayerController = FU_GetLocalPlayerController();

    State.SessionInterface = FU_GetSessionInterface<Provider>();

    if (!State.SessionInterface.IsValid() || !PlayerController || !PlayerController->GetLocalPlayer() || MaxResults <= 0)
    {
		FU_EmitDiagnostic<Provider>(RootTicket.Kind, RootTicket.OperationId, EFU_OnlineDiagnosticPhase::Preflight,
			EFU_OnlineDiagnosticSeverity::Error, TEXT("FU.Operation.PrerequisitesUnavailable"), TEXT("Find 所需接口、本地玩家或结果上限无效"));
        OnFindSessionCompleteV2.Broadcast(Provider, TArray<FFU_SessionResult>{}, false);
        return;
    }
	// Submitted 诊断是 Blueprint 可见边界；在它之前复制控制器 ID，避免诊断监听器移除
	// LocalPlayer 后，OSS 调用重新解引用旧 PlayerController 造成空指针或悬空访问。
	const int32 LocalUserNum = PlayerController->GetLocalPlayer()->GetControllerId();

    //搜索开始后
    State.CachedSearchResults.Reset();
    State.PendingJoinResult.Reset();
	// 每个公开 Find 都从项目关键字精确查询开始；只有该遍成功零结果时，Steam 特化才会切到一次性降级。
	State.bSteamFallbackFindInProgress = false;

    State.PendingFindRoomName = RoomName.TrimStartAndEnd();

    //FOnlineSessionSearch必须存放在State中，因为搜索是异步操作
    State.SessionSearch = MakeShared<FOnlineSessionSearch>();

    State.SessionSearch->MaxSearchResults = MaxResults;

    //Steam：平台在线搜索，并添加SEARCH_LOBBIES
    //NULL：局域网广播搜索，不添加SEARCH_LOBBIES
    FProviderTraits::ConfigureSearch(*State.SessionSearch);

	uint64 Generation = 0;
	if (!FU_ReserveOperation<Provider>(RootTicket, EFU_OperationKind::Find, Generation))
	{
		// Search 对象和筛选条件属于旧票据；必须在公开 StateChanged 以前清除，避免
		// 诊断监听器启动的新 Find 被这个已失败的外层栈帧重置。
		State.SessionSearch.Reset();
		State.PendingFindRoomName.Reset();
		State.bSteamFallbackFindInProgress = false;
		FU_EmitDiagnostic<Provider>(RootTicket.Kind, RootTicket.OperationId, EFU_OnlineDiagnosticPhase::Preflight,
			EFU_OnlineDiagnosticSeverity::Warning, TEXT("FU.Operation.Rejected.StateChanged"), TEXT("Find 提交前状态已变化"));
		OnFindSessionCompleteV2.Broadcast(Provider, TArray<FFU_SessionResult>{}, false);
		return;
	}

    //把Provider编译进回调地址
	const TSharedPtr<FOnlineSessionSearch> SubmittedSearch = State.SessionSearch;
	State.FindDelegateHandle = State.SessionInterface->AddOnFindSessionsCompleteDelegate_Handle(
		FOnFindSessionsCompleteDelegate::CreateUObject(
			this,
			&ThisClass::FU_OnFindSessionsComplete<Provider>,
			Generation,
			false,
			SubmittedSearch));
	FU_ArmOperationWatchdog<Provider>(Generation);
	FU_EmitOperationSubmitted<Provider>(EFU_OperationKind::Find, Generation);

	// Provider 返回 true 仍可能只是“已有外部搜索在途，忽略这次对象”；必须同时验证 SearchState。
	const bool bProviderReturnedStarted = State.SessionInterface->FindSessions(
		LocalUserNum,
		SubmittedSearch.ToSharedRef());

	// 同步 Provider 可能在 FindSessions 调用栈内完成第一遍。统一分类器先验证
	// generation/pass/Search 快照；如果内层已换成或完成 fallback，旧栈帧立即退出，
	// 只有所有身份仍属于 SubmittedSearch 时才能解释 Provider 返回值。
	const FUOnlineSessionProviderTraitsPrivate::EFU_SearchPostSubmitDisposition SubmitDisposition =
		FUOnlineSessionProviderTraitsPrivate::ClassifySearchPostSubmit(
			Machine.IsExpectedCallback(Generation, EFU_OperationKind::Find),
			Provider != EFU_OnlineProvider::Steam || !State.bSteamFallbackFindInProgress,
			bProviderReturnedStarted,
			SubmittedSearch,
			State.SessionSearch);
	if (SubmitDisposition
		== FUOnlineSessionProviderTraitsPrivate::EFU_SearchPostSubmitDisposition::CallbackAlreadyAdvancedState)
	{
		return;
	}

	const bool bFindStarted = SubmitDisposition
		== FUOnlineSessionProviderTraitsPrivate::EFU_SearchPostSubmitDisposition::AwaitCallback;

	// 【FU 修复：LAN/Steam 搜索诊断】区分“请求没有启动”和“启动后没有结果”。
	UE_LOG(
		LogFUOnlineSession,
		Display,
		TEXT("[%s] FindSessions Started=%s MaxResults=%d RoomName=\"%s\""),
		FProviderTraits::GetDebugName(),
		bFindStarted ? TEXT("true") : TEXT("false"),
		MaxResults,
		*State.PendingFindRoomName);

    if (!bFindStarted && Machine.IsExpectedCallback(Generation, EFU_OperationKind::Find))
    {
		const FGuid OperationId = Machine.Get().ActiveOperationId;
		const EFU_OperationAction Actions = Machine.HandleSynchronousReject(Generation);
		FU_ClearOperationWatchdog<Provider>();
        FU_ClearFindDelegate<Provider>();

		// HandleSynchronousReject 已把状态机恢复为 Idle。必须先清完本 generation 的共享字段，
		// 再公开诊断；否则诊断监听器启动的新 Find 会被下面的旧清理误删。
		State.SessionSearch.Reset();
		State.PendingFindRoomName.Reset();
		State.bSteamFallbackFindInProgress = false;
		FU_EmitDiagnostic<Provider>(
			EFU_OperationKind::Find, OperationId,
			EFU_OnlineDiagnosticPhase::Completed, EFU_OnlineDiagnosticSeverity::Warning,
			TEXT("FU.Operation.SynchronousReject"), TEXT("FindSessions 未让本次 Search 进入 InProgress；已按 generation 精确清理"));

		if (EnumHasAnyFlags(Actions, EFU_OperationAction::BroadcastFailure))
		{
			OnFindSessionCompleteV2.Broadcast(Provider, TArray<FFU_SessionResult>{}, false);
		}
    }
}

//实现搜索完成回调
template<EFU_OnlineProvider Provider>
void UFU_OnlineSessionSubsystem::FU_OnFindSessionsComplete(
	const bool bWasSuccessful,
	const uint64 Generation,
	const bool bSteamFallbackPass,
	const TSharedPtr<FOnlineSessionSearch> ExpectedSearch)
{
	using FProviderTraits = TFU_OnlineSessionProviderTraits<Provider>;

    FFU_OnlineProviderState& State = FU_GetProviderState<Provider>();
	FFU_OnlineOperationStateMachine& Machine = FU_GetOperationMachine<Provider>();
	// generation 保护不同公开操作，pass token 区分同一 Steam Find 的两遍，Search 终态则拦截
	// 接口级全局委托把取消后迟到的旧广播误投递给新委托；任一身份不匹配都必须绝对无副作用。
	const bool bSearchPassMatches =
		Provider != EFU_OnlineProvider::Steam
		|| State.bSteamFallbackFindInProgress == bSteamFallbackPass;
	if (!Machine.IsExpectedCallback(Generation, EFU_OperationKind::Find)
		|| !bSearchPassMatches
		|| !FUOnlineSessionProviderTraitsPrivate::IsExpectedSearchCompletion(ExpectedSearch, State.SessionSearch))
	{
		return;
	}
	const bool bWasRecovering = Machine.Get().Phase == EFU_OperationPhase::Recovering;
	const FGuid OperationId = Machine.Get().ActiveOperationId;
	const int32 RawResultCount =
		State.SessionSearch.IsValid()
			? State.SessionSearch->SearchResults.Num()
			: 0;
	bool bFallbackSubmissionRejected = false;

	if constexpr (Provider == EFU_OnlineProvider::Steam)
	{
		// 【Steam 双阶段发现】真实双机日志证明第一遍可“成功但 Raw=0”。此时状态机尚未消费
		// OriginalCompletion，先原地替换 OSS Search 与 delegate，并沿用同一 generation/OperationId。
		// Recovering 中绝不重试，因为 timeout/cancel 已经取得生命周期所有权。
		if (!bWasRecovering
			&& State.SessionSearch.IsValid()
			&& FProviderTraits::ShouldStartFallbackSearch(
				bWasSuccessful,
				RawResultCount,
				State.bSteamFallbackFindInProgress))
		{
			const int32 MaxSearchResults = State.SessionSearch->MaxSearchResults;
			const FString FallbackRoomName = State.PendingFindRoomName;
			const bool bRoomNameNotSpecified = FallbackRoomName.IsEmpty();
			FU_ClearOperationWatchdog<Provider>();
			FU_ClearFindDelegate<Provider>();

			State.bSteamFallbackFindInProgress = true;
			State.SessionSearch = MakeShared<FOnlineSessionSearch>();
			const TSharedPtr<FOnlineSessionSearch> FallbackSearch = State.SessionSearch;
			State.SessionSearch->MaxSearchResults = MaxSearchResults;
			FProviderTraits::ConfigureFallbackSearch(*State.SessionSearch, FallbackRoomName);

			APlayerController* const PlayerController = FU_GetLocalPlayerController();
			bool bFallbackStarted = false;
			if (State.SessionInterface.IsValid()
				&& PlayerController
				&& PlayerController->GetLocalPlayer())
			{
				State.FindDelegateHandle = State.SessionInterface->AddOnFindSessionsCompleteDelegate_Handle(
					FOnFindSessionsCompleteDelegate::CreateUObject(
						this,
						&ThisClass::FU_OnFindSessionsComplete<Provider>,
						Generation,
						true,
						FallbackSearch));
				FU_ArmOperationWatchdog<Provider>(Generation);
			const bool bProviderReturnedStarted = State.SessionInterface->FindSessions(
					PlayerController->GetLocalPlayer()->GetControllerId(),
					State.SessionSearch.ToSharedRef());
				// 真正的后续处理在下方统一分类；这里只保留 Provider 原始返回值。
				bFallbackStarted = bProviderReturnedStarted;
			}

			// 某些 Provider 在 FindSessions 调用栈内同步广播完成。同一分类器把“已完成”、
			// “已进入 InProgress”和“同步拒绝”分开，防止外层重复消费 generation。
			const FUOnlineSessionProviderTraitsPrivate::EFU_SearchPostSubmitDisposition FallbackDisposition =
				FUOnlineSessionProviderTraitsPrivate::ClassifySearchPostSubmit(
					Machine.IsExpectedCallback(Generation, EFU_OperationKind::Find),
					State.bSteamFallbackFindInProgress,
					bFallbackStarted,
					FallbackSearch,
					State.SessionSearch);
			if (FallbackDisposition
				== FUOnlineSessionProviderTraitsPrivate::EFU_SearchPostSubmitDisposition::CallbackAlreadyAdvancedState)
			{
				return;
			}
			bFallbackStarted = FallbackDisposition
				== FUOnlineSessionProviderTraitsPrivate::EFU_SearchPostSubmitDisposition::AwaitCallback;

			UE_LOG(
				LogFUOnlineSession,
				Display,
				TEXT("[Steam] FindSessions primary returned Raw=0; fallback Started=%s Mode=%s RoomName=\"%s\""),
				bFallbackStarted ? TEXT("true") : TEXT("false"),
				bRoomNameNotSpecified ? TEXT("ProjectProtocol") : TEXT("RoomName"),
				*FallbackRoomName);

			if (bFallbackStarted)
			{
				// 发出事件后不再访问共享 State；Blueprint 可能同步请求取消当前 Find。
				FU_EmitDiagnostic<Provider>(
					EFU_OperationKind::Find,
					OperationId,
					EFU_OnlineDiagnosticPhase::Submitted,
					EFU_OnlineDiagnosticSeverity::Info,
					TEXT("FU.FindSessions.Fallback.Submitted"),
					bRoomNameNotSpecified
						? TEXT("项目关键字查询零结果，已按备用项目协议提交一次 Steam Lobby 降级搜索")
						: TEXT("项目关键字查询零结果，已按独立房间名条件提交一次 Steam Lobby 降级搜索"));
				return;
			}

			// Provider 同步拒绝第二遍时不会产生回调；撤销刚注册的资源，再让第一遍成功零结果
			// 正常完成，避免把可恢复的“无匹配房间”错误升级成永久 Busy。
			FU_ClearOperationWatchdog<Provider>();
			FU_ClearFindDelegate<Provider>();
			// 恢复第一遍对象和 pass 标志，让后续完成/清理继续描述真正取得回调的请求。
			State.SessionSearch = ExpectedSearch;
			State.bSteamFallbackFindInProgress = false;
			bFallbackSubmissionRejected = true;
		}
	}

	const FFU_OnlineCallbackResourceSnapshot ResourceSnapshot = FU_CaptureCallbackResources<Provider>();
	const EFU_OperationAction Actions =
		Machine.HandleOriginalCompletion(Generation, EFU_OperationKind::Find, bWasSuccessful, false);
	if (bWasRecovering)
	{
		const FFU_OnlineDiagnosticEvent Event = FFU_OnlineOperationPathDiagnostics::BuildRecoveringFindOriginal(
			Provider,
			OperationId,
			bWasSuccessful,
			Actions);
		FFU_OnlineOperationPathDispatchSinks Sinks;
		Sinks.Diagnostic = [this](const FFU_OnlineDiagnosticEvent& InEvent)
		{
			FU_EmitDiagnostic<Provider>(InEvent);
		};
		Sinks.CurrentGeneration = [&Machine]() { return Machine.Get().ActiveGeneration; };
		Sinks.SharedResourcesStillMatch = [this, ResourceSnapshot, Actions]()
		{
			return FU_AreCapturedCallbackResourcesCurrent<Provider>(ResourceSnapshot, EFU_OperationKind::Find, Actions);
		};
		Sinks.ExactOldResourceCleanup = [this, ResourceSnapshot, Actions]()
		{
			FU_ClearCapturedCallbackResources(ResourceSnapshot, EFU_OperationKind::Find, Actions);
		};
		Sinks.GenerationMatchedCleanup = [this, ResourceSnapshot, Actions]()
		{
			FU_ResetMatchedCallbackResources<Provider>(ResourceSnapshot, EFU_OperationKind::Find, Actions);
		};
		Sinks.LegacyCompletion = [this]()
		{
			OnFindSessionCompleteV2.Broadcast(Provider, TArray<FFU_SessionResult>{}, false);
		};
		FFU_OnlineOperationPathDispatcher::DispatchInternal(Event, Generation, false, Sinks);
		return;
	}

	if (EnumHasAnyFlags(Actions, EFU_OperationAction::ClearWatchdog))
	{
		FU_ClearOperationWatchdog<Provider>();
	}
	if (EnumHasAnyFlags(Actions, EFU_OperationAction::ClearOriginalDelegate))
	{
		FU_ClearFindDelegate<Provider>();
	}
	if (EnumHasAnyFlags(Actions, EFU_OperationAction::ClearFindCancellationDelegate))
	{
		FU_ClearFindCancellationDelegate<Provider>();
	}
    TArray<FFU_SessionResult> BlueprintResults;
    State.CachedSearchResults.Reset();
	int32 ProjectRejectedCount = 0;
	int32 MissingRoomNameCount = 0;
	int32 RoomNameMismatchCount = 0;

    if (bWasSuccessful && State.SessionSearch.IsValid())
    {
        for (const FOnlineSessionSearchResult& SearchResult : State.SessionSearch->SearchResults)
        {
			if constexpr (Provider == EFU_OnlineProvider::Steam)
			{
				// 第一遍由后端过滤，第二遍必须在本地再次建立项目隔离；统一执行可防止
				// Provider 某次忽略查询条件时把 AppID 480 的其他游戏结果交给 Blueprint。
				if (!FProviderTraits::IsCurrentProjectSession(SearchResult.Session.SessionSettings))
				{
					++ProjectRejectedCount;
					continue;
				}
			}

            FString FoundRoomName;
        	FString FoundPassword;

            //没有插件房间名标记的Session不属于本插件创建的房间
            const bool bHasRoomName = SearchResult.Session.SessionSettings.Get( FUOnlineSession::RoomNameSetting, FoundRoomName);
        	const bool bHasPasswordSetting = SearchResult.Session.SessionSettings.Get(FUOnlineSession::RoomPasswordSetting,FoundPassword);
        	
        	
            // 房间名是识别本插件房间的必要标记；空密码元数据可能被部分 Provider 省略，
            // 因此不能因为没有读取到密码字段就把公开房间从搜索结果中过滤掉。
            if (!bHasRoomName)
            {
				++MissingRoomNameCount;
                continue;
            }

            //输入空房间名表示显示全部本插件房间；
            //非空时才执行精确名称过滤
            if (!State.PendingFindRoomName.IsEmpty() && FoundRoomName != State.PendingFindRoomName)
            {
				++RoomNameMismatchCount;
                continue;
            }

            FFU_SessionResult& BlueprintResult = BlueprintResults.AddDefaulted_GetRef();

            //这是防止把Steam搜索结果交给NULL加入函数的关键字段
            BlueprintResult.Provider = Provider;

            BlueprintResult.SessionId = SearchResult.GetSessionIdStr();

            BlueprintResult.RoomName = FoundRoomName;

            BlueprintResult.MaxPlayers = SearchResult.Session.SessionSettings.NumPublicConnections;

            BlueprintResult.CurrentPlayers = FMath::Clamp(BlueprintResult.MaxPlayers - SearchResult.Session.NumOpenPublicConnections,0,BlueprintResult.MaxPlayers);

            BlueprintResult.PingInMs = SearchResult.PingInMs;
        	
        	BlueprintResult.bPasswordProtected = bHasPasswordSetting && !FoundPassword.IsEmpty();

            //蓝图结构只用于显示；加入时仍然需要完整的原生结果
            State.CachedSearchResults.Add(SearchResult);
        }
    }

    //原生结果已复制进缓存，本次搜索对象可以释放
	const FString CompletedRoomName = State.PendingFindRoomName;
    State.SessionSearch.Reset();
    State.PendingFindRoomName.Reset();
	State.bSteamFallbackFindInProgress = false;

	// Raw=0 表示 Provider 没发现会话；Raw>0 且 Filtered=0 表示全部被插件元数据/房间名规则过滤。
	UE_LOG(
		LogFUOnlineSession,
		Display,
		TEXT("[%s] FindSessions Complete Pass=%s Success=%s Raw=%d Filtered=%d"),
		FProviderTraits::GetDebugName(),
		bSteamFallbackPass ? TEXT("Fallback") : TEXT("Primary"),
		bWasSuccessful ? TEXT("true") : TEXT("false"),
		RawResultCount,
		BlueprintResults.Num());

	// 【可行动搜索诊断】成功空列表并不总是错误，但 Steam 两套独立项目键都返回零时，
	// 正是双机日志需要被显式标出的边界。拒绝计数还能区分“后端没返回”与“本地规则过滤完”。
	FFU_OnlineDiagnosticEvent CompletionEvent;
	CompletionEvent.OperationId = OperationId;
	CompletionEvent.Operation = EFU_OnlineDiagnosticOperation::FindSessions;
	CompletionEvent.Phase = EFU_OnlineDiagnosticPhase::Callback;
	CompletionEvent.Status = bWasSuccessful
		? (BlueprintResults.IsEmpty() ? TEXT("Empty") : TEXT("Succeeded"))
		: TEXT("Failed");
	const bool bSteamFallbackEmpty =
		Provider == EFU_OnlineProvider::Steam
		&& bSteamFallbackPass
		&& bWasSuccessful
		&& RawResultCount == 0;
	const bool bAllRawResultsFiltered =
		bWasSuccessful && RawResultCount > 0 && BlueprintResults.IsEmpty();
	if (bSteamFallbackEmpty)
	{
		CompletionEvent.Severity = EFU_OnlineDiagnosticSeverity::Warning;
		CompletionEvent.Code = TEXT("FU.FindSessions.Fallback.Empty");
		CompletionEvent.Message = TEXT("Steam 两条独立 Lobby 查询路径均成功，但都没有返回 Lobby");
		CompletionEvent.Cause = TEXT("Steam 未返回公开且可加入的 Lobby；可能是默认地域距离、房间已关闭或满员、或 Steam 索引暂不可见");
		CompletionEvent.RecommendedAction = TEXT("确认主机仍在线且有空位；核对两台电脑的 Steam 下载区域/公网区域；然后导出双方 FU 诊断报告");
	}
	else if (bAllRawResultsFiltered)
	{
		CompletionEvent.Severity = EFU_OnlineDiagnosticSeverity::Warning;
		CompletionEvent.Code = TEXT("FU.FindSessions.FilteredEmpty");
		CompletionEvent.Message = TEXT("Provider 返回了原始会话，但全部被插件项目或房间规则过滤");
		CompletionEvent.Cause = TEXT("结果缺少当前项目协议、缺少房间名，或房间名与本次精确查询不一致");
		CompletionEvent.RecommendedAction = TEXT("检查同一 OperationId 的过滤计数，并确认主机和客户端来自同一插件构建");
	}
	else
	{
		CompletionEvent.Severity = bWasSuccessful
			? EFU_OnlineDiagnosticSeverity::Info
			: EFU_OnlineDiagnosticSeverity::Warning;
		CompletionEvent.Code = bWasSuccessful
			? (bSteamFallbackPass ? TEXT("FU.FindSessions.Fallback.Completed") : TEXT("FU.FindSessions.Completed"))
			: TEXT("FU.FindSessions.Failed");
		CompletionEvent.Message = FString::Printf(
			TEXT("Find 回调完成 Pass=%s Raw=%d Filtered=%d"),
			bSteamFallbackPass ? TEXT("Fallback") : TEXT("Primary"),
			RawResultCount,
			BlueprintResults.Num());
		CompletionEvent.RecommendedAction = bWasSuccessful
			? TEXT("使用返回的 SessionId 继续 Join；空列表表示当前没有匹配房间")
			: TEXT("检查同一 OperationId 的 Preflight、Submitted、Timeout 与 Steam/NULL Provider 日志");
	}

	auto AddCompletionField = [&CompletionEvent](const TCHAR* Key, const FString& Value)
	{
		FFU_OnlineDiagnosticField& Field = CompletionEvent.Fields.AddDefaulted_GetRef();
		Field.Key = FName(Key);
		Field.Value = Value;
	};
	AddCompletionField(TEXT("SearchPass"), bSteamFallbackPass ? TEXT("Fallback") : TEXT("Primary"));
	AddCompletionField(TEXT("RawResults"), LexToString(RawResultCount));
	AddCompletionField(TEXT("FilteredResults"), LexToString(BlueprintResults.Num()));
	AddCompletionField(TEXT("ProjectRejected"), LexToString(ProjectRejectedCount));
	AddCompletionField(TEXT("MissingRoomName"), LexToString(MissingRoomNameCount));
	AddCompletionField(TEXT("RoomNameMismatch"), LexToString(RoomNameMismatchCount));
	if constexpr (Provider == EFU_OnlineProvider::Steam)
	{
		if (bSteamFallbackPass)
		{
			AddCompletionField(
				TEXT("FallbackFilter"),
				CompletedRoomName.IsEmpty() ? TEXT("ProjectProtocol") : TEXT("RoomName"));
		}
	}
	if (!CompletedRoomName.IsEmpty())
	{
		AddCompletionField(TEXT("RoomName"), CompletedRoomName);
	}
	// 【Find 缓存交付顺序】完成事件必须先于诊断事件广播。否则诊断 Blueprint 监听器若重入
	// 新 Find，会先清空本批 Native 缓存，而 UI 随后收到的 SessionId 将无法立即用于 Join。
    OnFindSessionCompleteV2.Broadcast(Provider, BlueprintResults, bWasSuccessful);
	// Fallback 拒绝也是诊断 Blueprint 事件，必须和总完成诊断一样位于公开 Find 完成之后；
	// 这样监听器重入的新搜索不会先于旧请求的无 ID 完成委托到达 UI。
	if (bFallbackSubmissionRejected)
	{
		FU_EmitDiagnostic<Provider>(
			EFU_OperationKind::Find,
			OperationId,
			EFU_OnlineDiagnosticPhase::Completed,
			EFU_OnlineDiagnosticSeverity::Warning,
			TEXT("FU.FindSessions.Fallback.SynchronousReject"),
			TEXT("Steam OSS 同步拒绝第二遍搜索；本次仍按第一遍成功零结果完成"));
	}
	FU_EmitDiagnostic<Provider>(MoveTemp(CompletionEvent));
	

/*
    bWasSuccessful	Results	含义
	false	空	搜索请求或在线服务失败
	true	空	搜索正常，但没有匹配房间
	true	非空	搜索正常，并找到房间
	
 */

}

//实现加入入口
template<EFU_OnlineProvider Provider>
void UFU_OnlineSessionSubsystem::FU_JoinSession(const FString& SessionId,const FString& RoomPasswordInput)
{
	FFU_OperationTicket RootTicket = FU_BeginOperationAttempt<Provider>(EFU_OperationKind::Join);
    FFU_OnlineProviderState& State = FU_GetProviderState<Provider>();
	FFU_OnlineOperationStateMachine& Machine = FU_GetOperationMachine<Provider>();

	// Requested 诊断允许 Blueprint 同步重入；先保护 Busy 槽位，再验证票据是否仍是最新尝试，
	// 这样旧 Join 绝不会清掉或替换新请求刚建立的 PendingJoinResult。
	if (Machine.Get().Phase != EFU_OperationPhase::Idle)
	{
		FU_EmitDiagnostic<Provider>(RootTicket.Kind, RootTicket.OperationId, EFU_OnlineDiagnosticPhase::Preflight,
			EFU_OnlineDiagnosticSeverity::Warning, TEXT("FU.Operation.Rejected.Busy"), TEXT("Join 请求被在途 OSS 操作拒绝"));
		OnJoinSessionCompleteV2.Broadcast(Provider, EFU_JoinSessionResult::UnknownError);
		return;
	}
	if (!Machine.CanAcceptAttempt(RootTicket))
	{
		FU_EmitDiagnostic<Provider>(RootTicket.Kind, RootTicket.OperationId, EFU_OnlineDiagnosticPhase::Preflight,
			EFU_OnlineDiagnosticSeverity::Warning, TEXT("FU.Operation.Rejected.StateChanged"),
			TEXT("Join Requested 诊断返回后票据已被重入请求取代"));
		OnJoinSessionCompleteV2.Broadcast(Provider, EFU_JoinSessionResult::UnknownError);
		return;
	}

    //一个GameInstance只能有一种活动游戏会话
    if (ActiveGameplayProvider.IsSet() && ActiveGameplayProvider.GetValue() != Provider)
    {
		FU_EmitDiagnostic<Provider>(RootTicket.Kind, RootTicket.OperationId, EFU_OnlineDiagnosticPhase::Preflight,
			EFU_OnlineDiagnosticSeverity::Warning, TEXT("FU.Operation.Rejected.ProviderConflict"), TEXT("另一在线提供方仍有活动游戏会话；拒绝 Join"));
        OnJoinSessionCompleteV2.Broadcast(Provider,EFU_JoinSessionResult::UnknownError);
        return;
    }

	// 【FU 修复：加入前重新验证环境】搜索完成到点击加入之间，Steam 可能掉线，
	// NetDriver 也可能因地图状态改变而产生冲突；使用同一模板检查可以保留 Provider 类型信息。
	if (!FU_ValidateProviderReady<Provider>(RootTicket.Kind, RootTicket.OperationId, TEXT("JoinSession"), true, [this]()
	{
		OnJoinSessionCompleteV2.Broadcast(Provider, EFU_JoinSessionResult::UnknownError);
	}))
	{
		return;
	}

	// Provider Ready 诊断返回后再次验证票据，避免监听器重入后旧 Join 清空新搜索结果。
	if (!Machine.CanAcceptAttempt(RootTicket))
	{
		FU_EmitDiagnostic<Provider>(RootTicket.Kind, RootTicket.OperationId, EFU_OnlineDiagnosticPhase::Preflight,
			EFU_OnlineDiagnosticSeverity::Warning, TEXT("FU.Operation.Rejected.StateChanged"),
			TEXT("Join Ready 诊断返回后票据已被重入请求取代；未修改共享加入状态"));
		OnJoinSessionCompleteV2.Broadcast(Provider, EFU_JoinSessionResult::UnknownError);
		return;
	}

    State.SessionInterface = FU_GetSessionInterface<Provider>();

    if (!State.SessionInterface.IsValid())
    {
		FU_EmitDiagnostic<Provider>(RootTicket.Kind, RootTicket.OperationId, EFU_OnlineDiagnosticPhase::Preflight,
			EFU_OnlineDiagnosticSeverity::Error, TEXT("FU.Provider.SessionInterfaceUnavailable"), TEXT("Join 无法取得 Session Interface"));
        OnJoinSessionCompleteV2.Broadcast(Provider,EFU_JoinSessionResult::UnknownError);
        return;
    }

    State.PendingJoinResult.Reset();

    bool bFoundSessionId = false;

    for (const FOnlineSessionSearchResult& SearchResult : State.CachedSearchResults)
    {
        if (SearchResult.GetSessionIdStr() != SessionId)
        {
            continue;
        }

        bFoundSessionId = true;

        FString FoundPassword;

        const bool bHasPasswordSetting = SearchResult.Session.SessionSettings.Get(FUOnlineSession::RoomPasswordSetting,FoundPassword);

        // 部分 Provider 可能不会保留空字符串设置；缺少该字段等价于“没有密码”。
        // 有密码的房间仍会读取到非空值，并在下面执行匹配检查。
        if (!bHasPasswordSetting)
        {
            FoundPassword.Reset();
        }

        if (FoundPassword != RoomPasswordInput)
        {
			// 密码绝不写入诊断；只公开验证失败这一安全结论和本次入口的 OperationId。
			FU_EmitDiagnostic<Provider>(RootTicket.Kind, RootTicket.OperationId, EFU_OnlineDiagnosticPhase::Preflight,
				EFU_OnlineDiagnosticSeverity::Warning, TEXT("FU.JoinSession.InvalidPassword"), TEXT("Join 密码验证失败"));
            OnJoinSessionCompleteV2.Broadcast(Provider,EFU_JoinSessionResult::InvalidPassword);
            return;
        }

        //不能只保存SessionId，JoinSession需要完整的原生搜索结果
        State.PendingJoinResult = SearchResult;
        break;
    }

    if (!bFoundSessionId || !State.PendingJoinResult.IsSet())
    {
		FU_EmitDiagnostic<Provider>(RootTicket.Kind, RootTicket.OperationId, EFU_OnlineDiagnosticPhase::Preflight,
			EFU_OnlineDiagnosticSeverity::Warning, TEXT("FU.JoinSession.SessionNotFound"), TEXT("缓存搜索结果中不存在请求的会话"));
        OnJoinSessionCompleteV2.Broadcast(Provider,EFU_JoinSessionResult::SessionDoesNotExist);
        return;
    }

    //已有同Provider Session时，销毁完成回调会继续执行加入
	if (!FU_DestroyExistingSessionForPendingOperation<Provider>(EFU_PendingOperation::Join, RootTicket))
    {
		FU_JoinSessionInternal<Provider>(&RootTicket);
    }
}

//实现内部加入函数
template<EFU_OnlineProvider Provider>
void UFU_OnlineSessionSubsystem::FU_JoinSessionInternal(FFU_OperationTicket* RootTicket)
{
	FFU_OnlineProviderState& State = FU_GetProviderState<Provider>();
	FFU_OnlineOperationStateMachine& Machine = FU_GetOperationMachine<Provider>();
	FFU_OperationTicket ContinuationTicket;
	if (RootTicket == nullptr)
	{
		ContinuationTicket.Kind = Machine.Get().RootKind;
		ContinuationTicket.OperationId = Machine.Get().ActiveOperationId;
		ContinuationTicket.AttemptSequence = Machine.Get().AttemptSequence;
		ContinuationTicket.bAccepted = true;
		RootTicket = &ContinuationTicket;
	}

	//销毁完成回调必须先把Destroying恢复成Idle
	if (Machine.Get().Phase != EFU_OperationPhase::Idle)
	{
		// Busy 防御分支绝不能清 PendingJoinResult；它可能正被已提交 Join 的 OSS 调用引用。
		FU_EmitDiagnostic<Provider>(RootTicket->Kind, RootTicket->OperationId, EFU_OnlineDiagnosticPhase::Preflight,
			EFU_OnlineDiagnosticSeverity::Warning, TEXT("FU.Operation.Rejected.Busy"), TEXT("Join 续步时状态机仍处于 Busy"));
		OnJoinSessionCompleteV2.Broadcast(Provider, EFU_JoinSessionResult::UnknownError);
		return;
	}

	// 与 Create 一致，链式 Join 必须先把根请求续成独立 Join generation。Reserve 本身不发
	// Blueprint 事件，因而既能覆盖前置失败，也能在 Lease Acquire 诊断期间封闭共享状态。
	uint64 Generation = 0;
	if (!FU_ReserveOperation<Provider>(*RootTicket, EFU_OperationKind::Join, Generation))
	{
		FU_EmitDiagnostic<Provider>(RootTicket->Kind, RootTicket->OperationId, EFU_OnlineDiagnosticPhase::Preflight,
			EFU_OnlineDiagnosticSeverity::Warning, TEXT("FU.Operation.Rejected.StateChanged"), TEXT("Join 保留操作槽位前状态已变化"));
		OnJoinSessionCompleteV2.Broadcast(Provider, EFU_JoinSessionResult::UnknownError);
		return;
	}

	APlayerController* PlayerController = FU_GetLocalPlayerController();

	if (!State.SessionInterface.IsValid() || !PlayerController || !PlayerController->GetLocalPlayer() || !State.PendingJoinResult.IsSet())
	{
		const EFU_OperationAction Actions = Machine.HandleSynchronousReject(Generation);
		FU_ClearOperationWatchdog<Provider>();
		FU_ClearJoinDelegate<Provider>();
		State.PendingJoinResult.Reset();
		if (EnumHasAnyFlags(Actions, EFU_OperationAction::RequestLeaseRelease))
		{
			FU_RequestNetDriverLeaseRelease(Provider, TEXT("JoinSession prerequisites unavailable"));
		}
		FU_EmitDiagnostic<Provider>(RootTicket->Kind, RootTicket->OperationId, EFU_OnlineDiagnosticPhase::Preflight,
			EFU_OnlineDiagnosticSeverity::Error, TEXT("FU.Operation.PrerequisitesUnavailable"), TEXT("Join 所需本地玩家、Session Interface 或搜索结果不可用"));
		if (EnumHasAnyFlags(Actions, EFU_OperationAction::BroadcastFailure))
		{
			OnJoinSessionCompleteV2.Broadcast(Provider,EFU_JoinSessionResult::UnknownError);
		}
		return;
	}
	// 只在任何 Lease/Submitted 诊断之前读取 LocalPlayer；之后 OSS 调用使用稳定整数，
	// 防止 Blueprint 监听器同步删除本地玩家后外层 Join 解引用失效对象。
	const int32 LocalUserNum = PlayerController->GetLocalPlayer()->GetControllerId();

	// 【FU 修复：Client 传输层与诊断重入】先保留 generation，使 Lease Acquire 诊断期间
	// 同 Provider 公共入口只能按 Busy 拒绝；Prepare 失败则用同一 generation 精确回滚状态机和租约。
	if (!FU_PrepareGameNetDriver<Provider>(RootTicket->Kind, RootTicket->OperationId))
	{
		if (!Machine.IsExpectedCallback(Generation, EFU_OperationKind::Join))
		{
			return;
		}

		const EFU_OperationAction Actions = Machine.HandleSynchronousReject(Generation);
		FU_ClearOperationWatchdog<Provider>();
		FU_ClearJoinDelegate<Provider>();
		State.PendingJoinResult.Reset();
		if (EnumHasAnyFlags(Actions, EFU_OperationAction::RequestLeaseRelease))
		{
			FU_RequestNetDriverLeaseRelease(Provider, TEXT("JoinSession NetDriver preparation failed"));
		}
		FU_EmitDiagnostic<Provider>(RootTicket->Kind, RootTicket->OperationId, EFU_OnlineDiagnosticPhase::Preflight,
			EFU_OnlineDiagnosticSeverity::Error, TEXT("FU.NetDriver.PreparationFailed"), TEXT("Join 前无法准备目标 NetDriver"));
		if (EnumHasAnyFlags(Actions, EFU_OperationAction::BroadcastFailure))
		{
			OnJoinSessionCompleteV2.Broadcast(Provider, EFU_JoinSessionResult::NetDriverUnavailable);
		}
		return;
	}

	//将Provider编译进回调函数地址
	State.JoinDelegateHandle = State.SessionInterface->AddOnJoinSessionCompleteDelegate_Handle(
		FOnJoinSessionCompleteDelegate::CreateUObject(
			this,
			&ThisClass::FU_OnJoinSessionComplete<Provider>,
			Generation));
	FU_ArmOperationWatchdog<Provider>(Generation);
	FU_EmitOperationSubmitted<Provider>(EFU_OperationKind::Join, Generation);

	//JoinSession会使用搜索阶段保存下来的完整原生结果
	if (!State.SessionInterface->JoinSession(LocalUserNum, NAME_GameSession, State.PendingJoinResult.GetValue())
		&& Machine.IsExpectedCallback(Generation, EFU_OperationKind::Join))
	{
		const FGuid OperationId = Machine.Get().ActiveOperationId;
		const EFU_OperationAction Actions = Machine.HandleSynchronousReject(Generation);
		FU_ClearOperationWatchdog<Provider>();
		FU_ClearJoinDelegate<Provider>();
		// 先清本次原生搜索结果并请求释放租约；从第一个 Blueprint 事件起不再写共享状态。
		State.PendingJoinResult.Reset();
		// 同步拒绝不会再触发 Join 回调，因此这里是本次租约唯一可靠的归还点。
		if (EnumHasAnyFlags(Actions, EFU_OperationAction::RequestLeaseRelease))
		{
			FU_RequestNetDriverLeaseRelease(Provider, TEXT("JoinSession synchronous rejection"));
		}
		FU_EmitDiagnostic<Provider>(
			EFU_OperationKind::Join, OperationId,
			EFU_OnlineDiagnosticPhase::Completed, EFU_OnlineDiagnosticSeverity::Warning,
			TEXT("FU.Operation.SynchronousReject"), TEXT("JoinSession 同步返回 false；已按 generation 精确清理"));

		if (EnumHasAnyFlags(Actions, EFU_OperationAction::BroadcastFailure))
		{
			OnJoinSessionCompleteV2.Broadcast(Provider,EFU_JoinSessionResult::UnknownError);
		}
	}
}

//实现加入完成回调模板函数
template<EFU_OnlineProvider Provider>
void UFU_OnlineSessionSubsystem::FU_OnJoinSessionComplete(
	const FName SessionName,
	const EOnJoinSessionCompleteResult::Type Result,
	const uint64 Generation)
{
    FFU_OnlineProviderState& State = FU_GetProviderState<Provider>();
	FFU_OnlineOperationStateMachine& Machine = FU_GetOperationMachine<Provider>();
	if (SessionName != NAME_GameSession
		|| !Machine.IsExpectedCallback(Generation, EFU_OperationKind::Join))
	{
		return;
	}
	const bool bWasRecovering = Machine.Get().Phase == EFU_OperationPhase::Recovering;

    EFU_JoinSessionResult BlueprintResult = EFU_JoinSessionResult::UnknownError;
	FString PendingConnectString;
	// ClientTravel 前不保留未经验证的裸 UObject 指针；即使地图/对象生命周期在回调栈中变化，
	// TWeakObjectPtr 也能让最终提交安全降级为失败，而不是解引用失效控制器。
	TWeakObjectPtr<APlayerController> PendingTravelController;

    switch (Result)
    {
    case EOnJoinSessionCompleteResult::Success:
    {
        if (!State.SessionInterface.IsValid())
        {
            BlueprintResult = EFU_JoinSessionResult::UnknownError;
            break;
        }

        FString ConnectString;

        if (!State.SessionInterface->GetResolvedConnectString(SessionName,ConnectString))
        {
            BlueprintResult = EFU_JoinSessionResult::CouldNotRetrieveAddress;
            break;
        }

		// 【安全边界】ResolvedConnectString 可含 Steam ticket、token 或原始旅行 URL；只记录稳定阶段，
		// 完整地址只作为 ClientTravel 的短生命周期输入，绝不进入任何可观测输出。
		UE_LOG(
			LogFUOnlineSession,
			Display,
			TEXT("[%s] JoinSession 已解析安全连接地址，准备 ClientTravel"),
			TFU_OnlineSessionProviderTraits<Provider>::GetDebugName());

        APlayerController* PlayerController = FU_GetLocalPlayerController();

        if (!PlayerController)
        {
            BlueprintResult = EFU_JoinSessionResult::UnknownError;
            break;
        }

		// 先完成所有可失败的后处理，再让状态机决定是否仍允许旅行；watchdog 已触发时即使 OSS Success
		// 也只能补偿 Destroy，绝不能在公开失败之后悄悄 ClientTravel。
		PendingConnectString = MoveTemp(ConnectString);
		PendingTravelController = PlayerController;
        BlueprintResult = EFU_JoinSessionResult::Success;
        break;
    }

    case EOnJoinSessionCompleteResult::SessionIsFull:
        BlueprintResult = EFU_JoinSessionResult::SessionIsFull;
        break;

    case EOnJoinSessionCompleteResult::SessionDoesNotExist:
        BlueprintResult = EFU_JoinSessionResult::SessionDoesNotExist;
        break;

    case EOnJoinSessionCompleteResult::CouldNotRetrieveAddress:
        BlueprintResult = EFU_JoinSessionResult::CouldNotRetrieveAddress;
        break;

    case EOnJoinSessionCompleteResult::AlreadyInSession:
        BlueprintResult = EFU_JoinSessionResult::AlreadyInSession;
        break;

    default:
        BlueprintResult = EFU_JoinSessionResult::UnknownError;
        break;
    	
    	//进入JoinSession()以后，密码检查已经结束，引擎回调无法知道密码是否存在错误
    	//所以这里枚举没有InvalidPassword
    	// BlueprintResult = EFU_JoinSessionResult::UnknownError;
    }

	const bool bSessionStillExists = State.SessionInterface.IsValid()
		&& State.SessionInterface->GetNamedSession(NAME_GameSession) != nullptr;
	bool bOverallSucceeded = BlueprintResult == EFU_JoinSessionResult::Success && bSessionStillExists;
	if (!bOverallSucceeded && BlueprintResult == EFU_JoinSessionResult::Success)
	{
		BlueprintResult = EFU_JoinSessionResult::UnknownError;
	}
	const EFU_OperationKind RootKind = Machine.Get().RootKind;
	const FGuid OperationId = Machine.Get().ActiveOperationId;
	const FFU_OnlineCallbackResourceSnapshot ResourceSnapshot = FU_CaptureCallbackResources<Provider>();
	bool bClientTravelStarted = false;

	if (!bWasRecovering && bOverallSucceeded)
	{
		// 【Join 终态原子性】此刻 Machine 仍为 Submitted，因此先写活动 Provider 并直接发起
		// ClientTravel；不能在 Travel 前广播 Blueprint 诊断，否则监听器可重入 Destroy，旧回调仍会
		// 使用过期地址旅行并把 ActiveGameplayProvider 写回。可观察事件统一放到状态提交之后。
		if (PendingTravelController.IsValid())
		{
			ActiveGameplayProvider = Provider;
			PendingTravelController->ClientTravel(PendingConnectString, ETravelType::TRAVEL_Absolute);
			bClientTravelStarted = true;
		}
		else
		{
			BlueprintResult = EFU_JoinSessionResult::UnknownError;
			bOverallSucceeded = false;
		}
	}
	const EFU_OperationAction Actions = Machine.HandleOriginalCompletion(
		Generation,
		EFU_OperationKind::Join,
		bOverallSucceeded,
		bSessionStillExists);
	if (bWasRecovering)
	{
		const FFU_OnlineDiagnosticEvent Event = FFU_OnlineOperationPathDiagnostics::BuildLateCallback(
			Provider,
			EFU_OnlineDiagnosticOperation::JoinSession,
			OperationId,
			bOverallSucceeded,
			Actions);
		FFU_OnlineOperationPathDispatchSinks Sinks;
		Sinks.Diagnostic = [this](const FFU_OnlineDiagnosticEvent& InEvent)
		{
			FU_EmitDiagnostic<Provider>(InEvent);
		};
		Sinks.CurrentGeneration = [&Machine]() { return Machine.Get().ActiveGeneration; };
		Sinks.SharedResourcesStillMatch = [this, ResourceSnapshot, Actions]()
		{
			return FU_AreCapturedCallbackResourcesCurrent<Provider>(ResourceSnapshot, EFU_OperationKind::Join, Actions);
		};
		Sinks.ExactOldResourceCleanup = [this, ResourceSnapshot, Actions]()
		{
			FU_ClearCapturedCallbackResources(ResourceSnapshot, EFU_OperationKind::Join, Actions);
		};
		Sinks.GenerationMatchedCleanup = [this, ResourceSnapshot, Actions]()
		{
			FU_ResetMatchedCallbackResources<Provider>(ResourceSnapshot, EFU_OperationKind::Join, Actions);
			if (EnumHasAnyFlags(Actions, EFU_OperationAction::RequestLeaseRelease))
			{
				FU_RequestNetDriverLeaseRelease(Provider, TEXT("JoinSession late callback reached NoSession"));
			}
		};
		Sinks.RecoveryDestroySubmission = [this]() { FU_BeginRecoveryDestroy<Provider>(false); };
		Sinks.LegacyCompletion = [this, BlueprintResult]()
		{
			OnJoinSessionCompleteV2.Broadcast(Provider, BlueprintResult);
		};
		Sinks.Travel = [PendingTravelController, PendingConnectString]()
		{
			if (PendingTravelController.IsValid())
			{
				PendingTravelController->ClientTravel(PendingConnectString, ETravelType::TRAVEL_Absolute);
			}
		};
		FFU_OnlineOperationPathDispatcher::DispatchInternal(
			Event,
			Generation,
			EnumHasAnyFlags(Actions, EFU_OperationAction::StartRecoveryDestroy),
			Sinks);
		return;
	}
	FU_ClearOperationWatchdog<Provider>();
	FU_ClearJoinDelegate<Provider>();

    // generation 已经终止后才能丢弃原始待加入结果；Busy/预检拒绝从不走到这里。
    State.PendingJoinResult.Reset();

	if (!bOverallSucceeded && EnumHasAnyFlags(Actions, EFU_OperationAction::RequestLeaseRelease))
	{
		// 只有状态机证明回调终止且没有残留 NamedSession 才可释放；后处理失败但 Session 存在会先补偿 Destroy。
		FU_RequestNetDriverLeaseRelease(Provider, TEXT("JoinSession callback failure"));
	}

	if (!bWasRecovering)
	{
		// 旧 Join 的 Blueprint 完成先交付，之后本栈帧只发带旧 OperationId 的只读诊断，
		// 不再写 pending、lease、ActiveProvider 或执行 Travel。
		OnJoinSessionCompleteV2.Broadcast(Provider,BlueprintResult);
		if (bClientTravelStarted)
		{
			// 保留稳定 Code 兼容已有 Blueprint 分支；Message 明确事件是在安全调用 ClientTravel 后发布。
			FU_EmitDiagnostic<Provider>(
				RootKind,
				OperationId,
				EFU_OnlineDiagnosticPhase::Callback,
				EFU_OnlineDiagnosticSeverity::Info,
				TEXT("FU.JoinSession.ClientTravelStarting"),
				TEXT("Join 已安全调用 ClientTravel；后续网络或旅行失败由独立事件报告"));
		}
		FU_EmitDiagnostic<Provider>(
			RootKind,
			OperationId,
			EFU_OnlineDiagnosticPhase::Callback,
			bOverallSucceeded ? EFU_OnlineDiagnosticSeverity::Info : EFU_OnlineDiagnosticSeverity::Warning,
			bOverallSucceeded ? TEXT("FU.JoinSession.Completed") : TEXT("FU.JoinSession.Failed"),
			bOverallSucceeded ? TEXT("Join 回调已成功启动 ClientTravel") : TEXT("Join 回调未能完成会话或旅行前置"));
	}
	if (EnumHasAnyFlags(Actions, EFU_OperationAction::StartRecoveryDestroy))
	{
		// Submitted Join 的后处理失败可在本回调内首次转入 Recovering；保留 Task 6 原有补偿续步。
		FU_BeginRecoveryDestroy<Provider>(false);
	}
}

template<EFU_OnlineProvider Provider>
void UFU_OnlineSessionSubsystem::FU_BeginFindCancellation(const uint64 Generation)
{
	FFU_OnlineProviderState& State = FU_GetProviderState<Provider>();
	FFU_OnlineOperationStateMachine& Machine = FU_GetOperationMachine<Provider>();
	// 【A1 generation gate】只有刚由 timeout 转入 Recovering 且仍等待原 Find 的同 generation
	// 才能关联 OperationId；任何 stale/wrong-generation 调用保持无事件、无清理。
	if (!Machine.IsExpectedCallback(Generation, EFU_OperationKind::Find)
		|| Machine.Get().Phase != EFU_OperationPhase::Recovering
		|| !Machine.Get().bFindCancellationOutstanding)
	{
		return;
	}
	const FGuid OperationId = Machine.Get().ActiveOperationId;

	if (!State.SessionInterface.IsValid())
	{
		const EFU_OperationAction Actions = Machine.HandleFindCancellationCompletion(Generation, false);
		if (Actions == EFU_OperationAction::None)
		{
			return;
		}
		// 状态机已决定“失败取消、继续等待原 Find”；先公开诊断，且没有不存在的 Handle 可清。
		FU_EmitDiagnostic<Provider>(FFU_OnlineOperationPathDiagnostics::BuildFindCancellation(
			Provider,
			OperationId,
			EFU_FindCancellationDiagnosticOutcome::InterfaceUnavailable));
		return;
	}

	// UE 5.8 的 CancelFindSessions 使用全局完成委托；先绑定再调用，才能容忍同步完成。
	State.CancelFindDelegateHandle = State.SessionInterface->AddOnCancelFindSessionsCompleteDelegate_Handle(
		FOnCancelFindSessionsCompleteDelegate::CreateUObject(
			this,
			&ThisClass::FU_OnCancelFindSessionsComplete<Provider>,
			Generation));
	FU_EmitDiagnostic<Provider>(FFU_OnlineOperationPathDiagnostics::BuildFindCancellation(
		Provider,
		OperationId,
		EFU_FindCancellationDiagnosticOutcome::DelegateBound));

	const bool bRequestSubmitted = State.SessionInterface->CancelFindSessions();
	if (bRequestSubmitted)
	{
		// 若 Provider 在调用栈内同步完成，完成回调已经先转换并清理；此处不得再补一条过时 Pending。
		if (Machine.IsExpectedCallback(Generation, EFU_OperationKind::Find)
			&& Machine.Get().bFindCancellationOutstanding)
		{
			FU_EmitDiagnostic<Provider>(FFU_OnlineOperationPathDiagnostics::BuildFindCancellation(
				Provider,
				OperationId,
				EFU_FindCancellationDiagnosticOutcome::RequestSubmitted));
		}
		return;
	}

	const FFU_OnlineCallbackResourceSnapshot ResourceSnapshot = FU_CaptureCallbackResources<Provider>();
	const EFU_OperationAction Actions = Machine.HandleFindCancellationCompletion(Generation, false);
	if (Actions == EFU_OperationAction::None)
	{
		// 调用栈内若已有有效完成回调赢得竞争，其事件已经使用原 ID 发出；这里不错误关联新操作。
		return;
	}
	const FFU_OnlineDiagnosticEvent Event = FFU_OnlineOperationPathDiagnostics::BuildFindCancellation(
		Provider,
		OperationId,
		EFU_FindCancellationDiagnosticOutcome::SynchronousRejected);
	FFU_OnlineOperationPathDispatchSinks Sinks;
	Sinks.Diagnostic = [this](const FFU_OnlineDiagnosticEvent& InEvent)
	{
		FU_EmitDiagnostic<Provider>(InEvent);
	};
	Sinks.CurrentGeneration = [&Machine]() { return Machine.Get().ActiveGeneration; };
	Sinks.SharedResourcesStillMatch = [this, ResourceSnapshot, Actions]()
	{
		return FU_AreCapturedCallbackResourcesCurrent<Provider>(ResourceSnapshot, EFU_OperationKind::Find, Actions);
	};
	Sinks.ExactOldResourceCleanup = [this, ResourceSnapshot, Actions]()
	{
		FU_ClearCapturedCallbackResources(ResourceSnapshot, EFU_OperationKind::Find, Actions);
	};
	Sinks.GenerationMatchedCleanup = [this, ResourceSnapshot, Actions]()
	{
		FU_ResetMatchedCallbackResources<Provider>(ResourceSnapshot, EFU_OperationKind::Find, Actions);
	};
	Sinks.LegacyCompletion = [this]()
	{
		OnFindSessionCompleteV2.Broadcast(Provider, TArray<FFU_SessionResult>{}, false);
	};
	FFU_OnlineOperationPathDispatcher::DispatchInternal(Event, Generation, false, Sinks);
}

template<EFU_OnlineProvider Provider>
void UFU_OnlineSessionSubsystem::FU_OnOperationTimeout(const uint64 Generation)
{
	FFU_OnlineProviderState& State = FU_GetProviderState<Provider>();
	FFU_OnlineOperationStateMachine& Machine = FU_GetOperationMachine<Provider>();
	const FFU_OperationState Snapshot = Machine.Get();
	const EFU_OperationKind SubmittedKind = Snapshot.ActiveKind;
	const EFU_OperationKind RootKind = Snapshot.RootKind;
	const FGuid OperationId = Snapshot.ActiveOperationId;
	const bool bRepeatedRecoveryDestroyTimeout =
		Snapshot.Phase == EFU_OperationPhase::Recovering
		&& Snapshot.ActiveKind == EFU_OperationKind::Destroy
		&& Snapshot.bAwaitingOriginalCompletion;
	const FFU_OnlineCallbackResourceSnapshot ResourceSnapshot = FU_CaptureCallbackResources<Provider>();
	const EFU_OperationAction Actions = Machine.HandleTimeout(Generation);
	if (Actions == EFU_OperationAction::None)
	{
		return;
	}
	if (bRepeatedRecoveryDestroyTimeout)
	{
		const FFU_OnlineDiagnosticEvent Event = FFU_OnlineOperationPathDiagnostics::BuildRecoveryDestroy(
			Provider,
			OperationId,
			EFU_RecoveryDestroyDiagnosticOutcome::RepeatedTimeout,
			false,
			Actions);
		FFU_OnlineOperationPathDispatchSinks Sinks;
		Sinks.Diagnostic = [this](const FFU_OnlineDiagnosticEvent& InEvent)
		{
			FU_EmitDiagnostic<Provider>(InEvent);
		};
		Sinks.CurrentGeneration = [&Machine]() { return Machine.Get().ActiveGeneration; };
		Sinks.SharedResourcesStillMatch = [this, ResourceSnapshot, Actions]()
		{
			return FU_AreCapturedCallbackResourcesCurrent<Provider>(ResourceSnapshot, EFU_OperationKind::Destroy, Actions);
		};
		Sinks.ExactOldResourceCleanup = [this, ResourceSnapshot, Actions]()
		{
			FU_ClearCapturedCallbackResources(ResourceSnapshot, EFU_OperationKind::Destroy, Actions);
		};
		Sinks.GenerationMatchedCleanup = [this, ResourceSnapshot, Actions]()
		{
			FU_ResetMatchedCallbackResources<Provider>(ResourceSnapshot, EFU_OperationKind::Destroy, Actions);
		};
		Sinks.RecoveryDestroySubmission = [this]() { FU_BeginRecoveryDestroy<Provider>(true); };
		Sinks.LegacyCompletion = [this, RootKind]() { FU_BroadcastOperationFailure<Provider>(RootKind); };
		FFU_OnlineOperationPathDispatcher::DispatchInternal(Event, Generation, false, Sinks);
		return;
	}

	FU_EmitDiagnostic<Provider>(
		SubmittedKind,
		OperationId,
		EFU_OnlineDiagnosticPhase::Timeout,
		EFU_OnlineDiagnosticSeverity::Error,
		TEXT("FU.Operation.Timeout"),
		FString::Printf(TEXT("OSS 操作超时 Generation=%llu；保留不可取消原回调直到终态"), Generation));

	if (EnumHasAnyFlags(Actions, EFU_OperationAction::ClearWatchdog))
	{
		FU_ClearOperationWatchdog<Provider>();
	}

	if (EnumHasAnyFlags(Actions, EFU_OperationAction::BroadcastFailure))
	{
		// 预销毁超时后根 Create/Join 已失败，必须撤销续步意图；原 Destroy 回调仍保留用于收敛。
		if (SubmittedKind == EFU_OperationKind::Destroy)
		{
			State.PendingOperation = EFU_PendingOperation::None;
		}
		if (RootKind == EFU_OperationKind::Create)
		{
			State.PendingCreateRoomName.Reset();
			State.PendingCreateRoomPassword.Reset();
			State.PendingMaxPlayers = 0;
		}
		FU_BroadcastOperationFailure<Provider>(RootKind);
	}

	if (EnumHasAnyFlags(Actions, EFU_OperationAction::StartFindCancellation))
	{
		FU_BeginFindCancellation<Provider>(Generation);
	}
}

template<EFU_OnlineProvider Provider>
void UFU_OnlineSessionSubsystem::FU_OnCancelFindSessionsComplete(
	const bool bWasSuccessful,
	const uint64 Generation)
{
	FFU_OnlineOperationStateMachine& Machine = FU_GetOperationMachine<Provider>();
	const FFU_OnlineCallbackResourceSnapshot ResourceSnapshot = FU_CaptureCallbackResources<Provider>();
	const EFU_OperationAction Actions = Machine.HandleFindCancellationCompletion(Generation, bWasSuccessful);
	FFU_OnlineOperationPathDispatchSinks Sinks;
	Sinks.Diagnostic = [this](const FFU_OnlineDiagnosticEvent& InEvent)
	{
		FU_EmitDiagnostic<Provider>(InEvent);
	};
	Sinks.CurrentGeneration = [&Machine]() { return Machine.Get().ActiveGeneration; };
	Sinks.SharedResourcesStillMatch = [this, ResourceSnapshot, Actions]()
	{
		return FU_AreCapturedCallbackResourcesCurrent<Provider>(ResourceSnapshot, EFU_OperationKind::Find, Actions);
	};
	Sinks.ExactOldResourceCleanup = [this, ResourceSnapshot, Actions]()
	{
		FU_ClearCapturedCallbackResources(ResourceSnapshot, EFU_OperationKind::Find, Actions);
	};
	Sinks.GenerationMatchedCleanup = [this, ResourceSnapshot, Actions]()
	{
		FU_ResetMatchedCallbackResources<Provider>(ResourceSnapshot, EFU_OperationKind::Find, Actions);
	};
	Sinks.LegacyCompletion = [this]()
	{
		OnFindSessionCompleteV2.Broadcast(Provider, TArray<FFU_SessionResult>{}, false);
	};
	if (Actions == EFU_OperationAction::None)
	{
		// unset event 经过 production dispatcher；它必须连 diagnostic/cleanup/legacy/travel 全部保持 no-op。
		FFU_OnlineOperationPathDispatcher::DispatchInternal(
			TOptional<FFU_OnlineDiagnosticEvent>(), Generation, false, Sinks);
		return;
	}
	// 【A1 stale 安全】OperationId 只在状态机已验证 generation/kind/phase 并返回有效 action 后读取；
	// None 分支绝不借用当前新操作的 ActiveOperationId，也不清任何新 Handle。
	const FGuid OperationId = Machine.Get().ActiveOperationId;
	const FFU_OnlineDiagnosticEvent Event = FFU_OnlineOperationPathDiagnostics::BuildFindCancellation(
		Provider,
		OperationId,
		bWasSuccessful
			? EFU_FindCancellationDiagnosticOutcome::CancelWonRace
			: EFU_FindCancellationDiagnosticOutcome::FailedWaitingForOriginal);
	FFU_OnlineOperationPathDispatcher::DispatchInternal(Event, Generation, false, Sinks);
}

template<EFU_OnlineProvider Provider>
bool UFU_OnlineSessionSubsystem::FU_BeginRecoveryDestroy(const bool bExplicitRetry)
{
	FFU_OnlineProviderState& State = FU_GetProviderState<Provider>();
	FFU_OnlineOperationStateMachine& Machine = FU_GetOperationMachine<Provider>();
	const FFU_OperationState Snapshot = Machine.Get();
	// 只有已经存在的根 operation 才能归因恢复事件；防御性无 ID 调用保持 no-op，
	// 不能让 sanitizer 自动分配一个与真实根请求无关的新 ID。
	if (!Snapshot.ActiveOperationId.IsValid())
	{
		return false;
	}
	if (!State.SessionInterface.IsValid())
	{
		FU_EmitDiagnostic<Provider>(FFU_OnlineOperationPathDiagnostics::BuildRecoveryDestroy(
			Provider,
			Snapshot.ActiveOperationId,
			EFU_RecoveryDestroyDiagnosticOutcome::InterfaceUnavailable,
			false,
			EFU_OperationAction::None));
		return false;
	}
	if (State.SessionInterface->GetNamedSession(NAME_GameSession) == nullptr)
	{
		// NoSession 只证明无需再提交 Destroy；最终 Idle 仍由 TryRecoverProvider 的 callback/driver/lease 四证据门决定。
		FU_EmitDiagnostic<Provider>(FFU_OnlineOperationPathDiagnostics::BuildRecoveryDestroy(
			Provider,
			Snapshot.ActiveOperationId,
			EFU_RecoveryDestroyDiagnosticOutcome::NoSession,
			false,
			EFU_OperationAction::None));
		return false;
	}

	uint64 RecoveryGeneration = 0;
	if (!Machine.BeginRecoveryDestroyAttempt(RecoveryGeneration, bExplicitRetry))
	{
		FU_EmitDiagnostic<Provider>(FFU_OnlineOperationPathDiagnostics::BuildRecoveryDestroy(
			Provider,
			Snapshot.ActiveOperationId,
			EFU_RecoveryDestroyDiagnosticOutcome::StateRejected,
			true,
			EFU_OperationAction::None));
		return false;
	}

	// generation 与尝试次数已由状态机先提交，诊断同步可见之后才允许绑定 delegate/timer、
	// 写 pending 并调用 OSS；Blueprint 重入只能观察到新的 recovery generation。
	FU_EmitDiagnostic<Provider>(FFU_OnlineOperationPathDiagnostics::BuildRecoveryDestroy(
		Provider,
		Machine.Get().ActiveOperationId,
		EFU_RecoveryDestroyDiagnosticOutcome::SubmitAccepted,
		true,
		EFU_OperationAction::None));
	State.PendingOperation = EFU_PendingOperation::None;
	State.DestroyDelegateHandle = State.SessionInterface->AddOnDestroySessionCompleteDelegate_Handle(
		FOnDestroySessionCompleteDelegate::CreateUObject(
			this,
			&ThisClass::FU_OnRecoveryDestroyComplete<Provider>,
			RecoveryGeneration));
	FU_ArmOperationWatchdog<Provider>(RecoveryGeneration);
	// OSS 调用可能同步回调并触发 Blueprint 重入；提交前锁定本次 delegate/timer/search 身份。
	const FFU_OnlineCallbackResourceSnapshot ResourceSnapshot = FU_CaptureCallbackResources<Provider>();
	const bool bStarted = State.SessionInterface->DestroySession(NAME_GameSession);
	if (!bStarted && Machine.IsExpectedCallback(RecoveryGeneration, EFU_OperationKind::Destroy))
	{
		const bool bSessionStillExists = State.SessionInterface->GetNamedSession(NAME_GameSession) != nullptr;
		const EFU_OperationAction RejectActions = Machine.HandleOriginalCompletion(
			RecoveryGeneration,
			EFU_OperationKind::Destroy,
			false,
			bSessionStillExists);
		if (RejectActions == EFU_OperationAction::None)
		{
			// 同步调用栈内若回调已经完成，它已通过专用回调发出可归因事件；这里不得重复关联。
			return false;
		}
		const FGuid OperationId = Machine.Get().ActiveOperationId;
		const FFU_OnlineDiagnosticEvent Event = FFU_OnlineOperationPathDiagnostics::BuildRecoveryDestroy(
			Provider,
			OperationId,
			EFU_RecoveryDestroyDiagnosticOutcome::SynchronousRejected,
			bSessionStillExists,
			RejectActions);
		FFU_OnlineOperationPathDispatchSinks Sinks;
		Sinks.Diagnostic = [this](const FFU_OnlineDiagnosticEvent& InEvent)
		{
			FU_EmitDiagnostic<Provider>(InEvent);
		};
		Sinks.CurrentGeneration = [&Machine]() { return Machine.Get().ActiveGeneration; };
		Sinks.SharedResourcesStillMatch = [this, ResourceSnapshot, RejectActions]()
		{
			return FU_AreCapturedCallbackResourcesCurrent<Provider>(ResourceSnapshot, EFU_OperationKind::Destroy, RejectActions);
		};
		Sinks.ExactOldResourceCleanup = [this, ResourceSnapshot, RejectActions]()
		{
			FU_ClearCapturedCallbackResources(ResourceSnapshot, EFU_OperationKind::Destroy, RejectActions);
		};
		Sinks.GenerationMatchedCleanup = [this, ResourceSnapshot, RejectActions]()
		{
			FU_ResetMatchedCallbackResources<Provider>(ResourceSnapshot, EFU_OperationKind::Destroy, RejectActions);
			if (EnumHasAnyFlags(RejectActions, EFU_OperationAction::RequestLeaseRelease))
			{
				FU_RequestNetDriverLeaseRelease(Provider, TEXT("Recovery Destroy rejected after reaching NoSession"));
			}
		};
		Sinks.LegacyCompletion = [this, &Machine]()
		{
			FU_BroadcastOperationFailure<Provider>(Machine.Get().RootKind);
		};
		FFU_OnlineOperationPathDispatcher::DispatchInternal(Event, RecoveryGeneration, false, Sinks);
	}
	return bStarted;
}

template<EFU_OnlineProvider Provider>
bool UFU_OnlineSessionSubsystem::FU_TryRecoverProvider()
{
	FFU_OnlineProviderState& State = FU_GetProviderState<Provider>();
	// 【同步诊断重入门】本函数的 NotRecovering/Unsafe/Lease/Completed 每条事件都公开给 Blueprint。
	// 若监听器再次调用恢复而这里没有栈级门，会递归发同一事件直至栈溢出；更糟时内层先把
	// Machine 完成到 Idle，外层随后又报告 Unsafe。嵌套调用静默返回，最外层仍会给出完整结论。
	if (State.bRecoveryEntryInProgress)
	{
		return false;
	}
	TGuardValue<bool> RecoveryEntryGuard(State.bRecoveryEntryInProgress, true);

	FFU_OnlineOperationStateMachine& Machine = FU_GetOperationMachine<Provider>();
	const FFU_OperationState Snapshot = Machine.Get();
	if (Snapshot.Phase != EFU_OperationPhase::Recovering)
	{
		FU_EmitDiagnostic<Provider>(
			Snapshot.RootKind, Snapshot.ActiveOperationId,
			EFU_OnlineDiagnosticPhase::Recovery, EFU_OnlineDiagnosticSeverity::Warning,
			TEXT("FU.Recovery.Rejected.NotRecovering"), TEXT("Provider 当前不在 Recovering，未执行任何恢复写操作"));
		return false;
	}

	if (!State.SessionInterface.IsValid())
	{
		FU_EmitDiagnostic<Provider>(
			Snapshot.RootKind, Snapshot.ActiveOperationId,
			EFU_OnlineDiagnosticPhase::Recovery, EFU_OnlineDiagnosticSeverity::Error,
			TEXT("FU.Recovery.Rejected.InterfaceUnknown"), TEXT("Session Interface 不可用，无法证明 NoSession"));
		return false;
	}

	const bool bSessionStillExists = State.SessionInterface->GetNamedSession(NAME_GameSession) != nullptr;
	const bool bNoLiveOrPendingDriver = FFU_OnlineSessionNetDriverLease::AreAllWorldsClearForRecovery();
	if (Machine.CanRetryRecoveryDestroy(Snapshot.bOriginalCallbackSeen, bSessionStillExists))
	{
		if (!bNoLiveOrPendingDriver)
		{
			FU_EmitDiagnostic<Provider>(
				Snapshot.RootKind, Snapshot.ActiveOperationId,
				EFU_OnlineDiagnosticPhase::Recovery, EFU_OnlineDiagnosticSeverity::Warning,
				TEXT("FU.Recovery.Rejected.DriverActive"), TEXT("仍有活动或待定 GameNetDriver，拒绝重试恢复 Destroy"));
			return false;
		}

		FU_BeginRecoveryDestroy<Provider>(true);
		return false;
	}

	const bool bNoSession = !bSessionStillExists;
	if (Machine.CanFinishRecovery(Snapshot.bOriginalCallbackSeen, bNoSession))
	{
		// 先由 exact owner+provider API 请求释放；若 World 尚不安全，ticker 会延迟且本次仍返回 false。
		FU_RequestNetDriverLeaseRelease(Provider, TEXT("TryRecoverProvider terminal operation"));
	}
	const bool bLeaseReleased = FFU_OnlineSessionNetDriverLease::IsReleaseComplete(GetGameInstance(), Provider);
	if (!Machine.CanStartExplicitRecovery(
		Snapshot.bOriginalCallbackSeen,
		bNoSession,
		bNoLiveOrPendingDriver,
		bLeaseReleased))
	{
		FU_EmitDiagnostic<Provider>(
			Snapshot.RootKind, Snapshot.ActiveOperationId,
			EFU_OnlineDiagnosticPhase::Recovery, EFU_OnlineDiagnosticSeverity::Warning,
			TEXT("FU.Recovery.Rejected.Unsafe"),
			FString::Printf(
				TEXT("恢复证据不足 Callback=%s NoSession=%s DriversClear=%s LeaseReleased=%s"),
				Snapshot.bOriginalCallbackSeen ? TEXT("true") : TEXT("false"),
				bNoSession ? TEXT("true") : TEXT("false"),
				bNoLiveOrPendingDriver ? TEXT("true") : TEXT("false"),
				bLeaseReleased ? TEXT("true") : TEXT("false")));
		return false;
	}

	FU_ClearOperationWatchdog<Provider>();
	FU_ClearOperationDelegate<Provider>(Snapshot.ActiveKind);
	FU_ClearFindCancellationDelegate<Provider>();
	State.SessionSearch.Reset();
	State.PendingJoinResult.Reset();
	State.PendingCreateRoomName.Reset();
	State.PendingCreateRoomPassword.Reset();
	State.PendingFindRoomName.Reset();
	State.bSteamFallbackFindInProgress = false;
	State.PendingMaxPlayers = 0;
	State.PendingOperation = EFU_PendingOperation::None;
	if (ActiveGameplayProvider.IsSet() && ActiveGameplayProvider.GetValue() == Provider)
	{
		ActiveGameplayProvider.Reset();
	}
	const bool bFinished = Machine.FinishExplicitRecovery();
	FU_EmitDiagnostic<Provider>(
		Snapshot.RootKind, Snapshot.ActiveOperationId,
		EFU_OnlineDiagnosticPhase::Completed,
		bFinished ? EFU_OnlineDiagnosticSeverity::Info : EFU_OnlineDiagnosticSeverity::Error,
		bFinished ? TEXT("FU.Recovery.Completed") : TEXT("FU.Recovery.Rejected.StateChanged"),
		bFinished ? TEXT("Provider 已在完整安全证据下恢复 Idle") : TEXT("恢复提交期间状态发生变化，保持保守失败"));
	return bFinished;
}

void UFU_OnlineSessionSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// 【每个 GameInstance 独立】PIE 多实例不能共享历史、浮层或 Blueprint 广播目标；
	// 诊断分发器因此由本 Subsystem 创建和销毁，而不是由全局 Runtime Module 持有。
	const UFU_OnlineSessionSettings& Settings = UFU_OnlineSessionSettings::GetRuntimeSettings();
	FFU_OnlineDiagnosticDispatchConfig DiagnosticConfig;
	DiagnosticConfig.HistoryLimit = Settings.DiagnosticHistoryLimit;
	DiagnosticConfig.bEmitToLog = Settings.bEnableDiagnosticLog;
	DiagnosticConfig.bEnableOverlay = Settings.bEnableDiagnosticOverlay;
	DiagnosticConfig.MinimumOverlaySeverity = FUOnlineSession::GetOverlayMinimumSeverity(Settings.MinimumOverlaySeverity);
	DiagnosticConfig.OverlayDurationSeconds = Settings.OverlayDurationSeconds;
	DiagnosticConfig.OverlayRowLimit = Settings.OverlayRowLimit;

	Diagnostics = TUniquePtr<FFU_OnlineSessionDiagnostics, FFU_OnlineSessionDiagnosticsDeleter>(
		new FFU_OnlineSessionDiagnostics(
			DiagnosticConfig,
			[WeakThis = TWeakObjectPtr<ThisClass>(this)](const FFU_OnlineDiagnosticEvent& Event)
			{
				// 文件失败会紧接原事件反馈；若前一个 Blueprint 回调结束了 GameInstance，后续反馈不能访问旧对象。
				// 输出开关仍不影响活跃实例的 Blueprint 通知，只拒绝失效/已反初始化生命周期。
				if (ThisClass* Self = WeakThis.Get(); Self && Self->Diagnostics.IsValid())
				{
					Self->OnOnlineDiagnosticEvent.Broadcast(Event);
				}
			}));

	if (UWorld* World = GetWorld())
	{
		// 专用 Slate Widget 只在真实 GameViewport 存在时附着；Commandlet/无窗口测试仍保留完整历史。
		Diagnostics->AttachViewport(World->GetGameViewport());
	}

	FFU_OnlineDiagnosticEvent StartupEvent;
	StartupEvent.Operation = EFU_OnlineDiagnosticOperation::Environment;
	StartupEvent.Phase = EFU_OnlineDiagnosticPhase::Completed;
	StartupEvent.Severity = EFU_OnlineDiagnosticSeverity::Info;
	StartupEvent.Code = TEXT("FU.Diagnostics.Initialized");
	StartupEvent.Status = TEXT("Ready");
	StartupEvent.Message = TEXT("FU Online Session 诊断分发器已初始化；后续事件将统一脱敏并可供 Blueprint 查询");
	StartupEvent.RecommendedAction = TEXT("联机问题出现后调用 SaveDiagnosticReport，并附上 Saved/Logs/FUOnlineSession 中的报告");
	FU_EmitDiagnostic(MoveTemp(StartupEvent));

	// 【FU 修复：监听 Session 成功之后的失败】
	// OnlineSubsystem 的 JoinSession 回调早于真正的网络握手，因此还要监听引擎旅行阶段。
	if (GEngine)
	{
		NetworkFailureDelegateHandle = GEngine->OnNetworkFailure().AddUObject(
			this,
			&ThisClass::FU_OnNetworkFailure);

		TravelFailureDelegateHandle = GEngine->OnTravelFailure().AddUObject(
			this,
			&ThisClass::FU_OnTravelFailure);
	}
}

void FFU_OnlineSessionDiagnosticsDeleter::operator()(FFU_OnlineSessionDiagnostics* InDiagnostics) const
{
	// 【不完整类型隔离】Private 诊断实现只能在本 cpp 释放，Public Subsystem 头不会暴露 Slate/文件细节。
	delete InDiagnostics;
}

void FFU_OnlineOperationStateMachineDeleter::operator()(FFU_OnlineOperationStateMachine* StateMachine) const
{
	// 与 Diagnostics 相同，Private 类型只在已包含完整定义的 Runtime cpp 中释放。
	delete StateMachine;
}

TArray<FFU_OnlineDiagnosticEvent> UFU_OnlineSessionSubsystem::GetDiagnosticHistory() const
{
	return Diagnostics.IsValid() ? Diagnostics->GetHistory() : TArray<FFU_OnlineDiagnosticEvent>();
}

void UFU_OnlineSessionSubsystem::ClearDiagnosticHistory()
{
	if (Diagnostics.IsValid())
	{
		Diagnostics->ClearHistory();
	}
}

FString UFU_OnlineSessionSubsystem::BuildDiagnosticReport() const
{
	return Diagnostics.IsValid()
		? Diagnostics->BuildReport()
		: TEXT("FU Online Session diagnostics are not initialized.");
}

bool UFU_OnlineSessionSubsystem::SaveDiagnosticReport(FString& OutSavedPath, FString& OutError)
{
	OutSavedPath.Reset();
	OutError.Reset();
	if (!Diagnostics.IsValid())
	{
		OutError = TEXT("FU Online Session 诊断分发器尚未初始化");
		return false;
	}

	return Diagnostics->SaveReport(OutSavedPath, OutError);
}

FString UFU_OnlineSessionSubsystem::GetProviderLogPath(const EFU_OnlineProvider Provider) const
{
	// 查询不得隐式初始化 OSS 或创建目录；失效生命周期只返回空路径。
	return Diagnostics.IsValid() ? Diagnostics->GetProviderLogPath(Provider) : FString();
}

FFU_OnlineProviderStatus UFU_OnlineSessionSubsystem::RunProviderDiagnostics(const EFU_OnlineProvider Provider)
{
	// 【Task 7 Blueprint 同步诊断】状态检查保持既有只读行为；新建 GUID 只属于这次环境快照，
	// 不触碰任何 Provider 的 attempt/generation，因而 ClearHistory 和并行在途 OSS 操作都不会被改写。
	const FGuid OperationId = FGuid::NewGuid();
	switch (Provider)
	{
	case EFU_OnlineProvider::Steam:
	{
		const FFU_OnlineProviderStatus Status = FU_CheckProviderStatus<EFU_OnlineProvider::Steam>();
		FU_EmitEnvironmentDiagnostic<EFU_OnlineProvider::Steam>(OperationId, Status);
		return Status;
	}
	case EFU_OnlineProvider::Lan:
	{
		const FFU_OnlineProviderStatus Status = FU_CheckProviderStatus<EFU_OnlineProvider::Lan>();
		FU_EmitEnvironmentDiagnostic<EFU_OnlineProvider::Lan>(OperationId, Status);
		return Status;
	}
	default:
	{
		// 非法 Blueprint 枚举不查询或污染任一真实 Provider；仍给 UI 一条无敏感数据的环境诊断。
		FFU_OnlineProviderStatus Status;
		Status.Provider = Provider;
		Status.StatusCode = EFU_OnlineProviderStatusCode::SubsystemUnavailable;
		Status.Message = TEXT("请求的在线提供方无效，未执行任何 OnlineSubsystem 查询");
		FFU_OnlineDiagnosticEvent Event;
		Event.OperationId = OperationId;
		Event.Provider = Provider;
		Event.Operation = EFU_OnlineDiagnosticOperation::Environment;
		Event.Phase = EFU_OnlineDiagnosticPhase::Preflight;
		Event.Severity = EFU_OnlineDiagnosticSeverity::Warning;
		Event.Code = TEXT("FU.Provider.Invalid");
		Event.Status = TEXT("Rejected");
		Event.Message = Status.Message;
		Event.RecommendedAction = TEXT("仅传入 Steam 或 Lan 枚举值后重新运行诊断");
		FU_EmitDiagnostic(MoveTemp(Event));
		return Status;
	}
	}
}

bool UFU_OnlineSessionSubsystem::TryRecoverProvider(const EFU_OnlineProvider Provider)
{
	// Blueprint 枚举只允许现有 Steam/LAN 序号；default 仍 fail-closed，且不触碰任一 Provider 状态。
	switch (Provider)
	{
	case EFU_OnlineProvider::Steam:
		return FU_TryRecoverProvider<EFU_OnlineProvider::Steam>();
	case EFU_OnlineProvider::Lan:
		return FU_TryRecoverProvider<EFU_OnlineProvider::Lan>();
	default:
		// 非法枚举拒绝只通过统一诊断出口公开观察；builder 不读取 ProviderState，
		// 因而不会改变 Steam/LAN 的 generation、timer、delegate、pending 或完成状态。
		if (const TOptional<FFU_OnlineDiagnosticEvent> Event =
			FFU_OnlineSessionDiagnostics::BuildUnsupportedRecoveryProviderDiagnostic(Provider))
		{
			FU_EmitDiagnostic(Event.GetValue());
		}
		return false;
	}
}

TOptional<EFU_OnlineProvider> UFU_OnlineSessionSubsystem::FU_GetFailureProvider() const
{
	if (ActiveGameplayProvider.IsSet())
	{
		return ActiveGameplayProvider;
	}

	return PreparedNetDriverProvider;
}

bool UFU_OnlineSessionSubsystem::FU_CanReleaseNetDriverAfterConnectionFailure(
	const EFU_OnlineProvider Provider) const
{
	// 运行态 Provider 仍由两个模板实例各自保存；这里只把精确实例的只读证据折叠成纯快照，
	// 不尝试重新取得接口，也不改变任何 pending/delegate/state-machine 字段。
	const FFU_OnlineProviderState* ProviderState = nullptr;
	switch (Provider)
	{
	case EFU_OnlineProvider::Steam:
		ProviderState = &SteamState;
		break;
	case EFU_OnlineProvider::Lan:
		ProviderState = &LanState;
		break;
	default:
		return false;
	}

	FFU_ConnectionFailureLeaseReleaseSnapshot Snapshot;
	Snapshot.bSessionInterfaceValid = ProviderState->SessionInterface.IsValid();
	Snapshot.bNamedSessionAbsent = Snapshot.bSessionInterfaceValid
		&& ProviderState->SessionInterface->GetNamedSession(NAME_GameSession) == nullptr;
	Snapshot.bNoOperationInFlight = !ProviderState->OperationMachine.IsValid()
		|| !ProviderState->OperationMachine->Get().bAwaitingOriginalCompletion;
	Snapshot.bAllWorldsClear = FFU_OnlineSessionNetDriverLease::AreAllWorldsClearForRecovery();
	return FFU_OnlineOperationStateMachine::CanReleaseLeaseAfterConnectionFailure(Snapshot);
}

EFU_NetDriverLeaseResult UFU_OnlineSessionSubsystem::FU_RequestNetDriverLeaseRelease(
	const EFU_OnlineProvider Provider,
	const TCHAR* Reason)
{
	const FFU_OnlineProviderState& State = Provider == EFU_OnlineProvider::Steam ? SteamState : LanState;
	const EFU_OperationKind RootKind = State.OperationMachine.IsValid()
		? State.OperationMachine->Get().RootKind
		: EFU_OperationKind::Destroy;
	const FGuid OperationId = State.OperationMachine.IsValid() && State.OperationMachine->Get().ActiveOperationId.IsValid()
		? State.OperationMachine->Get().ActiveOperationId
		: FGuid::NewGuid();
	return FU_RequestNetDriverLeaseRelease(Provider, Reason, RootKind, OperationId);
}

EFU_NetDriverLeaseResult UFU_OnlineSessionSubsystem::FU_RequestNetDriverLeaseRelease(
	const EFU_OnlineProvider Provider,
	const TCHAR* Reason,
	const EFU_OperationKind DiagnosticRootKind,
	const FGuid& DiagnosticOperationId)
{
	// GameInstanceSubsystem 正常析构期间 GetGameInstance 仍有效；若已经失效，nullptr 只允许协调器
	// 释放同样已经失效的弱所有者；Provider 也必须一致，错误调用 DestroyLan 不会释放 Steam 租约。
	const EFU_NetDriverLeaseResult Result = FFU_OnlineSessionNetDriverLease::RequestRelease(GetGameInstance(), Provider, Reason);
	if (PreparedNetDriverProvider.IsSet() && PreparedNetDriverProvider.GetValue() == Provider)
	{
		PreparedNetDriverProvider.Reset();
	}
	const FFU_NetDriverLeaseDiagnosticOutcome Outcome = FFU_OnlineSessionNetDriverLease::GetDiagnosticOutcome(Result);
	if (Provider == EFU_OnlineProvider::Steam)
	{
		FU_EmitDiagnostic<EFU_OnlineProvider::Steam>(DiagnosticRootKind, DiagnosticOperationId, EFU_OnlineDiagnosticPhase::Recovery, Outcome.bIsError ? EFU_OnlineDiagnosticSeverity::Warning : EFU_OnlineDiagnosticSeverity::Info, *Outcome.Code.ToString(), TEXT("GameNetDriver 租约协调器已完成 Release 决策"), FString(), *Outcome.Status.ToString());
	}
	else
	{
		FU_EmitDiagnostic<EFU_OnlineProvider::Lan>(DiagnosticRootKind, DiagnosticOperationId, EFU_OnlineDiagnosticPhase::Recovery, Outcome.bIsError ? EFU_OnlineDiagnosticSeverity::Warning : EFU_OnlineDiagnosticSeverity::Info, *Outcome.Code.ToString(), TEXT("GameNetDriver 租约协调器已完成 Release 决策"), FString(), *Outcome.Status.ToString());
	}
	return Result;
}

void UFU_OnlineSessionSubsystem::FU_EmitDiagnostic(FFU_OnlineDiagnosticEvent Event)
{
	if (!Diagnostics.IsValid())
	{
		return;
	}

	// 【跨环境上下文】打包版没有 PIE WorldContext 时保持 INDEX_NONE；PIE 多窗口则记录实例号，
	// 让导出的报告能区分服务器窗口和客户端窗口，而不是把它们的失败混为一谈。
	if (const UWorld* World = GetWorld())
	{
		// GameInstanceSubsystem 可能早于可渲染 Viewport 初始化；每次发事件都幂等检查一次，
		// 让随后出现的 PIE/打包窗口自动接上浮层，同时 AttachViewport 会在 World 切换时精确解绑旧窗口。
		Diagnostics->AttachViewport(World->GetGameViewport());

		if (Event.WorldName.IsEmpty())
		{
			Event.WorldName = World->GetName();
		}

		if (Event.PIEInstanceId == INDEX_NONE && GEngine)
		{
			if (const FWorldContext* WorldContext = GEngine->GetWorldContextFromWorld(World))
			{
				Event.PIEInstanceId = WorldContext->PIEInstance;
			}
		}
	}

	Diagnostics->Emit(Event);
}

template<EFU_OnlineProvider Provider>
void UFU_OnlineSessionSubsystem::FU_EmitDiagnostic(FFU_OnlineDiagnosticEvent Event)
{
	using FProviderTraits = TFU_OnlineSessionProviderTraits<Provider>;
	static_assert(
		Provider == EFU_OnlineProvider::Steam || Provider == EFU_OnlineProvider::Lan,
		"不受支持的 FU 在线提供商");

	// 【A1 模板唯一真相】纯 policy 可供无 UObject 测试构造事件，但生产分发前必须由模板特化
	// 覆盖 Provider 并补 traits Subsystem；任何运行时输入都不能伪装成另一提供方。
	Event.Provider = Provider;
	FFU_OnlineDiagnosticField SubsystemField;
	SubsystemField.Key = FName(TEXT("Subsystem"));
	SubsystemField.Value = FProviderTraits::GetSubsystemName().ToString();
	Event.Fields.Add(MoveTemp(SubsystemField));
	FU_EmitDiagnostic(MoveTemp(Event));
}

template<EFU_OnlineProvider Provider>
void UFU_OnlineSessionSubsystem::FU_EmitDiagnostic(
	const EFU_OperationKind Kind,
	const FGuid& OperationId,
	const EFU_OnlineDiagnosticPhase Phase,
	const EFU_OnlineDiagnosticSeverity Severity,
	const TCHAR* Code,
	const FString& Message,
	const FString& RoomName,
	const TCHAR* StatusOverride)
{
	using FProviderTraits = TFU_OnlineSessionProviderTraits<Provider>;
	static_assert(
		Provider == EFU_OnlineProvider::Steam || Provider == EFU_OnlineProvider::Lan,
		"不受支持的 FU 在线提供商");

	// 【Task 7 统一模板边界】Provider 从实例化模板及其 traits 派生，避免诊断事件再维护
	// 一份运行时 Provider 真相；所有自由文本仍只在下层分发器一次性脱敏后才会离开 Runtime。
	FFU_OnlineDiagnosticEvent Event;
	Event.Provider = Provider;
	Event.OperationId = OperationId;
	Event.Phase = Phase;
	Event.Severity = Severity;
	Event.Code = Code ? Code : TEXT("FU.Operation.Unknown");
	// 【结果优先】Callback 不等于 Pending：调用方提供稳定 outcome/subtype 时必须原样保留，
	// 否则按生命周期派生有限状态，避免失败与延迟在 Blueprint/报告中混为一谈。
	Event.Status = StatusOverride && *StatusOverride
		? StatusOverride
		: (Phase == EFU_OnlineDiagnosticPhase::Completed ? TEXT("Completed")
			: Phase == EFU_OnlineDiagnosticPhase::Timeout ? TEXT("Failed")
			: Phase == EFU_OnlineDiagnosticPhase::Recovery ? TEXT("Deferred")
			: Phase == EFU_OnlineDiagnosticPhase::Preflight ? TEXT("Rejected")
			: TEXT("Pending"));
	Event.Message = Message;
	Event.RecommendedAction = Phase == EFU_OnlineDiagnosticPhase::Recovery
		? TEXT("等待原始 OSS 回调及驱动/租约安全条件后再次调用 TryRecoverProvider")
		: TEXT("按同一 OperationId 检查后续 Callback、Timeout 与 Recovery 事件");
	switch (Kind)
	{
	case EFU_OperationKind::Create: Event.Operation = EFU_OnlineDiagnosticOperation::CreateSession; break;
	case EFU_OperationKind::Find: Event.Operation = EFU_OnlineDiagnosticOperation::FindSessions; break;
	case EFU_OperationKind::Join: Event.Operation = EFU_OnlineDiagnosticOperation::JoinSession; break;
	case EFU_OperationKind::Destroy: Event.Operation = EFU_OnlineDiagnosticOperation::DestroySession; break;
	default: Event.Operation = EFU_OnlineDiagnosticOperation::Recovery; break;
	}
	FFU_OnlineDiagnosticField SubsystemField;
	SubsystemField.Key = FName(TEXT("Subsystem"));
	SubsystemField.Value = FProviderTraits::GetSubsystemName().ToString();
	Event.Fields.Add(MoveTemp(SubsystemField));
	if (!RoomName.IsEmpty())
	{
		// 房间名只帮助区分同名搜索/创建操作；密码、SessionId、连接串和 Travel URL 一律不进入事件。
		FFU_OnlineDiagnosticField RoomField;
		RoomField.Key = FName(TEXT("RoomName"));
		RoomField.Value = RoomName;
		Event.Fields.Add(MoveTemp(RoomField));
	}
	FU_EmitDiagnostic(MoveTemp(Event));
}

template<EFU_OnlineProvider Provider>
void UFU_OnlineSessionSubsystem::FU_EmitEnvironmentDiagnostic(
	const FGuid& OperationId,
	const FFU_OnlineProviderStatus& Status)
{
	using FProviderTraits = TFU_OnlineSessionProviderTraits<Provider>;
	static_assert(
		Provider == EFU_OnlineProvider::Steam || Provider == EFU_OnlineProvider::Lan,
		"不受支持的 FU 在线提供商");

	// 【Task 7 环境快照】该公开诊断不是 OSS 操作，不能借用或覆盖在途状态机的 ActiveOperationId；
	// 但仍必须生成一次非零 ID，供 Blueprint 将单次检查的所有输出关联起来。
	FFU_OnlineDiagnosticEvent Event;
	Event.OperationId = OperationId;
	Event.Provider = Provider;
	Event.Operation = EFU_OnlineDiagnosticOperation::Environment;
	Event.Phase = EFU_OnlineDiagnosticPhase::Preflight;
	Event.Severity = Status.bIsReady ? EFU_OnlineDiagnosticSeverity::Info : EFU_OnlineDiagnosticSeverity::Warning;
	Event.Code = FUOnlineSession::GetProviderStatusDiagnosticCode(Status.StatusCode);
	Event.Status = Status.bIsReady ? TEXT("Ready") : TEXT("Rejected");
	Event.Message = Status.Message;
	Event.RecommendedAction = Status.bIsReady
		? TEXT("环境已就绪；可继续调用对应的 Create、Find、Join 或 Destroy 蓝图入口")
		: TEXT("根据 StatusCode 修复环境后重新运行 RunProviderDiagnostics");
	FFU_OnlineDiagnosticField SubsystemField;
	SubsystemField.Key = FName(TEXT("Subsystem"));
	SubsystemField.Value = FProviderTraits::GetSubsystemName().ToString();
	Event.Fields.Add(MoveTemp(SubsystemField));
	FFU_OnlineDiagnosticField StatusField;
	StatusField.Key = FName(TEXT("StatusCode"));
	StatusField.Value = LexToString(static_cast<uint8>(Status.StatusCode));
	Event.Fields.Add(MoveTemp(StatusField));
	FU_EmitDiagnostic(MoveTemp(Event));
}

FGuid UFU_OnlineSessionSubsystem::FU_EmitConnectionFailureDiagnostic(
	const EFU_OnlineProvider Provider,
	const bool bIsTravelFailure,
	const TCHAR* StableStatus,
	EFU_OperationKind& OutRootKind)
{
	// 【Task 6 关联继承】不要在网络失败路径重建生命周期表；只读现有 Provider 状态机保留的
	// ActiveOperationId/ActiveKind。即使回调已结束并转 Idle，该 ID 仍能把随后的 Travel 错误串回 Join。
	const FFU_OnlineProviderState* State = nullptr;
	switch (Provider)
	{
	case EFU_OnlineProvider::Steam: State = &SteamState; break;
	case EFU_OnlineProvider::Lan: State = &LanState; break;
	default: return FGuid();
	}
	OutRootKind = State->OperationMachine.IsValid()
		? State->OperationMachine->Get().RootKind
		: EFU_OperationKind::Join;
	const FGuid OperationId = State->OperationMachine.IsValid()
		&& State->OperationMachine->Get().ActiveOperationId.IsValid()
		? State->OperationMachine->Get().ActiveOperationId
		: FGuid::NewGuid();
	const TCHAR* Code = bIsTravelFailure ? TEXT("FU.TravelFailure") : TEXT("FU.NetworkFailure");
	const FString Message = bIsTravelFailure
		? TEXT("引擎报告地图旅行失败；原始 URL 与错误文本已按安全策略省略")
		: TEXT("引擎报告网络连接失败；原始连接信息与错误文本已按安全策略省略");

	// Provider 运行时只做一次最外层分派；真正构造/分发仍进入 traits 模板边界。
	switch (Provider)
	{
	case EFU_OnlineProvider::Steam:
		FU_EmitDiagnostic<EFU_OnlineProvider::Steam>(OutRootKind, OperationId, EFU_OnlineDiagnosticPhase::Callback, EFU_OnlineDiagnosticSeverity::Error, Code, Message, FString(), StableStatus);
		break;
	case EFU_OnlineProvider::Lan:
		FU_EmitDiagnostic<EFU_OnlineProvider::Lan>(OutRootKind, OperationId, EFU_OnlineDiagnosticPhase::Callback, EFU_OnlineDiagnosticSeverity::Error, Code, Message, FString(), StableStatus);
		break;
	default:
		break;
	}
	return OperationId;
}

void UFU_OnlineSessionSubsystem::FU_OnNetworkFailure(
	UWorld* World,
	UNetDriver* NetDriver,
	const ENetworkFailure::Type FailureType,
	const FString& ErrorString)
{
	// PIE 可以同时存在多个 World；只把属于当前 GameInstance 的错误广播给本对象。
	if (World != GetWorld())
	{
		return;
	}

	const TOptional<EFU_OnlineProvider> Provider = FU_GetFailureProvider();
	if (!Provider.IsSet())
	{
		return;
	}

	// 【安全边界】引擎 ErrorString 可能包含连接串、Travel URL 或 token，绝不能先直写 UE_LOG。
	// 只公开稳定 FailureType；详细原文不进入日志、浮层、历史或 Blueprint 任一 sink。
	const FString SafeStatus = ENetworkFailure::ToString(FailureType);
	EFU_OperationKind FailureRootKind = EFU_OperationKind::Join;
	const FGuid FailureOperationId = FU_EmitConnectionFailureDiagnostic(
		Provider.GetValue(), false, *SafeStatus, FailureRootKind);
	const FString FailureMessage = FString::Printf(TEXT("网络连接失败：%s"), *SafeStatus);

	if (FU_CanReleaseNetDriverAfterConnectionFailure(Provider.GetValue()))
	{
		// 四项安全证据在同一游戏线程快照内同时成立；NamedSession 仍在时必定走保留分支，
		// 等待显式 Destroy 或受控恢复，而不是把网络断开误当成会话已经删除。
		// 失败事件会同步广播给 Blueprint；监听器可能在返回前启动另一操作。
		// 显式传入事件前快照，确保 Lease.Release 不会误关联到重入后的新 OperationId。
		FU_RequestNetDriverLeaseRelease(
			Provider.GetValue(), TEXT("Network failure"), FailureRootKind, FailureOperationId);
	}
	else if (Provider.GetValue() == EFU_OnlineProvider::Steam)
	{
		FU_EmitDiagnostic<EFU_OnlineProvider::Steam>(FailureRootKind, FailureOperationId, EFU_OnlineDiagnosticPhase::Recovery, EFU_OnlineDiagnosticSeverity::Warning, TEXT("FU.Lease.Retained"), TEXT("四项安全证据不足，保留 GameNetDriver 租约"), FString(), TEXT("Retained"));
	}
	else
	{
		FU_EmitDiagnostic<EFU_OnlineProvider::Lan>(FailureRootKind, FailureOperationId, EFU_OnlineDiagnosticPhase::Recovery, EFU_OnlineDiagnosticSeverity::Warning, TEXT("FU.Lease.Retained"), TEXT("四项安全证据不足，保留 GameNetDriver 租约"), FString(), TEXT("Retained"));
	}

	OnOnlineConnectionFailure.Broadcast(
		Provider.GetValue(),
		EFU_OnlineConnectionFailureType::NetworkFailure,
		FailureMessage);
}

void UFU_OnlineSessionSubsystem::FU_OnTravelFailure(
	UWorld* World,
	const ETravelFailure::Type FailureType,
	const FString& ErrorString)
{
	// 与 NetworkFailure 相同，只处理当前 GameInstance 所属 World。
	if (World != GetWorld())
	{
		return;
	}

	const TOptional<EFU_OnlineProvider> Provider = FU_GetFailureProvider();
	if (!Provider.IsSet())
	{
		return;
	}

	// Travel 的 ErrorString 同样可能带原始 URL；稳定枚举足以让 Blueprint 选择恢复提示。
	const FString SafeStatus = UEnum::GetValueAsString(FailureType);
	EFU_OperationKind FailureRootKind = EFU_OperationKind::Join;
	const FGuid FailureOperationId = FU_EmitConnectionFailureDiagnostic(
		Provider.GetValue(), true, *SafeStatus, FailureRootKind);
	const FString FailureMessage = FString::Printf(TEXT("地图旅行失败：%s"), *SafeStatus);

	if (FU_CanReleaseNetDriverAfterConnectionFailure(Provider.GetValue()))
	{
		// Travel 失败还可能保留 ActiveNetDriver/PendingNetGame/NextURL；全 World 扫描与
		// Session/操作证据必须同时通过，不能仅依赖延迟 ticker 在未来看到 World 为空。
		// 与 NetworkFailure 一致，租约决策必须继承刚刚已公开失败事件的身份，
		// 不能在 Blueprint 重入后从可变状态机重新取样。
		FU_RequestNetDriverLeaseRelease(
			Provider.GetValue(), TEXT("Travel failure"), FailureRootKind, FailureOperationId);
	}
	else if (Provider.GetValue() == EFU_OnlineProvider::Steam)
	{
		FU_EmitDiagnostic<EFU_OnlineProvider::Steam>(FailureRootKind, FailureOperationId, EFU_OnlineDiagnosticPhase::Recovery, EFU_OnlineDiagnosticSeverity::Warning, TEXT("FU.Lease.Retained"), TEXT("四项安全证据不足，保留 GameNetDriver 租约"), FString(), TEXT("Retained"));
	}
	else
	{
		FU_EmitDiagnostic<EFU_OnlineProvider::Lan>(FailureRootKind, FailureOperationId, EFU_OnlineDiagnosticPhase::Recovery, EFU_OnlineDiagnosticSeverity::Warning, TEXT("FU.Lease.Retained"), TEXT("四项安全证据不足，保留 GameNetDriver 租约"), FString(), TEXT("Retained"));
	}

	OnOnlineConnectionFailure.Broadcast(
		Provider.GetValue(),
		EFU_OnlineConnectionFailureType::TravelFailure,
		FailureMessage);
}

void UFU_OnlineSessionSubsystem::Deinitialize()
{
	const TOptional<EFU_OnlineProvider> LeaseProviderToRelease = PreparedNetDriverProvider;
	// 析构不能假装未完成操作成功终止；在解绑 UObject delegate 前，先把仍等待原 OSS 回调的
	// generation 登记到进程级 Provider blocker。解绑后已不存在可靠外部终态证明，因此该 blocker
	// 永不自动清除，只能通过重启进程恢复；这正是阻止 stale-owner reclaim 的跨生命周期证据。
	auto RegisterDeinitializeUncertainty = [this](
		const EFU_OnlineProvider Provider,
		const FFU_OnlineProviderState& State)
	{
		if (State.OperationMachine.IsValid()
			&& State.OperationMachine->Get().Phase != EFU_OperationPhase::Idle)
		{
			const FFU_OperationState& Operation = State.OperationMachine->Get();
			const bool bTerminalCallbackUnknown = Operation.bAwaitingOriginalCompletion;
			if (bTerminalCallbackUnknown)
			{
				FFU_OnlineSessionNetDriverLease::RegisterUncertainOperation(
					Provider,
					TEXT("GameInstanceSubsystem deinitialize with OSS callback outstanding"));
			}

			const EFU_OnlineDiagnosticSeverity Severity = bTerminalCallbackUnknown
				? EFU_OnlineDiagnosticSeverity::Error
				: EFU_OnlineDiagnosticSeverity::Warning;
			const TCHAR* Code = bTerminalCallbackUnknown
				? TEXT("FU.Operation.DeinitializeUncertain.RestartRequired")
				: TEXT("FU.Operation.DeinitializeRecovering");
			const FString Message = bTerminalCallbackUnknown
				? FString::Printf(
					TEXT("Subsystem 析构时 OSS 终态未知 Generation=%llu；已阻断该 Provider 的租约释放与新 Owner 获取，必须重启进程"),
					Operation.ActiveGeneration)
				: FString::Printf(
					TEXT("Subsystem 析构时操作仍处于 Recovering 但无原回调在途 Generation=%llu"),
					Operation.ActiveGeneration);
			// 析构路径的 Provider 是运行时参数；只在此最外层分派到两条 traits 模板边界，
			// 不复制任何子系统/NetDriver/行为映射，且保留原操作 ID。
			switch (Provider)
			{
			case EFU_OnlineProvider::Steam:
				FU_EmitDiagnostic<EFU_OnlineProvider::Steam>(Operation.ActiveKind, Operation.ActiveOperationId, EFU_OnlineDiagnosticPhase::Recovery, Severity, Code, Message);
				break;
			case EFU_OnlineProvider::Lan:
				FU_EmitDiagnostic<EFU_OnlineProvider::Lan>(Operation.ActiveKind, Operation.ActiveOperationId, EFU_OnlineDiagnosticPhase::Recovery, Severity, Code, Message);
				break;
			default:
				break;
			}
		}
	};
	RegisterDeinitializeUncertainty(EFU_OnlineProvider::Steam, SteamState);
	RegisterDeinitializeUncertainty(EFU_OnlineProvider::Lan, LanState);

	// 引擎委托的生命周期长于 GameInstanceSubsystem，必须先解绑，避免对象销毁后仍收到回调。
	if (GEngine)
	{
		if (NetworkFailureDelegateHandle.IsValid())
		{
			GEngine->OnNetworkFailure().Remove(NetworkFailureDelegateHandle);
			NetworkFailureDelegateHandle.Reset();
		}

		if (TravelFailureDelegateHandle.IsValid())
		{
			GEngine->OnTravelFailure().Remove(TravelFailureDelegateHandle);
			TravelFailureDelegateHandle.Reset();
		}
	}

	// 先停 watchdog、清精确 OSS/cancel handles，再取消可取消 Find；不可取消请求只记录为不确定。
	FU_ClearProviderState<EFU_OnlineProvider::Steam>();
	FU_ClearProviderState<EFU_OnlineProvider::Lan>();

	// 所有本对象回调入口已解除后才请求进程级租约恢复；若上面登记了同 Provider blocker，
	// RequestRelease 会明确拒绝并保留安装值，不会因弱 Owner 随后失效而被新 GameInstance 回收。
	if (LeaseProviderToRelease.IsSet())
	{
		FU_RequestNetDriverLeaseRelease(
			LeaseProviderToRelease.GetValue(),
			TEXT("GameInstanceSubsystem deinitialize"));
	}

	// 诊断分发器最后销毁，确保上面的不确定操作和释放决定都可被导出。
	if (Diagnostics.IsValid())
	{
		Diagnostics->DetachViewport();
		Diagnostics.Reset();
	}

	//GameInstance销毁后，不再存在活动游戏会话
	ActiveGameplayProvider.Reset();
	PreparedNetDriverProvider.Reset();

	Super::Deinitialize();
}

APlayerController* UFU_OnlineSessionSubsystem::FU_GetLocalPlayerController() const
{
	if (const UWorld* World = GetWorld())
	{
		return World->GetFirstPlayerController();
	}

	return nullptr;
}





//---------------------------------------------------------------------------
//蓝图只能调用普通UFUNCTION；这些薄包装只负责在编译期选择 Steam Provider，
//创建、查找、加入和销毁的实际逻辑仍然统一复用模板实现
FFU_OnlineProviderStatus UFU_OnlineSessionSubsystem::CheckSteamProviderStatus() const
{
	return FU_CheckProviderStatus<EFU_OnlineProvider::Steam>();
}

FFU_OnlineProviderStatus UFU_OnlineSessionSubsystem::CheckLanProviderStatus() const
{
	return FU_CheckProviderStatus<EFU_OnlineProvider::Lan>();
}

void UFU_OnlineSessionSubsystem::CreateSteamSession(const int32 MaxPlayers, const FString& RoomName, const FString& RoomPassword)
{
	FU_CreateSession<EFU_OnlineProvider::Steam>(MaxPlayers, RoomName, RoomPassword);
}

void UFU_OnlineSessionSubsystem::FindSteamSessions(const FString& RoomName, const int32 MaxResults)
{
	FU_FindSessions<EFU_OnlineProvider::Steam>(RoomName, MaxResults);
}

void UFU_OnlineSessionSubsystem::JoinSteamSession(const FString& SessionId, const FString& RoomPasswordInput)
{
	FU_JoinSession<EFU_OnlineProvider::Steam>(SessionId, RoomPasswordInput);
}

void UFU_OnlineSessionSubsystem::DestroySteamSession()
{
	FU_DestroySession<EFU_OnlineProvider::Steam>();
}
//---------------------------------------------------------------------------


//***************************************************************************
//LAN包装选择NULL Provider；与Steam共用同一套模板流程，但持有独立状态和委托
void UFU_OnlineSessionSubsystem::CreateLanSession(const int32 MaxPlayers, const FString& RoomName, const FString& RoomPassword)
{
	FU_CreateSession<EFU_OnlineProvider::Lan>(MaxPlayers, RoomName, RoomPassword);
}

void UFU_OnlineSessionSubsystem::FindLanSessions(const FString& RoomName, const int32 MaxResults)
{
	FU_FindSessions<EFU_OnlineProvider::Lan>(RoomName, MaxResults);
}

void UFU_OnlineSessionSubsystem::JoinLanSession(const FString& SessionId, const FString& RoomPasswordInput)
{
	FU_JoinSession<EFU_OnlineProvider::Lan>(SessionId, RoomPasswordInput);
}

void UFU_OnlineSessionSubsystem::DestroyLanSession()
{
	FU_DestroySession<EFU_OnlineProvider::Lan>();
}
//***************************************************************************

int32 UFU_OnlineSessionSubsystem::GetSessionPing(
	const EFU_OnlineProvider Provider, const FString& SessionId, bool& bIsEstimated) const
{
	// 直接读取搜索/清理流程维护的两份缓存，避免为房间列表维护另一份会过期的 Ping 状态。
	return FUOnlineSession::GetCachedSessionPing(
		SteamState.CachedSearchResults, LanState.CachedSearchResults, Provider, SessionId, bIsEstimated);
}
