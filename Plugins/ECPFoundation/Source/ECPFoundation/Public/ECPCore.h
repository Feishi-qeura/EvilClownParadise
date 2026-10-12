#pragma once

#include "CoreMinimal.h"
#include "Logging/LogMacros.h"

/** 公共日志在常驻基础模块定义一次；具体功能不得反向链接宿主取得日志符号。 */
ECPFOUNDATION_API DECLARE_LOG_CATEGORY_EXTERN(LogECP, Log, All);

/** 角色通过只读队伍契约暴露阵营，AI 无需识别具体玩家实现类型。 */
namespace ECPTeam
{
	inline constexpr uint8 Player = 0;
	inline constexpr uint8 Monster = 1;
	inline constexpr uint8 Neutral = 255;
}
