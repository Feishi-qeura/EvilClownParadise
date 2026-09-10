#pragma once

#include "CoreMinimal.h"

class UFU_OnlineSessionSettings;

/** 一次性遗留配置迁移的对外兼容结果。 */
enum class EFU_OnlineConfigResult : uint8
{
    // 项目中不存在历史 FU 受管区块。
    Unchanged,

    // 历史区块已从 DefaultEngine.ini 原子迁移，需要重启编辑器重新读取配置层。
    Updated,

    // 标记损坏、读取失败或无法安全发布；详情由 Editor 日志给出。
    Failed
};

/**
 * 只在 Editor 模块使用的工程配置管理器。
 *
 * 它不保存 Session 运行时状态，
 * 只负责运行一次历史 DefaultEngine.ini 迁移，绝不生成当前配置。
 */
class FFUOnlineSessionConfigManager final
{
public:
    static EFU_OnlineConfigResult EnsureProjectConfiguration();
};
