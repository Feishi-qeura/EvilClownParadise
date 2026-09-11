#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "PluginDescriptor.h"
#include "UObject/UnrealType.h"
#include "FUOnlineSessionConfigManager.h"
#include "FUOnlineSessionLegacyConfigMigration.h"
#include "FU_OnlineSessionSettings.h"
#include "Misc/SecureHash.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFUOnlineSessionProviderOperationPreflightWiringTest,
	"FUOnlineSession.Runtime.ProviderOperationsRequireReadyStatus",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFUOnlineSessionProviderOperationPreflightWiringTest::RunTest(const FString& Parameters)
{
	// 【FU 回归测试：运行时仍需自我保护】
	// CheckSteamProviderStatus/CheckLanProviderStatus 是给蓝图展示 UI 的便利接口，
	// 但插件不能假设每个使用者都会先调用它。这里直接检查 Runtime 模板入口的源码接线，
	// 防止以后重构时漏掉某一个入口，使未登录 Steam 的请求再次进入异步 OSS 流程。
	const FString RuntimeSourcePath = FPaths::Combine(
		FPaths::ProjectPluginsDir(),
		TEXT("FUOnlineSession/Source/FUOnlineSession/Private/FU_OnlineSessionSubsystem.cpp"));

	FString RuntimeSource;
	TestTrue(
		TEXT("能够读取 FU Online Session Runtime 实现"),
		FFileHelper::LoadFileToString(RuntimeSource, *RuntimeSourcePath));

	TestTrue(
		TEXT("创建模板入口必须检查 Provider 是否 Ready"),
		RuntimeSource.Contains(TEXT("FU_ValidateProviderReady<Provider>(TEXT(\"CreateSession\"))")));

	TestTrue(
		TEXT("搜索模板入口必须检查 Provider 是否 Ready，但不得占用传输 NetDriver 租约"),
		// 【FU 回归测试：搜索与传输解耦】FindSessions 仍需验证 OSS、身份与 AppID，
		// 但它不会 Listen/ClientTravel，所以必须显式传入 false，避免另一 Provider 的活动连接阻止搜索。
		RuntimeSource.Contains(TEXT("FU_ValidateProviderReady<Provider>(TEXT(\"FindSessions\"), false)")));

	TestTrue(
		TEXT("加入模板入口必须检查 Provider 是否 Ready"),
		RuntimeSource.Contains(TEXT("FU_ValidateProviderReady<Provider>(TEXT(\"JoinSession\"))")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFUOnlineSessionDescriptorConfigContractTest,
	"FUOnlineSession.EditorConfig.Descriptor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFUOnlineSessionDescriptorConfigContractTest::RunTest(const FString& Parameters)
{
	const FString PluginDirectory = FPaths::Combine(FPaths::ProjectPluginsDir(), TEXT("FUOnlineSession"));
	const FString DescriptorPath = FPaths::Combine(PluginDirectory, TEXT("FUOnlineSession.uplugin"));
	const FString EngineIniPath = FPaths::Combine(PluginDirectory, TEXT("Config/Engine.ini"));
	const FString DefaultSettingsIniPath = FPaths::Combine(PluginDirectory, TEXT("Config/DefaultFUOnlineSession.ini"));
	const FString EditorModuleSourcePath = FPaths::Combine(
		PluginDirectory,
		TEXT("Source/FUOnlineSessionEditor/Private/FUOnlineSessionEditorModule.cpp"));

	FPluginDescriptor Descriptor;
	FText DescriptorLoadFailure;
	TestTrue(
		TEXT("能够解析 FUOnlineSession 插件描述符"),
		Descriptor.Load(DescriptorPath, DescriptorLoadFailure));

	TestEqual(TEXT("描述符锁定 UE 5.8"), Descriptor.EngineVersion, FString(TEXT("5.8.0")));
	TestEqual(TEXT("插件只支持 Win64"), Descriptor.SupportedTargetPlatforms, TArray<FString>{ TEXT("Win64") });

	const FModuleDescriptor* RuntimeModule = Descriptor.Modules.FindByPredicate([](const FModuleDescriptor& Module)
	{
		return Module.Name == TEXT("FUOnlineSession");
	});
	const FModuleDescriptor* EditorModule = Descriptor.Modules.FindByPredicate([](const FModuleDescriptor& Module)
	{
		return Module.Name == TEXT("FUOnlineSessionEditor");
	});

	TestNotNull(TEXT("描述符包含 Runtime 模块"), RuntimeModule);
	TestNotNull(TEXT("描述符包含 Editor 模块"), EditorModule);

	// 【配置契约】两个模块都只能面向 Win64，且必须排除 arm64 架构，避免生成不受支持的二进制。
	auto AssertModulePlatformContract = [this](const FModuleDescriptor* Module, const ELoadingPhase::Type ExpectedLoadingPhase, const TCHAR* ModuleLabel)
	{
		if (Module == nullptr)
		{
			return;
		}

		TestEqual(FString::Printf(TEXT("%s 模块加载阶段正确"), ModuleLabel), Module->LoadingPhase, ExpectedLoadingPhase);
		TestEqual(FString::Printf(TEXT("%s 模块只允许 Win64"), ModuleLabel), Module->PlatformAllowList, TArray<FString>{ TEXT("Win64") });

		const TArray<FString>* DeniedArchitectures = Module->PlatformArchitectureDenyList.Find(TEXT("Win64"));
		TestNotNull(FString::Printf(TEXT("%s 模块声明 Win64 架构拒绝列表"), ModuleLabel), DeniedArchitectures);
		if (DeniedArchitectures != nullptr)
		{
			TestEqual(FString::Printf(TEXT("%s 模块拒绝 Win64 arm64"), ModuleLabel), *DeniedArchitectures, TArray<FString>{ TEXT("arm64") });
		}
	};

	AssertModulePlatformContract(RuntimeModule, ELoadingPhase::PreDefault, TEXT("Runtime"));
	AssertModulePlatformContract(EditorModule, ELoadingPhase::PostEngineInit, TEXT("Editor"));

	for (const TCHAR* RequiredPlugin : { TEXT("OnlineSubsystem"), TEXT("OnlineSubsystemUtils"), TEXT("OnlineSubsystemNull"), TEXT("OnlineSubsystemSteam"), TEXT("SteamSockets") })
	{
		const FPluginReferenceDescriptor* Dependency = Descriptor.Plugins.FindByPredicate([RequiredPlugin](const FPluginReferenceDescriptor& Plugin)
		{
			return Plugin.Name == RequiredPlugin;
		});

		TestNotNull(FString::Printf(TEXT("描述符声明 %s 依赖"), RequiredPlugin), Dependency);
		if (Dependency != nullptr)
		{
			TestTrue(FString::Printf(TEXT("%s 依赖默认启用"), RequiredPlugin), Dependency->bEnabled);
		}
	}

	FString EngineIniText;
	TestTrue(TEXT("能够读取插件专属 Engine.ini"), FFileHelper::LoadFileToString(EngineIniText, *EngineIniPath));
	FConfigFile EngineIni;
	EngineIni.Read(EngineIniPath);
	FString DefaultPlatformService;
	bool bSteamEnabled = false;
	bool bUseSteamNetworking = true;
	bool bAllowP2PPacketRelay = false;
	TestTrue(TEXT("Engine.ini 解析 Null 平台服务"), EngineIni.GetString(TEXT("OnlineSubsystem"), TEXT("DefaultPlatformService"), DefaultPlatformService));
	TestEqual(TEXT("Engine.ini 默认使用 Null 平台服务"), DefaultPlatformService, FString(TEXT("Null")));
	TestTrue(TEXT("Engine.ini 解析 Steam 启用开关"), EngineIni.GetBool(TEXT("OnlineSubsystemSteam"), TEXT("bEnabled"), bSteamEnabled));
	TestTrue(TEXT("Engine.ini 启用 Steam 子系统"), bSteamEnabled);
	TestTrue(TEXT("Engine.ini 解析 SteamSockets 全局接管开关"), EngineIni.GetBool(TEXT("OnlineSubsystemSteam"), TEXT("bUseSteamNetworking"), bUseSteamNetworking));
	TestFalse(TEXT("Engine.ini 不让 SteamSockets 接管全局 Socket"), bUseSteamNetworking);
	TestTrue(TEXT("Engine.ini 解析 Steam P2P 中继开关"), EngineIni.GetBool(TEXT("OnlineSubsystemSteam"), TEXT("bAllowP2PPacketRelay"), bAllowP2PPacketRelay));
	TestTrue(TEXT("Engine.ini 允许 Steam P2P 中继"), bAllowP2PPacketRelay);
	TestFalse(TEXT("Engine.ini 不得保存开发 Steam AppID"), EngineIniText.Contains(TEXT("SteamDevAppId=")));

	FString DefaultSettingsIniText;
	TestTrue(TEXT("能够读取插件专属默认设置 ini"), FFileHelper::LoadFileToString(DefaultSettingsIniText, *DefaultSettingsIniPath));
	TestTrue(TEXT("默认设置 ini 明确配置 SteamDevAppId=480"), DefaultSettingsIniText.Contains(TEXT("SteamDevAppId=480")));
	TestTrue(TEXT("默认设置 ini 明确配置 ExpectedShippingSteamAppId=0"), DefaultSettingsIniText.Contains(TEXT("ExpectedShippingSteamAppId=0")));
	TestTrue(TEXT("默认设置 ini 明确配置 OperationTimeoutSeconds=30.0"), DefaultSettingsIniText.Contains(TEXT("OperationTimeoutSeconds=30.0")));

	FString EditorModuleSource;
	TestTrue(TEXT("能够读取 FU Online Session Editor 模块实现"), FFileHelper::LoadFileToString(EditorModuleSource, *EditorModuleSourcePath));
	// 【迁移契约】启动仅调用一次迁移入口；设置变更路径不得重新触发，避免持续管理项目配置。
	TestTrue(
		TEXT("Editor 启动调用一次历史配置迁移"),
		EditorModuleSource.Contains(TEXT("FFUOnlineSessionConfigManager::EnsureProjectConfiguration")));

	const UFU_OnlineSessionSettings* Settings = GetDefault<UFU_OnlineSessionSettings>();
	TestEqual(TEXT("设置保存到插件专属配置域"), Settings->GetClass()->ClassConfigName, FName(TEXT("FUOnlineSession")));
	TestEqual(TEXT("Steam 开发 AppID 默认值"), Settings->SteamDevAppId, 480);

	// 【前置约束】新增字段以反射方式读取，使 RED 阶段能明确报出“字段尚未实现”，而不是测试本身无法编译。
	auto TestNumericDefault = [this, Settings](const FName PropertyName, const double ExpectedValue)
	{
		const FNumericProperty* Property = FindFProperty<FNumericProperty>(Settings->GetClass(), PropertyName);
		TestNotNull(FString::Printf(TEXT("设置包含 %s"), *PropertyName.ToString()), Property);
		if (Property != nullptr)
		{
			const void* ValueAddress = Property->ContainerPtrToValuePtr<void>(Settings);
			const double ActualValue = Property->IsFloatingPoint()
				? Property->GetFloatingPointPropertyValue(ValueAddress)
				: static_cast<double>(Property->GetSignedIntPropertyValue(ValueAddress));
			TestTrue(
				FString::Printf(TEXT("%s 默认值正确"), *PropertyName.ToString()),
				FMath::IsNearlyEqual(ActualValue, ExpectedValue));
		}
	};

	TestNumericDefault(TEXT("ExpectedShippingSteamAppId"), 0.0);
	TestNumericDefault(TEXT("OperationTimeoutSeconds"), 30.0);

	return true;
}

namespace FUOnlineSessionLegacyMigrationTests
{
	/**
	 * 【迁移测试辅助】从固定字节构造哈希，避免测试把二进制 ini 重新编码为字符串。
	 * 这里的哈希仅用于验证发布结果分类；输入为空时仍保留 SHA 的确定性结果。
	 */
	FSHAHash HashBytes(const TArray<uint8>& Bytes)
	{
		FSHAHash Hash;
		FSHA1::HashBuffer(Bytes.GetData(), Bytes.Num(), Hash.Hash);
		return Hash;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFUOnlineSessionLegacyConfigMigrationTest,
	"FUOnlineSession.EditorConfig.LegacyMigration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFUOnlineSessionLegacyConfigMigrationTest::RunTest(const FString& Parameters)
{
	using namespace FUOnlineSessionLegacyMigrationTests;

	const TArray<uint8> ValidFixture = {
		0xEF, 0xBB, 0xBF,
		'P', 'r', 'e', 'f', 'i', 'x', '\r', '\n',
		';', ' ', 'B', 'E', 'G', 'I', 'N', ' ', 'F', 'U', 'O', 'N', 'L', 'I', 'N', 'E', 'S', 'E', 'S', 'S', 'I', 'O', 'N', ' ', 'A', 'U', 'T', 'O', ' ', 'C', 'O', 'N', 'F', 'I', 'G', '\n',
		0x80, 'M', 'i', 'd', '\r', '\n',
		';', ' ', 'E', 'N', 'D', ' ', 'F', 'U', 'O', 'N', 'L', 'I', 'N', 'E', 'S', 'E', 'S', 'S', 'I', 'O', 'N', ' ', 'A', 'U', 'T', 'O', ' ', 'C', 'O', 'N', 'F', 'I', 'G', '\r', '\n',
		'S', 'u', 'f', 'f', 'i', 'x', 0x00, '\n'
	};
	const TArray<uint8> ExpectedValidBytes = {
		0xEF, 0xBB, 0xBF,
		'P', 'r', 'e', 'f', 'i', 'x', '\r', '\n',
		'S', 'u', 'f', 'f', 'i', 'x', 0x00, '\n'
	};

	// 【字节保真契约】BOM、混合换行和非文本字节都必须原样保留，只删除完整受管行范围。
	const FFU_LegacyTransformResult ValidTransform = FFU_LegacyConfigMigration::Transform(ValidFixture);
	TestEqual(TEXT("完整标记对可以发布"), ValidTransform.Result, EFU_LegacyMigrationResult::Published);
	TestTrue(TEXT("完整标记对只删除受管字节"), ValidTransform.TransformedBytes == ExpectedValidBytes);

	const TArray<uint8> NoMarkerFixture = { 0xEF, 0xBB, 0xBF, 'A', '\r', '\n', 0x80, 'B', '\n' };
	const FFU_LegacyTransformResult NoMarkerTransform = FFU_LegacyConfigMigration::Transform(NoMarkerFixture);
	TestEqual(TEXT("没有标记不触发发布"), NoMarkerTransform.Result, EFU_LegacyMigrationResult::NoManagedBlock);
	TestTrue(TEXT("没有标记保持输入字节"), NoMarkerTransform.TransformedBytes == NoMarkerFixture);

	const TArray<TArray<uint8>> InvalidFixtures = {
		{ ';', ' ', 'B', 'E', 'G', 'I', 'N', ' ', 'F', 'U', 'O', 'N', 'L', 'I', 'N', 'E', 'S', 'E', 'S', 'S', 'I', 'O', 'N', ' ', 'A', 'U', 'T', 'O', ' ', 'C', 'O', 'N', 'F', 'I', 'G', '\n' },
		{ ';', ' ', 'E', 'N', 'D', ' ', 'F', 'U', 'O', 'N', 'L', 'I', 'N', 'E', 'S', 'E', 'S', 'S', 'I', 'O', 'N', ' ', 'A', 'U', 'T', 'O', ' ', 'C', 'O', 'N', 'F', 'I', 'G', '\n' },
		{ ';', ' ', 'E', 'N', 'D', ' ', 'F', 'U', 'O', 'N', 'L', 'I', 'N', 'E', 'S', 'E', 'S', 'S', 'I', 'O', 'N', ' ', 'A', 'U', 'T', 'O', ' ', 'C', 'O', 'N', 'F', 'I', 'G', '\n', ';', ' ', 'B', 'E', 'G', 'I', 'N', ' ', 'F', 'U', 'O', 'N', 'L', 'I', 'N', 'E', 'S', 'E', 'S', 'S', 'I', 'O', 'N', ' ', 'A', 'U', 'T', 'O', ' ', 'C', 'O', 'N', 'F', 'I', 'G', '\n' },
		{ ';', ' ', 'B', 'E', 'G', 'I', 'N', ' ', 'F', 'U', 'O', 'N', 'L', 'I', 'N', 'E', 'S', 'E', 'S', 'S', 'I', 'O', 'N', ' ', 'A', 'U', 'T', 'O', ' ', 'C', 'O', 'N', 'F', 'I', 'G', '\n', ';', ' ', 'E', 'N', 'D', ' ', 'F', 'U', 'O', 'N', 'L', 'I', 'N', 'E', 'S', 'E', 'S', 'S', 'I', 'O', 'N', ' ', 'A', 'U', 'T', 'O', ' ', 'C', 'O', 'N', 'F', 'I', 'G', '\n', ';', ' ', 'B', 'E', 'G', 'I', 'N', ' ', 'F', 'U', 'O', 'N', 'L', 'I', 'N', 'E', 'S', 'E', 'S', 'S', 'I', 'O', 'N', ' ', 'A', 'U', 'T', 'O', ' ', 'C', 'O', 'N', 'F', 'I', 'G', '\n', ';', ' ', 'E', 'N', 'D', ' ', 'F', 'U', 'O', 'N', 'L', 'I', 'N', 'E', 'S', 'E', 'S', 'S', 'I', 'O', 'N', ' ', 'A', 'U', 'T', 'O', ' ', 'C', 'O', 'N', 'F', 'I', 'G', '\n' },
		{ 'x', ';', ' ', 'B', 'E', 'G', 'I', 'N', ' ', 'F', 'U', 'O', 'N', 'L', 'I', 'N', 'E', 'S', 'E', 'S', 'S', 'I', 'O', 'N', ' ', 'A', 'U', 'T', 'O', ' ', 'C', 'O', 'N', 'F', 'I', 'G', '\n' }
	};
	for (const TArray<uint8>& InvalidFixture : InvalidFixtures)
	{
		const FFU_LegacyTransformResult InvalidTransform = FFU_LegacyConfigMigration::Transform(InvalidFixture);
		TestEqual(TEXT("损坏或非完整行标记必须失败"), InvalidTransform.Result, EFU_LegacyMigrationResult::InvalidMarkers);
		TestTrue(TEXT("损坏标记不能产生可发布字节"), InvalidTransform.TransformedBytes.IsEmpty());
	}

	const FSHAHash OriginalHash = HashBytes(ValidFixture);
	const FSHAHash TransformedHash = HashBytes(ExpectedValidBytes);
	TestEqual(
		TEXT("替换失败且目标仍是原始文件必须失败"),
		FFU_LegacyConfigMigration::ClassifyReplaceOutcome(OriginalHash, TransformedHash, OriginalHash),
		EFU_LegacyMigrationResult::Failed);
	TestEqual(
		TEXT("替换 API 失败但目标已更新必须带警告发布"),
		FFU_LegacyConfigMigration::ClassifyReplaceOutcome(OriginalHash, TransformedHash, TransformedHash),
		EFU_LegacyMigrationResult::PublishedWithWarning);
	TestEqual(
		TEXT("目标缺失必须要求人工恢复"),
		FFU_LegacyConfigMigration::ClassifyReplaceOutcome(OriginalHash, TransformedHash, TOptional<FSHAHash>()),
		EFU_LegacyMigrationResult::ManualRecoveryRequired);
	const TArray<uint8> ThirdBytes = { 'T', 'h', 'i', 'r', 'd' };
	TestEqual(
		TEXT("目标哈希未知必须要求人工恢复"),
		FFU_LegacyConfigMigration::ClassifyReplaceOutcome(OriginalHash, TransformedHash, HashBytes(ThirdBytes)),
		EFU_LegacyMigrationResult::ManualRecoveryRequired);

	return true;
}

#endif
