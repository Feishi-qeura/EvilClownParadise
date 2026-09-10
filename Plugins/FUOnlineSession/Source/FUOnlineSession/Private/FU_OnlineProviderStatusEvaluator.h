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

	//【FU 修复：NetDriver 状态层】OnlineSubsystem 可用并不代表地图连接驱动也可用
	bool bHasNetDriverDefinition = false;
	bool bHasNetDriverClass = false;
	bool bHasConflictingActiveNetDriver = false;
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

		// NULL/LAN 只依赖 Session Interface，不需要平台账号身份。
		if (Provider == EFU_OnlineProvider::Lan)
		{
			return EFU_OnlineProviderStatusCode::Ready;
		}

		if (!Inputs.bHasIdentityInterface)
		{
			return EFU_OnlineProviderStatusCode::IdentityInterfaceUnavailable;
		}

		if (!Inputs.bIsLoggedIn)
		{
			return EFU_OnlineProviderStatusCode::NotLoggedIn;
		}

		return EFU_OnlineProviderStatusCode::Ready;
	}
};
