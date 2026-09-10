#include "FU_OnlineSessionSettings.h"

FName UFU_OnlineSessionSettings::GetCategoryName() const
{
    // UDeveloperSettings 会自动注册到 Project Settings。
    return TEXT("Plugins");
}

const UFU_OnlineSessionSettings& UFU_OnlineSessionSettings::GetRuntimeSettings()
{
    UFU_OnlineSessionSettings* Settings = GetMutableDefault<UFU_OnlineSessionSettings>();

    // 【运行时不变量】所有 Runtime 调用都经此入口取得设置，ini 被手动改坏时也不能把无效时限传入异步流程。
    Settings->SanitizeForRuntime();
    return *Settings;
}

void UFU_OnlineSessionSettings::SanitizeForRuntime()
{
    // 【失败保护】负数 Shipping AppID 表示无效配置，归零后由后续 Shipping 前置检查明确报错。
    ExpectedShippingSteamAppId = FMath::Max(ExpectedShippingSteamAppId, 0);

    // 【运行时边界】限制等待、历史和浮层容量，防止损坏 ini 造成永久等待或无界诊断内存占用。
    OperationTimeoutSeconds = FMath::Clamp(OperationTimeoutSeconds, 5.0f, 300.0f);
    DiagnosticHistoryLimit = FMath::Clamp(DiagnosticHistoryLimit, 1, 1000);
    // 【配置兼容】0=Info、1=Warning、2=Error 是 Task 1 已发布的 ini 契约；Task 4 的公开枚举
    // 另含 Verbose，因此不能直接 static_cast，必须先把此 byte 收束到既有三个有效值。
    MinimumOverlaySeverity = FMath::Clamp<uint8>(MinimumOverlaySeverity, 0, 2);
    OverlayDurationSeconds = FMath::Clamp(OverlayDurationSeconds, 1.0f, 60.0f);
    OverlayRowLimit = FMath::Clamp(OverlayRowLimit, 1, 20);
}
