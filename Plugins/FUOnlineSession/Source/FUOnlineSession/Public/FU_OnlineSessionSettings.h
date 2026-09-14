#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "FU_OnlineSessionSettings.generated.h"

/**
 * FUOnlineSession 的插件专属配置
 *
 * Runtime 模块保存配置数据；
 * 配置只归属插件，绝不要求项目 DefaultEngine.ini 承担插件设置。
 */
UCLASS(Config = FUOnlineSession,DefaultConfig,meta = (DisplayName = "FU Online Session"))
class FUONLINESESSION_API UFU_OnlineSessionSettings : public UDeveloperSettings
{
    GENERATED_BODY()

public:
    // 将设置页面放到 Project Settings -> Plugins 分类。
    virtual FName GetCategoryName() const override;

    /**
     * 是否允许 Editor 一次性清理旧版本写入 DefaultEngine.ini 的 FU 成对标记块。
     *
     * 当前版本不再向项目文件生成配置；关闭此项后迁移器不会读取或改写项目文件，
     * Session 的插件自有 Config 层和运行时功能不受影响。
     */
	UPROPERTY(Config,EditAnywhere,Category = "FUOnlineSession|Online Session|Steam|Legacy Migration")
    bool bAutoConfigureProject = true;

    /**
     * Steam 开发 App ID。
     *
     * 480 是 Valve 提供的 Spacewar 测试 App ID；
     * 正式发布时必须换成游戏自己的 App ID。
     */
    UPROPERTY(Config,EditAnywhere,Category = "FUOnlineSession|Online Session|Steam",meta = (ClampMin = "1", UIMin = "1"))
    int32 SteamDevAppId = 480;

    /**
     * Shipping 环境预期的 Steam App ID；0 表示尚未配置，运行时应将其视为前置条件失败。
     */
    UPROPERTY(Config,EditAnywhere,Category = "FUOnlineSession|Online Session|Steam",meta = (ClampMin = "0", UIMin = "0"))
    int32 ExpectedShippingSteamAppId = 0;

    /** 单次 OnlineSubsystem 操作的等待上限，避免异步操作无限挂起。 */
    UPROPERTY(Config,EditAnywhere,Category = "FUOnlineSession|Runtime",meta = (ClampMin = "5.0", ClampMax = "300.0", UIMin = "5.0", UIMax = "300.0"))
    float OperationTimeoutSeconds = 30.0f;

    /** 保留的运行时诊断记录数量。 */
    UPROPERTY(Config,EditAnywhere,Category = "FUOnlineSession|Diagnostics",meta = (ClampMin = "1", ClampMax = "1000", UIMin = "1", UIMax = "1000"))
    int32 DiagnosticHistoryLimit = 200;

    /** 同时控制 UE_LOG 与 Saved/Logs/FUOnlineSession/OnlineSubsystemLog 下的 Provider 自动文件；不影响历史和 Blueprint。 */
    UPROPERTY(Config,EditAnywhere,Category = "FUOnlineSession|Diagnostics")
    bool bEnableDiagnosticLog = true;

    /** 是否显示运行时诊断浮层。 */
    UPROPERTY(Config,EditAnywhere,Category = "FUOnlineSession|Diagnostics")
    bool bEnableDiagnosticOverlay = true;

    /**
     * 配置继续使用已发布的稳定 byte 契约，避免公开枚举加入 Verbose 后让旧 ini 数值改义。
     * 约定 0=Info、1=Warning、2=Error，默认 Warning。
     */
    UPROPERTY(Config,EditAnywhere,Category = "FUOnlineSession|Diagnostics",meta = (ClampMin = "0", ClampMax = "2", UIMin = "0", UIMax = "2"))
    uint8 MinimumOverlaySeverity = 1;

    /** 单条诊断浮层的显示时长；默认三秒后自动消失，且浮层始终不参与鼠标命中。 */
    UPROPERTY(Config,EditAnywhere,Category = "FUOnlineSession|Diagnostics",meta = (ClampMin = "1.0", ClampMax = "60.0", UIMin = "1.0", UIMax = "60.0"))
    float OverlayDurationSeconds = 3.0f;

    /** 同时显示的诊断浮层行数。 */
    UPROPERTY(Config,EditAnywhere,Category = "FUOnlineSession|Diagnostics",meta = (ClampMin = "1", ClampMax = "20", UIMin = "1", UIMax = "20"))
    int32 OverlayRowLimit = 6;

    /**
     * 运行时读取入口：返回前会纠正损坏或手改 ini 中的危险值，保证调用方总能获得安全范围。
     */
    static const UFU_OnlineSessionSettings& GetRuntimeSettings();

    /**
     * 非 Blueprint 的运行时配置消毒：只修正边界值，不持久化修改，避免运行时重写用户 ini。
     */
    void SanitizeForRuntime();
};
