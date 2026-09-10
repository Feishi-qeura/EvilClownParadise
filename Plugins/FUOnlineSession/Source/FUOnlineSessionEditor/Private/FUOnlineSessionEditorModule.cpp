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
	// Cook、命令行工具以及没有工程上下文的进程不应修改项目源配置。
	if (IsRunningCommandlet() || !FPaths::IsProjectFilePathSet())
	{
		return;
	}

	// 先注册设置回调，之后用户在 Project Settings 中修改选项即可立即重新生成配置。
	RegisterSettingsChangedHandler();
	ApplyProjectConfiguration(false);
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
		TEXT("FU Online Session 设置已改变：%s，开始同步 DefaultEngine.ini"),
		PropertyName.IsNone() ? TEXT("Unknown") : *PropertyName.ToString()
	);

	ApplyProjectConfiguration(true);
}

void FFUOnlineSessionEditorModule::ApplyProjectConfiguration(
	const bool bShowEditorNotification
)
{
	const EFU_OnlineConfigResult Result =
		FFUOnlineSessionConfigManager::EnsureProjectConfiguration();

	// 通知只用于用户主动修改设置的场景；编辑器启动时继续使用日志，避免每次打开项目都弹提示。
	const auto ShowNotification = [bShowEditorNotification](
		const FText& Text,
		const SNotificationItem::ECompletionState CompletionState)
	{
		if (!bShowEditorNotification)
		{
			return;
		}

		FNotificationInfo NotificationInfo(Text);
		NotificationInfo.ExpireDuration = 8.0f;
		NotificationInfo.bUseSuccessFailIcons = true;
		NotificationInfo.bUseLargeFont = false;

		const TSharedPtr<SNotificationItem> Notification =
			FSlateNotificationManager::Get().AddNotification(NotificationInfo);

		if (Notification.IsValid())
		{
			Notification->SetCompletionState(CompletionState);
		}
	};

	switch (Result)
	{
	case EFU_OnlineConfigResult::Updated:
		// 配置层在本次启动早期已经加载，写盘后必须等下次启动才会完全生效。
		UE_LOG(
			LogFUOnlineSessionEditor,
			Warning,
			TEXT("FU Online Session 已更新 DefaultEngine.ini，请重启编辑器使配置完全生效")
		);
		ShowNotification(
			FText::FromString(TEXT("FU Online Session 配置已更新，请重启编辑器后再测试联网")),
			SNotificationItem::CS_Success
		);
		break;

	case EFU_OnlineConfigResult::Unchanged:
		UE_LOG(LogFUOnlineSessionEditor, Log, TEXT("FU Online Session 配置已经是最新状态"));
		break;

	case EFU_OnlineConfigResult::Disabled:
		UE_LOG(LogFUOnlineSessionEditor, Log, TEXT("FU Online Session 自动配置已关闭"));
		ShowNotification(
			FText::FromString(TEXT("FU Online Session 自动配置已关闭；已有管理区块不会被自动删除")),
			SNotificationItem::CS_None
		);
		break;

	case EFU_OnlineConfigResult::Conflict:
		UE_LOG(
			LogFUOnlineSessionEditor,
			Error,
			TEXT("FU Online Session 检测到自定义 GameNetDriver，未覆盖 DefaultEngine.ini")
		);
		ShowNotification(
			FText::FromString(TEXT("检测到自定义 GameNetDriver，FU Online Session 已停止自动覆盖配置")),
			SNotificationItem::CS_Fail
		);
		break;

	case EFU_OnlineConfigResult::Failed:
	default:
		// 失败时绝不继续覆盖文件，保留用户已有配置以便人工检查。
		UE_LOG(
			LogFUOnlineSessionEditor,
			Error,
			TEXT("FU Online Session 无法更新 DefaultEngine.ini，请检查文件权限或自动配置标记")
		);
		ShowNotification(
			FText::FromString(TEXT("FU Online Session 配置更新失败，请查看 Output Log")),
			SNotificationItem::CS_Fail
		);
		break;
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
