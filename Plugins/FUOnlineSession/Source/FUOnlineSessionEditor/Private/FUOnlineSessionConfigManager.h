#pragma once

#include "CoreMinimal.h"

class UFU_OnlineSessionSettings;

/**
 * 自动配置操作的结果。
 * Editor 模块根据结果决定是否提示用户重启。
 */
enum class EFU_OnlineConfigResult : uint8
{
    // 配置已经正确，不需要写文件。
    Unchanged,

    // DefaultEngine.ini 已经更新，需要重启编辑器。
    Updated,

    // 用户关闭了自动配置。
    Disabled,

    // 管理区块之外存在插件无法安全接管的自定义 GameNetDriver。
    Conflict,

    // 配置文件读取、标记检查或者保存失败。
    Failed
};

/** 自动管理区块之外的 GameNetDriver 分析结果。 */
enum class EFU_ExternalGameNetDriverState : uint8
{
    // 没有找到外部 GameNetDriver 定义。
    None,

    // 找到 SteamNetDriver 或 IpNetDriver 的旧定义；语义兼容，可以继续配置。
    Compatible,

    // 找到项目自定义驱动；插件无法证明覆盖它是安全的。
    Conflict
};

/**
 * 只在 Editor 模块使用的工程配置管理器。
 *
 * 它不保存 Session 运行时状态，
 * 只负责把插件设置转换成 DefaultEngine.ini 配置。
 */
class FFUOnlineSessionConfigManager final
{
public:
    static EFU_OnlineConfigResult EnsureProjectConfiguration();

    /**
     * 分析不属于插件管理区块的 GameNetDriver。
     * 函数不读写磁盘，放在 Editor 私有模块中公开是为了让自动化测试直接验证规则。
     */
    static EFU_ExternalGameNetDriverState AnalyzeExternalGameNetDriver(
        const FString& ExternalConfigContent,
        bool bUseSteamNetworking
    );

private:
    static FString BuildManagedConfigBlock(
        const UFU_OnlineSessionSettings& Settings
    );

    static EFU_OnlineConfigResult WriteManagedConfigBlock(
        const FString& ManagedBlock
    );
};
