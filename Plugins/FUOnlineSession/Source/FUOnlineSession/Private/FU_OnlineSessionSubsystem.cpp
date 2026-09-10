#include "FU_OnlineSessionSubsystem.h"
//模板通过ProviderTraits取得Steam/NULL的固定特化版本
#include "ProviderTraits/FU_OnlineSessionProviderTraits.h"
#include "FU_OnlineProviderStatusEvaluator.h"
#include "FU_OnlineSessionRequestValidation.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Online/OnlineSessionNames.h"
#include "Interfaces/OnlineIdentityInterface.h"
#include "OnlineSessionSettings.h"
#include "OnlineSubsystem.h"
#include "OnlineSubsystemNames.h"
#include "OnlineSubsystemUtils.h"


//https://dev.epicgames.com/documentation/unreal-engine/online-subsystem-steam-interface-in-unreal-engine?lang=zh-CN
//https://partner.steamgames.com/doc/api/ISteamMatchmaking#LobbyCreated_t
//https://partner.steamgames.com/
//https://dev.epicgames.com/documentation/unreal-engine/online-subsystem-session-interface-in-unreal-engine
namespace FUOnlineSession
{
	const FName RoomNameSetting(TEXT("FU_RoomName"));
	const FName RoomPasswordSetting(TEXT("FU_RoomPassword"));

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
				TEXT("%s OnlineSubsystem 未加载，请检查插件启用状态和 DefaultEngine.ini"),
				ProviderName);

		case EFU_OnlineProviderStatusCode::SessionInterfaceUnavailable:
			return FString::Printf(TEXT("%s OnlineSubsystem 没有提供 Session Interface"), ProviderName);

		case EFU_OnlineProviderStatusCode::IdentityInterfaceUnavailable:
			return TEXT("Steam Identity Interface 不可用，请确认 Steam 客户端和 OnlineSubsystemSteam 已启动");

		case EFU_OnlineProviderStatusCode::NotLoggedIn:
			return TEXT("本地用户尚未登录 Steam，请先启动并登录 Steam 客户端");

		default:
			return TEXT("未知的在线提供方状态");
		}
	}
	
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
		//参数；Traits同时为Steam和NULL提供可读的日志名称。
		UE_LOG(LogTemp, Error, TEXT("在线子系统不可用：%s"), FProviderTraits::GetDebugName());
		return nullptr;
	}
	
	return OnlineSubsystem->GetSessionInterface();
}

//运行时状态检测模板：只读取接口状态，不缓存接口，也不改变 Provider 的异步操作状态。
template<EFU_OnlineProvider Provider>
FFU_OnlineProviderStatus UFU_OnlineSessionSubsystem::FU_CheckProviderStatus() const
{
	using FProviderTraits = TFU_OnlineSessionProviderTraits<Provider>;

	FFU_OnlineProviderStatus Result;
	Result.Provider = Provider;
	Result.SubsystemName = FProviderTraits::GetSubsystemName();

	FFU_OnlineProviderStatusInputs Inputs;
	const UWorld* World = GetWorld();
	Inputs.bHasWorld = World != nullptr;

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
		}
	}

	Result.StatusCode = FFU_OnlineProviderStatusEvaluator::Evaluate(Provider, Inputs);
	Result.bIsReady = Result.StatusCode == EFU_OnlineProviderStatusCode::Ready;
	Result.Message = FUOnlineSession::GetProviderStatusMessage(Provider, Result.StatusCode);

	return Result;
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
	
	//状态Creating结束
	if (State.OperationState == EFU_ProviderOperationState::Creating)
	{
		State.OperationState = EFU_ProviderOperationState::Idle;
	}
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

	if (State.OperationState == EFU_ProviderOperationState::Finding)
	{
		State.OperationState = EFU_ProviderOperationState::Idle;
	}
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

	if (State.OperationState == EFU_ProviderOperationState::Joining)
	{
		State.OperationState = EFU_ProviderOperationState::Idle;
	}
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

	if (State.OperationState == EFU_ProviderOperationState::Destroying)
	{
		State.OperationState = EFU_ProviderOperationState::Idle;
	}
}

//清理Provider状态
template<EFU_OnlineProvider Provider>
void UFU_OnlineSessionSubsystem::FU_ClearProviderState()
{
	FFU_OnlineProviderState& State = FU_GetProviderState<Provider>();

	//先保存接口和搜索状态，因为后面的清理函数会改变OperationState
	const IOnlineSessionPtr SessionInterfaceToRelease = State.SessionInterface;

	const bool bShouldCancelFind = State.OperationState == EFU_ProviderOperationState::Finding && SessionInterfaceToRelease.IsValid() && State.SessionSearch.IsValid();

	//先解除所有委托，防止取消搜索时再次回调当前对象
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
	State.PendingCreateRoomPassword.Reset();
	State.PendingMaxPlayers = 0;
	State.PendingOperation = EFU_PendingOperation::None;
	State.OperationState = EFU_ProviderOperationState::Idle;
	State.SessionInterface.Reset();
}

//检查并销毁已有的Session
template<EFU_OnlineProvider Provider>
bool UFU_OnlineSessionSubsystem::FU_DestroyExistingSessionForPendingOperation(const EFU_PendingOperation InPendingOperation)
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
	FU_DestroySession<Provider>(InPendingOperation);
	return true;
}

//销毁入口
template<EFU_OnlineProvider Provider>
void UFU_OnlineSessionSubsystem::FU_DestroySession(const EFU_PendingOperation InPendingOperation)
{
    FFU_OnlineProviderState& State = FU_GetProviderState<Provider>();

    //同一个Provider同一时间只能执行一个Session异步操作
    if (State.OperationState != EFU_ProviderOperationState::Idle)
    {
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

    //公开销毁入口可能尚未缓存接口，因此在这里主动取得
    if (!State.SessionInterface.IsValid())
    {
        State.SessionInterface = FU_GetSessionInterface<Provider>();
    }

    if (!State.SessionInterface.IsValid())
    {
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
            FU_CreateSessionInternal<Provider>();
        }
        else if (InPendingOperation == EFU_PendingOperation::Join)
        {
            FU_JoinSessionInternal<Provider>();
        }
        else
        {
            if (ActiveGameplayProvider.IsSet() && ActiveGameplayProvider.GetValue() == Provider)
            {
                ActiveGameplayProvider.Reset();
            }

            OnDestroySessionComplete.Broadcast(Provider, true);
        }

        return;
    }

    State.PendingOperation = InPendingOperation;
    State.OperationState = EFU_ProviderOperationState::Destroying;

    //将Provider编译进回调地址，回调触发时不会丢失来源
    State.DestroyDelegateHandle = State.SessionInterface->AddOnDestroySessionCompleteDelegate_Handle(FOnDestroySessionCompleteDelegate::CreateUObject(this,&ThisClass::FU_OnDestroySessionComplete<Provider>));

    //返回false代表请求没有成功启动，不会再收到完成回调
    if (!State.SessionInterface->DestroySession(NAME_GameSession))
    {
        const EFU_PendingOperation FailedOperation = State.PendingOperation;

        FU_ClearDestroyDelegate<Provider>();
        State.PendingOperation = EFU_PendingOperation::None;

        if (FailedOperation == EFU_PendingOperation::None)
        {
            OnDestroySessionComplete.Broadcast(Provider, false);
        }
        else if (FailedOperation == EFU_PendingOperation::Create)
        {
            //销毁未能启动，后续创建不会执行，丢弃等待中的创建参数
            State.PendingCreateRoomName.Reset();
            State.PendingCreateRoomPassword.Reset();
            State.PendingMaxPlayers = 0;
            OnCreateSessionCompleteV2.Broadcast(Provider, false);
        }
        else
        {
            //销毁未能启动，后续加入不会执行，丢弃等待中的搜索结果
            State.PendingJoinResult.Reset();
            OnJoinSessionCompleteV2.Broadcast(Provider, EFU_JoinSessionResult::UnknownError);
        }
    }
}

//销毁完成后回调
template<EFU_OnlineProvider Provider>
void UFU_OnlineSessionSubsystem::FU_OnDestroySessionComplete(FName SessionName,const bool bWasSuccessful)
{
	FFU_OnlineProviderState& State = FU_GetProviderState<Provider>();

	//清理函数会把Destroying恢复为Idle
	const EFU_PendingOperation CompletedOperation = State.PendingOperation;

	FU_ClearDestroyDelegate<Provider>();
	State.PendingOperation = EFU_PendingOperation::None;

	if (!bWasSuccessful)
	{
		if (CompletedOperation == EFU_PendingOperation::None)
		{
			OnDestroySessionComplete.Broadcast(Provider, false);
		}
		else if (CompletedOperation == EFU_PendingOperation::Create)
		{
			//销毁失败后创建链终止
			State.PendingCreateRoomName.Reset();
			State.PendingCreateRoomPassword.Reset();
			State.PendingMaxPlayers = 0;
			OnCreateSessionCompleteV2.Broadcast(Provider, false);
		}
		else
		{
			//销毁失败后加入链终止，原搜索结果不能继续使用
			State.PendingJoinResult.Reset();
			OnJoinSessionCompleteV2.Broadcast(Provider, EFU_JoinSessionResult::UnknownError);
		}

		return;
	}

	//销毁成功后，旧游戏会话已经不再有效
	if (ActiveGameplayProvider.IsSet() && ActiveGameplayProvider.GetValue() == Provider)
	{
		ActiveGameplayProvider.Reset();
	}

	if (CompletedOperation == EFU_PendingOperation::Create)
	{
		//下一阶段会实现这个模板
		FU_CreateSessionInternal<Provider>();
	}
	else if (CompletedOperation == EFU_PendingOperation::Join)
	{
		//加入阶段会实现这个模板
		FU_JoinSessionInternal<Provider>();
	}
	else
	{
		//只有玩家显式销毁时才广播销毁完成
		OnDestroySessionComplete.Broadcast(Provider, true);
	}
}

//实现创建房间入口
template<EFU_OnlineProvider Provider>
void UFU_OnlineSessionSubsystem::FU_CreateSession(const int32 MaxPlayers, const FString& RoomName, const FString& RoomPassword)
{
	FFU_OnlineProviderState& State = FU_GetProviderState<Provider>();

	//一个游戏实例只能有一种活动游戏会话
	//如果另一种Provider已经处于游戏会话中，要求先显式销毁
	if (ActiveGameplayProvider.IsSet() && ActiveGameplayProvider.GetValue() != Provider)
	{
		OnCreateSessionCompleteV2.Broadcast(Provider, false);
		return;
	}

	//当前Provider正在执行其他异步操作时，不能覆盖它保存的数据和委托
	if (State.OperationState != EFU_ProviderOperationState::Idle)
	{
		OnCreateSessionCompleteV2.Broadcast(Provider, false);
		return;
	}

	if (!FFU_SessionRequestValidation::CanCreate(MaxPlayers, RoomName))
	{
		OnCreateSessionCompleteV2.Broadcast(Provider, false);
		return;
	}

	State.SessionInterface = FU_GetSessionInterface<Provider>();

	if (!State.SessionInterface.IsValid())
	{
		OnCreateSessionCompleteV2.Broadcast(Provider, false);
		return;
	}

	//密码不应Trim；
	State.PendingCreateRoomName = RoomName.TrimStartAndEnd();
	State.PendingCreateRoomPassword = RoomPassword;
	State.PendingMaxPlayers = MaxPlayers;

	//待销毁回调继续创建
	if (!FU_DestroyExistingSessionForPendingOperation<Provider>(EFU_PendingOperation::Create))
	{
		FU_CreateSessionInternal<Provider>();
	}
}

//Creating应该由FU_CreateSessionInternal()设置
template<EFU_OnlineProvider Provider>
void UFU_OnlineSessionSubsystem::FU_CreateSessionInternal()
{
    using FProviderTraits = TFU_OnlineSessionProviderTraits<Provider>;

    FFU_OnlineProviderState& State = FU_GetProviderState<Provider>();

    //销毁完成回调会先把状态恢复为Idle，然后才能继续创建
    if (State.OperationState != EFU_ProviderOperationState::Idle)
    {
        OnCreateSessionCompleteV2.Broadcast(Provider, false);
        return;
    }

    APlayerController* PlayerController = FU_GetLocalPlayerController();

    if (!State.SessionInterface.IsValid() || !PlayerController || !PlayerController->GetLocalPlayer())
    {
        //创建无法开始时，等待参数已经没有继续保留的意义
        State.PendingCreateRoomName.Reset();
        State.PendingCreateRoomPassword.Reset();
        State.PendingMaxPlayers = 0;

        OnCreateSessionCompleteV2.Broadcast(Provider,false);
        return;
    }

    FOnlineSessionSettings Settings;

    //Traits在编译期为Steam与NULL写入不同配置
    FProviderTraits::ConfigureCreateSettings(Settings);

    Settings.NumPublicConnections = State.PendingMaxPlayers;

    Settings.Set(FUOnlineSession::RoomNameSetting, State.PendingCreateRoomName,EOnlineDataAdvertisementType::ViaOnlineServiceAndPing);

    Settings.Set(FUOnlineSession::RoomPasswordSetting, State.PendingCreateRoomPassword, EOnlineDataAdvertisementType::ViaOnlineService
    );

    State.OperationState = EFU_ProviderOperationState::Creating;

    //Provider被编译进回调函数地址，回调时不会丢失来源
    State.CreateDelegateHandle = State.SessionInterface->AddOnCreateSessionCompleteDelegate_Handle(FOnCreateSessionCompleteDelegate::CreateUObject(this, &ThisClass::FU_OnCreateSessionComplete<Provider>));

    //返回false表示创建请求没有启动，不会收到异步完成回调
    if (!State.SessionInterface->CreateSession(PlayerController->GetLocalPlayer()->GetControllerId(),NAME_GameSession,Settings))
    {
        FU_ClearCreateDelegate<Provider>();

        State.PendingCreateRoomName.Reset();
        State.PendingCreateRoomPassword.Reset();
        State.PendingMaxPlayers = 0;

        OnCreateSessionCompleteV2.Broadcast(Provider, false);
    }
}

//实现创建完成回调
template<EFU_OnlineProvider Provider>
void UFU_OnlineSessionSubsystem::FU_OnCreateSessionComplete(FName SessionName, const bool bWasSuccessful)
{
	FFU_OnlineProviderState& State = FU_GetProviderState<Provider>();

	//先解除委托并把Creating恢复为Idle
	FU_ClearCreateDelegate<Provider>();

	if (bWasSuccessful)
	{
		//只有创建真正成功后，才能记录活动游戏会话来源
		ActiveGameplayProvider = Provider;
	}

	//无论成功还是失败，本次请求参数都已经完成使命
	State.PendingCreateRoomName.Reset();
	State.PendingCreateRoomPassword.Reset();
	State.PendingMaxPlayers = 0;

	OnCreateSessionCompleteV2.Broadcast(Provider, bWasSuccessful);
}

//实现搜索入口
template<EFU_OnlineProvider Provider>
void UFU_OnlineSessionSubsystem::FU_FindSessions(const FString& RoomName,const int32 MaxResults)
{
    using FProviderTraits = TFU_OnlineSessionProviderTraits<Provider>;

    FFU_OnlineProviderState& State = FU_GetProviderState<Provider>();

    //同一个Provider不能同时创建、搜索、加入或销毁
    if (State.OperationState != EFU_ProviderOperationState::Idle)
    {
        OnFindSessionCompleteV2.Broadcast(Provider, TArray<FFU_SessionResult>{}, false);
        return;
    }

    APlayerController* PlayerController = FU_GetLocalPlayerController();

    State.SessionInterface = FU_GetSessionInterface<Provider>();

    if (!State.SessionInterface.IsValid() || !PlayerController || !PlayerController->GetLocalPlayer() || MaxResults <= 0)
    {
        OnFindSessionCompleteV2.Broadcast(Provider, TArray<FFU_SessionResult>{}, false);
        return;
    }

    //搜索开始后
    State.CachedSearchResults.Reset();
    State.PendingJoinResult.Reset();

    State.PendingFindRoomName = RoomName.TrimStartAndEnd();

    //FOnlineSessionSearch必须存放在State中，因为搜索是异步操作
    State.SessionSearch = MakeShared<FOnlineSessionSearch>();

    State.SessionSearch->MaxSearchResults = MaxResults;

    //Steam：平台在线搜索，并添加SEARCH_LOBBIES
    //NULL：局域网广播搜索，不添加SEARCH_LOBBIES
    FProviderTraits::ConfigureSearch(*State.SessionSearch);

    State.OperationState = EFU_ProviderOperationState::Finding;

    //把Provider编译进回调地址
    State.FindDelegateHandle = State.SessionInterface->AddOnFindSessionsCompleteDelegate_Handle(
    FOnFindSessionsCompleteDelegate::CreateUObject(this, &ThisClass::FU_OnFindSessionsComplete<Provider>));

    //返回false表示搜索没有启动，也不会产生完成回调
    if (!State.SessionInterface->FindSessions(PlayerController->GetLocalPlayer()->GetControllerId(),State.SessionSearch.ToSharedRef()))
    {
        FU_ClearFindDelegate<Provider>();

        State.SessionSearch.Reset();
        State.PendingFindRoomName.Reset();

        OnFindSessionCompleteV2.Broadcast(Provider, TArray<FFU_SessionResult>{}, false);
    }
}

//实现搜索完成回调
template<EFU_OnlineProvider Provider>
void UFU_OnlineSessionSubsystem::FU_OnFindSessionsComplete(const bool bWasSuccessful)
{
    FFU_OnlineProviderState& State = FU_GetProviderState<Provider>();

    //解除委托，并把Finding恢复成Idle
    FU_ClearFindDelegate<Provider>();

    TArray<FFU_SessionResult> BlueprintResults;
    State.CachedSearchResults.Reset();

    if (bWasSuccessful && State.SessionSearch.IsValid())
    {
        for (const FOnlineSessionSearchResult& SearchResult : State.SessionSearch->SearchResults)
        {
            FString FoundRoomName;
        	FString FoundPassword;

            //没有插件房间名标记的Session不属于本插件创建的房间
            const bool bHasRoomName = SearchResult.Session.SessionSettings.Get( FUOnlineSession::RoomNameSetting, FoundRoomName);
        	const bool bHasPasswordSetting = SearchResult.Session.SessionSettings.Get(FUOnlineSession::RoomPasswordSetting,FoundPassword);
        	
        	
            // 房间名是识别本插件房间的必要标记；空密码元数据可能被部分 Provider 省略，
            // 因此不能因为没有读取到密码字段就把公开房间从搜索结果中过滤掉。
            if (!bHasRoomName)
            {
                continue;
            }

            //输入空房间名表示显示全部本插件房间；
            //非空时才执行精确名称过滤
            if (!State.PendingFindRoomName.IsEmpty() && FoundRoomName != State.PendingFindRoomName)
            {
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
    State.SessionSearch.Reset();
    State.PendingFindRoomName.Reset();

    //搜索成功但Results为空不是网络失败，而是当前没有匹配房间
    OnFindSessionCompleteV2.Broadcast(Provider, BlueprintResults, bWasSuccessful);
	

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
    FFU_OnlineProviderState& State = FU_GetProviderState<Provider>();

    //一个GameInstance只能有一种活动游戏会话
    if (ActiveGameplayProvider.IsSet() && ActiveGameplayProvider.GetValue() != Provider)
    {
        OnJoinSessionCompleteV2.Broadcast(Provider,EFU_JoinSessionResult::UnknownError);
        return;
    }

    //防止加入请求覆盖该Provider正在执行的异步操作
    if (State.OperationState != EFU_ProviderOperationState::Idle)
    {
        OnJoinSessionCompleteV2.Broadcast(Provider,EFU_JoinSessionResult::UnknownError);
        return;
    }

    State.SessionInterface = FU_GetSessionInterface<Provider>();

    if (!State.SessionInterface.IsValid())
    {
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
            OnJoinSessionCompleteV2.Broadcast(Provider,EFU_JoinSessionResult::InvalidPassword);
            return;
        }

        //不能只保存SessionId，JoinSession需要完整的原生搜索结果
        State.PendingJoinResult = SearchResult;
        break;
    }

    if (!bFoundSessionId || !State.PendingJoinResult.IsSet())
    {
        OnJoinSessionCompleteV2.Broadcast(Provider,EFU_JoinSessionResult::SessionDoesNotExist);
        return;
    }

    //已有同Provider Session时，销毁完成回调会继续执行加入
    if (!FU_DestroyExistingSessionForPendingOperation<Provider>(EFU_PendingOperation::Join))
    {
        FU_JoinSessionInternal<Provider>();
    }
}

//实现内部加入函数
template<EFU_OnlineProvider Provider>
void UFU_OnlineSessionSubsystem::FU_JoinSessionInternal()
{
	FFU_OnlineProviderState& State = FU_GetProviderState<Provider>();

	//销毁完成回调必须先把Destroying恢复成Idle
	if (State.OperationState != EFU_ProviderOperationState::Idle)
	{
		State.PendingJoinResult.Reset();

		OnJoinSessionCompleteV2.Broadcast(Provider, EFU_JoinSessionResult::UnknownError);
		return;
	}

	APlayerController* PlayerController = FU_GetLocalPlayerController();

	if (!State.SessionInterface.IsValid() || !PlayerController || !PlayerController->GetLocalPlayer() || !State.PendingJoinResult.IsSet())
	{
		State.PendingJoinResult.Reset();

		OnJoinSessionCompleteV2.Broadcast(Provider,EFU_JoinSessionResult::UnknownError);
		return;
	}

	State.OperationState = EFU_ProviderOperationState::Joining;

	//将Provider编译进回调函数地址
	State.JoinDelegateHandle = State.SessionInterface->AddOnJoinSessionCompleteDelegate_Handle(
	FOnJoinSessionCompleteDelegate::CreateUObject(this, &ThisClass::FU_OnJoinSessionComplete<Provider>));

	//JoinSession会使用搜索阶段保存下来的完整原生结果
	if (!State.SessionInterface->JoinSession(PlayerController->GetLocalPlayer()->GetControllerId(),NAME_GameSession,State.PendingJoinResult.GetValue()))
	{
		FU_ClearJoinDelegate<Provider>();
		State.PendingJoinResult.Reset();

		OnJoinSessionCompleteV2.Broadcast(Provider,EFU_JoinSessionResult::UnknownError);
	}
}

//实现加入完成回调模板函数
template<EFU_OnlineProvider Provider>
void UFU_OnlineSessionSubsystem::FU_OnJoinSessionComplete(FName SessionName,const EOnJoinSessionCompleteResult::Type Result)
{
    FFU_OnlineProviderState& State = FU_GetProviderState<Provider>();

    //先解绑回调并把Joining恢复为Idle
    FU_ClearJoinDelegate<Provider>();

    EFU_JoinSessionResult BlueprintResult = EFU_JoinSessionResult::UnknownError;

    switch (Result)
    {
    case EOnJoinSessionCompleteResult::Success:
    {
        //引擎已经成功加入命名Session，因此记录实际所属Provider
        ActiveGameplayProvider = Provider;

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

        APlayerController* PlayerController = FU_GetLocalPlayerController();

        if (!PlayerController)
        {
            BlueprintResult = EFU_JoinSessionResult::UnknownError;
            break;
        }

        //只有取得Provider返回的真实连接地址后才能切换地图
        PlayerController->ClientTravel(ConnectString,ETravelType::TRAVEL_Absolute);

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

    //加入请求已结束，原始待加入结果不再需要
    State.PendingJoinResult.Reset();

    OnJoinSessionCompleteV2.Broadcast(Provider,BlueprintResult);
}

//

void UFU_OnlineSessionSubsystem::Deinitialize()
{
	// GameInstance 退出时分别清理两套 Provider 状态；旧的共享状态链已经移除。
	FU_ClearProviderState<EFU_OnlineProvider::Steam>();
	FU_ClearProviderState<EFU_OnlineProvider::Lan>();

	//GameInstance销毁后，不再存在活动游戏会话
	ActiveGameplayProvider.Reset();

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
