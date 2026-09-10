#include "FUOnlineSessionModule.h"

#include "FU_SteamAppIdBootstrap.h"
#include "FU_OnlineSessionSettings.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY(LogFUOnlineSession);

void FFU_SteamAppIdBootstrapDeleter::operator()(FFU_SteamAppIdBootstrap* Bootstrap) const
{
	// 【不完整类型隔离】只有包含 Private Bootstrap 定义的本 cpp 可以释放它，避免 Public 头依赖 PCH。
	delete Bootstrap;
}

FFUOnlineSessionModule::~FFUOnlineSessionModule() = default;

void FFUOnlineSessionModule::StartupModule()
{
	// 【PreDefault 时序】在 FU 任意会话代码读取 STEAM 之前注入仅进程有效的开发层。
	const UFU_OnlineSessionSettings& Settings = UFU_OnlineSessionSettings::GetRuntimeSettings();
	// 【专属所有权】自定义删除器让 Private 实现保持私有；构造后立刻由 TUniquePtr 接管，异常路径不会泄漏。
	SteamAppIdBootstrap = TUniquePtr<FFU_SteamAppIdBootstrap, FFU_SteamAppIdBootstrapDeleter>(new FFU_SteamAppIdBootstrap());
	SteamAppIdBootstrap->Initialize(Settings.SteamDevAppId);
}

void FFUOnlineSessionModule::ShutdownModule()
{
	// 【禁用/卸载恢复】只撤销本模块添加的虚拟层，项目和插件磁盘配置均保持原状。
	if (SteamAppIdBootstrap.IsValid())
	{
		SteamAppIdBootstrap->Shutdown();
		SteamAppIdBootstrap.Reset();
	}
}

bool FFUOnlineSessionModule::IsSteamAppIdBootstrapReady() const
{
	return SteamAppIdBootstrap.IsValid() && SteamAppIdBootstrap->IsReadyForRuntime();
}

IMPLEMENT_MODULE(FFUOnlineSessionModule, FUOnlineSession)
