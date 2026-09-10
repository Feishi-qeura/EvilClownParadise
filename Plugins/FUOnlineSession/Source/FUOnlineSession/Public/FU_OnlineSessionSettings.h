#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "FU_OnlineSessionSettings.generated.h"

/**
 * FUOnlineSession的项目级配置
 *
 * Runtime模块保存配置数据；
 * Editor模块负责把这些设置转换为OnlineSubsystem的Engine配置
 */
UCLASS(Config = Engine,DefaultConfig,meta = (DisplayName = "FU Online Session"))
class FUONLINESESSION_API UFU_OnlineSessionSettings : public UDeveloperSettings
{
    GENERATED_BODY()

public:
    // 将设置页面放到 Project Settings -> Plugins 分类。
    virtual FName GetCategoryName() const override;

    /**
     * 启用后，Editor模块会检查并自动维护OnlineSubsystem 配置。
     *
     * 关闭它不会禁用 Session 功能，
     * 只是停止插件自动修改 DefaultEngine.ini。
     */
    UPROPERTY(Config,EditAnywhere,Category = "FUOnlineSession|Online Session|Steam|Automatic Configuration")
    bool bAutoConfigureProject = true;

    /**
     * Steam 开发 App ID。
     *
     * 480 是 Valve 提供的 Spacewar 测试 App ID；
     * 正式发布时必须换成游戏自己的 App ID。
     */
    UPROPERTY(Config,EditAnywhere,Category = "FUOnlineSession|Online Session|Steam",meta = (ClampMin = "1", UIMin = "1"))
    int32 SteamDevAppId = 480;

    // 【FU 修复：移除旧式全局驱动开关】
    // Steam 与 LAN 已由独立蓝图入口和 ProviderTraits 选择传输层：
    // Steam 始终使用 SteamSockets，LAN 始终使用 IpNetDriver，不再让用户配置互相矛盾的组合。
};
