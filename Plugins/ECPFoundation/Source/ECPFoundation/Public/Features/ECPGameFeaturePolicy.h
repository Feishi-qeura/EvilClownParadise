#pragma once

#include "CoreMinimal.h"
#include "GameFeaturesProjectPolicies.h"
#include "ECPGameFeaturePolicy.generated.h"

/** 引擎进程的功能状态，不能当成网络复制数据。每个客户端进程都独立注册。 */
UENUM(BlueprintType)
enum class EECPGameFeatureState : uint8
{
	Unknown,
	Waiting,
	Activating,
	Active,
	Failed
};

/** 蓝图可读取的功能诊断快照；Error 非空时不得开始依赖该功能的玩法。 */
USTRUCT(BlueprintType)
struct ECPFOUNDATION_API FECPGameFeatureStatus
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly, Category = "ECP|Features") FName PluginName;
	UPROPERTY(BlueprintReadOnly, Category = "ECP|Features") EECPGameFeatureState State = EECPGameFeatureState::Unknown;
	UPROPERTY(BlueprintReadOnly, Category = "ECP|Features") FString Error;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FECPFeaturesReady);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FECPFeatureStartupFailed, FName, PluginName, const FString&, Reason);

/**
 * 项目唯一注册入口，由 GameFeatures 在引擎初始化时创建。
 * 生命周期长于所有 GameInstance，多个 PIE 共用它，不会因单个玩家退出而卸载功能。
 */
UCLASS(BlueprintType)
class ECPFOUNDATION_API UECPGameFeaturePolicy : public UDefaultGameFeaturesProjectPolicies
{
	GENERATED_BODY()
public:
	virtual void InitGameFeatureManager() override;
	virtual void ShutdownGameFeatureManager() override;

	/** 获取本进程注册入口；初始化前返回空。该指针不是跨网络可复制的玩法状态。 */
	UFUNCTION(BlueprintPure, Category = "ECP|Features")
	static UECPGameFeaturePolicy* GetFeatureRegistry();

	/** 返回所有清单功能是否实际处于 Active；晚绑定就绪事件的蓝图须先调用此查询。
	 * 网络：仅查询当前进程的功能状态，不发送 RPC；true 不代表其他客户端或服务器已就绪。
	 */
	UFUNCTION(BlueprintPure, Category = "ECP|Features")
	bool AreFeaturesReady() const;

	/** 按 .uplugin 名读取本进程状态；未登记名称返回 Unknown，不触发加载或网络请求。 */
	UFUNCTION(BlueprintPure, Category = "ECP|Features")
	FECPGameFeatureStatus GetFeatureStatus(FName PluginName) const;

	/** 返回依赖顺序排列的快照，供启动界面显示具体未就绪或失败功能。
	 * 网络：客户端与服务器各自读取本进程快照，不请求或汇总其他玩家的加载状态。
	 */
	UFUNCTION(BlueprintPure, Category = "ECP|Features")
	TArray<FECPGameFeatureStatus> GetFeatureStatuses() const;

	/** 区分首次加载尚未完成与不可继续的启动失败。
	 * 曾经全部就绪后有功能失活，也必须按需重启的失败处理；一次性就绪事件不会再次唤醒等待者。
	 * @return 当前进程存在启动错误，或已就绪的功能集合不再完整 Active 时返回 true。
	 * 网络：客户端与服务器分别读取各自进程状态，不发送 RPC。
	 */
	UFUNCTION(BlueprintPure, Category = "ECP|Features")
	bool HasStartupFailed() const { return !StartupFailure.IsEmpty() || (bReady && !AreFeaturesReady()); }

	/** 返回启动失败原因；尚未初始化或未全部激活也会返回可读说明，不能据空字符串单独判断就绪。
	 * 网络：只读当前进程的启动诊断，不发送 RPC，也不能据此判断其他玩家的启动结果。
	 */
	UFUNCTION(BlueprintPure, Category = "ECP|Features")
	FString GetStartupFailureReason() const;

	/** 引擎级一次性事件；每个 GameInstance 不重复广播，应先查询 AreFeaturesReady。 */
	UPROPERTY(BlueprintAssignable, Category = "ECP|Features") FECPFeaturesReady OnFeaturesReady;
	/** 失败会停止后续依赖激活，蓝图可以展示名称与原因，不能继续进入玩法地图。 */
	UPROPERTY(BlueprintAssignable, Category = "ECP|Features") FECPFeatureStartupFailed OnFeatureStartupFailed;

private:
	void ActivateNext();
	void CompleteActivation(const UE::GameFeatures::FResult& Result, FName PluginName);
	void FailStartup(FName PluginName, const FString& Reason);
	UPROPERTY(Transient) TArray<FECPGameFeatureStatus> Statuses;
	TArray<FName> ActivationOrder;
	int32 NextFeatureIndex = 0;
	bool bStarted = false;
	bool bReady = false;
	bool bShuttingDown = false;
	FString StartupFailure;
};
