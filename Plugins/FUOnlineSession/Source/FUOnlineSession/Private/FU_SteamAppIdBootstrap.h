#pragma once

#include "CoreMinimal.h"

/**
 * Steam 开发 AppID 动态层的可观察状态。
 *
 * 枚举值只追加，便于日志、自动化测试和后续诊断记录稳定地表达启动期失败原因。
 */
enum class EFU_SteamBootstrapStatus : uint8
{
	Ready,
	ShippingNoInjection,
	InvalidDevelopmentAppId,
	SteamAlreadyInstantiated,
	BranchUnavailable,
	LayerAddFailed,
	LayerOwnershipConflict,
	EffectiveValueConflict,
	LayerRemoveFailed
};

/** 纯判断所需的启动快照；测试不需要真实加载 Steam 或修改配置缓存。 */
struct FFU_SteamAppIdBootstrapInputs
{
	bool bShippingBuild = false;
	bool bSteamInstanceAlreadyExists = false;
	bool bEngineBranchAvailable = false;
	bool bLayerAdded = false;
	bool bOwnedTagResolvesToEngineBranch = false;
	int32 RequestedAppId = 0;
	int32 EffectiveAppId = 0;
};

/** Runtime 模块唯一拥有的动态层凭据；文件名是 Shutdown 时唯一允许移除的对象。 */
struct FFU_SteamAppIdBootstrapTicket
{
	EFU_SteamBootstrapStatus Status = EFU_SteamBootstrapStatus::BranchUnavailable;
	FString SyntheticLayerFilename;
	bool bOwnsAddedLayer = false;
};

/**
 * 仅在当前进程内注入开发 Steam AppID 的启动器。
 *
 * 它从不落盘、从不调用 GConfig 写入接口；生命周期由 Runtime 模块持有并在卸载时精确撤层。
 */
class FFU_SteamAppIdBootstrap final
{
public:
	/** 不访问引擎全局状态的纯分类函数，供自动化测试覆盖所有失败路径。 */
	static EFU_SteamBootstrapStatus Evaluate(const FFU_SteamAppIdBootstrapInputs& Inputs);

	/** 在 Runtime 模块 PreDefault 启动期执行一次；Shipping 只记录“不注入”的正常状态。 */
	void Initialize(int32 RequestedDevelopmentAppId);

	/** 仅移除本实例已经成功添加的进程内动态层，绝不改写任何 ini 文件。 */
	void Shutdown();

	const FFU_SteamAppIdBootstrapTicket& GetTicket() const { return Ticket; }

	/** ShippingNoInjection 是 Shipping 的正常状态，因此同样可通过 Bootstrap 前置检查。 */
	bool IsReadyForRuntime() const;

private:
	FFU_SteamAppIdBootstrapTicket Ticket;
};
