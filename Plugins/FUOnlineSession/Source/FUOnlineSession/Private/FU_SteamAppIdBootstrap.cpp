#include "FU_SteamAppIdBootstrap.h"

#include "FUOnlineSessionModule.h"

#include "HAL/PlatformProcess.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Guid.h"
#include "OnlineSubsystem.h"
#include "OnlineSubsystemNames.h"

namespace FUOnlineSessionSteamBootstrap
{
	/** Engine 分支必须使用实际 GEngineIni 文件名定位，不能凭空创建一个同名配置分支。 */
	const FName EngineConfigBaseName(TEXT("Engine"));

	/** 固定标签仅用于确认 FU 的动态层所有权；Shutdown 仍按唯一文件名移除。 */
	const FName OwnershipTag(TEXT("FUOnlineSession.SteamAppIdBootstrap"));

	FConfigBranch* FindEngineBranch()
	{
		if (GConfig == nullptr || GEngineIni.IsEmpty())
		{
			return nullptr;
		}

		return GConfig->FindBranch(EngineConfigBaseName, GEngineIni);
	}

	/** 每个进程层使用独一无二的虚拟文件名，避免撤层时误伤任何插件或项目配置。 */
	FString MakeSyntheticLayerFilename()
	{
		return FString::Printf(
			TEXT("FUOnlineSession-SteamDevAppId-%u-%s.ini"),
			FPlatformProcess::GetCurrentProcessId(),
			*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	}

	/** AppID 是可公开的配置期望值；日志永不包含 Steam 身份、票据或连接地址。 */
	void LogBootstrapStatus(const EFU_SteamBootstrapStatus Status, const int32 RequestedAppId)
	{
		UE_LOG(
			LogFUOnlineSession,
			Log,
			TEXT("Steam AppID Bootstrap 状态=%d RequestedAppId=%d"),
			static_cast<int32>(Status),
			RequestedAppId);
	}
}

EFU_SteamBootstrapStatus FFU_SteamAppIdBootstrap::Evaluate(const FFU_SteamAppIdBootstrapInputs& Inputs)
{
	// 【Shipping 封闭】Shipping 永远不注入开发 AppID；这不是失败，而是后续正式 AppID 校验的前置状态。
	if (Inputs.bShippingBuild)
	{
		return EFU_SteamBootstrapStatus::ShippingNoInjection;
	}

	// 【输入安全】开发 AppID 不可用时不触碰配置缓存或 Steam，避免 0/负数配置造成不确定初始化。
	if (Inputs.RequestedAppId <= 0)
	{
		return EFU_SteamBootstrapStatus::InvalidDevelopmentAppId;
	}

	// 【时序安全】Steam 已实例化后再注入配置无效且可能造成同进程配置与 SDK 状态不一致。
	if (Inputs.bSteamInstanceAlreadyExists)
	{
		return EFU_SteamBootstrapStatus::SteamAlreadyInstantiated;
	}

	if (!Inputs.bEngineBranchAvailable)
	{
		return EFU_SteamBootstrapStatus::BranchUnavailable;
	}

	// 【返回值契约】AddDynamicLayerStringToHierarchy 返回 false 时，即使现有值恰好相同也不能声称本模块拥有该层。
	if (!Inputs.bLayerAdded)
	{
		return EFU_SteamBootstrapStatus::LayerAddFailed;
	}

	if (!Inputs.bOwnedTagResolvesToEngineBranch)
	{
		return EFU_SteamBootstrapStatus::LayerOwnershipConflict;
	}

	// 【有效值校验】动态层成功加入仍需从有效配置重新读取，防止优先级或外部层悄悄覆盖开发 AppID。
	if (Inputs.EffectiveAppId != Inputs.RequestedAppId)
	{
		return EFU_SteamBootstrapStatus::EffectiveValueConflict;
	}

	return EFU_SteamBootstrapStatus::Ready;
}

void FFU_SteamAppIdBootstrap::Initialize(const int32 RequestedDevelopmentAppId)
{
	using namespace FUOnlineSessionSteamBootstrap;

	// 【重复启动防护】模块热重载或异常重入不允许覆盖旧凭据；先交由 Shutdown 精确撤掉已拥有的层。
	if (Ticket.bOwnsAddedLayer)
	{
		Shutdown();
		if (Ticket.bOwnsAddedLayer)
		{
			// 【失败保持】撤层失败时 Ticket 仍是唯一恢复凭据；禁止清空它再创建新层，以免永久丢失精确文件名。
			return;
		}
	}
	Ticket = FFU_SteamAppIdBootstrapTicket();

	FFU_SteamAppIdBootstrapInputs Inputs;
	Inputs.RequestedAppId = RequestedDevelopmentAppId;

#if UE_BUILD_SHIPPING
	Inputs.bShippingBuild = true;
	Ticket.Status = Evaluate(Inputs);
	LogBootstrapStatus(Ticket.Status, RequestedDevelopmentAppId);
	return;
#else
	// 【不加载 Steam】只查询已有实例；DoesInstanceExist 不会因为 FU 启动而解析或创建 Steam 子系统。
	Inputs.bSteamInstanceAlreadyExists = IOnlineSubsystem::DoesInstanceExist(STEAM_SUBSYSTEM);
	if (Inputs.RequestedAppId <= 0 || Inputs.bSteamInstanceAlreadyExists)
	{
		Ticket.Status = Evaluate(Inputs);
		LogBootstrapStatus(Ticket.Status, RequestedDevelopmentAppId);
		return;
	}

	FConfigBranch* const EngineBranch = FindEngineBranch();
	Inputs.bEngineBranchAvailable = EngineBranch != nullptr;
	if (EngineBranch == nullptr)
	{
		Ticket.Status = Evaluate(Inputs);
		LogBootstrapStatus(Ticket.Status, RequestedDevelopmentAppId);
		return;
	}

	// 【所有权冲突】任何残留同标签层都不是本次启动创建的层；拒绝叠加，防止 Shutdown 删除错误对象。
	if (GConfig->FindBranchWithDynamicLayerTag(EngineConfigBaseName, OwnershipTag) != nullptr)
	{
		Ticket.Status = EFU_SteamBootstrapStatus::LayerOwnershipConflict;
		LogBootstrapStatus(Ticket.Status, RequestedDevelopmentAppId);
		return;
	}

	Ticket.SyntheticLayerFilename = MakeSyntheticLayerFilename();
	const FString DynamicLayerContents = FString::Printf(
		TEXT("[OnlineSubsystemSteam]\nSteamDevAppId=%d\n"),
		RequestedDevelopmentAppId);

	// 【进程内层】该 API 只修改当前 FConfigBranch 内存层级；严禁用 SetString、RuntimeChanges 或 Flush 落盘。
	Inputs.bLayerAdded = EngineBranch->AddDynamicLayerStringToHierarchy(
		Ticket.SyntheticLayerFilename,
		DynamicLayerContents,
		OwnershipTag,
		DynamicLayerPriority::ProjectPluginOverrides);
	Ticket.bOwnsAddedLayer = Inputs.bLayerAdded;
	if (!Inputs.bLayerAdded)
	{
		Ticket.Status = Evaluate(Inputs);
		LogBootstrapStatus(Ticket.Status, RequestedDevelopmentAppId);
		return;
	}

	// 【标签验证】标签必须重新解析回同一个真实 Engine 分支；否则仍会在 Shutdown 按唯一文件名撤掉本层。
	Inputs.bOwnedTagResolvesToEngineBranch =
		GConfig->FindBranchWithDynamicLayerTag(EngineConfigBaseName, OwnershipTag) == EngineBranch;

	int32 EffectiveAppId = 0;
	const bool bReadEffectiveAppId = GConfig->GetInt(
		TEXT("OnlineSubsystemSteam"),
		TEXT("SteamDevAppId"),
		EffectiveAppId,
		GEngineIni);
	Inputs.EffectiveAppId = bReadEffectiveAppId ? EffectiveAppId : 0;
	Ticket.Status = Evaluate(Inputs);
	LogBootstrapStatus(Ticket.Status, RequestedDevelopmentAppId);
#endif
}

void FFU_SteamAppIdBootstrap::Shutdown()
{
	using namespace FUOnlineSessionSteamBootstrap;

	if (!Ticket.bOwnsAddedLayer)
	{
		return;
	}

	FConfigBranch* const EngineBranch = FindEngineBranch();
	if (EngineBranch == nullptr
		|| Ticket.SyntheticLayerFilename.IsEmpty()
		|| !EngineBranch->RemoveDynamicLayerFromHierarchy(Ticket.SyntheticLayerFilename))
	{
		// 【撤层失败】绝不尝试写入/重建 ini 或移除标签；保留 Ticket 文件名供日志和后续人工调查。
		Ticket.Status = EFU_SteamBootstrapStatus::LayerRemoveFailed;
		UE_LOG(LogFUOnlineSession, Error, TEXT("Steam AppID 动态层撤除失败：%s"), *Ticket.SyntheticLayerFilename);
		return;
	}

	// 【精确清理】只在引擎确认按本实例虚拟文件名移除成功后，才放弃拥有标记。
	Ticket.bOwnsAddedLayer = false;
	UE_LOG(LogFUOnlineSession, Log, TEXT("Steam AppID 动态层已撤除：%s"), *Ticket.SyntheticLayerFilename);
}

bool FFU_SteamAppIdBootstrap::IsReadyForRuntime() const
{
	return Ticket.Status == EFU_SteamBootstrapStatus::Ready
		|| Ticket.Status == EFU_SteamBootstrapStatus::ShippingNoInjection;
}
