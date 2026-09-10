#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"

class UFU_OnlineSessionSettings;
class UObject;
struct FPropertyChangedEvent;

/**
 * 只在 Unreal Editor中运行
 * 后续负责检查并初始化项目的OnlineSubsystem配置
 */
class FFUOnlineSessionEditorModule final : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;

private:
    /** 注册到这个插件自己的 DeveloperSettings，而不是监听编辑器中所有 UObject。 */
    void RegisterSettingsChangedHandler();

    /** 设置页面中的值改变后保存设置，并重新生成插件管理的 Engine 配置。 */
    void HandleSettingsChanged(UObject* SettingsObject, FPropertyChangedEvent& PropertyChangedEvent);

    /** 统一处理启动检查和设置变更检查，避免两处日志/错误分支逐渐不一致。 */
    void ApplyProjectConfiguration(bool bShowEditorNotification);

    // 弱引用不会延长设置 CDO 的生命周期；模块卸载时只在对象仍有效时解除委托。
    TWeakObjectPtr<UFU_OnlineSessionSettings> RegisteredSettings;

    // 保存 Handle 才能在 ShutdownModule 中精确移除本模块注册的回调。
    FDelegateHandle SettingsChangedHandle;
};
