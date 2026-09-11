#pragma once

#include "CoreMinimal.h"
#include "Interfaces/OnlineSessionInterface.h"
#include "OnlineSessionSettings.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Engine/EngineBaseTypes.h"
#include "Net/Core/Connection/NetEnums.h"
#include "Templates/UniquePtr.h"
#include "TimerManager.h"
#include "FU_OnlineDiagnosticTypes.h"
#include "FU_OnlineSessionTypes.h"
#include "FU_OnlineSessionSubsystem.generated.h"

// 【自包含声明】该头只通过指针接收 NetDriver；明确前置声明避免严格编译依赖 PCH 间接包含。
class UNetDriver;
class FFU_OnlineSessionDiagnostics;
class FFU_OnlineOperationStateMachine;
struct FFU_OperationTicket;
enum class EFU_OperationKind : uint8;
enum class EFU_NetDriverLeaseResult : uint8;

/**
 * 诊断分发器是 Private 实现；自定义删除器避免把私有 Slate/文件实现暴露到 Public 头，
 * 同时满足严格编译下 TUniquePtr 删除不完整类型的要求。
 */
struct FFU_OnlineSessionDiagnosticsDeleter
{
	void operator()(FFU_OnlineSessionDiagnostics* Diagnostics) const;
};

/** Private 操作状态机的删除也固定在 Runtime cpp，Public 头不泄露其实现。 */
struct FFU_OnlineOperationStateMachineDeleter
{
	void operator()(FFU_OnlineOperationStateMachine* StateMachine) const;
};



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

	/**
	 * 返回本 GameInstance 的有界、已脱敏诊断历史。
	 * 该数组是副本，Blueprint 不会修改 Runtime 内部的故障调查记录。
	 */
	UFUNCTION(BlueprintPure, Category="FUOnlineSession|Diagnostics")
	TArray<FFU_OnlineDiagnosticEvent> GetDiagnosticHistory() const;

	/** 清除当前 GameInstance 的诊断历史；不会关闭后续日志、浮层或 Blueprint 广播。 */
	UFUNCTION(BlueprintCallable, Category="FUOnlineSession|Diagnostics")
	void ClearDiagnosticHistory();

	/** 生成当前诊断快照的安全纯文本；不写文件，适合 Blueprint UI 预览或复制。 */
	UFUNCTION(BlueprintPure, Category="FUOnlineSession|Diagnostics")
	FString BuildDiagnosticReport() const;

	/**
	 * 将已脱敏报告保存到 Project/Saved/Logs/FUOnlineSession。
	 * 输出路径由插件固定，Blueprint 无法传入任意路径，失败时只返回安全的错误说明。
	 */
	UFUNCTION(BlueprintCallable, Category="FUOnlineSession|Diagnostics")
	bool SaveDiagnosticReport(FString& OutSavedPath, FString& OutError);

	/**
	 * 同步读取指定 Provider 的现有状态，并将一次已脱敏的环境快照广播到诊断历史。
	 * 该入口不提交 OSS 请求、不改变任一在途操作的 generation 或 OperationId；返回值与状态查询完全一致。
	 */
	UFUNCTION(BlueprintCallable, Category="FU Online Session|Diagnostics")
	FFU_OnlineProviderStatus RunProviderDiagnostics(EFU_OnlineProvider Provider);

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

	/**
	 * 保守尝试收敛指定 Provider；只有原始 OSS 回调终止、无 Session、全 World 无驱动且租约已释放才返回 true。
	 * 本函数不会强制取消不可取消的 OSS 操作，也不会越过进程级租约所有权规则。新 API 追加在旧蓝图入口之后。
	 */
	UFUNCTION(BlueprintCallable, Category="FU Online Session|Diagnostics")
	bool TryRecoverProvider(EFU_OnlineProvider Provider);
	
	
	

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

	/**
	 * 所有渠道共用同一份已脱敏事件：即使关闭 UE_LOG 或屏幕浮层，Blueprint 仍会收到它。
	 * UI 可以按 Severity、Code 和 OperationId 显示提示或上传安全诊断报告。
	 */
	UPROPERTY(BlueprintAssignable, Category = "FUOnlineSession|Diagnostics")
	FFU_OnOnlineDiagnosticEvent OnOnlineDiagnosticEvent;

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

		//委托Handle清除
		FDelegateHandle CreateDelegateHandle;
		FDelegateHandle FindDelegateHandle;
		FDelegateHandle JoinDelegateHandle;
		FDelegateHandle DestroyDelegateHandle;

		// Find 取消拥有独立 OSS 完成委托；失败取消只清本 Handle，原 Find delegate 继续等待。
		FDelegateHandle CancelFindDelegateHandle;

		// 每次提交都使用 generation 捕获的一次性 watchdog；Busy/预检拒绝不得触碰该 Handle。
		FTimerHandle OperationWatchdogHandle;

		// 状态机是操作生命周期唯一事实源；旧 OperationState 已删除，避免双份状态漂移。
		TUniquePtr<FFU_OnlineOperationStateMachine, FFU_OnlineOperationStateMachineDeleter> OperationMachine;
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

	// 每个 GameInstance 独占自己的诊断历史和 Slate 生命周期，避免 PIE 多实例串台。
	TUniquePtr<FFU_OnlineSessionDiagnostics, FFU_OnlineSessionDiagnosticsDeleter> Diagnostics;
	
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
	FFU_OnlineProviderStatus FU_CheckProviderStatus(bool bRequiresNetDriver = true) const;

	/**
	 * 【FU 修复：异步操作的统一准入门】
	 * 蓝图可以调用 CheckSteamProviderStatus/CheckLanProviderStatus 来控制按钮，
	 * 但 Runtime 不能假设使用者一定会正确接线。因此创建、搜索、加入模板还会在内部
	 * 同步检查一次状态；失败时只写诊断日志并返回 false，不会启动任何 Session 异步任务。
	 */
	template<EFU_OnlineProvider Provider>
	bool FU_ValidateProviderReady(
		EFU_OperationKind RootKind,
		const FGuid& OperationId,
		const TCHAR* OperationName,
		bool bRequiresNetDriver,
		TFunctionRef<void()> FailureContinuation);

	/**
	 * 【FU 修复：模板化传输选择】
	 * Provider 在编译期决定 GameNetDriver：Steam -> SteamSockets，LAN -> IpNetDriver。
	 * 函数必须在 OpenLevel(?listen) 或 ClientTravel 创建 NetDriver 之前调用。
	 */
	template<EFU_OnlineProvider Provider>
	bool FU_PrepareGameNetDriver(EFU_OperationKind RootKind, const FGuid& OperationId);
	
	template<EFU_OnlineProvider Provider> 
	void FU_CreateSession(int32 MaxPlayers, const FString& RoomName, const FString& RoomPassword);
	
	template<EFU_OnlineProvider Provider>
	void FU_CreateSessionInternal(FFU_OperationTicket* RootTicket = nullptr);
	
	template<EFU_OnlineProvider Provider>
    void FU_FindSessions(const FString& RoomName, int32 MaxResults);
	
	template<EFU_OnlineProvider Provider>
	void FU_JoinSession(const FString& SessionId, const FString& RoomPasswordInput); 

	template<EFU_OnlineProvider Provider>
	void FU_JoinSessionInternal(FFU_OperationTicket* RootTicket = nullptr);
	
	template<EFU_OnlineProvider Provider>
    void FU_DestroySession(EFU_PendingOperation InPendingOperation = EFU_PendingOperation::None, FFU_OperationTicket* RootTicket = nullptr);

	template<EFU_OnlineProvider Provider>
	bool FU_DestroyExistingSessionForPendingOperation(EFU_PendingOperation InPendingOperation, FFU_OperationTicket& RootTicket);
	
	//回调模板
	//SessionName:本地命名会话名称为NAME_GameSession
	//bWasSuccessful是否成功
	template<EFU_OnlineProvider Provider>
	void FU_OnCreateSessionComplete(FName SessionName, bool bWasSuccessful, uint64 Generation);

	//保存State.SessionSearch->SearchResults
	//bWasSuccessful为true，SearchResults为空则表示查找正常，房间没有被找到
	template<EFU_OnlineProvider Provider>
	void FU_OnFindSessionsComplete(bool bWasSuccessful, uint64 Generation);

	//NAME_GameSession。Result：输出对应的结果
	template<EFU_OnlineProvider Provider>
	void FU_OnJoinSessionComplete(FName SessionName, EOnJoinSessionCompleteResult::Type Result, uint64 Generation);

	template<EFU_OnlineProvider Provider>
	void FU_OnDestroySessionComplete(FName SessionName, bool bWasSuccessful, uint64 Generation);

	template<EFU_OnlineProvider Provider>
	void FU_OnCancelFindSessionsComplete(bool bWasSuccessful, uint64 Generation);

	template<EFU_OnlineProvider Provider>
	void FU_OnOperationTimeout(uint64 Generation);
	
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

	/**
	 * 网络/旅行失败通常发生在 OSS 回调已经把状态机置 Idle 之后；状态机仍保留最近的
	 * ActiveOperationId，因此这里仅只读继承该身份。没有任何关联操作时才创建独立 ID。
	 */
	FGuid FU_EmitConnectionFailureDiagnostic(
		EFU_OnlineProvider Provider,
		bool bIsTravelFailure,
		const TCHAR* StableStatus);

	/**
	 * Network/TravelFailure 只在接口有效、NamedSession 已消失、无 OSS 回调在途且全 World 安全时释放。
	 * 该门与普通 Destroy 完成路径分离，避免一次连接错误把仍被正式会话持有的进程租约提前恢复。
	 */
	bool FU_CanReleaseNetDriverAfterConnectionFailure(EFU_OnlineProvider Provider) const;

	/**
	 * 统一请求释放本 GameInstance 持有的进程级 NetDriver 租约。
	 * 协调器会在任一 World 仍有驱动或 PendingNetGame 时延迟恢复，因此调用方不需要冒险强改 GEngine。
	 */
	EFU_NetDriverLeaseResult FU_RequestNetDriverLeaseRelease(EFU_OnlineProvider Provider, const TCHAR* Reason);

	/** 以下 helper 只在 generation/kind/phase 验证通过后操作精确资源，保证迟到回调不能清新请求。 */
	template<EFU_OnlineProvider Provider>
	FFU_OnlineOperationStateMachine& FU_GetOperationMachine();

	template<EFU_OnlineProvider Provider>
	FFU_OperationTicket FU_BeginOperationAttempt(EFU_OperationKind RootKind);

	template<EFU_OnlineProvider Provider>
	bool FU_SubmitOperation(FFU_OperationTicket& Ticket, EFU_OperationKind SubmittedKind, uint64& OutGeneration);

	template<EFU_OnlineProvider Provider>
	void FU_ArmOperationWatchdog(uint64 Generation);

	template<EFU_OnlineProvider Provider>
	void FU_ClearOperationWatchdog();

	template<EFU_OnlineProvider Provider>
	void FU_ClearOperationDelegate(EFU_OperationKind SubmittedKind);

	template<EFU_OnlineProvider Provider>
	void FU_ClearFindCancellationDelegate();

	/** Find watchdog 触发后的唯一取消入口；取消失败仍保留原 Find completion 作为终点。 */
	template<EFU_OnlineProvider Provider>
	void FU_BeginFindCancellation(uint64 Generation);

	template<EFU_OnlineProvider Provider>
	void FU_BroadcastOperationFailure(EFU_OperationKind RootKind);

	template<EFU_OnlineProvider Provider>
	bool FU_StartRecoveryDestroy(bool bExplicitRetry);

	template<EFU_OnlineProvider Provider>
	bool FU_TryRecoverProvider();

	/**
	 * 所有 Steam/LAN 操作事件的唯一模板化出口。Provider 只能由调用模板和 traits 决定，
	 * 避免诊断层另建运行时映射表；RoomName 仅在确有排障价值时作为普通元数据传入。
	 */
	template<EFU_OnlineProvider Provider>
	void FU_EmitDiagnostic(
		EFU_OperationKind Kind,
		const FGuid& OperationId,
		EFU_OnlineDiagnosticPhase Phase,
		EFU_OnlineDiagnosticSeverity Severity,
		const TCHAR* Code,
		const FString& Message,
		const FString& RoomName = FString(),
		const TCHAR* StatusOverride = nullptr);

	/** production outcome builder 已填充 operation/code/status；模板边界只补 traits Provider/Subsystem。 */
	template<EFU_OnlineProvider Provider>
	void FU_EmitDiagnostic(FFU_OnlineDiagnosticEvent Event);

	/** Provider 状态查询的环境事件同样只能由模板和 traits 决定 Provider。 */
	template<EFU_OnlineProvider Provider>
	void FU_EmitEnvironmentDiagnostic(const FGuid& OperationId, const FFU_OnlineProviderStatus& Status);

	/** 为系统级失败补齐 World/PIE 上下文后进入统一诊断分发器。 */
	void FU_EmitDiagnostic(FFU_OnlineDiagnosticEvent Event);
};
