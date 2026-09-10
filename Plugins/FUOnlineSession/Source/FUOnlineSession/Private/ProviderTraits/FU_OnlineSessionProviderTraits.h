#pragma once
#include "FU_OnlineSessionTypes.h"
#include "OnlineSubsystemNames.h"
#include "OnlineSessionSettings.h"
#include "Online/OnlineSessionNames.h"

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
	}

	static void ConfigureSearch(FOnlineSessionSearch& Search)
	{
		Search.bIsLanQuery = false;
		Search.QuerySettings.Set(SEARCH_LOBBIES,true,EOnlineComparisonOp::Equals);
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
