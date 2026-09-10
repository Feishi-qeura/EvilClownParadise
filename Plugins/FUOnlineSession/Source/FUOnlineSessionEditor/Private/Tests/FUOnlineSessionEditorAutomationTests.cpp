#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "PluginDescriptor.h"
#include "UObject/UnrealType.h"
#include "FUOnlineSessionConfigManager.h"
#include "FU_OnlineSessionSettings.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFUOnlineSessionExternalDriverAnalysisTest,
	"FUOnlineSession.EditorConfig.ExternalDriverAnalysis",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFUOnlineSessionExternalDriverAnalysisTest::RunTest(const FString& Parameters)
{
	const FString NoExternalDriver = TEXT(
		"[OnlineSubsystem]\n"
		"DefaultPlatformService=Steam\n");

	TestEqual(
		TEXT("没有外部 GameNetDriver 时可以安全写入"),
		FFUOnlineSessionConfigManager::AnalyzeExternalGameNetDriver(NoExternalDriver),
		EFU_ExternalGameNetDriverState::None);

	const FString CompatibleLegacySteamDriver = TEXT(
		"[/Script/Engine.GameEngine]\n"
		"+NetDriverDefinitions=(DefName=\"GameNetDriver\",DriverClassName=\"OnlineSubsystemSteam.SteamNetDriver\",DriverClassNameFallback=\"OnlineSubsystemUtils.IpNetDriver\")\n");

	TestEqual(
		TEXT("旧写法但语义相同的 Steam 驱动只视为兼容重复项"),
		FFUOnlineSessionConfigManager::AnalyzeExternalGameNetDriver(CompatibleLegacySteamDriver),
		EFU_ExternalGameNetDriverState::Compatible);

	const FString ConflictingCustomDriver = TEXT(
		"[/Script/Engine.GameEngine]\n"
		"+NetDriverDefinitions=(DefName=\"GameNetDriver\",DriverClassName=\"/Script/MyNetwork.MyNetDriver\",DriverClassNameFallback=\"/Script/OnlineSubsystemUtils.IpNetDriver\")\n");

	TestEqual(
		TEXT("自定义 GameNetDriver 不允许被插件静默覆盖"),
		FFUOnlineSessionConfigManager::AnalyzeExternalGameNetDriver(ConflictingCustomDriver),
		EFU_ExternalGameNetDriverState::Conflict);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFUOnlineSessionManagedConfigBlockTest,
	"FUOnlineSession.EditorConfig.ManagedBlockUsesSteamSockets",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFUOnlineSessionManagedConfigBlockTest::RunTest(const FString& Parameters)
{
	UFU_OnlineSessionSettings* Settings = NewObject<UFU_OnlineSessionSettings>();
	Settings->SteamDevAppId = 480;

	const FString ManagedBlock =
		FFUOnlineSessionConfigManager::BuildManagedConfigBlock(*Settings);

	// 自动配置必须生成 UE 5.8 SteamSockets 模块中的真实驱动类。
	TestTrue(
		TEXT("管理区块包含 SteamSocketsNetDriver"),
		ManagedBlock.Contains(TEXT("/Script/SteamSockets.SteamSocketsNetDriver")));

	TestTrue(
		TEXT("管理区块包含 SteamSocketsNetConnection"),
		ManagedBlock.Contains(TEXT("/Script/SteamSockets.SteamSocketsNetConnection")));

	// 【FU 回归测试：双 Provider 的 SocketSubsystem 隔离】
	// bUseSteamNetworking 控制 SteamSockets 是否成为“全局默认”SocketSubsystem。
	// 如果这里为 true，运行时即使把 NetDriver 类切换成 IpNetDriver，IpNetDriver 仍可能
	// 拿到 SteamSockets，并在创建 LAN 广播 Socket 时因 SO_BROADCAST 不受支持而监听失败。
	// 因此全局默认必须保留为平台原生 Socket；Steam 模板仍会通过显式
	// SteamSocketsNetDriver 使用 SteamSockets，这不会关闭 Steam Lobby 联机。
	TestTrue(
		TEXT("管理区块禁止 SteamSockets 接管全局默认 SocketSubsystem"),
		ManagedBlock.Contains(TEXT("bUseSteamNetworking=false")));

	TestFalse(
		TEXT("管理区块不能重新生成会破坏 LAN 广播的旧配置"),
		ManagedBlock.Contains(TEXT("bUseSteamNetworking=true")));

	// 旧路径会加载失败并静默回退到 IpNetDriver，这个回归必须被测试阻止。
	TestFalse(
		TEXT("管理区块不再生成旧 OnlineSubsystemSteam NetDriver 路径"),
		ManagedBlock.Contains(TEXT("/Script/OnlineSubsystemSteam.SteamNetDriver")));

	return true;
}

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
		TEXT("搜索模板入口必须检查 Provider 是否 Ready"),
		RuntimeSource.Contains(TEXT("FU_ValidateProviderReady<Provider>(TEXT(\"FindSessions\"))")));

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

#endif
