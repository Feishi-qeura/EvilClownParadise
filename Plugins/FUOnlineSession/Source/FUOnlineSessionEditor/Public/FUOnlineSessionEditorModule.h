#pragma once

#include "CoreMinimal.h"
#include "Delegates/Delegate.h"
#include "Modules/ModuleInterface.h"
#include "UObject/WeakObjectPtr.h"

class UFU_OnlineSessionSettings;
class UObject;
struct FPropertyChangedEvent;

/**
 * 只在 Unreal Editor中运行
 * 启动时只迁移一次历史项目配置；当前配置由插件配置层拥有
 */
class FFUOnlineSessionEditorModule final : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;

private:
    /** 注册到这个插件自己的 DeveloperSettings，而不是监听编辑器中所有 UObject。 */
    void RegisterSettingsChangedHandler();

    /** 设置页面中的值改变后只保存插件设置并提示重启，绝不重新写入项目 Engine 配置。 */
    void HandleSettingsChanged(UObject* SettingsObject, FPropertyChangedEvent& PropertyChangedEvent);

    // 弱引用不会延长设置 CDO 的生命周期；模块卸载时只在对象仍有效时解除委托。
    TWeakObjectPtr<UFU_OnlineSessionSettings> RegisteredSettings;

    // 保存 Handle 才能在 ShutdownModule 中精确移除本模块注册的回调。
    FDelegateHandle SettingsChangedHandle;
};
