#pragma once

#include "CoreMinimal.h"
#include "FU_OnlineSessionTypes.h"

/**
 * 状态评估器的输入快照。
 *
 * 把“从引擎读取接口”与“根据接口状态作判断”分开后，判断规则可以被自动化测试覆盖，
 * 而测试不需要真的启动 Steam 客户端、登录账号或者扫描局域网。
 */
struct FFU_OnlineProviderStatusInputs
{
	bool bHasWorld = false;
	bool bHasSubsystem = false;
	bool bHasSessionInterface = false;
	bool bHasIdentityInterface = false;
	bool bIsLoggedIn = false;
	// Find 只读取 OnlineSubsystem 搜索服务，不会创建 NetDriver；Create/Join 才把该值保持为 true。
	bool bRequiresNetDriver = true;

	//【FU 修复：NetDriver 状态层】OnlineSubsystem 可用并不代表地图连接驱动也可用
	bool bHasNetDriverDefinition = false;
	bool bHasNetDriverClass = false;
	bool bHasConflictingActiveNetDriver = false;
	// 纯评估器默认保持旧测试基线；真实状态采样会明确写入进程级租约探针结果。
	bool bNetDriverLeaseAvailable = true;

	// Steam 专属环境在调用者处一次性采样；Evaluator 保持纯函数，LAN 不读取这些字段。
	bool bSteamAppIdBootstrapReady = false;
	bool bSteamSocketsModuleAvailable = false;
	bool bSteamSocketsEnabled = false;
	bool bSteamSocketsSocketSubsystemAvailable = false;
	bool bShippingBuild = false;
	bool bShippingSteamAppIdExpected = false;
	bool bSteamAppIdMatchesExpectation = false;
};

/** 只负责状态分类，不保存数据，也不会调用任何异步 OnlineSubsystem 操作。 */
class FFU_OnlineProviderStatusEvaluator final
{
public:
	static EFU_OnlineProviderStatusCode Evaluate(
		EFU_OnlineProvider Provider,
		const FFU_OnlineProviderStatusInputs& Inputs)
	{
		// 按依赖关系从外到内判断，保证错误信息指向最早失效的一层。
		if (!Inputs.bHasWorld)
		{
			return EFU_OnlineProviderStatusCode::WorldUnavailable;
		}

		if (!Inputs.bHasSubsystem)
		{
			return EFU_OnlineProviderStatusCode::SubsystemUnavailable;
		}

		if (!Inputs.bHasSessionInterface)
		{
			return EFU_OnlineProviderStatusCode::SessionInterfaceUnavailable;
		}

		if (Inputs.bRequiresNetDriver)
		{
			// Session Interface 只负责会话操作；没有 GameNetDriver 时无法 Listen 或 ClientTravel。
			if (!Inputs.bHasNetDriverDefinition)
			{
				return EFU_OnlineProviderStatusCode::NetDriverDefinitionUnavailable;
			}

			// 目标驱动类不可用时必须提前失败，不能让引擎静默回退后再解析错误地址。
			if (!Inputs.bHasNetDriverClass)
			{
				return EFU_OnlineProviderStatusCode::NetDriverClassUnavailable;
			}

			// NetDriver 属于当前网络世界；正在使用另一种驱动时不能热切换 Provider。
			if (Inputs.bHasConflictingActiveNetDriver)
			{
				return EFU_OnlineProviderStatusCode::ActiveNetDriverConflict;
			}

			// 当前 World 没有可复用的目标驱动时，必须先证明全进程租约可取得；
			// 这会捕获 PIE 另一实例占用、重复定义、PendingNetGame 与第三方外部修改。
			if (!Inputs.bNetDriverLeaseAvailable)
			{
				return EFU_OnlineProviderStatusCode::NetDriverLeaseUnavailable;
			}
		}

		// 【故障顺序】通用 World/OSS/Session/NetDriver 前置先报告，随后 LAN 立即 Ready；
		// Steam 专属探针绝不影响 NULL/LAN，Steam 则按 Bootstrap->Sockets->Identity->AppID 顺序报告最早故障。
		// NULL/LAN 只依赖 Session Interface，不需要平台账号身份。
		if (Provider == EFU_OnlineProvider::Lan)
		{
			return EFU_OnlineProviderStatusCode::Ready;
		}

		if (!Inputs.bSteamAppIdBootstrapReady)
		{
			return EFU_OnlineProviderStatusCode::SteamAppIdBootstrapInvalid;
		}

		// Steam Lobby 搜索依赖 OSS/Identity/AppID，但不会创建 SteamSocketsNetDriver；
		// 只有 Create/Join 这类会进入网络旅行的能力才要求传输模块与 SocketSubsystem。
		if (Inputs.bRequiresNetDriver)
		{
			if (!Inputs.bSteamSocketsModuleAvailable)
			{
				return EFU_OnlineProviderStatusCode::SteamSocketsModuleUnavailable;
			}

			if (!Inputs.bSteamSocketsEnabled)
			{
				return EFU_OnlineProviderStatusCode::SteamSocketsDisabled;
			}

			if (!Inputs.bSteamSocketsSocketSubsystemAvailable)
			{
				return EFU_OnlineProviderStatusCode::SteamSocketsSocketSubsystemUnavailable;
			}
		}

		if (!Inputs.bHasIdentityInterface)
		{
			return EFU_OnlineProviderStatusCode::IdentityInterfaceUnavailable;
		}

		if (!Inputs.bIsLoggedIn)
		{
			return EFU_OnlineProviderStatusCode::NotLoggedIn;
		}

		if (Inputs.bShippingBuild && !Inputs.bShippingSteamAppIdExpected)
		{
			return EFU_OnlineProviderStatusCode::ShippingSteamAppIdMissing;
		}

		if (!Inputs.bSteamAppIdMatchesExpectation)
		{
			return EFU_OnlineProviderStatusCode::SteamAppIdMismatch;
		}

		return EFU_OnlineProviderStatusCode::Ready;
	}
};
