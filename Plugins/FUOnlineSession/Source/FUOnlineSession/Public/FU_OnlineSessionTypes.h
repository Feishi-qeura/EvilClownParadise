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
	InvalidPassword

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
	NotLoggedIn UMETA(DisplayName = "Not Logged In")
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

	//面向玩家或开发者的可读说明；正式项目可以根据StatusCode换成本地化文本
	UPROPERTY(BlueprintReadOnly, Category="FUOnlineSession|Online Session|Provider Status")
	FString Message;
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
