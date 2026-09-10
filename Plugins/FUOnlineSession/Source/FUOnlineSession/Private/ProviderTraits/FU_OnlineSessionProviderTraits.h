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