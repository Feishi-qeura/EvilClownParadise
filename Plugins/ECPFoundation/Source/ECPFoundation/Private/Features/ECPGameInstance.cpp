#include "Features/ECPGameInstance.h"
#include "Features/ECPGameFeaturePolicy.h"
#include "ECPCore.h"

bool UECPGameInstance::CheckRequiredFeatures(FString& OutReason) const
{
	const UECPGameFeaturePolicy* Registry = UECPGameFeaturePolicy::GetFeatureRegistry();
	// 客户端/服务器分别检查本地装配，不能把连接成功视为功能或资源已加载。
	if (!Registry)
	{
		OutReason = TEXT("ECP Game Feature policy was not initialized. Check GameFeaturesManagerClassName.");
		return false;
	}
	if (!Registry->AreFeaturesReady())
	{
		OutReason = Registry->GetStartupFailureReason();
		return false;
	}
	OutReason.Reset();
	return true;
}

void UECPGameInstance::StartGameInstance()
{
	if (bGameStarted) return;
	bStartRequested = true;
	UECPGameFeaturePolicy* Registry = UECPGameFeaturePolicy::GetFeatureRegistry();
	if (!Registry || Registry->HasStartupFailed())
	{
		FString Reason;
		CheckRequiredFeatures(Reason);
		HandleFeatureStartupFailed(NAME_None, Reason);
		return;
	}
	if (Registry->AreFeaturesReady())
	{
		HandleFeaturesReady();
		return;
	}
	// 正常内置加载同步完成；若引擎某阶段延后，订阅完成事件，不能把 Pending 当成永久失败。
	Registry->OnFeaturesReady.AddUniqueDynamic(this, &ThisClass::HandleFeaturesReady);
	Registry->OnFeatureStartupFailed.AddUniqueDynamic(this, &ThisClass::HandleFeatureStartupFailed);
}

void UECPGameInstance::HandleFeaturesReady()
{
	if (!bStartRequested || bGameStarted) return;
	FString Reason;
	if (!CheckRequiredFeatures(Reason))
	{
		HandleFeatureStartupFailed(NAME_None, Reason);
		return;
	}
	UnbindFeatureStartup();
	// 先置位再调用引擎启动，防止关卡初始化触发的重入重复创建游戏世界。
	bGameStarted = true;
	Super::StartGameInstance();
}

void UECPGameInstance::HandleFeatureStartupFailed(FName PluginName, const FString& Reason)
{
	UnbindFeatureStartup();
	bStartRequested = false;
	UE_LOG(LogECP, Error, TEXT("Game startup blocked (%s): %s"), *PluginName.ToString(), *Reason);
}

void UECPGameInstance::UnbindFeatureStartup()
{
	if (UECPGameFeaturePolicy* Registry = UECPGameFeaturePolicy::GetFeatureRegistry())
	{
		Registry->OnFeaturesReady.RemoveDynamic(this, &ThisClass::HandleFeaturesReady);
		Registry->OnFeatureStartupFailed.RemoveDynamic(this, &ThisClass::HandleFeatureStartupFailed);
	}
}

void UECPGameInstance::Shutdown()
{
	// 一个实例退出只撤销自己的监听，绝不停用进程级功能或其他实例的 FU 后端。
	bStartRequested = false;
	UnbindFeatureStartup();
	Super::Shutdown();
}

#if WITH_EDITOR
FGameInstancePIEResult UECPGameInstance::InitializeForPlayInEditor(int32 PIEInstanceIndex,
	const FGameInstancePIEParameters& Params)
{
	FString Reason;
	if (!CheckRequiredFeatures(Reason))
	{
		// PIE 初始化接口是同步结果；未就绪时明确拒绝，编辑器仍可修复配置后重新开始。
		return FGameInstancePIEResult::Failure(FText::FromString(Reason));
	}
	return Super::InitializeForPlayInEditor(PIEInstanceIndex, Params);
}
#endif
