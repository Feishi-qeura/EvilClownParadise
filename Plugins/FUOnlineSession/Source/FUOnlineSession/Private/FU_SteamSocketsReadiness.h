#pragma once

#include "CoreMinimal.h"

/** SteamSockets 三层可用性快照；不会把 Steam 失败外溢到 NULL/LAN Provider。 */
struct FFU_SteamSocketsReadiness
{
	bool bModuleAvailable = false;
	bool bModuleEnabled = false;
	bool bSocketSubsystemAvailable = false;
};

/** 仅供 Steam Provider 状态检查调用的非致命探针。 */
class FFU_SteamSocketsReadinessProbe final
{
public:
	static FFU_SteamSocketsReadiness Probe();
};
