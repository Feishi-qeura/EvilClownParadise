#pragma once

#include "CoreMinimal.h"
#include "Interfaces/OnlineSessionInterface.h"
#include "OnlineSessionSettings.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Engine/EngineBaseTypes.h"
#include "Net/Core/Connection/NetEnums.h"
#include "FU_OnlineSessionTypes.h"
#include "FU_OnlineSessionSubsystem.generated.h"




/** Manages one game-instance session through the currently configured OnlineSubsystem provider. */
UCLASS(BlueprintType)
class FUONLINESESSION_API UFU_OnlineSessionSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/**
	 * 同步检查Steam Lobby所需运行环境
	 * 本函数只读取状态，不会尝试自动登录，也不会开始任何Session异步操作
	 */
	UFUNCTION(BlueprintPure, Category="FUOnlineSession|Online Session|Steam|Status")
	FFU_OnlineProviderStatus CheckSteamProviderStatus() const;

	/**
	 * 同步检查NULL/LAN所需运行环境
	 * NULL不需要Steam Identity登录，只要NULL子系统和Session Interface可用即可
	 */
	UFUNCTION(BlueprintPure, Category="FUOnlineSession|Online Session|LAN|Status")
	FFU_OnlineProviderStatus CheckLanProviderStatus() const;
	
	//Steam蓝图入口:负责选择Provider，实现由template去做
	/* Steam创建房间
	 * 1.负责创建房间，是谁？ Steam的会话，在线子系统会走Steam Lobby
	 * 2.形参说明：MaxPlayers=最大玩家数量 RoomName=房间名称 RoomPassword=房间密码
	 */
	UFUNCTION(BlueprintCallable, Category="FUOnlineSession|Online Session|Steam")
	void CreateSteamSession(int32 MaxPlayers, const FString& RoomName, const FString& RoomPassword);
	
	//Steam查找房间：
	UFUNCTION(BlueprintCallable, Category="FUOnlineSession|Online Session|Steam")
	void FindSteamSessions(const FString& RoomName, int32 MaxResults = 20);
	
	//Steam加入房间
	UFUNCTION(BlueprintCallable, Category="FUOnlineSession|Online Session|Steam")
	void JoinSteamSession(const FString& SessionId, const FString& RoomPasswordInput);
	
	//销毁Steam房间
	UFUNCTION(BlueprintCallable, Category="FUOnlineSession|Online Session|Steam")
	void DestroySteamSession();
	
	
	//LAN本地局域网蓝图入口：使用OnlineSubsystemNULL
	//创建房间
	UFUNCTION(BlueprintCallable, Category="FUOnlineSession|Online Session|LAN")
	void CreateLanSession(int32 MaxPlayers, const FString& RoomName, const FString& RoomPassword);
	
	//查找房间
	UFUNCTION(BlueprintCallable, Category="FUOnlineSession|Online Session|LAN")
	void FindLanSessions(const FString& RoomName, int32 MaxResults = 20);
	
	//加入房间
	UFUNCTION(BlueprintCallable, Category="FUOnlineSession|Online Session|LAN")
	void JoinLanSession(const FString& SessionId, const FString& RoomPasswordInput);
	
	//销毁房间
	UFUNCTION(BlueprintCallable, Category="FUOnlineSession|Online Session|LAN")
	void DestroyLanSession();
	
	
	

	UPROPERTY(BlueprintAssignable, Category = "FUOnlineSession|Online Session|Provider")
	FFU_OnCreateSessionCompleteV2 OnCreateSessionCompleteV2;

	UPROPERTY(BlueprintAssignable, Category = "FUOnlineSession|Online Session|Provider")
	FFU_OnFindSessionCompleteV2 OnFindSessionCompleteV2;

	UPROPERTY(BlueprintAssignable, Category = "FUOnlineSession|Online Session|Provider")
	FFU_OnJoinSessionCompleteV2 OnJoinSessionCompleteV2;

	UPROPERTY(BlueprintAssignable, Category = "FUOnlineSession|Online Session|Provider")
	FFU_OnDestroySessionComplete OnDestroySessionComplete;

	/**
	 * 【FU 修复：连接阶段错误】
	 * JoinSession 的 Success 只表示已经取得连接地址并开始 ClientTravel。
	 * 随后的 PendingConnectionFailure、地图不存在等错误会从本事件返回蓝图。
	 */
	UPROPERTY(BlueprintAssignable, Category = "FUOnlineSession|Online Session|Provider")
	FFU_OnOnlineConnectionFailure OnOnlineConnectionFailure;

private:
	//状态，待处理操作
	enum class EFU_PendingOperation : uint8
	{
		//没有，无，销毁
		None,
		//创建
		Create,
		//加入
		Join
	};
	
	//初始Idle -> 创建Creating -> 查找Finding -> 加入Joining -> 完成和失败Idle
	//状态，同一时间只执行一个Session异步操作，避免多个布尔值形成矛盾状态
	enum class EFU_ProviderOperationState : uint8
	{
		//等待
		Idle,
		//创建
		Creating,
		//查找
		Finding,
		//加入
		Joining,
		//移除
		Destroying
	};
	
	
	/*
	 *FFU_OnlineProviderState结构体 在线提供异步运行的方法，lobby和NULL格子保留一个实例
	 *搜索和创建可能需要不同提示
	 *理时需要知道哪种操作还在进行
	 *调试日志可以准确显示状态
	 *扩展可以允许搜索与某些非冲突操作并存
	 *
	 *某一个OnlineSubsystem提供方，在当前GameInstance中执行异步会话操作时，需要保存的全部运行时数据
	 *
	 *会特化2个实例，结构相同，数据独立
	*1.FFU_OnlineProviderState SteamState;
	*2.FFU_OnlineProviderState LanState;
	*/
	struct FFU_OnlineProviderState
	{
		//会话接口
		//1.SteamState.SessionInterface
		//2.LanState.SessionInterface
		IOnlineSessionPtr SessionInterface;
		
		//保存一次搜索的结果
		/* MaxSearchResults
		 * bIsLanQuery
		 * QuerySettings
		 * SearchResults 
		 * 注意：后台搜索，异步需要等待回调，所以它不能声明在局部变量 */
		TSharedPtr<FOnlineSessionSearch> SessionSearch;
		
		//完成搜索后保留下来的原生结果
		TArray<FOnlineSessionSearchResult> CachedSearchResults;
		
		//TOptional拥有两种明确状态
		/* 没有选中结果
		   PendingJoinResult.IsSet() == false

		   已经选中一个结果
		   PendingJoinResult.IsSet() == true */
		TOptional<FOnlineSessionSearchResult> PendingJoinResult;
		
		//创建使用
		FString PendingCreateRoomName;
		//查找完成后过滤房间名称时使用
		FString PendingFindRoomName;
		
		//测试密码
		FString PendingCreateRoomPassword;
		
		//当前公开连接数量
		int32 PendingMaxPlayers = 0;
		//当前正在销毁旧Session。销毁成功后应该继续做什么
		EFU_PendingOperation PendingOperation = EFU_PendingOperation::None;

		//记录当前异步操作；完成或同步启动失败后必须恢复为 Idle。
		EFU_ProviderOperationState OperationState = EFU_ProviderOperationState::Idle;
		
		//委托Handle清除
		FDelegateHandle CreateDelegateHandle;
		FDelegateHandle FindDelegateHandle;
		FDelegateHandle JoinDelegateHandle;
		FDelegateHandle DestroyDelegateHandle;
	};

	//Steam与NULL的接口、搜索结果和委托必须独立保存，不能复用旧的单状态字段，2个实例对象
	FFU_OnlineProviderState SteamState;
	FFU_OnlineProviderState LanState;

	//搜索不会修改该值；只有创建或加入成功后才记录当前实际游戏会话是否是NULL还是Lobby
	TOptional<EFU_OnlineProvider> ActiveGameplayProvider;

	//记录最近一次为 Create/Join 准备的驱动；网络失败发生在活动会话记录之前时也能判断来源。
	TOptional<EFU_OnlineProvider> PreparedNetDriverProvider;

	//引擎级失败委托必须保存 Handle，并在 GameInstanceSubsystem 销毁时显式解绑。
	FDelegateHandle NetworkFailureDelegateHandle;
	FDelegateHandle TravelFailureDelegateHandle;
	
	//声明模板成员函数
	template<EFU_OnlineProvider Provider> 
	FFU_OnlineProviderState& FU_GetProviderState();

	//const
	template<EFU_OnlineProvider Provider> 
	const FFU_OnlineProviderState& FU_GetProviderState() const;
	
	template<EFU_OnlineProvider Provider> 
	IOnlineSessionPtr FU_GetSessionInterface() const;

	//与创建/搜索模板相同：蓝图入口分开，状态检测核心通过 Provider 在编译期选择实现。
	template<EFU_OnlineProvider Provider>
	FFU_OnlineProviderStatus FU_CheckProviderStatus() const;

	/**
	 * 【FU 修复：异步操作的统一准入门】
	 * 蓝图可以调用 CheckSteamProviderStatus/CheckLanProviderStatus 来控制按钮，
	 * 但 Runtime 不能假设使用者一定会正确接线。因此创建、搜索、加入模板还会在内部
	 * 同步检查一次状态；失败时只写诊断日志并返回 false，不会启动任何 Session 异步任务。
	 */
	template<EFU_OnlineProvider Provider>
	bool FU_ValidateProviderReady(const TCHAR* OperationName) const;

	/**
	 * 【FU 修复：模板化传输选择】
	 * Provider 在编译期决定 GameNetDriver：Steam -> SteamSockets，LAN -> IpNetDriver。
	 * 函数必须在 OpenLevel(?listen) 或 ClientTravel 创建 NetDriver 之前调用。
	 */
	template<EFU_OnlineProvider Provider>
	bool FU_PrepareGameNetDriver();
	
	template<EFU_OnlineProvider Provider> 
	void FU_CreateSession(int32 MaxPlayers, const FString& RoomName, const FString& RoomPassword);
	
	template<EFU_OnlineProvider Provider>
	void FU_CreateSessionInternal();
	
	template<EFU_OnlineProvider Provider>
    void FU_FindSessions(const FString& RoomName, int32 MaxResults);
	
	template<EFU_OnlineProvider Provider>
	void FU_JoinSession(const FString& SessionId, const FString& RoomPasswordInput); 

	template<EFU_OnlineProvider Provider>
	void FU_JoinSessionInternal();
	
	template<EFU_OnlineProvider Provider>
    void FU_DestroySession(EFU_PendingOperation InPendingOperation = EFU_PendingOperation::None);

	template<EFU_OnlineProvider Provider>
	bool FU_DestroyExistingSessionForPendingOperation(EFU_PendingOperation InPendingOperation);
	
	//回调模板
	//SessionName:本地命名会话名称为NAME_GameSession
	//bWasSuccessful是否成功
	template<EFU_OnlineProvider Provider>
	void FU_OnCreateSessionComplete(FName SessionName, bool bWasSuccessful);

	//保存State.SessionSearch->SearchResults
	//bWasSuccessful为true，SearchResults为空则表示查找正常，房间没有被找到
	template<EFU_OnlineProvider Provider>
	void FU_OnFindSessionsComplete(bool bWasSuccessful);

	//NAME_GameSession。Result：输出对应的结果
	template<EFU_OnlineProvider Provider>
	void FU_OnJoinSessionComplete(FName SessionName, EOnJoinSessionCompleteResult::Type Result);

	template<EFU_OnlineProvider Provider>
	void FU_OnDestroySessionComplete(FName SessionName, bool bWasSuccessful);
	
	//清理函数
	/*
	 * FU_ClearCreateDelegate
	 * FU_ClearFindDelegate
	 * FU_ClearJoinDelegate
	 * FU_ClearDestroyDelegate
	 * 获取对应的ProviderState
	 * 从State.SessionInterface清除Handle
	 * OperationState状态为Idle
	 */
	template<EFU_OnlineProvider Provider>
    void FU_ClearCreateDelegate();

	template<EFU_OnlineProvider Provider>
	void FU_ClearFindDelegate();

	template<EFU_OnlineProvider Provider>
	void FU_ClearJoinDelegate();

	template<EFU_OnlineProvider Provider>
	void FU_ClearDestroyDelegate();
	
	//清理所有Delegate委托，清空SessionSearch，清空缓存结果，清空PendingJoinResult，清空SessionInterface，重置PendingOperation，OperationState = Idle
	template<EFU_OnlineProvider Provider>
	void FU_ClearProviderState();
	
	APlayerController* FU_GetLocalPlayerController() const;

	//只处理属于当前 GameInstance World 的引擎错误，避免 PIE 多 World 互相接收通知。
	void FU_OnNetworkFailure(UWorld* World, UNetDriver* NetDriver, ENetworkFailure::Type FailureType, const FString& ErrorString);
	void FU_OnTravelFailure(UWorld* World, ETravelFailure::Type FailureType, const FString& ErrorString);

	//旅行失败时优先使用活动 Provider，否则使用最近准备过驱动的 Provider。
	TOptional<EFU_OnlineProvider> FU_GetFailureProvider() const;
};
