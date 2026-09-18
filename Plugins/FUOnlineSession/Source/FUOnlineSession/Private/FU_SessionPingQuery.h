#pragma once

#include "FU_OnlineSessionTypes.h"
#include "OnlineSessionSettings.h"

namespace FUOnlineSession
{
	// 纯查询共用原生搜索缓存，不加入房间、不初始化 OSS，也不引入额外定时器或探测包。
	inline int32 GetCachedSessionPing(
		const TArray<FOnlineSessionSearchResult>& SteamResults,
		const TArray<FOnlineSessionSearchResult>& LanResults,
		const EFU_OnlineProvider Provider,
		const FString& SessionId,
		bool& bIsEstimated)
	{
		// 用户约定以 999ms 展示不可用结果；先清空输出，防止上一房间的估算标记残留。
		constexpr int32 UnavailablePing = 999;
		bIsEstimated = false;
		if (SessionId.IsEmpty())
		{
			return UnavailablePing;
		}

		// 两种 Provider 的 ID 空间和缓存相互独立；非法枚举不能默认落入另一种 Provider。
		const TArray<FOnlineSessionSearchResult>* Results = nullptr;
		switch (Provider)
		{
		case EFU_OnlineProvider::Steam:
			Results = &SteamResults;
			break;
		case EFU_OnlineProvider::Lan:
			Results = &LanResults;
			break;
		default:
			return UnavailablePing;
		}

		for (const FOnlineSessionSearchResult& Result : *Results)
		{
			// 缓存可能已被下一次搜索替换；只有有效原生身份且 ID 完全一致时才读取对应 Ping。
			if (!Result.IsValid() || Result.GetSessionIdStr() != SessionId)
			{
				continue;
			}
			// UE 用 MAX_QUERY_PING 表示不可达/无有效结果；真实的 0ms 或大于 999ms 的有效值原样保留。
			if (Result.PingInMs < 0 || Result.PingInMs >= MAX_QUERY_PING)
			{
				return UnavailablePing;
			}
			bIsEstimated = Provider == EFU_OnlineProvider::Steam;
			return Result.PingInMs;
		}

		return UnavailablePing;
	}
}
