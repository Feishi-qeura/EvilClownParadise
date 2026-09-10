#pragma once

#include "CoreMinimal.h"
#include "FU_OnlineSessionTypes.generated.h"

/*针对已完成加入请求的蓝图安全结果（
 *Success成功
 *SessionIsFull会话已满
 *SessionDoesNotExist会话不存在
 *CouldNotRetrieveAddress无法获取地址
 *AlreadyInSession会话已存在
 *UnknownError未知错误
 */
UENUM(BlueprintType)
enum class EFU_JoinSessionResult : uint8
{
	//成功
	Success,
	//会话已满
	SessionIsFull,
	//会话不存在
	SessionDoesNotExist,
	//无法获取地址
	CouldNotRetrieveAddress,
	//会话已存在
	AlreadyInSession,
	//未知错误
	UnknownError,
	//用户输入的房间密码不正确
	InvalidPassword,

	//目标 Provider 所需的 NetDriver 没有准备成功；此时不会启动 ClientTravel
	NetDriverUnavailable

};
//扩展Steam和NULL在线提供方
UENUM(BlueprintType)
enum class EFU_OnlineProvider : uint8
{
	//steam联机，返回steam的lobby
	Steam UMETA(DisplayName = "Steam"),
	//本地局域网联机，所以需要NULL
	Lan UMETA(DisplayName = "Lan/NULL"),
};

/**
 * 检测某一种在线提供方时可能得到的结果。
 *
 * 这里不使用一个简单 bool，是因为“Steam 没有启动”和“Session 接口不存在”
 * 虽然都会导致不能创建房间，但应该在蓝图界面中显示不同的解决办法。
 */
UENUM(BlueprintType)
enum class EFU_OnlineProviderStatusCode : uint8
{
	//World、子系统、Session接口以及该提供方要求的登录条件都已经满足
	Ready UMETA(DisplayName = "Ready"),

	//GameInstance尚未关联有效World，通常表示调用时机太早或对象正在销毁
	WorldUnavailable UMETA(DisplayName = "World Unavailable"),

	//指定名称的OnlineSubsystem（STEAM或NULL）没有成功加载
	SubsystemUnavailable UMETA(DisplayName = "Subsystem Unavailable"),

	//OnlineSubsystem存在，但它没有提供Session Interface
	SessionInterfaceUnavailable UMETA(DisplayName = "Session Interface Unavailable"),

	//Steam子系统没有提供Identity Interface，因而无法确认本地Steam用户
	IdentityInterfaceUnavailable UMETA(DisplayName = "Identity Interface Unavailable"),

	//Steam Identity存在，但本地用户0当前没有登录Steam
	NotLoggedIn UMETA(DisplayName = "Not Logged In"),

	//引擎配置中没有名为 GameNetDriver 的定义，无法决定地图连接使用哪一种驱动
	NetDriverDefinitionUnavailable UMETA(DisplayName = "NetDriver Definition Unavailable"),

	//Provider 要求的驱动类没有加载；Steam 最常见原因是 SteamSockets 插件没有启用
	NetDriverClassUnavailable UMETA(DisplayName = "NetDriver Class Unavailable"),

	//当前 World 已经使用另一种驱动联网，必须先退出该网络世界才能切换 Provider
	ActiveNetDriverConflict UMETA(DisplayName = "Active NetDriver Conflict"),

	//Runtime 开发 AppID 动态层未通过所有权与有效值验证；Steam 不得继续实例化
	SteamAppIdBootstrapInvalid UMETA(DisplayName = "Steam AppID Bootstrap Invalid"),

	//SteamSockets 插件模块无法非致命加载
	SteamSocketsModuleUnavailable UMETA(DisplayName = "SteamSockets Module Unavailable"),

	//SteamSockets 模块已加载但因 Steam OSS/配置原因处于禁用状态
	SteamSocketsDisabled UMETA(DisplayName = "SteamSockets Disabled"),

	//SteamSockets 已启用但命名 SocketSubsystem 尚未注册
	SteamSocketsSocketSubsystemUnavailable UMETA(DisplayName = "SteamSockets SocketSubsystem Unavailable"),

	//Shipping 没有配置 ExpectedShippingSteamAppId，插件不能使用开发 AppID 代替
	ShippingSteamAppIdMissing UMETA(DisplayName = "Shipping Steam AppID Missing"),

	//运行中 OnlineSubsystem 的 AppID 与开发或 Shipping 配置期望不一致
	SteamAppIdMismatch UMETA(DisplayName = "Steam AppID Mismatch")
};

/**
 * Session 加入成功以后仍可能在 ClientTravel 阶段失败。
 * 该枚举让蓝图区分“网络连接失败”和“地图旅行失败”。
 */
UENUM(BlueprintType)
enum class EFU_OnlineConnectionFailureType : uint8
{
	NetworkFailure UMETA(DisplayName = "Network Failure"),
	TravelFailure UMETA(DisplayName = "Travel Failure")
};

/**
 * 蓝图安全的联网环境快照。
 *
 * 这是一次同步检查结果，不会启动创建、搜索或登录请求，也不会改变 Session 状态。
 * 蓝图可以使用 bIsReady 控制按钮是否可点击，再用 StatusCode 或 Message 显示原因。
 */
USTRUCT(BlueprintType)
struct FUONLINESESSION_API FFU_OnlineProviderStatus
{
	GENERATED_BODY()

	//本次检查的是Steam Lobby还是NULL/LAN
	UPROPERTY(BlueprintReadOnly, Category="FUOnlineSession|Online Session|Provider Status")
	EFU_OnlineProvider Provider = EFU_OnlineProvider::Steam;

	//便于蓝图直接接Branch；它只在StatusCode == Ready时为true
	UPROPERTY(BlueprintReadOnly, Category="FUOnlineSession|Online Session|Provider Status")
	bool bIsReady = false;

	//精确的机器可读状态，适合Switch on EFU_OnlineProviderStatusCode
	UPROPERTY(BlueprintReadOnly, Category="FUOnlineSession|Online Session|Provider Status")
	EFU_OnlineProviderStatusCode StatusCode = EFU_OnlineProviderStatusCode::WorldUnavailable;

	//对应的OnlineSubsystem名称，当前会是STEAM或NULL
	UPROPERTY(BlueprintReadOnly, Category="FUOnlineSession|Online Session|Provider Status")
	FName SubsystemName = NAME_None;

	//以下字段保留逐层检测结果，调试时可以快速定位失败停在哪一层
	UPROPERTY(BlueprintReadOnly, Category="FUOnlineSession|Online Session|Provider Status")
	bool bSubsystemAvailable = false;

	UPROPERTY(BlueprintReadOnly, Category="FUOnlineSession|Online Session|Provider Status")
	bool bSessionInterfaceAvailable = false;

	//LAN不要求Identity，因此该字段在LAN结果中允许为false，同时bIsReady仍可为true
	UPROPERTY(BlueprintReadOnly, Category="FUOnlineSession|Online Session|Provider Status")
	bool bIdentityInterfaceAvailable = false;

	//只有Steam检查会读取登录状态；LAN不依赖平台账号
	UPROPERTY(BlueprintReadOnly, Category="FUOnlineSession|Online Session|Provider Status")
	bool bLoggedIn = false;

	//当前 Provider 真正需要的传输驱动；Steam 与 LAN 的值不同。
	UPROPERTY(BlueprintReadOnly, Category="FUOnlineSession|Online Session|Provider Status")
	FName RequiredNetDriverClass = NAME_None;

	//引擎中是否存在 GameNetDriver 定义。
	UPROPERTY(BlueprintReadOnly, Category="FUOnlineSession|Online Session|Provider Status")
	bool bNetDriverDefinitionAvailable = false;

	//RequiredNetDriverClass 当前是否已经注册并能够被引擎加载。
	UPROPERTY(BlueprintReadOnly, Category="FUOnlineSession|Online Session|Provider Status")
	bool bNetDriverClassAvailable = false;

	//当前 World 已经存在活动驱动时记录其真实类名，便于判断是否需要先退出联网关卡。
	UPROPERTY(BlueprintReadOnly, Category="FUOnlineSession|Online Session|Provider Status")
	FName ActiveNetDriverClass = NAME_None;

	//面向玩家或开发者的可读说明；正式项目可以根据StatusCode换成本地化文本
	UPROPERTY(BlueprintReadOnly, Category="FUOnlineSession|Online Session|Provider Status")
	FString Message;

	//以下 Steam 专属字段始终保留原始探测结果；LAN 不依赖它们，仍可能 Ready。
	UPROPERTY(BlueprintReadOnly, Category="FUOnlineSession|Online Session|Provider Status")
	bool bSteamAppIdBootstrapReady = false;

	UPROPERTY(BlueprintReadOnly, Category="FUOnlineSession|Online Session|Provider Status")
	bool bSteamSocketsModuleAvailable = false;

	UPROPERTY(BlueprintReadOnly, Category="FUOnlineSession|Online Session|Provider Status")
	bool bSteamSocketsEnabled = false;

	UPROPERTY(BlueprintReadOnly, Category="FUOnlineSession|Online Session|Provider Status")
	bool bSteamSocketsSocketSubsystemAvailable = false;

	UPROPERTY(BlueprintReadOnly, Category="FUOnlineSession|Online Session|Provider Status")
	bool bSteamAppIdMatchesExpectation = false;
};

/** Blueprint-safe snapshot of a discovered online session. */
USTRUCT(BlueprintType)
struct FUONLINESESSION_API FFU_SessionResult
{
	GENERATED_BODY()
	
	//记录产生该结果的提供方，防止将NULL结果交给Steam的加入函数
	UPROPERTY(BlueprintReadOnly, Category="FUOnlineSession|Online Session|Property")
	EFU_OnlineProvider Provider = EFU_OnlineProvider::Steam;
	
	//搜索列表的Session
	UPROPERTY(BlueprintReadWrite, Category = "FUOnlineSession|Online Session|Property")
	FString SessionId;
	
	//房间名称
	UPROPERTY(BlueprintReadWrite, Category = "FUOnlineSession|Online Session|Property")
	FString RoomName;

	//最大玩家数量
	UPROPERTY(BlueprintReadWrite, Category = "FUOnlineSession|Online Session|Property")
	int32 MaxPlayers = 0;
	
	//当前玩家数量
	UPROPERTY(BlueprintReadWrite, Category = "FUOnlineSession|Online Session|Property")
	int32 CurrentPlayers = 0;
	
	//网络延迟ping
	UPROPERTY(BlueprintReadWrite, Category = "FUOnlineSession|Online Session|Property")
	int32 PingInMs = 0;
	
	//房间是否要求密码
	UPROPERTY(BlueprintReadOnly, Category = "FUOnlineSession|Online Session|Property")
	bool bPasswordProtected = false; 
};

//新版创建结果携带Provider，使Steam和LAN控件能够识别事件来源
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FFU_OnCreateSessionCompleteV2, EFU_OnlineProvider, Provider, bool,bWasSuccessful);
//搜索结果既携带Provider，每一条FFU_SessionResult也会记录Provider
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FFU_OnFindSessionCompleteV2, EFU_OnlineProvider, Provider, const TArray<FFU_SessionResult>&, Results, bool, bWasSuccessful);
//加入结果携带Provider，避免Steam控件处理NULL的加入结果
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FFU_OnJoinSessionCompleteV2, EFU_OnlineProvider, Provider, EFU_JoinSessionResult, Result);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FFU_OnDestroySessionComplete, EFU_OnlineProvider, Provider, bool, bWasSuccessful);

//【FU 修复：旅行阶段错误】JoinSession Success 之后的网络/地图错误通过独立事件返回蓝图。
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(
	FFU_OnOnlineConnectionFailure,
	EFU_OnlineProvider, Provider,
	EFU_OnlineConnectionFailureType, FailureType,
	const FString&, Message);
