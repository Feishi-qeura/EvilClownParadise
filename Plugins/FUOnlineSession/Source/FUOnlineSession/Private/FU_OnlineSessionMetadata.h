#pragma once

#include "CoreMinimal.h"

/**
 * FUOnlineSession 写入 Provider 会话的稳定元数据键。
 *
 * 创建、Steam 后端查询、搜索结果本地校验与自动化测试都必须引用同一份常量；如果把字符串
 * 分散在多个 .cpp 中，任何一次拼写漂移都会表现成“FindSessions 成功但找不到房间”。
 * inline 变量保证 Runtime 模块内只有一个语义定义，同时不把插件内部协议暴露给 Blueprint API。
 */
namespace FUOnlineSession
{
	inline const FName RoomNameSetting(TEXT("FU_RoomName"));
	inline const FName RoomPasswordSetting(TEXT("FU_RoomPassword"));
	// 与字符串 SEARCH_KEYWORDS 使用不同的键和值类型，专供 Steam 零结果时的第二查询路径。
	inline const FName ProjectProtocolHashSetting(TEXT("FU_ProjectProtocolHash"));
}
