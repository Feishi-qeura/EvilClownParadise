#include "FU_SteamSocketsReadiness.h"

#include "Modules/ModuleManager.h"
#include "SocketSubsystem.h"
#include "SteamSocketsModule.h"
#include "SteamSocketsTypes.h"

FFU_SteamSocketsReadiness FFU_SteamSocketsReadinessProbe::Probe()
{
	FFU_SteamSocketsReadiness Result;

	// 【非致命加载】LoadModulePtr 允许缺失插件/二进制成为可报告状态；禁止使用会断言的 FSteamSocketsModule::Get()。
	FSteamSocketsModule* Module = FModuleManager::LoadModulePtr<FSteamSocketsModule>(TEXT("SteamSockets"));
	Result.bModuleAvailable = Module != nullptr;
	Result.bModuleEnabled = Module != nullptr && Module->IsSteamSocketsEnabled();
	Result.bSocketSubsystemAvailable =
		Result.bModuleEnabled && ISocketSubsystem::Get(STEAM_SOCKETS_SUBSYSTEM) != nullptr;

	return Result;
}
