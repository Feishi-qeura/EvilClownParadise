#include "FUOnlineSessionEditorModule.h"

#include "FUOnlineSessionConfigManager.h"
#include "FU_OnlineSessionSettings.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "UObject/UnrealType.h"
#include "Widgets/Notifications/SNotificationList.h"

DEFINE_LOG_CATEGORY_STATIC(LogFUOnlineSessionEditor, Log, All);

void FFUOnlineSessionEditorModule::StartupModule()
{
	// Cook、命令行工具以及没有工程上下文的进程不应注册 Editor 设置回调。
	if (IsRunningCommandlet() || !FPaths::IsProjectFilePathSet())
	{
		return;
	}

	// 【一次性迁移】启动期只调用历史块清理一次；成功后标记消失，后续启动不会再写项目配置。
	const EFU_OnlineConfigResult MigrationResult =
		FFUOnlineSessionConfigManager::EnsureProjectConfiguration();
	if (MigrationResult == EFU_OnlineConfigResult::Updated)
	{
		UE_LOG(LogFUOnlineSessionEditor, Log, TEXT("FU Online Session 已迁移历史项目配置；请重启编辑器使配置层重新加载"));
	}

	// 迁移完成后才注册设置回调；Settings 改动始终只保存插件专属域。
	RegisterSettingsChangedHandler();
}

void FFUOnlineSessionEditorModule::RegisterSettingsChangedHandler()
{
	UFU_OnlineSessionSettings* Settings =
		GetMutableDefault<UFU_OnlineSessionSettings>();

	if (!Settings)
	{
		UE_LOG(LogFUOnlineSessionEditor, Error, TEXT("无法取得 FU Online Session 设置对象"));
		return;
	}

	RegisteredSettings = Settings;

	// UDeveloperSettings 自己提供的委托只会响应本设置对象，
	// 比全局 FCoreUObjectDelegates::OnObjectPropertyChanged 更精确，也减少无关回调。
	SettingsChangedHandle = Settings->OnSettingChanged().AddRaw(
		this,
		&FFUOnlineSessionEditorModule::HandleSettingsChanged
	);
}

void FFUOnlineSessionEditorModule::HandleSettingsChanged(
	UObject* SettingsObject,
	FPropertyChangedEvent& PropertyChangedEvent
)
{
	UFU_OnlineSessionSettings* Settings =
		Cast<UFU_OnlineSessionSettings>(SettingsObject);

	if (!Settings)
	{
		return;
	}

	// 设置面板通常也会保存 Config；这里主动保存一次，确保插件在其他自定义详情面板中使用时同样可靠。
	Settings->SaveConfig();

	const FName PropertyName =
		PropertyChangedEvent.GetPropertyName();

	UE_LOG(
		LogFUOnlineSessionEditor,
		Log,
		TEXT("FU Online Session 设置已改变：%s，已保存插件专属配置，不会写入 DefaultEngine.ini"),
		PropertyName.IsNone() ? TEXT("Unknown") : *PropertyName.ToString()
	);

	// 【设置变更】只保存插件自身配置并提示重启；禁止在此路径重跑迁移或写 DefaultEngine.ini。
	UE_LOG(
		LogFUOnlineSessionEditor,
		Log,
		TEXT("FU Online Session 设置已保存到插件配置；如需重新加载传输设置，请重启编辑器")
	);
	FNotificationInfo NotificationInfo(FText::FromString(TEXT("FU Online Session 设置已保存到插件配置；请重启编辑器后再测试联网")));
	NotificationInfo.ExpireDuration = 8.0f;
	NotificationInfo.bUseSuccessFailIcons = true;
	NotificationInfo.bUseLargeFont = false;
	const TSharedPtr<SNotificationItem> Notification = FSlateNotificationManager::Get().AddNotification(NotificationInfo);
	if (Notification.IsValid())
	{
		Notification->SetCompletionState(SNotificationItem::CS_None);
	}
}

void FFUOnlineSessionEditorModule::ShutdownModule()
{
	// 模块可能因热重载被卸载；必须解除 AddRaw 注册，避免委托以后调用已经销毁的模块对象。
	if (RegisteredSettings.IsValid() && SettingsChangedHandle.IsValid())
	{
		RegisteredSettings->OnSettingChanged().Remove(SettingsChangedHandle);
	}

	SettingsChangedHandle.Reset();
	RegisteredSettings.Reset();
}

IMPLEMENT_MODULE(
    FFUOnlineSessionEditorModule,
    FUOnlineSessionEditor
)
