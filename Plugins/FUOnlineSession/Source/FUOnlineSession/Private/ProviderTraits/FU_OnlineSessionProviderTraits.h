#pragma once
#include "FU_OnlineSessionTypes.h"
#include "FU_OnlineSessionMetadata.h"
#include "Misc/App.h"
#include "Misc/Crc.h"
#include "OnlineSubsystemNames.h"
#include "OnlineSessionSettings.h"
#include "Online/OnlineSessionNames.h"

namespace FUOnlineSessionProviderTraitsPrivate
{
	/**
	 * FindSessions 返回后的唯一后续决策。将三种结果显式分开，避免“同步回调已
	 * 换成 fallback/已完成”被外层误解为 Provider 同步拒绝。
	 */
	enum class EFU_SearchPostSubmitDisposition : uint8
	{
		AwaitCallback,
		SynchronousReject,
		CallbackAlreadyAdvancedState
	};

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

	/**
	 * Steam 降级查询使用的稳定数值协议标识。
	 *
	 * CRC 输入就是本插件已版本化的完整项目关键字，因此同一打包版本的主客机必然相同；
	 * int32 是 Steam Lobby 数值过滤原生支持的类型。它只负责后端限流，最终仍用完整字符串
	 * 精确校验，所以即使理论上发生 CRC 碰撞，也不会把其他项目结果暴露给 Blueprint。
	 */
	inline int32 GetSteamLobbyProjectProtocolHash()
	{
		static const int32 ProtocolHash = static_cast<int32>(
			FCrc::StrCrc32(*GetSteamLobbyProjectKeyword()));
		return ProtocolHash;
	}

	/**
	 * OnlineSubsystem 的 Find 完成委托是接口级全局广播，委托本身不携带触发它的 Search 指针。
	 * 因此仅比较 generation 不足以拦住取消后迟到的旧 Steam 广播：它会调用当时新注册的委托。
	 * 只有当前指针仍是本次提交对象，且该对象已进入终态，才允许消费完成回调。
	 */
	inline bool IsExpectedSearchCompletion(
		const TSharedPtr<FOnlineSessionSearch>& ExpectedSearch,
		const TSharedPtr<FOnlineSessionSearch>& CurrentSearch)
	{
		if (!ExpectedSearch.IsValid() || ExpectedSearch != CurrentSearch)
		{
			return false;
		}

		return ExpectedSearch->SearchState == EOnlineAsyncTaskState::Done
			|| ExpectedSearch->SearchState == EOnlineAsyncTaskState::Failed;
	}

	/**
	 * Steam 在已有搜索占用接口时会忽略新 Search，却仍返回 true/ONLINE_IO_PENDING。
	 * SearchState 只有真正被接口接纳时才会变为 InProgress，所以提交结果必须同时验证两项。
	 */
	inline bool DidSearchRequestEnterProgress(
		const bool bProviderReturnedStarted,
		const TSharedPtr<FOnlineSessionSearch>& Search)
	{
		return bProviderReturnedStarted
			&& Search.IsValid()
			&& Search->SearchState == EOnlineAsyncTaskState::InProgress;
	}

	/**
	 * 【同步 Provider/重入测试缝】FindSessions 可在调用栈内完成并进入回调，所以仅依靠
	 * 返回值或 SubmittedSearch->SearchState 无法判断谁拥有生命周期。必须先验证
	 * generation/kind、当前 pass 与 Search 指针都没有被同步回调推进，才能把未进入
	 * InProgress 归类为真正的同步拒绝。这个纯函数同时被 primary/fallback 调用和测试。
	 */
	inline EFU_SearchPostSubmitDisposition ClassifySearchPostSubmit(
		const bool bGenerationKindStillExpected,
		const bool bPassStillExpected,
		const bool bProviderReturnedStarted,
		const TSharedPtr<FOnlineSessionSearch>& SubmittedSearch,
		const TSharedPtr<FOnlineSessionSearch>& CurrentSearch)
	{
		if (!bGenerationKindStillExpected
			|| !bPassStillExpected
			|| !SubmittedSearch.IsValid()
			|| SubmittedSearch != CurrentSearch)
		{
			return EFU_SearchPostSubmitDisposition::CallbackAlreadyAdvancedState;
		}

		return DidSearchRequestEnterProgress(bProviderReturnedStarted, SubmittedSearch)
			? EFU_SearchPostSubmitDisposition::AwaitCallback
			: EFU_SearchPostSubmitDisposition::SynchronousReject;
	}

	/**
	 * 两种 Provider 共用的房间元数据协议。房间名始终是插件结果的身份字段；空密码则不发布，
	 * 因为 Steam 会把空字符串丢弃并输出误导性的 Empty session setting 警告。搜索/加入路径
	 * 已把缺失密码定义为公开房间，所以省略空值不会改变 Blueprint 行为。
	 */
	inline void ConfigureRoomMetadata(
		FOnlineSessionSettings& Settings,
		const FString& RoomName,
		const FString& RoomPassword)
	{
		Settings.Set(
			FUOnlineSession::RoomNameSetting,
			RoomName,
			EOnlineDataAdvertisementType::ViaOnlineServiceAndPing);
		if (!RoomPassword.IsEmpty())
		{
			Settings.Set(
				FUOnlineSession::RoomPasswordSetting,
				RoomPassword,
				EOnlineDataAdvertisementType::ViaOnlineService);
		}
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

		// 【备用发现协议】第二遍故意换用自有数值键，避免重复提交已经返回零结果的
		// SEARCH_KEYWORDS 字符串条件；ViaOnlineService 会把它发布为 Steam Lobby 元数据。
		Settings.Set(
			FUOnlineSession::ProjectProtocolHashSetting,
			FUOnlineSessionProviderTraitsPrivate::GetSteamLobbyProjectProtocolHash(),
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

	/**
	 * 【Steam 真实双机修复：第二查询路径】
	 *
	 * UE/Steam 会把成功且零结果视为正常完成，因此第一遍项目关键字条件若在后端没有命中，
	 * 只看 bWasSuccessful 无法恢复。第二遍明确不再携带 SEARCH_KEYWORDS：指定房间名时只用
	 * 已发布的 FU_RoomName 建立真正独立的后端路径；空房间名浏览则用自有 int32 协议哈希
	 * 限流，避免 AppID 480 的无关 Lobby 占满返回上限。结果仍须经过 IsCurrentProjectSession。
	 */
	static void ConfigureFallbackSearch(FOnlineSessionSearch& Search, const FString& RoomName)
	{
		Search.bIsLanQuery = false;
		Search.QuerySettings.Set(SEARCH_LOBBIES, true, EOnlineComparisonOp::Equals);
		Search.QuerySettings.SearchParams.Remove(SEARCH_KEYWORDS);

		const FString TrimmedRoomName = RoomName.TrimStartAndEnd();
		if (!TrimmedRoomName.IsEmpty())
		{
			// 有名搜索故意只用旧主机已经发布的房间键；即使用户尚未更新主机包也能复测此路径。
			Search.QuerySettings.Set(
				FUOnlineSession::RoomNameSetting,
				TrimmedRoomName,
				EOnlineComparisonOp::Equals);
		}
		else
		{
			// 无名浏览没有可用的房间键，只能用新版主机发布的数值协议保护共享 AppID 结果上限。
			Search.QuerySettings.Set(
				FUOnlineSession::ProjectProtocolHashSetting,
				FUOnlineSessionProviderTraitsPrivate::GetSteamLobbyProjectProtocolHash(),
				EOnlineComparisonOp::Equals);
		}
	}

	/**
	 * 第二遍搜索可能接触共享 SteamDevAppId=480 的其他 Lobby；只有携带当前项目协议关键字的
	 * Session 才能进入原生结果缓存和 Blueprint。这里故意使用完全一致比较，避免 V1/V2
	 * 元数据协议混用后在 Join 阶段才失败。
	 */
	static bool IsCurrentProjectSession(const FOnlineSessionSettings& Settings)
	{
		FString AdvertisedKeyword;
		return Settings.Get(SEARCH_KEYWORDS, AdvertisedKeyword)
			&& AdvertisedKeyword == FUOnlineSessionProviderTraitsPrivate::GetSteamLobbyProjectKeyword();
	}

	/** 只允许成功零结果触发一次降级；Provider 故障和已有结果都必须原样结束。 */
	static bool ShouldStartFallbackSearch(
		const bool bWasSuccessful,
		const int32 RawResultCount,
		const bool bFallbackAlreadyStarted)
	{
		return bWasSuccessful && RawResultCount == 0 && !bFallbackAlreadyStarted;
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
