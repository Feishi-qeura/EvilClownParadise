#include "Features/ECPGameFeaturePolicy.h"
#include "Features/ECPGameFeatureSettings.h"
#include "ECPCore.h"
#include "GameFeaturePluginOperationResult.h"
#include "Interfaces/IPluginManager.h"

namespace
{
	// 仅用于安全查询已创建的策略，不在蓝图查询时隐式创建/启动引擎子系统。
	TWeakObjectPtr<UECPGameFeaturePolicy> ActiveFeatureRegistry;
}

UECPGameFeaturePolicy* UECPGameFeaturePolicy::GetFeatureRegistry()
{
	return ActiveFeatureRegistry.Get();
}

void UECPGameFeaturePolicy::InitGameFeatureManager()
{
	// 引擎级入口幂等；不调用默认的无清单全插件激活，避免两套并行启动流程。
	if (bStarted) return;
	bStarted = true;
	ActiveFeatureRegistry = this;
	FString Error;
	if (!ECPFeatureManifest::BuildActivationOrder(GetDefault<UECPGameFeatureSettings>()->Features, ActivationOrder, Error))
	{
		FailStartup(NAME_None, Error);
		return;
	}
	for (FName Name : ActivationOrder)
	{
		FECPGameFeatureStatus Status;
		Status.PluginName = Name;
		Status.State = EECPGameFeatureState::Waiting;
		Statuses.Add(Status);
	}
	// 在任何激活前校验完整插件集合，防止缺包被误报为游戏已就绪。
	for (FName Name : ActivationOrder)
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(Name.ToString());
		if (!Plugin || !Plugin->IsEnabled() || !UGameFeaturesSubsystem::IsGameFeaturePlugin(Plugin))
		{
			FailStartup(Name, FString::Printf(TEXT("Missing, disabled or invalid Game Feature plugin: %s"), *Name.ToString()));
			return;
		}
	}
	ActivateNext();
}

void UECPGameFeaturePolicy::ActivateNext()
{
	if (bShuttingDown || !StartupFailure.IsEmpty()) return;
	if (NextFeatureIndex == ActivationOrder.Num())
	{
		// 激活另一功能的 Action 也可能改变旧功能状态；最终就绪事件必须核对当前事实。
		for (FName Name : ActivationOrder)
		{
			if (!UGameFeaturesSubsystem::Get().IsGameFeaturePluginActiveByName(Name.ToString()))
			{
				FailStartup(Name, TEXT("Feature was deactivated before startup completed."));
				return;
			}
		}
		bReady = true;
		UE_LOG(LogECP, Display, TEXT("All %d project Game Features are active."), ActivationOrder.Num());
		OnFeaturesReady.Broadcast();
		return;
	}
	const FName Name = ActivationOrder[NextFeatureIndex];
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(Name.ToString());
	if (!Plugin)
	{
		FailStartup(Name, TEXT("Plugin disappeared during startup."));
		return;
	}
	Statuses[NextFeatureIndex].State = EECPGameFeatureState::Activating;
	// 本地内置功能同步装配，保证打开关卡/解析旧蓝图父类前已挂载并注册。
	// 即使引擎将某阶段推迟，完成状态仍以回调为准，GameInstance 就绪门会阻止提前进入地图。
	UGameFeaturesSubsystem::Get().LoadBuiltInGameFeaturePlugin(Plugin.ToSharedRef(),
		[](const FString&, const FGameFeaturePluginDetails&, FBuiltInGameFeaturePluginBehaviorOptions& Options)
		{
			Options.AutoStateOverride = EBuiltInAutoState::Active;
			Options.bForceSyncLoading = true;
			return true;
		}, FGameFeaturePluginLoadComplete::CreateUObject(this, &ThisClass::CompleteActivation, Name));
}

void UECPGameFeaturePolicy::CompleteActivation(const UE::GameFeatures::FResult& Result, FName PluginName)
{
	// 迟到的回调不能在引擎退出时发起下一项加载，亦不能覆盖另一个功能的状态。
	if (bShuttingDown || !StartupFailure.IsEmpty()) return;
	if (!ActivationOrder.IsValidIndex(NextFeatureIndex) || ActivationOrder[NextFeatureIndex] != PluginName)
	{
		FailStartup(PluginName, TEXT("Unexpected feature activation completion order."));
		return;
	}
	if (Result.HasError())
	{
		FailStartup(PluginName, UE::GameFeatures::ToString(Result));
		return;
	}
	if (!UGameFeaturesSubsystem::Get().IsGameFeaturePluginActiveByName(PluginName.ToString()))
	{
		FailStartup(PluginName, TEXT("Load completed without reaching Active state."));
		return;
	}
	Statuses[NextFeatureIndex].State = EECPGameFeatureState::Active;
	++NextFeatureIndex;
	ActivateNext();
}

void UECPGameFeaturePolicy::FailStartup(FName PluginName, const FString& Reason)
{
	bReady = false;
	StartupFailure = FString::Printf(TEXT("%s: %s"), *PluginName.ToString(), *Reason);
	for (FECPGameFeatureStatus& Status : Statuses)
	{
		if (Status.State != EECPGameFeatureState::Active)
		{
			Status.State = EECPGameFeatureState::Failed;
			Status.Error = StartupFailure;
		}
	}
	UE_LOG(LogECP, Error, TEXT("Game Feature startup failed: %s"), *StartupFailure);
	OnFeatureStartupFailed.Broadcast(PluginName, StartupFailure);
}

bool UECPGameFeaturePolicy::AreFeaturesReady() const
{
	if (!bReady || bShuttingDown || ActivationOrder.IsEmpty()) return false;
	// 编辑器可以手动改变 Feature 状态，所以不能仅返回历史上的成功缓存。
	for (FName Name : ActivationOrder)
	{
		if (!UGameFeaturesSubsystem::Get().IsGameFeaturePluginActiveByName(Name.ToString())) return false;
	}
	return true;
}

FECPGameFeatureStatus UECPGameFeaturePolicy::GetFeatureStatus(FName PluginName) const
{
	for (const FECPGameFeatureStatus& Status : Statuses)
	{
		if (Status.PluginName == PluginName)
		{
			FECPGameFeatureStatus Result = Status;
			if (Result.State == EECPGameFeatureState::Active
				&& !UGameFeaturesSubsystem::Get().IsGameFeaturePluginActiveByName(PluginName.ToString()))
			{
				Result.State = EECPGameFeatureState::Failed;
				Result.Error = TEXT("Feature is no longer active; restart before entering gameplay.");
			}
			return Result;
		}
	}
	FECPGameFeatureStatus Unknown;
	Unknown.PluginName = PluginName;
	return Unknown;
}

TArray<FECPGameFeatureStatus> UECPGameFeaturePolicy::GetFeatureStatuses() const
{
	TArray<FECPGameFeatureStatus> Result;
	Result.Reserve(Statuses.Num());
	// 全量与单项查询使用相同实时检查，编辑器手工停用后不会继续展示旧 Active。
	for (const FECPGameFeatureStatus& Status : Statuses)
	{
		Result.Add(GetFeatureStatus(Status.PluginName));
	}
	return Result;
}

FString UECPGameFeaturePolicy::GetStartupFailureReason() const
{
	if (!StartupFailure.IsEmpty()) return StartupFailure;
	return AreFeaturesReady() ? FString() : TEXT("Required Game Features are not active. Check the manifest and startup log.");
}

void UECPGameFeaturePolicy::ShutdownGameFeatureManager()
{
	bShuttingDown = true;
	bReady = false;
	OnFeaturesReady.Clear();
	OnFeatureStartupFailed.Clear();
	if (ActiveFeatureRegistry.Get() == this) ActiveFeatureRegistry.Reset();
	// 实际插件清理由引擎 GameFeatures 负责；不会从某个 PIE GameInstance 反向销毁 FU 后端。
	Super::ShutdownGameFeatureManager();
}
