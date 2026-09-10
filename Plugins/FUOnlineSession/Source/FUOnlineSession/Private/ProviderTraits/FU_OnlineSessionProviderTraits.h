#pragma once
#include "FU_OnlineSessionTypes.h"
#include "Misc/App.h"
#include "OnlineSubsystemNames.h"
#include "OnlineSessionSettings.h"
#include "Online/OnlineSessionNames.h"

namespace FUOnlineSessionProviderTraitsPrivate
{
	/**
	 * 【FU 修复：Steam Lobby 项目隔离】
	 *
	 * SteamDevAppId=480 是公开的 Spacewar 测试环境，许多互不相关的游戏会共享同一个
	 * Lobby 池。如果只设置 SEARCH_LOBBIES，Steam 可能先返回几十个其他项目的 Lobby，
	 * 而本项目房间在结果数量上限之外，最终表现为 FindSessions 成功但没有有效结果。
	 *
	 * 这里把 Unreal 项目名写入搜索关键字，使插件安装后无需用户手工维护 ini 或额外配置：
	 * 同一项目的两台电脑会生成相同值，不同项目则自然分开。末尾的 V1 是 Lobby 数据协议
	 * 版本；未来若房间元数据出现不兼容变更，可以提升版本，让新旧客户端不再互相发现。
	 *
	 * 返回 const FString& 是为了让创建和搜索始终读取同一份、只初始化一次的字符串，
	 * 既避免重复构造，也从结构上防止“创建写 A、搜索查 B”的维护错误。
	 */
	inline const FString& GetSteamLobbyProjectKeyword()
	{
		static const FString Keyword = FString::Printf(
			TEXT("FUOnlineSession_%s_V1"),
			FApp::GetProjectName());
		return Keyword;
	}
}

//模板参数Provider发生在编译器
template<EFU_OnlineProvider Provider>
struct TFU_OnlineSessionProviderTraits;

//steam特化
template<>
struct TFU_OnlineSessionProviderTraits<EFU_OnlineProvider::Steam>
{
	static FName GetSubsystemName()
	{
		return STEAM_SUBSYSTEM;
	}

	static constexpr bool IsLan()
	{
		return false;
	}

	static const TCHAR* GetDebugName()
	{
		return TEXT("Steam");
	}

	/**
	 * 【FU 修复：Steam 传输层】
	 * OnlineSubsystemSteam 负责 Lobby 的创建和发现；真正执行地图连接的是 NetDriver。
	 * Steam Lobby 返回 steam.<SteamId> 形式的 P2P 地址，所以不能交给普通 IpNetDriver。
	 */
	static FName GetNetDriverClassName()
	{
		return TEXT("/Script/SteamSockets.SteamSocketsNetDriver");
	}

	/** SteamSocketsNetDriver 创建连接对象时使用的配套 NetConnection 类型。 */
	static FName GetNetConnectionClassName()
	{
		return TEXT("/Script/SteamSockets.SteamSocketsNetConnection");
	}

	static void ConfigureCreateSettings(FOnlineSessionSettings& Settings)
	{
		Settings.bIsLANMatch = false;
		Settings.bShouldAdvertise = true;
		Settings.bAllowJoinInProgress = true;
		Settings.bUsesPresence = true;
		Settings.bAllowJoinViaPresence = true;
		Settings.bAllowInvites = true;
		Settings.bUseLobbiesIfAvailable = true;

		// 【FU 修复：先发布、后匹配】
		// ViaOnlineService 会把关键字作为 Steam Lobby 元数据发布到在线服务。
		// 搜索端只有在房主先发布了完全相同的值时，才能在 Steam 后端精确筛选到它。
		Settings.Set(
			SEARCH_KEYWORDS,
			FUOnlineSessionProviderTraitsPrivate::GetSteamLobbyProjectKeyword(),
			EOnlineDataAdvertisementType::ViaOnlineService);
	}

	static void ConfigureSearch(FOnlineSessionSearch& Search)
	{
		Search.bIsLanQuery = false;
		Search.QuerySettings.Set(SEARCH_LOBBIES,true,EOnlineComparisonOp::Equals);

		// 【FU 修复：在 Steam 后端过滤，而不是下载结果后再本地过滤】
		// 这条条件会随 FindSessions 请求发送给 Steam。Equals 要求 Lobby 中发布的关键字
		// 与当前项目关键字完全一致，因此共享 AppID 480 的其他项目不会占用返回结果名额。
		Search.QuerySettings.Set(
			SEARCH_KEYWORDS,
			FUOnlineSessionProviderTraitsPrivate::GetSteamLobbyProjectKeyword(),
			EOnlineComparisonOp::Equals);
	}
};

//NULL特化
template<>
struct TFU_OnlineSessionProviderTraits<EFU_OnlineProvider::Lan>
{
	static FName GetSubsystemName()
	{
		return NULL_SUBSYSTEM;
	}

	static constexpr bool IsLan()
	{
		return true;
	}

	static const TCHAR* GetDebugName()
	{
		return TEXT("NULL LAN");
	}

	/**
	 * 【FU 修复：LAN 传输层】
	 * NULL Session 返回普通 IPv4 地址，局域网 Listen Server 应继续使用 IpNetDriver。
	 */
	static FName GetNetDriverClassName()
	{
		return TEXT("/Script/OnlineSubsystemUtils.IpNetDriver");
	}

	/** IpNetDriver 创建连接对象时使用的标准 IpConnection 类型。 */
	static FName GetNetConnectionClassName()
	{
		return TEXT("/Script/OnlineSubsystemUtils.IpConnection");
	}

	static void ConfigureCreateSettings(FOnlineSessionSettings& Settings)
	{
		Settings.bIsLANMatch = true;
		Settings.bShouldAdvertise = true;
		Settings.bAllowJoinInProgress = true;
		Settings.bUsesPresence = false;
		Settings.bAllowJoinViaPresence = false;
		Settings.bAllowInvites = false;
		Settings.bUseLobbiesIfAvailable = false;
	}

	static void ConfigureSearch(FOnlineSessionSearch& Search)
	{
		Search.bIsLanQuery = true;
		//NULL不使用SEARCH_LOBBIES，不要设置该查询条件
	}
};
