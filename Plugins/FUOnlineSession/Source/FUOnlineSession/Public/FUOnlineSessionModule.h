#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"
#include "Templates/UniquePtr.h"

class FFU_SteamAppIdBootstrap;

/**
 * Public 模块头只能前置声明 Private Bootstrap；自定义删除器把真正的 delete 留在 Runtime cpp，
 * 从而保持严格编译下的完整类型要求，同时不把 Private 实现暴露给插件使用者。
 */
struct FFU_SteamAppIdBootstrapDeleter
{
	void operator()(FFU_SteamAppIdBootstrap* Bootstrap) const;
};

/** 所有 Runtime/Blueprint 联机诊断共享的日志类别；只在 Runtime 模块 cpp 中定义一次。 */
DECLARE_LOG_CATEGORY_EXTERN(LogFUOnlineSession, Log, All);

/** Runtime module that exposes provider-agnostic OnlineSubsystem session features. */
class FUONLINESESSION_API FFUOnlineSessionModule final : public IModuleInterface
{
public:
	virtual ~FFUOnlineSessionModule() override;
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	/**
	 * 状态检查只读取 Runtime 模块持有的 Bootstrap ticket。
	 * Shipping 的“不注入开发 AppID”属于正常前置状态，返回 true 后仍须校验正式 AppID。
	 */
	bool IsSteamAppIdBootstrapReady() const;

private:
	// 前置声明加自定义删除器让 Public 头无需暴露 Private Bootstrap 实现，同时模块仍独占其生命周期。
	TUniquePtr<FFU_SteamAppIdBootstrap, FFU_SteamAppIdBootstrapDeleter> SteamAppIdBootstrap;
};
